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

CFLAGS_COMMON := -std=c11 -O2 -g -fno-omit-frame-pointer -Wall -Wextra \
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

# ===========================================================================
#  Linux (host) — headless host binary used by tests and snapshots
# ===========================================================================
LINUX_BIN := $(BUILD)/openspeech
LINUX_OBJ := $(patsubst %.c,$(BUILD)/lin/%.o,$(LIB_SRC)) \
             $(BUILD)/lin/src/platform/mic.o \
             $(BUILD)/lin/src/main_linux.o \
             $(BUILD)/lin/sqlite3.o $(BUILD)/lin/stb_vorbis.o \
             $(BUILD)/lin/ft.a $(BUILD)/lin/hb.o

.PHONY: linux
linux: $(LINUX_BIN)

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
WIN_OBJ  := $(patsubst %.c,$(BUILD)/win/%.o,$(LIB_SRC)) \
            $(BUILD)/win/src/platform/mic.o \
            $(BUILD)/win/src/platform/win32/main_win.o \
            $(BUILD)/win/sqlite3.o $(BUILD)/win/stb_vorbis.o \
            $(BUILD)/win/ft.a $(BUILD)/win/hb.o $(BUILD)/win/res.o
# Only system DLLs. No D2D/DWrite: the canvas renders with GDI, which exists on
# every supported Windows version and keeps the import table tiny.
WIN_LIBS := -lws2_32 -lshlwapi -lshell32 -lole32 -loleaut32 -lgdi32 -luser32 \
            -ladvapi32 -luuid -lcomdlg32 -lversion -lbcrypt -lwinmm -lpsapi -lkernel32

.PHONY: win
win: $(WIN_BIN)

$(BUILD)/win/%.o: %.c
	@mkdir -p $(dir $@)
	$(WIN_CC) -std=c11 $(INCLUDES) -c $< -o $@

$(BUILD)/win/sqlite3.o: $(SRC)/third_party/sqlite/sqlite3.c
	@mkdir -p $(dir $@)
	$(WIN_CC) $(SQLITE_OPTS) -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_JSON1 -c $< -o $@

$(BUILD)/win/stb_vorbis.o: $(SRC)/third_party/stb_vorbis/stb_vorbis.c
	@mkdir -p $(dir $@)
	$(WIN_CC) -std=c11 -DSTB_VORBIS_IMPLEMENTATION -I$(SRC)/third_party -c $< -o $@

$(BUILD)/win/hb.o: $(HB_DIR)/src/harfbuzz.cc
	@mkdir -p $(dir $@)
	$(WIN_CXX) -fno-exceptions -fno-rtti $(HBFLAGS) -c $< -o $@

$(BUILD)/win/ft.a: $(FT_CORE_SRC)
	@mkdir -p $(BUILD)/win/ftobj
	@for f in $(FT_CORE_SRC); do \
	  $(WIN_CC) $(FTCFG) -c $$f -o $(BUILD)/win/ftobj/$$(basename $$f .c).o || exit 1; \
	done
	@rm -f $@
	ar rcs $@ $(BUILD)/win/ftobj/*.o

$(BUILD)/win/res.o: $(SRC)/platform/win32/app.rc $(SRC)/platform/win32/app.manifest \
                    $(SRC)/platform/win32/app.ico
	@mkdir -p $(dir $@)
	$(ZIG) rc /fo $@ /I $(SRC)/platform/win32 $(SRC)/platform/win32/app.rc

# -Wl,--subsystem,windows is what actually sets PE subsystem 2 (GUI); the -mwindows
# driver flag is accepted by zig but does not change the header.
$(WIN_BIN): $(WIN_OBJ)
	@mkdir -p $(dir $@)
	$(WIN_CC) -Wl,--subsystem,windows -o $@ $^ $(WIN_LIBS)

# ===========================================================================
#  Tests, snapshots, distribution
# ===========================================================================
.PHONY: tests
tests: unittest $(LINUX_BIN)
	@echo ""
	@echo "== headless self-test =="
	$(LINUX_BIN) --selftest

.PHONY: snapshots
snapshots: $(LINUX_BIN)
	@mkdir -p $(DIST)/screenshots
	$(LINUX_BIN) --snapshots $(DIST)/screenshots

.PHONY: dist
dist: win
	$(PYTHON) tools/make_dist.py

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
