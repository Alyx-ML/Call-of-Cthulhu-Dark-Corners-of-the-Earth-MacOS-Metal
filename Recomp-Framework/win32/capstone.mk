# Capstone 5.0.7 (x86 only), the disassembler of the translator in C (convert/translate32.c). Included by win32.mk.
#
# The source (third_party/capstone-5.0.7: capstone-5.0.7.tar.gz of
# https://github.com/capstone-engine/capstone/archive/refs/tags/5.0.7.tar.gz, SHA-256
# 6427a724726d161d1e05fb49fff8cd0064f67836c04ffca3c11d6d859e719caa, not changed) is not in the repository. It is the
# version of the Python binding that tools/recomp_w32.py uses, so both translators decode the same instructions.
CAPSTONE_SRC  = $(W32)/../third_party/capstone-5.0.7
CAPSTONE_OUT  = $(BUILD)/capstone
CAPSTONE_LIB  = $(CAPSTONE_OUT)/libcapstone.a
CAPSTONE_INC  = -I$(CAPSTONE_SRC)/include
CAPSTONE_C    = cs.c Mapping.c MCInst.c MCInstrDesc.c MCRegisterInfo.c SStream.c utils.c \
                arch/X86/X86Disassembler.c arch/X86/X86DisassemblerDecoder.c arch/X86/X86IntelInstPrinter.c \
                arch/X86/X86InstPrinterCommon.c arch/X86/X86Mapping.c arch/X86/X86Module.c
CAPSTONE_OBJ  = $(patsubst %.c,$(CAPSTONE_OUT)/%.o,$(CAPSTONE_C))
CAPSTONE_FLAGS = -O2 $(ARCH) -mmacosx-version-min=$(MACOS_MIN) -DCAPSTONE_HAS_X86 -DCAPSTONE_USE_SYS_DYN_MEM \
                 -DCAPSTONE_X86_ATT_DISABLE -I$(CAPSTONE_SRC)/include -I$(CAPSTONE_SRC)
$(CAPSTONE_OUT)/%.o: $(CAPSTONE_SRC)/%.c
	@test -f "$(CAPSTONE_SRC)/cs.c" || { echo "the Capstone source is not in $(CAPSTONE_SRC)"; exit 1; }
	@mkdir -p $(dir $@)
	$(CC) $(CAPSTONE_FLAGS) -c $< -o $@
$(CAPSTONE_LIB): $(CAPSTONE_OBJ)
	rm -f $@ && ar rcs $@ $^

# translate32: the translator in C as a command-line tool (the options of tools/recomp_w32.py)
TRANSLATE32_FLAGS = -O2 -g $(ARCH) -mmacosx-version-min=$(MACOS_MIN) -Wall -Wextra -Wno-unused-parameter $(CAPSTONE_INC)
$(BUILD)/translate32: $(W32)/convert/translate32.c $(W32)/convert/translate32_main.c $(W32)/convert/translate32.h $(CAPSTONE_LIB)
	$(CC) $(TRANSLATE32_FLAGS) $(W32)/convert/translate32.c $(W32)/convert/translate32_main.c $(CAPSTONE_LIB) -o $@
translate32: $(BUILD)/translate32

.PHONY: translate32
