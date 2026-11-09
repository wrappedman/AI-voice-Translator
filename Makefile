# OpenSpeech AI — build system
# ---------------------------------------------------------------------------
#  make linux      : portable core + headless host (Linux x86-64)
#  make tests      : self-test suite, no network, no display
#  make snapshots  : render every screen to PNG (dist/screenshots)
#  make win        : dist/bin/OpenSpeechAI.exe — static x64, no runtime deps
#  make dist       : release zip of the Windows build + SHA-256 sums
#  make bootstrap  : fetch the vendored zig toolchain
#  make clean
# ---------------------------------------------------------------------------
CC      ?= gcc
CXX     ?= g++
PYTHON  ?= python3

# zig is not vendored in git (it is ~120 MB). `make bootstrap` puts it here.
ZIG     ?= $(HOME)/.cache/openspeech/zig/zig

BUILD   := build
SRC     := src
DIST    := dist

# -D_DEFAULT_SOURCE: setenv/unsetenv live behind it in glibc's strict-C11 mode.
CFLAGS_COMMON := -std=c11 -O2 -g -fno-omit-frame-pointer -Wall -Wextra -D_DEFAULT_SOURCE \
                 -Wno-unused-parameter -Wno-missing-field-initializers -MMD -MP -pthread
CXXFLAGS_COMMON := -std=c++17 -O2 -fno-omit-frame-pointer -fno-rtti -fno-exceptions \
                   -pthread -MMD -MP

FT_DIR  := $(SRC)/third_party/freetype
HB_DIR  := $(SRC)/third_party/harfbuzz
FTCFG   := -DFT2_BUILD_LIBRARY -DFT_CONFIG_MODULES_H='<ftmodule_osai.h>' \
           -DFT_CONFIG_OPTION_H='<ftoption_osai.h>' -I$(SRC)/platform/ft \
           -I$(FT_DIR)/include -I$(FT_DIR)

INCLUDES := -I$(SRC) -I$(SRC)/core -I$(SRC)/third_party -I$(SRC)/third_party/sqlite \
            -I$(SRC)/app -I$(SRC)/ui -I$(SRC)/platform -I$(SRC)/engines -I$(HB_DIR)/src \
            -I$(FT_DIR)/include

SQLITE_OPTS := -DSQLITE_THREADSAFE=1 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_LOAD_EXTENSION \
               -DSQLITE_OMIT_DEPRECATED -DSQLITE_DQS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
               -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA

# Portable sources: identical on every target.
CORE_SRC := $(wildcard $(SRC)/core/*.c)
ENG_SRC  := $(wildcard $(SRC)/engines/*.c)
APP_SRC  := $(wildcard $(SRC)/app/*.c)
UI_SRC   := $(wildcard $(SRC)/ui/*.c)
LIB_SRC  := $(CORE_SRC) $(ENG_SRC) $(APP_SRC) $(UI_SRC)

FT_CORE_SRC := $(FT_DIR)/src/autofit/autofit.c $(FT_DIR)/src/base/ftbase.c \
               $(FT_DIR)/src/base/ftbbox.c $(FT_DIR)/src/base/ftbitmap.c \
               $(FT_DIR)/src/base/ftdebug.c $(FT_DIR)/src/base/ftglyph.c \
               $(FT_DIR)/src/base/ftinit.c $(FT_DIR)/src/base/ftmm.c \
               $(FT_DIR)/src/base/ftstroke.c $(FT_DIR)/src/base/ftsynth.c \
               $(FT_DIR)/src/base/ftsystem.c $(FT_DIR)/src/base/fttype1.c \
               $(FT_DIR)/src/cff/cff.c $(FT_DIR)/src/gzip/ftgzip.c \
               $(FT_DIR)/src/psaux/psaux.c $(FT_DIR)/src/pshinter/pshinter.c \
               $(FT_DIR)/src/psnames/psnames.c $(FT_DIR)/src/sfnt/sfnt.c \
               $(FT_DIR)/src/smooth/smooth.c $(FT_DIR)/src/truetype/truetype.c

# HarfBuzz is one amalgamated C++ file; FreeType is compiled in.
HBFLAGS := -DHB_NO_MT -DHAVE_FREETYPE -DHB_EXPERIMENTAL_API \
           -I$(HB_DIR)/src -I$(FT_DIR)/include

# whisper.cpp 1.7.6 (MIT, src/third_party/whisper/LICENSE): the speech engine for
# ggml model files. C and C++ sources are listed separately; the C++ part and
# the bridge are the only C++ the application's own code reaches.
WSP_DIR := $(SRC)/third_party/whisper
WSP_C   := $(WSP_DIR)/ggml/src/ggml.c $(WSP_DIR)/ggml/src/ggml-alloc.c \
           $(WSP_DIR)/ggml/src/ggml-quants.c $(WSP_DIR)/ggml/src/ggml-cpu/ggml-cpu.c \
           $(WSP_DIR)/ggml/src/ggml-cpu/quants.c
WSP_CPP := $(WSP_DIR)/ggml/src/ggml.cpp $(WSP_DIR)/ggml/src/ggml-backend.cpp \
           $(WSP_DIR)/ggml/src/ggml-backend-reg.cpp $(WSP_DIR)/ggml/src/ggml-opt.cpp \
           $(WSP_DIR)/ggml/src/ggml-threading.cpp $(WSP_DIR)/ggml/src/gguf.cpp \
           $(WSP_DIR)/ggml/src/ggml-cpu/ggml-cpu.cpp $(WSP_DIR)/ggml/src/ggml-cpu/traits.cpp \
           $(WSP_DIR)/ggml/src/ggml-cpu/ops.cpp $(WSP_DIR)/ggml/src/ggml-cpu/binary-ops.cpp \
           $(WSP_DIR)/ggml/src/ggml-cpu/unary-ops.cpp $(WSP_DIR)/ggml/src/ggml-cpu/vec.cpp \
           $(WSP_DIR)/ggml/src/ggml-cpu/hbm.cpp $(WSP_DIR)/ggml/src/ggml-cpu/repack.cpp \
           $(WSP_DIR)/src/whisper.cpp
WSP_INC := -I$(WSP_DIR)/include -I$(WSP_DIR)/ggml/include -I$(WSP_DIR)/ggml/src \
           -I$(WSP_DIR)/ggml/src/ggml-cpu -I$(WSP_DIR)/src
# Generic CPU kernels: one binary that runs on every x86-64 PC, no SIMD dispatch.
WSP_DEF := -D_GNU_SOURCE -DNDEBUG -DGGML_USE_CPU -DGGML_CPU_GENERIC -DGGML_USE_LLAMAFILE=0
WSP_HDR := $(WSP_DIR)/include/whisper.h $(SRC)/engines/whisper_bridge.h
WSP_BRIDGE_SRC := $(SRC)/engines/whisper_bridge.cpp

# ===========================================================================
#  Linux (host) — headless host binary used by tests and snapshots
# ===========================================================================
LINUX_BIN := $(BUILD)/openspeech
LINUX_WSP_OBJ := $(patsubst $(WSP_DIR)/%.c,$(BUILD)/lin/third/whisper/%.c.o,$(WSP_C)) \
                 $(patsubst $(WSP_DIR)/%.cpp,$(BUILD)/lin/third/whisper/%.cpp.o,$(WSP_CPP)) \
                 $(BUILD)/lin/src/engines/whisper_bridge.o
LINUX_OBJ := $(patsubst %.c,$(BUILD)/lin/%.o,$(LIB_SRC)) \
             $(BUILD)/lin/src/platform/mic.o \
             $(BUILD)/lin/src/main_linux.o \
             $(BUILD)/lin/sqlite3.o $(BUILD)/lin/stb_vorbis.o \
             $(BUILD)/lin/ft.a $(BUILD)/lin/hb.o $(LINUX_WSP_OBJ)

# The neural MT / language worker is deliberately a separate executable. Its
# pinned ggml version must not collide with Whisper's ggml in the GUI process.
LLAMA_DIR := $(SRC)/third_party/llama
FTXT_DIR  := $(SRC)/third_party/fasttext
LLAMA_C   := $(wildcard $(LLAMA_DIR)/ggml/src/*.c) $(wildcard $(LLAMA_DIR)/ggml/src/ggml-cpu/*.c)
LLAMA_CPP := $(wildcard $(LLAMA_DIR)/src/*.cpp) $(wildcard $(LLAMA_DIR)/ggml/src/*.cpp) \
             $(filter-out $(LLAMA_DIR)/ggml/src/ggml-cpu/arch/x82%,$(wildcard $(LLAMA_DIR)/ggml/src/ggml-cpu/*.cpp))
LLAMA_INC := -I$(LLAMA_DIR)/include -I$(LLAMA_DIR)/ggml/include -I$(LLAMA_DIR)/ggml/src \
             -I$(LLAMA_DIR)/ggml/src/ggml-cpu -I$(LLAMA_DIR)/src -I$(SRC)/third_party \
             -I$(FTXT_DIR)/src -I$(SRC)/engines -I$(SRC)/core
LLAMA_DEF := -DGGML_VERSION=\"b6018\" -DGGML_COMMIT=\"bda62193\" -D_GNU_SOURCE -DNDEBUG -DGGML_USE_CPU -DGGML_CPU_GENERIC -DGGML_USE_LLAMAFILE=0
FTXT_SRC  := $(wildcard $(FTXT_DIR)/src/*.cc)
WORKER_SRC := $(SRC)/worker/worker.cpp
# The worker shares the language/script table with the GUI (it is what decides
# whether a translation came back in the target language).
LANGSCRIPT_SRC := $(SRC)/engines/lang_script.c
WORKER_LINUX := $(BUILD)/OpenSpeechWorker
WORKER_WIN := $(DIST)/bin/OpenSpeechWorker.exe
WORKER_OBJ := $(patsubst %.c,$(BUILD)/worker-lin/%.c.o,$(LLAMA_C)) \
              $(patsubst %.cpp,$(BUILD)/worker-lin/%.cpp.o,$(LLAMA_CPP)) \
              $(patsubst %.cc,$(BUILD)/worker-lin/%.cc.o,$(FTXT_SRC)) \
              $(BUILD)/worker-lin/src/engines/lang_script.o \
              $(BUILD)/worker-lin/$(WORKER_SRC).o

$(BUILD)/worker-lin/src/engines/lang_script.o: $(LANGSCRIPT_SRC) $(SRC)/engines/lang_script.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 $(LLAMA_INC) -I$(SRC)/core -I$(SRC)/engines -c $< -o $@

.PHONY: worker
worker: $(WORKER_LINUX)
$(BUILD)/worker-lin/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -pthread $(LLAMA_DEF) $(LLAMA_INC) -c $< -o $@
$(BUILD)/worker-lin/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -O2 -pthread $(LLAMA_DEF) $(LLAMA_INC) -c $< -o $@
$(BUILD)/worker-lin/%.cc.o: %.cc
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -O2 -pthread $(LLAMA_INC) -c $< -o $@
$(WORKER_LINUX): $(WORKER_OBJ)
	$(CXX) -o $@ $^ -lm -lpthread -ldl

.PHONY: linux
linux: $(LINUX_BIN) $(WORKER_LINUX)

$(BUILD)/lin/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_COMMON) $(INCLUDES) -c $< -o $@

$(BUILD)/lin/sqlite3.o: $(SRC)/third_party/sqlite/sqlite3.c
	@mkdir -p $(dir $@)
	$(CC) -O2 $(SQLITE_OPTS) -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_JSON1 -c $< -o $@

# audio.c includes the vorbis *header*; this translation unit provides the code.
$(BUILD)/lin/stb_vorbis.o: $(SRC)/third_party/stb_vorbis/stb_vorbis.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -std=c11 -DSTB_VORBIS_IMPLEMENTATION -I$(SRC)/third_party -c $< -o $@

$(BUILD)/lin/hb.o: $(HB_DIR)/src/harfbuzz.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS_COMMON) $(HBFLAGS) -c $< -o $@

$(BUILD)/lin/third/whisper/%.c.o: $(WSP_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -pthread $(WSP_DEF) $(WSP_INC) -c $< -o $@

$(BUILD)/lin/third/whisper/%.cpp.o: $(WSP_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -O2 -pthread $(WSP_DEF) $(WSP_INC) -c $< -o $@

$(BUILD)/lin/src/engines/whisper_bridge.o: $(WSP_BRIDGE_SRC) $(WSP_HDR)
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -O2 -pthread $(WSP_DEF) $(WSP_INC) -c $< -o $@

$(BUILD)/lin/ft.a: $(FT_CORE_SRC)
	@mkdir -p $(BUILD)/lin/ftobj
	@for f in $(FT_CORE_SRC); do \
	  $(CC) -O2 -std=c11 $(FTCFG) -c $$f -o $(BUILD)/lin/ftobj/$$(basename $$f .c).o || exit 1; \
	done
	@rm -f $@
	ar rcs $@ $(BUILD)/lin/ftobj/*.o

$(LINUX_BIN): $(LINUX_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) -o $@ $^ -lm -lpthread -ldl

# ===========================================================================
#  Windows x64 — single self-contained exe (zig cc, MinGW, static CRT)
# ===========================================================================
WIN_BIN  := $(DIST)/bin/OpenSpeechAI.exe
WIN_CC   := $(ZIG) cc -target x86_64-windows-gnu -O2 -static
WIN_CXX  := $(ZIG) c++ -target x86_64-windows-gnu -O2 -static
WIN_WSP_OBJ := $(patsubst $(WSP_DIR)/%.c,$(BUILD)/win/third/whisper/%.c.o,$(WSP_C)) \
               $(patsubst $(WSP_DIR)/%.cpp,$(BUILD)/win/third/whisper/%.cpp.o,$(WSP_CPP)) \
               $(BUILD)/win/src/engines/whisper_bridge.o
WIN_OBJ  := $(patsubst %.c,$(BUILD)/win/%.o,$(LIB_SRC)) \
            $(BUILD)/win/src/platform/mic.o \
            $(BUILD)/win/src/platform/win32/main_win.o \
            $(BUILD)/win/src/platform/win32/m4a_mf.o \
            $(BUILD)/win/sqlite3.o $(BUILD)/win/stb_vorbis.o \
            $(BUILD)/win/ft.a $(BUILD)/win/hb.o $(BUILD)/win/res.o $(WIN_WSP_OBJ)
# Only system DLLs. No D2D/DWrite: the canvas renders with GDI, which exists on
# every supported Windows version and keeps the import table tiny.
# mfplat/mfreadwrite are *not* here on purpose: m4a_mf.c loads them by name, so
# the exe keeps starting on a system where Media Foundation is absent.
WIN_LIBS := -lws2_32 -lshlwapi -lshell32 -lole32 -loleaut32 -lgdi32 -luser32 \
            -ladvapi32 -luuid -lcomdlg32 -lversion -lbcrypt -lwinmm -lpsapi \
            -lwinhttp -lkernel32

WORKER_WIN_OBJ := $(patsubst %.c,$(BUILD)/worker-win/%.c.o,$(LLAMA_C)) \
                  $(patsubst %.cpp,$(BUILD)/worker-win/%.cpp.o,$(LLAMA_CPP)) \
                  $(patsubst %.cc,$(BUILD)/worker-win/%.cc.o,$(FTXT_SRC)) \
                  $(BUILD)/worker-win/src/engines/lang_script.o \
                  $(BUILD)/worker-win/$(WORKER_SRC).o
$(BUILD)/worker-win/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(WIN_CC) -std=c11 -MMD -MP $(LLAMA_DEF) $(LLAMA_INC) -c $< -o $@
$(BUILD)/worker-win/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(WIN_CXX) -std=c++17 -MMD -MP $(LLAMA_DEF) $(LLAMA_INC) -c $< -o $@
$(BUILD)/worker-win/%.cc.o: %.cc
	@mkdir -p $(dir $@)
	$(WIN_CXX) -std=c++17 -MMD -MP $(LLAMA_INC) -c $< -o $@
$(BUILD)/worker-win/src/engines/lang_script.o: $(LANGSCRIPT_SRC) $(SRC)/engines/lang_script.h
	@mkdir -p $(dir $@)
	$(WIN_CC) -std=c11 -MMD -MP -I$(SRC)/core -I$(SRC)/engines -c $< -o $@
$(BUILD)/worker-win/res.o: $(SRC)/platform/win32/worker.rc $(SRC)/platform/win32/worker.manifest
	@mkdir -p $(dir $@)
	$(ZIG) rc /fo $@ /I $(SRC)/platform/win32 $(SRC)/platform/win32/worker.rc
$(WORKER_WIN): $(WORKER_WIN_OBJ) $(BUILD)/worker-win/res.o
	@mkdir -p $(dir $@)
	$(WIN_CXX) -static -o $@ $^ -lm -lpthread -lshell32
.PHONY: win
win: $(WIN_BIN) $(WORKER_WIN)

$(BUILD)/win/%.o: %.c
	@mkdir -p $(dir $@)
	$(WIN_CC) -std=c11 -MMD -MP $(INCLUDES) -DOSAI_WIN_MEDIA_FOUNDATION -DOSAI_WIN_SAPI -c $< -o $@

$(BUILD)/win/sqlite3.o: $(SRC)/third_party/sqlite/sqlite3.c
	@mkdir -p $(dir $@)
	$(WIN_CC) $(SQLITE_OPTS) -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_JSON1 -c $< -o $@

$(BUILD)/win/stb_vorbis.o: $(SRC)/third_party/stb_vorbis/stb_vorbis.c
	@mkdir -p $(dir $@)
	$(WIN_CC) -std=c11 -DSTB_VORBIS_IMPLEMENTATION -I$(SRC)/third_party -c $< -o $@

$(BUILD)/win/hb.o: $(HB_DIR)/src/harfbuzz.cc
	@mkdir -p $(dir $@)
	$(WIN_CXX) -fno-exceptions -fno-rtti $(HBFLAGS) -c $< -o $@

$(BUILD)/win/third/whisper/%.c.o: $(WSP_DIR)/%.c
	@mkdir -p $(dir $@)
	$(WIN_CC) -std=c11 $(WSP_DEF) $(WSP_INC) -c $< -o $@

$(BUILD)/win/third/whisper/%.cpp.o: $(WSP_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(WIN_CXX) -std=c++17 $(WSP_DEF) $(WSP_INC) -c $< -o $@

$(BUILD)/win/src/engines/whisper_bridge.o: $(WSP_BRIDGE_SRC) $(WSP_HDR)
	@mkdir -p $(dir $@)
	$(WIN_CXX) -std=c++17 $(WSP_DEF) $(WSP_INC) -c $< -o $@

$(BUILD)/win/ft.a: $(FT_CORE_SRC)
	@mkdir -p $(BUILD)/win/ftobj
	@for f in $(FT_CORE_SRC); do \
	  $(WIN_CC) $(FTCFG) -c $$f -o $(BUILD)/win/ftobj/$$(basename $$f .c).o || exit 1; \
	done
	@rm -f $@
	ar rcs $@ $(BUILD)/win/ftobj/*.o

# The version resource is generated from OSAI_VERSION before it is compiled, so
# the file properties of the exe cannot drift away from the version the rest of
# the build reports.
$(SRC)/platform/win32/app_version.h: $(SRC)/core/osai_common.h tools/make_dist.py
	$(PYTHON) tools/make_dist.py --rc-only

# The app resource embeds the worker binary, so the worker must be linked first.
$(BUILD)/win/res.o: $(SRC)/platform/win32/app.rc $(SRC)/platform/win32/app.manifest \
                    $(SRC)/platform/win32/app.ico $(SRC)/platform/win32/app_version.h \
                    $(WORKER_WIN)
	@mkdir -p $(dir $@)
	$(ZIG) rc /fo $@ /I $(SRC)/platform/win32 /I $(DIST)/bin $(SRC)/platform/win32/app.rc

# -Wl,--subsystem,windows is what actually sets PE subsystem 2 (GUI); the -mwindows
# driver flag is accepted by zig but does not change the header.
$(WIN_BIN): $(WIN_OBJ)
	@mkdir -p $(dir $@)
	$(WIN_CXX) -Wl,--subsystem,windows -o $@ $^ $(WIN_LIBS)

# ===========================================================================
#  Tests, snapshots, distribution
# ===========================================================================
.PHONY: tests
# A tiny synthetic fastText file (two labels, 288 bytes) lets the suite run a
# real worker handshake and a real detect round trip without any download.
$(BUILD)/test-lid.bin: tools/make_test_lid.py
	@mkdir -p $(dir $@)
	$(PYTHON) tools/make_test_lid.py $@

$(BUILD)/test-lid-flores.bin: tools/make_test_lid.py
	@mkdir -p $(dir $@)
	$(PYTHON) tools/make_test_lid.py $@ --flores

tests: unittest $(LINUX_BIN) $(WORKER_LINUX) $(BUILD)/test-lid.bin $(BUILD)/test-lid-flores.bin
	@echo ""
	@echo "== headless self-test =="
	$(LINUX_BIN) --selftest

.PHONY: snapshots
snapshots: $(LINUX_BIN)
	@mkdir -p $(DIST)/screenshots
	$(LINUX_BIN) --snapshots $(DIST)/screenshots

.PHONY: dist
dist: win $(LINUX_BIN)
	$(PYTHON) tools/catalog/check.py
	$(PYTHON) tools/make_dist.py
	$(PYTHON) tools/verify_exe.py $(WIN_BIN)
	$(PYTHON) tools/verify_exe.py $(WORKER_WIN) --console

.PHONY: bootstrap
bootstrap:
	@mkdir -p $(dir $(ZIG))
	$(PYTHON) tools/bootstrap_zig.py $(dir $(ZIG))

.PHONY: clean
clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name "*.d" 2>/dev/null)

# ===========================================================================
#  Unit tests — tests/test_main.c against the portable objects
# ===========================================================================
TEST_BIN := $(BUILD)/tests
TEST_OBJ := $(filter-out $(BUILD)/lin/src/main_linux.o,$(LINUX_OBJ))

.PHONY: unittest
unittest: $(TEST_BIN)
	$(TEST_BIN)

$(TEST_BIN): tests/test_main.c $(TEST_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_COMMON) $(INCLUDES) -c tests/test_main.c -o $(BUILD)/test_main.o
	$(CXX) -o $@ $(BUILD)/test_main.o $(TEST_OBJ) -lm -lpthread -ldl
