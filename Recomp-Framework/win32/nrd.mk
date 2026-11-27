# NVIDIA Real-time Denoisers (NRD, third_party/nrd: NVIDIA-RTX/NRD at a fixed commit, not changed), built as a static
# library for the global illumination denoiser (runtime/d3d9_nrd.mm, REBLUR_DIFFUSE). Included by win32.mk.
#
# The NRD build (CMake) downloads its own dependencies (NVIDIA-RTX/ShaderMake, NVIDIA-RTX/MathLib, sse2neon). It
# embeds no shaders and runs no shader compiler. tools/nrd_dump.cpp writes the shader identifiers of the
# REBLUR_DIFFUSE pipelines; tools/nrd_msl.py compiles each shader with Slang to SPIR-V and changes it to Metal Shading
# Language with SPIRV-Cross (and compiles runtime/nrd_glue.hlsl the same way). The result is $(NRD_OUT)/nrd_msl.h,
# which d3d9_nrd.mm compiles into the runtime.
#
# Tools: CMake, Slang (slangc; install.sh downloads a fixed release) and SPIRV-Cross.
# `make nrd` builds $(NRD_LIBS) and $(NRD_OUT)/nrd_msl.h.
NRD_SRC     = $(W32)/../third_party/nrd
NRD_BUILD   = $(BUILD)/nrd-build
NRD_OUT     = $(BUILD)/nrd
NRD_LIBS    = $(NRD_OUT)/libNRD.a $(NRD_BUILD)/_deps/shadermake-build/libShaderMakeBlob.a
NRD_INC     = -I$(NRD_SRC)/Include -I$(NRD_OUT)
NRD_MSL     = $(NRD_OUT)/nrd_msl.h
CMAKE       ?= cmake
SLANG       ?= slangc
SPIRV_CROSS ?= spirv-cross
CXX         ?= clang++

nrd: $(NRD_LIBS) $(NRD_MSL)
# NRD_NORMAL_ENCODING 2 (R10G10B10A2, the default) and NRD_ROUGHNESS_ENCODING 1 (linear, the default) are the
# encodings that runtime/nrd_glue.hlsl writes. Quad intrinsics need compute shader derivatives, which SPIRV-Cross does
# not give to Metal, so they are off. With NRD_EMBEDS_SPIRV_SHADERS OFF and SHADERMAKE_FIND_COMPILERS OFF, the NRD
# build does not compile shaders and does not look for a shader compiler (nrd_msl.py compiles the shaders).
$(NRD_OUT)/libNRD.a:
	@test -f "$(NRD_SRC)/CMakeLists.txt" || { echo "NRD is not in $(NRD_SRC) (install.sh downloads it)"; exit 1; }
	@mkdir -p "$(NRD_BUILD)" "$(NRD_OUT)"
	$(CMAKE) -S "$(NRD_SRC)" -B "$(NRD_BUILD)" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
	    -DCMAKE_OSX_DEPLOYMENT_TARGET=$(MACOS_MIN) -DNRD_STATIC_LIBRARY=ON -DNRD_EMBEDS_SPIRV_SHADERS=OFF \
	    -DNRD_SUPPORTS_QUAD_INTRINSICS=OFF -DNRD_NORMAL_ENCODING=2 -DNRD_ROUGHNESS_ENCODING=1 \
	    -DSHADERMAKE_FIND_COMPILERS=OFF \
	    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$(abspath $(NRD_OUT))" -DNRD_SHADERS_PATH="$(abspath $(NRD_BUILD))/shaders"
	$(CMAKE) --build "$(NRD_BUILD)" --config Release -j$$(sysctl -n hw.ncpu)
$(NRD_BUILD)/_deps/shadermake-build/libShaderMakeBlob.a: $(NRD_OUT)/libNRD.a

$(NRD_OUT)/nrd_dump: $(W32)/tools/nrd_dump.cpp $(NRD_LIBS)
	$(CXX) -std=c++17 -O1 $(ARCH) -mmacosx-version-min=$(MACOS_MIN) -Wall -Wextra -DNRD_STATIC_LIBRARY=1 \
	    -I$(NRD_SRC)/Include $< $(NRD_LIBS) -o $@
$(NRD_MSL): $(NRD_OUT)/nrd_dump $(W32)/tools/nrd_msl.py $(W32)/runtime/nrd_glue.hlsl
	$(PYTHON) $(W32)/tools/nrd_msl.py --dump $(NRD_OUT)/nrd_dump --slang "$(SLANG)" --spirv-cross "$(SPIRV_CROSS)" \
	    --nrd "$(NRD_SRC)" --mathlib "$(NRD_BUILD)/_deps/mathlib-src" --glue $(W32)/runtime/nrd_glue.hlsl --out $@

.PHONY: nrd
