# FFmpeg (third_party/ffmpeg, a git submodule), built as minimal LGPL libraries for the Windows Media Format SDK
# replacement (runtime/wmvcore.c): the ASF demuxer, the WMV / VC-1 / WMA decoders, swscale and swresample.
# Included by win32.mk; `make ffmpeg` builds them.
# The libraries are dylibs (install name @rpath/<name>), and the app has them in Contents/Frameworks: the LGPL lets
# the player replace them. $(FFMPEG_OUT)/bundle has each dylib with the file name of its install name (for
# make_app.py --lib).
FFMPEG_SRC   = $(W32)/../third_party/ffmpeg
FFMPEG_BUILD = $(BUILD)/ffmpeg-build
FFMPEG_OUT   = $(BUILD)/ffmpeg
FFMPEG_NAMES = avformat avcodec swscale swresample avutil
FFMPEG_LIBS  = $(foreach l,$(FFMPEG_NAMES),$(FFMPEG_OUT)/lib/lib$(l).dylib)
FFMPEG_STAMP = $(FFMPEG_OUT)/bundle/.done
FFMPEG_INC   = -I$(FFMPEG_OUT)/include
FFMPEG_DECODERS = wmv1,wmv2,wmv3,wmv3image,vc1,vc1image,msmpeg4v3,wmav1,wmav2,wmapro,wmavoice,wmalossless,pcm_s16le

ffmpeg: $(FFMPEG_STAMP)
$(FFMPEG_LIBS): $(FFMPEG_STAMP)
$(FFMPEG_STAMP):
	@test -f "$(FFMPEG_SRC)/configure" || git -C "$(W32)/.." submodule update --init third_party/ffmpeg
	@mkdir -p "$(FFMPEG_BUILD)"
	@# configure refuses an out-of-tree source path with spaces; a `src` link in the build folder is its supported layout.
	@# The prefix is relative to FFMPEG_BUILD (make install runs there): the libraries keep the configure arguments
	@# (avutil_configuration), so an absolute path would put the path of the build Mac in the app.
	src="$$(cd "$(FFMPEG_SRC)" && pwd)" && \
	cd "$(FFMPEG_BUILD)" && ln -sfn "$$src" src && ./src/configure --prefix=../$(notdir $(FFMPEG_OUT)) --arch=aarch64 --cc=clang \
	    --extra-cflags="-mmacosx-version-min=$(MACOS_MIN)" --extra-ldflags="-mmacosx-version-min=$(MACOS_MIN)" \
	    --enable-shared --disable-static --install-name-dir=@rpath --enable-pic --disable-programs --disable-doc --disable-network \
	    --disable-autodetect --disable-everything --disable-avdevice --disable-avfilter \
	    --enable-demuxer=asf --enable-decoder=$(FFMPEG_DECODERS) --enable-parser=vc1 --enable-swscale --enable-swresample
	$(MAKE) -C "$(FFMPEG_BUILD)" -j$$(sysctl -n hw.ncpu) install
	rm -rf "$(FFMPEG_OUT)/bundle" && mkdir -p "$(FFMPEG_OUT)/bundle"
	for l in $(FFMPEG_NAMES); do \
	    id="$$(otool -D "$(FFMPEG_OUT)/lib/lib$$l.dylib" | tail -1)" && case "$$id" in @rpath/*) ;; *) echo "lib$$l: install name $$id"; exit 1 ;; esac && \
	    cp -L "$(FFMPEG_OUT)/lib/lib$$l.dylib" "$(FFMPEG_OUT)/bundle/$${id#@rpath/}" || exit 1; \
	done
	touch $@

.PHONY: ffmpeg
