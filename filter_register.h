typedef const GF_FilterRegister *(*filter_reg_fun)(GF_FilterSession *session);

typedef struct {
    const char *name;
    filter_reg_fun fun;
} BuiltinReg;

void gf_filter_auto_register(const char *name, filter_reg_fun fun);
