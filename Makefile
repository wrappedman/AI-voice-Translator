# OpenSpeech AI — build system
# ---------------------------------------------------------------------------
#  make linux      : portable core + headless runner + tests (Linux host)
#  make tests      : unit/integration tests (no network)
#  make win        : dist/bin/OpenSpeechAI.exe (Windows x64, static, no deps)
#  make snapshots  : render every screen to PNG
#  make dist       : zip package + SHA-256
# ---------------------------------------------------------------------------
CC      ?= gcc
CXX     ?= g++
ZIG     ?= $(HOME)/.cache/openspeech/zig/zig

BUILD   := build
SRC     := src

CFLAGS_COMMON := -std=c11 -O2 -g -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter \
                 -Wno-missing-field-initializers -MMD -MP -pthread
CXXFLAGS_COMMON := -std=c++17 -O2 -fno-omit-frame-pointer -fno-rtti -fno-exceptions -pthread -MMD -MP

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

# ===========================================================================
#  Linux (host) — portable core, headless runner, tests, snapshots
# ===========================================================================
LINUX_BIN := $(BUILD)/openspeech-linux
LINUX_OBJ := $(patsubst %.c,$(BUILD)/lin/%.o,$(LIB_SRC)) \
             $(BUILD)/lin/platform/platform_net.o $(BUILD)/lin/main_linux.o \
             $(BUILD)/lin/sqlite3.o $(BUILD)/lin/stb_vorbis.o $(BUILD)/lin/ft.a $(BUILD)/lin/hb.o

.PHONY: linux
linux: $(LINUX_BIN)

$(BUILD)/lin/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_COMMON) $(INCLUDES) $(SQLITE_OPTS) -c $< -o $@

$(BUILD)/lin/sqlite3.o: $(SRC)/third_party/sqlite/sqlite3.c
	@mkdir -p $(dir $@)
	$(CC) -O2 $(SQLITE_OPTS) -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_JSON1 -c $< -o $@

$(BUILD)/lin/stb_vorbis.o: $(SRC)/third_party/stb_vorbis/stb_vorbis.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_COMMON) -Wno-unused-but-set-variable -c $< -o $@

$(BUILD)/lin/hb.o: $(SRC)/platform/ft/hbwrap.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS_COMMON) -DHB_NO_MT -DHB_EXPERIMENTAL_API -I$(HB_DIR)/src \
	       -I$(FT_DIR)/include -c $(SRC)/platform/ft/hbwrap.cc -o $@

$(BUILD)/lin/ft.a: $(FT_CORE_SRC)
	@mkdir -p $(BUILD)/lin/ftobj
	@for f in $(FT_CORE_SRC); do \
	  $(CC) -O2 $(FTCFG) -c $$f -o $(BUILD)/lin/ftobj/$$(basename $$f .c).o || exit 1; \
	done
	ar rcs $@ $(BUILD)/lin/ftobj/*.o

$(LINUX_BIN): $(LINUX_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) -o $@ $^ -lm -lpthread -ldl -lstdc++

# ===========================================================================
#  Windows x64 — single self-contained exe (zig cc, MinGW, static CRT)
# ===========================================================================
WIN_BIN  := dist/bin/OpenSpeechAI.exe
WIN_CC   := $(ZIG) cc -target x86_64-windows-gnu -O2 -static
WIN_CXX  := $(ZIG) c++ -target x86_64-windows-gnu -O2 -static
WIN_OBJ  := $(patsubst %.c,$(BUILD)/win/%.o,$(LIB_SRC)) \
            $(BUILD)/win/platform/platform_net.o $(BUILD)/win/platform/win_platform.o \
            $(BUILD)/win/platform/win_audio.o $(BUILD)/win/main_win32.o \
            $(BUILD)/win/sqlite3.o $(BUILD)/win/stb_vorbis.o $(BUILD)/win/ft.a \
            $(BUILD)/win/hb.o $(BUILD)/win/res.o
WIN_LIBS := -lws2_32 -lwinhttp -lshlwapi -lshell32 -lole32 -loleaut32 -lgdi32 -luser32 \
            -ladvapi32 -ld2d1 -ldwrite -ldwmapi -luuid -lcomdlg32 -lversion -lbcrypt \
            -lwinmm -lstdc++ -lkernel32

.PHONY: win
win: $(WIN_BIN)

$(BUILD)/win/%.o: %.c
	@mkdir -p $(dir $@)
	$(WIN_CC) -std=c11 $(INCLUDES) $(SQLITE_OPTS) -c $< -o $@

$(BUILD)/win/sqlite3.o: $(SRC)/third_party/sqlite/sqlite3.c
	@mkdir -p $(dir $@)
	$(WIN_CC) $(SQLITE_OPTS) -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_JSON1 -c $< -o $@

$(BUILD)/win/stb_vorbis.o: $(SRC)/third_party/stb_vorbis/stb_vorbis.c
	@mkdir -p $(dir $@)
	$(WIN_CC) -std=c11 -c $< -o $@

$(BUILD)/win/hb.o: $(SRC)/platform/ft/hbwrap.cc
	@mkdir -p $(dir $@)
	$(WIN_CXX) -DHB_NO_MT -DHB_EXPERIMENTAL_API -I$(HB_DIR)/src \
	           -I$(FT_DIR)/include -c $(SRC)/platform/ft/hbwrap.cc -o $@

$(BUILD)/win/ft.a: $(FT_CORE_SRC)
	@mkdir -p $(BUILD)/win/ftobj
	@for f in $(FT_CORE_SRC); do \
	  $(WIN_CC) $(FTCFG) -c $$f -o $(BUILD)/win/ftobj/$$(basename $$f .c).o || exit 1; \
	done
	ar rcs $@ $(BUILD)/win/ftobj/*.o

$(BUILD)/win/res.o: $(SRC)/platform/win32/app.rc $(SRC)/platform/win32/app.manifest \
                    $(SRC)/platform/win32/app.ico
	@mkdir -p $(dir $@)
	$(ZIG) rc /fo $@ /I $(SRC)/platform/win32 $(SRC)/platform/win32/app.rc

$(WIN_BIN): $(WIN_OBJ)
	@mkdir -p $(dir $@)
	$(WIN_CC) -o $@ $^ -mwindows $(WIN_LIBS)

# ===========================================================================
#  Tests, snapshots, distribution
# ===========================================================================
.PHONY: tests
tests: $(LINUX_BIN)
	$(LINUX_BIN) selftest

.PHONY: snapshots
snapshots: $(LINUX_BIN)
	@mkdir -p dist/screenshots
	$(LINUX_BIN) snapshots dist/screenshots

.PHONY: dist
dist: win
	python3 tools/make_dist.py

.PHONY: bootstrap
bootstrap: $(LINUX_BIN)
	$(LINUX_BIN) bootstrap

.PHONY: clean
clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name "*.d" 2>/dev/null)
