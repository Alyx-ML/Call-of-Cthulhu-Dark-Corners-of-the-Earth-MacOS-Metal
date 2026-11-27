# The compiler of the app (clang and lld of LLVM 23.1.6), built from the LLVM source release on the developer's Mac.
# The app compiles and links the translated game code with it on the player's Mac (runtime/convert32.c), so the
# player needs no developer tools. Included by win32.mk.
#
# The source (third_party/llvm-project-23.1.6.src: the folders llvm, clang, lld, cmake, third-party and
# libunwind/include of llvm-project-23.1.6.src.tar.xz, SHA-256
# d3b86ab29cfb4445a6652e5c2642ccc75e04e38084dde3701d323320f35a31da, not changed) is not in the repository.
# The build has only the AArch64 target and no optional libraries (zlib, zstd, libxml2, libedit), so the binaries use
# only the libraries of macOS. LLVM_APPEND_VC_REV=OFF: the version has no revision of the repository that holds the
# source, so the conversion ID changes only with the compiler.
#
# LLVM_TOOL_LLVM_DRIVER_BUILD=ON: clang and lld are one binary (`llvm clang ...`, `llvm ld64.lld ...`), 91 MB after
# strip -x instead of 130 MB for the two binaries.
#
# Tools: CMake and Ninja.
# `make llvm` builds $(LLVM_BIN) (stripped) and the clang headers ($(LLVM_OUT)/lib/clang).
LLVM_VERSION = 23.1.6
LLVM_SRC     = $(W32)/../third_party/llvm-project-$(LLVM_VERSION).src
LLVM_BUILD   = $(BUILD)/llvm-build-driver
LLVM_OUT     = $(BUILD)/llvm
LLVM_BIN     = $(LLVM_OUT)/bin/llvm
LLVM_RESOURCE = $(LLVM_OUT)/lib/clang/23
LLVM_CLANG   = $(LLVM_BIN) clang -resource-dir $(LLVM_RESOURCE)
LLVM_LLD     = $(LLVM_BIN) ld64.lld
NINJA       ?= ninja

$(LLVM_BUILD)/build.ninja: $(W32)/llvm.mk
	@test -f "$(LLVM_SRC)/llvm/CMakeLists.txt" || { echo "the LLVM $(LLVM_VERSION) source is not in $(LLVM_SRC)"; exit 1; }
	$(CMAKE) -G Ninja -S "$(LLVM_SRC)/llvm" -B "$(LLVM_BUILD)" -DCMAKE_MAKE_PROGRAM="$$(command -v $(NINJA))" \
	    -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$(MACOS_MIN) \
	    -DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_TARGETS_TO_BUILD=AArch64 -DLLVM_ENABLE_ASSERTIONS=OFF \
	    -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_LIBEDIT=OFF \
	    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF \
	    -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_PLUGIN_SUPPORT=OFF -DLLVM_APPEND_VC_REV=OFF \
	    -DLLVM_TOOL_LLVM_DRIVER_BUILD=ON
$(LLVM_BIN): $(LLVM_BUILD)/build.ninja
	$(NINJA) -C "$(LLVM_BUILD)" llvm-driver clang-resource-headers
	@mkdir -p "$(LLVM_OUT)/bin"
	cp "$(LLVM_BUILD)/bin/llvm" "$@.tmp" && strip -x "$@.tmp" && mv "$@.tmp" "$@"
	rm -rf "$(LLVM_OUT)/lib" && mkdir -p "$(LLVM_OUT)/lib" && cp -R "$(LLVM_BUILD)/lib/clang" "$(LLVM_OUT)/lib/clang"
llvm: $(LLVM_BIN)

.PHONY: llvm
