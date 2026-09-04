/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / VVC (H.266) video decoder filter
 *  based on VVdeC (https://github.com/fraunhoferhhi/vvdec)
 *
 *  Pairs with a container demuxer (e.g. isobmff for vvc1/vvi1 tracks, or the
 *  m2tsdmx built into the solver) that provides framed, length-prefixed VVC
 *  access units with an out-of-band VVCC decoder config
 *  (GF_PROP_PID_DECODER_CONFIG) - this filter does not parse Annex-B start
 *  codes on its input itself.
 *
 *  VVdeC only accepts Annex-B formatted access units, so every NAL unit is
 *  re-emitted here with a 4-byte start code into a scratch buffer, with the
 *  VPS/SPS/PPS from the VVCC prepended to the first access unit.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <gpac/mpeg4_odf.h>
#include <string.h>
#include <stdio.h>

#include <vvdec/vvdec.h>

typedef struct
{
	GF_FilterPid *ipid, *opid;

	vvdecDecoder *decoder;
	vvdecAccessUnit *au;

	/*Annex-B copy of the VPS/SPS/PPS carried in the VVCC, prepended to the
	first access unit pushed to the decoder*/
	u8 *param_sets;
	u32 param_sets_size;
	Bool param_sets_sent;

	u32 nalu_size_length;
	u32 width, height, stride, pix_fmt;
	/*output format announced from the VVCC at configure time, before any
	picture has been decoded*/
	u32 cfg_chroma_format, cfg_bit_depth;
	Bool is_playing;
} GF_VVCDecCtx;

static const u8 vvcdec_start_code[4] = {0, 0, 0, 1};

/*grows ctx->au payload so that it can hold at least needed bytes.
vvdec allocates that payload with plain malloc (vvdec_accessUnit_alloc_payload)
and releases it the same way from vvdec_accessUnit_free, so realloc keeps
ownership consistent - and unlike alloc_payload it preserves the bytes already
written into the access unit*/
static GF_Err vvcdec_au_ensure_size(GF_VVCDecCtx *ctx, int needed)
{
	unsigned char *buf;
	int new_size;

	if (ctx->au->payloadSize >= needed) return GF_OK;

	/*grow geometrically: an access unit is filled one NAL at a time*/
	new_size = ctx->au->payloadSize * 2;
	if (new_size < needed) new_size = needed;

	buf = (unsigned char *)realloc(ctx->au->payload, new_size);
	if (!buf) return GF_OUT_OF_MEM;

	ctx->au->payload = buf;
	ctx->au->payloadSize = new_size;
	return GF_OK;
}

static GF_Err vvcdec_au_append(GF_VVCDecCtx *ctx, const u8 *data, u32 size)
{
	GF_Err e = vvcdec_au_ensure_size(ctx, ctx->au->payloadUsedSize + (int)size);
	if (e) return e;
	memcpy(ctx->au->payload + ctx->au->payloadUsedSize, data, size);
	ctx->au->payloadUsedSize += size;
	return GF_OK;
}

/*maps a chroma format / bit depth pair onto a gpac pixel format, or 0 if it is
not one this filter can forward as-is. Takes the raw values so it can be fed
either from a decoded frame or from the chroma_format/bit_depth of the VVCC,
which is what lets configure_pid announce the output format before the first
picture comes out.*/
static u32 vvcdec_get_pixel_format(u32 color_format, u32 bit_depth)
{
	Bool is_10b = (bit_depth > 8) ? GF_TRUE : GF_FALSE;
	switch (color_format)
	{
	case VVDEC_CF_YUV400_PLANAR:
		return is_10b ? 0 : GF_PIXEL_GREYSCALE;
	case VVDEC_CF_YUV420_PLANAR:
		return is_10b ? GF_PIXEL_YUV_10 : GF_PIXEL_YUV;
	case VVDEC_CF_YUV422_PLANAR:
		return is_10b ? GF_PIXEL_YUV422_10 : GF_PIXEL_YUV422;
	case VVDEC_CF_YUV444_PLANAR:
		return is_10b ? GF_PIXEL_YUV444_10 : GF_PIXEL_YUV444;
	default:
		return 0;
	}
}

/*builds the Annex-B blob of parameter set NALs found in the VVCC, and picks up
the NAL length field size used by the input access units*/
static GF_Err vvcdec_parse_dsi(GF_VVCDecCtx *ctx, const GF_PropertyValue *dsi)
{
	GF_VVCConfig *cfg;
	u32 i, j, size = 0, pos = 0;

	cfg = gf_odf_vvc_cfg_read(dsi->value.data.ptr, dsi->value.data.size);
	if (!cfg) return GF_NON_COMPLIANT_BITSTREAM;

	ctx->nalu_size_length = cfg->nal_unit_size;
	ctx->cfg_chroma_format = cfg->chroma_format;
	ctx->cfg_bit_depth = cfg->bit_depth;

	for (i = 0; i < gf_list_count(cfg->param_array); i++)
	{
		GF_NALUFFParamArray *ar = (GF_NALUFFParamArray *)gf_list_get(cfg->param_array, i);
		for (j = 0; j < gf_list_count(ar->nalus); j++)
		{
			GF_NALUFFParam *sl = (GF_NALUFFParam *)gf_list_get(ar->nalus, j);
			size += 4 + sl->size;
		}
	}

	if (ctx->param_sets) gf_free(ctx->param_sets);
	ctx->param_sets = NULL;
	ctx->param_sets_size = 0;
	ctx->param_sets_sent = GF_FALSE;

	if (size)
	{
		ctx->param_sets = gf_malloc(sizeof(u8) * size);
		if (!ctx->param_sets)
		{
			gf_odf_vvc_cfg_del(cfg);
			return GF_OUT_OF_MEM;
		}
		for (i = 0; i < gf_list_count(cfg->param_array); i++)
		{
			GF_NALUFFParamArray *ar = (GF_NALUFFParamArray *)gf_list_get(cfg->param_array, i);
			for (j = 0; j < gf_list_count(ar->nalus); j++)
			{
				GF_NALUFFParam *sl = (GF_NALUFFParam *)gf_list_get(ar->nalus, j);
				memcpy(ctx->param_sets + pos, vvcdec_start_code, 4);
				pos += 4;
				memcpy(ctx->param_sets + pos, sl->data, sl->size);
				pos += sl->size;
			}
		}
		ctx->param_sets_size = size;
	}

	gf_odf_vvc_cfg_del(cfg);
	return GF_OK;
}

static GF_Err vvcdec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	const GF_PropertyValue *dsi;
	u32 pix_fmt;
	GF_VVCDecCtx *ctx = (GF_VVCDecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		if (ctx->decoder)
		{
			vvdec_decoder_close(ctx->decoder);
			ctx->decoder = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;

	if (!ctx->opid)
	{
		ctx->opid = gf_filter_pid_new(filter);
	}

	dsi = gf_filter_pid_get_property(pid, GF_PROP_PID_DECODER_CONFIG);
	if (dsi && dsi->value.data.size)
	{
		GF_Err e = vvcdec_parse_dsi(ctx, dsi);
		if (e) return e;
	}

	gf_filter_pid_copy_properties(ctx->opid, ctx->ipid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));

	/*announce the pixel format now, from the VVCC, rather than waiting for the
	first decoded picture: the graph is resolved at connection time, and a RAW
	video PID with no GF_PROP_PID_PIXFMT matches no encoder - the session then
	links this filter straight to the muxer and writes raw frames instead of
	going through the requested encoder. vvcdec_send_frame() still corrects
	these if the stream turns out to differ from its config record.*/
	pix_fmt = vvcdec_get_pixel_format(ctx->cfg_chroma_format, ctx->cfg_bit_depth);
	if (pix_fmt)
	{
		const GF_PropertyValue *p = gf_filter_pid_get_property(pid, GF_PROP_PID_WIDTH);
		u32 w = p ? p->value.uint : 0;

		ctx->pix_fmt = pix_fmt;
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(pix_fmt));
		if (w)
		{
			ctx->width = w;
			ctx->stride = w * ((ctx->cfg_bit_depth > 8) ? 2 : 1);
			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(ctx->stride));
		}
	}

	if (!ctx->decoder)
	{
		vvdecParams params;
		vvdec_params_default(&params);
		/*0 = decode in the calling thread: vvdec is built here without
		pthread support, see filters/third_parties/build_thirdparties.sh*/
		params.threads = 0;
		params.parseDelay = 0;
		params.logLevel = VVDEC_SILENT;
		params.filmGrainSynthesis = false;
		params.verifyPictureHash = false;

		ctx->decoder = vvdec_decoder_open(&params);
		if (!ctx->decoder) return GF_OUT_OF_MEM;
	}
	if (!ctx->au)
	{
		ctx->au = vvdec_accessUnit_alloc();
		if (!ctx->au) return GF_OUT_OF_MEM;
	}

	return GF_OK;
}

static Bool vvcdec_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	GF_VVCDecCtx *ctx = (GF_VVCDecCtx *)gf_filter_get_udta(filter);
	switch (evt->base.type)
	{
	case GF_FEVT_PLAY:
		ctx->is_playing = GF_TRUE;
		return GF_FALSE;
	case GF_FEVT_STOP:
		ctx->is_playing = GF_FALSE;
		return GF_FALSE;
	default:
		return GF_FALSE;
	}
}

static void vvcdec_send_frame(GF_VVCDecCtx *ctx, vvdecFrame *frame)
{
	GF_FilterPacket *dst_pck;
	u8 *output;
	u32 i, y, out_size = 0, luma_stride, pix_fmt;

	pix_fmt = vvcdec_get_pixel_format(frame->colorFormat, frame->bitDepth);
	if (!pix_fmt)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[VVCDec] Unsupported output format (color format %d, bit depth %d)\n", frame->colorFormat, frame->bitDepth));
		return;
	}

	/*gpac describes the packet with a single luma stride and expects the
	chroma planes packed right after the luma one, so the planes are copied
	row by row, dropping vvdec's own padding*/
	luma_stride = frame->planes[0].width * frame->planes[0].bytesPerSample;
	for (i = 0; i < frame->numPlanes; i++)
	{
		out_size += frame->planes[i].width * frame->planes[i].height * frame->planes[i].bytesPerSample;
	}

	if ((frame->width != ctx->width) || (frame->height != ctx->height)
		|| (luma_stride != ctx->stride) || (pix_fmt != ctx->pix_fmt))
	{
		ctx->width = frame->width;
		ctx->height = frame->height;
		ctx->stride = luma_stride;
		ctx->pix_fmt = pix_fmt;
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(frame->width));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(frame->height));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(luma_stride));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(pix_fmt));
	}

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, out_size, &output);
	if (!dst_pck) return;

	for (i = 0; i < frame->numPlanes; i++)
	{
		vvdecPlane *plane = &frame->planes[i];
		u32 row_size = plane->width * plane->bytesPerSample;
		for (y = 0; y < plane->height; y++)
		{
			memcpy(output, plane->ptr + (size_t)y * plane->stride, row_size);
			output += row_size;
		}
	}

	if (frame->ctsValid)
		gf_filter_pck_set_cts(dst_pck, frame->cts);
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_send(dst_pck);
}

/*runs one vvdec call and forwards the picture it may have produced. Returns the
vvdec error code so the caller can tell "no picture yet" from a real failure*/
static int vvcdec_decode_au(GF_VVCDecCtx *ctx, Bool flush)
{
	vvdecFrame *frame = NULL;
	int ret;

	if (flush)
		ret = vvdec_flush(ctx->decoder, &frame);
	else
		ret = vvdec_decode(ctx->decoder, ctx->au, &frame);

	if (frame)
	{
		vvcdec_send_frame(ctx, frame);
		vvdec_frame_unref(ctx->decoder, frame);
	}

	if ((ret != VVDEC_OK) && (ret != VVDEC_TRY_AGAIN) && (ret != VVDEC_EOF))
	{
		GF_LOG(GF_LOG_WARNING, GF_LOG_CODEC, ("[VVCDec] %s: %s (%s)\n",
			flush ? "flush" : "decode", vvdec_get_error_msg(ret), vvdec_get_last_error(ctx->decoder)));
	}
	return ret;
}

/*pushes one Annex-B NAL unit (start code included) into the decoder.
VVdeC hands out at most one decoded picture per vvdec_decode() call, so feeding
it one NAL at a time - the way vvdecapp does - is what lets the reordering
backlog drain: an access unit made of several NALs gives the decoder as many
opportunities to output a picture as it has NALs. Feeding whole access units
instead leaves the pictures buffered ahead of the first output stuck there for
the rest of the stream.*/
static GF_Err vvcdec_push_annexb(GF_VVCDecCtx *ctx, const u8 *annexb, u32 size, u64 cts, Bool rap)
{
	GF_Err e;

	ctx->au->payloadUsedSize = 0;
	ctx->au->cts = cts;
	ctx->au->ctsValid = true;
	ctx->au->dtsValid = false;
	ctx->au->rap = rap ? true : false;

	e = vvcdec_au_append(ctx, annexb, size);
	if (e) return e;

	vvcdec_decode_au(ctx, GF_FALSE);
	return GF_OK;
}

static GF_Err vvcdec_push_nal(GF_VVCDecCtx *ctx, const u8 *nal, u32 size, u64 cts, Bool rap)
{
	GF_Err e;

	ctx->au->payloadUsedSize = 0;
	ctx->au->cts = cts;
	ctx->au->ctsValid = true;
	ctx->au->dtsValid = false;
	ctx->au->rap = rap ? true : false;

	e = vvcdec_au_append(ctx, vvcdec_start_code, 4);
	if (!e) e = vvcdec_au_append(ctx, nal, size);
	if (e) return e;

	vvcdec_decode_au(ctx, GF_FALSE);
	return GF_OK;
}

static GF_Err vvcdec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck;
	const u8 *data;
	u32 size, pos;
	u64 cts;
	Bool rap;
	GF_Err e;
	GF_VVCDecCtx *ctx = (GF_VVCDecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			int ret;
			do
			{
				ret = vvcdec_decode_au(ctx, GF_TRUE);
			} while (ret == VVDEC_OK);
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = gf_filter_pck_get_data(pck, &size);
	if (!data)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_IO_ERR;
	}
	cts = gf_filter_pck_get_cts(pck);

	if (ctx->param_sets_size && !ctx->param_sets_sent)
	{
		e = vvcdec_push_annexb(ctx, ctx->param_sets, ctx->param_sets_size, cts, GF_FALSE);
		if (e)
		{
			gf_filter_pid_drop_packet(ctx->ipid);
			return e;
		}
		ctx->param_sets_sent = GF_TRUE;
	}

	rap = (gf_filter_pck_get_sap(pck) == GF_FILTER_SAP_1) ? GF_TRUE : GF_FALSE;
	pos = 0;
	while (pos + ctx->nalu_size_length <= size)
	{
		u32 i, nal_len = 0;
		for (i = 0; i < ctx->nalu_size_length; i++)
		{
			nal_len = (nal_len << 8) | data[pos + i];
		}
		pos += ctx->nalu_size_length;
		if (pos + nal_len > size) break;

		e = vvcdec_push_nal(ctx, data + pos, nal_len, cts, rap);
		if (e)
		{
			gf_filter_pid_drop_packet(ctx->ipid);
			return e;
		}
		pos += nal_len;
	}

	gf_filter_pid_drop_packet(ctx->ipid);
	return GF_OK;
}

static GF_Err vvcdec_initialize(GF_Filter *filter)
{
	GF_VVCDecCtx *ctx = (GF_VVCDecCtx *)gf_filter_get_udta(filter);
	/*VVCC always carries this, but a stream configured without a decoder
	config still needs a sane default (4-byte lengths)*/
	ctx->nalu_size_length = 4;
	return GF_OK;
}

static void vvcdec_finalize(GF_Filter *filter)
{
	GF_VVCDecCtx *ctx = (GF_VVCDecCtx *)gf_filter_get_udta(filter);
	if (ctx->decoder)
	{
		vvdec_decoder_close(ctx->decoder);
		ctx->decoder = NULL;
	}
	if (ctx->au)
	{
		vvdec_accessUnit_free(ctx->au);
		ctx->au = NULL;
	}
	if (ctx->param_sets)
	{
		gf_free(ctx->param_sets);
		ctx->param_sets = NULL;
	}
}

static const GF_FilterCapability VVCDecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_CODECID, GF_CODECID_VVC),
		CAP_BOOL(GF_CAPS_INPUT_EXCLUDED, GF_PROP_PID_UNFRAMED, GF_TRUE),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister VVCDecoderRegister = {
	.name = "vvcdec",
	GF_FS_SET_DESCRIPTION("VVC video decoder")
		GF_FS_SET_HELP("This filter decodes VVC (H.266) video elementary streams using VVdeC.")
			.private_size = sizeof(GF_VVCDecCtx),
	SETCAPS(VVCDecCaps),
	.initialize = vvcdec_initialize,
	.configure_pid = vvcdec_configure_pid,
	.process = vvcdec_process,
	.process_event = vvcdec_process_event,
	.finalize = vvcdec_finalize,
};

const GF_FilterRegister * EMSCRIPTEN_KEEPALIVE vvcdec_register(GF_FilterSession *session)
{
	return &VVCDecoderRegister;
}

/*Bevara: side modules register their own filters at load time.*/
#include "filter_register.h"
__attribute__((constructor))
void register_vvcdec(void) {
    gf_filter_auto_register("vvcdec", vvcdec_register);
}
