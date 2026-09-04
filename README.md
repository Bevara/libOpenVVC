# libOpenVVC
This filter decodes VVC (H.266) video streams using VVdeC.

## Requirements

[CMake](https://cmake.org/) is used as a build system. To install it, follow
[Debian build instructions](developing_in_debian.md).

[Emscripten SDK](https://emscripten.org/) is required for building
WebAssembly artifacts. To install it, follow the
[Download and Install](https://emscripten.org/docs/getting_started/downloads.html)
guide:

```bash
cd $OPT

# Get the emsdk repo.
git clone https://github.com/emscripten-core/emsdk.git

# Enter that directory.
cd emsdk

# Download and install the latest SDK tools.
./emsdk install latest

# Make the "latest" SDK "active" for the current user. (writes ~/.emscripten file)
./emsdk activate latest
```

## Building the accessor

```bash
# Setup EMSDK and other environment variables. In practice EMSDK is set to be
# $OPT/emsdk.
source $OPT/emsdk/emsdk_env.sh

# Assuming you are in the root level of the cloned repo :
emcmake cmake .
emmake make
```

Once built, you can use and distribute libOpenVVC_1.wasm with your universal tags.

## Documentation

For more details, please visit our documentation at https://bevara.com/documentation/develop/.
