# ==============================================================
#  baudsh - Portable Makefile (POSIX + Windows/MinGW)
#
#  Targets:
#    make          -> release (optimized + stripped)
#    make static   -> static release (where supported) + stripped
#    make debug    -> debug build (no strip)
#    make clean
#    make help
#
#  Notes:
#    - POSIX build uses:  baudsh_main.cpp + baudsh_posix.cpp
#    - Windows build uses: baudsh_main.cpp + baudsh_win.cpp   (ConPTY)
#    - Windows build requires a recent MinGW-w64 toolchain (ConPTY defs).
# ==============================================================

CXX     ?= g++
STRIP   ?= strip

TARGET  := baudsh

UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)

# Detect Windows-like environments
IS_WIN := 0
ifeq ($(findstring MINGW,$(UNAME_S)),MINGW)
  IS_WIN := 1
endif
ifeq ($(findstring MSYS,$(UNAME_S)),MSYS)
  IS_WIN := 1
endif
ifeq ($(findstring CYGWIN,$(UNAME_S)),CYGWIN)
  IS_WIN := 1
endif
ifeq ($(UNAME_S),Windows)
  IS_WIN := 1
endif

ifeq ($(IS_WIN),1)
  TARGET := baudsh.exe
endif

# Sources
SRC_POSIX := baudsh_main.cpp baudsh_posix.cpp
SRC_WIN   := baudsh_main.cpp baudsh_win.cpp

ifeq ($(IS_WIN),1)
  SRC := $(SRC_WIN)
else
  SRC := $(SRC_POSIX)
endif

# Common flags
CXXSTD  := -std=c++17
WARN    := -Wall -Wextra -pedantic
OPTREL  := -O2
OPTDBG  := -O0 -g

CXXFLAGS_BASE := $(CXXSTD) $(WARN)
LDFLAGS_BASE  :=
LDLIBS        :=

# Platform-specific libraries
ifeq ($(UNAME_S),Linux)
  LDLIBS += -lutil
endif

# Strip command differences
STRIP_CMD := $(STRIP) --strip-all
ifeq ($(UNAME_S),Darwin)
  STRIP_CMD := strip -x
endif

# Static flags (applied only for make static)
STATIC_FLAGS :=
ifeq ($(UNAME_S),Linux)
  STATIC_FLAGS += -static -static-libstdc++ -static-libgcc
endif

ifneq (,$(findstring BSD,$(UNAME_S)))
  STATIC_FLAGS += -static
endif

ifeq ($(IS_WIN),1)
  STATIC_FLAGS += -static -static-libstdc++ -static-libgcc
endif

# macOS does NOT support static linking of system libs
ifeq ($(UNAME_S),Darwin)
  STATIC_FLAGS :=
endif

# Windows-specific compile defines (harmless on MinGW, but keeps intent explicit)
ifeq ($(IS_WIN),1)
  CXXFLAGS_BASE += -D_WIN32
endif

ifeq ($(IS_WIN),1)
  REMOVE_CMD := del /f
else
  REMOVE_CMD := rm -f
endif

# --------------------------------------------------------------
# Targets
# --------------------------------------------------------------

.PHONY: all
all: release

.PHONY: release
release: CXXFLAGS := $(CXXFLAGS_BASE) $(OPTREL)
release: LDFLAGS  := $(LDFLAGS_BASE)
release: $(TARGET)
	@echo "Stripping binary..."
	@$(STRIP_CMD) $(TARGET) || true
	@echo "Release build complete."

.PHONY: static
static: CXXFLAGS := $(CXXFLAGS_BASE) $(OPTREL)
static: LDFLAGS  := $(LDFLAGS_BASE) $(STATIC_FLAGS)
static: $(TARGET)
	@echo "Stripping binary..."
	@$(STRIP_CMD) $(TARGET) || true
	@echo "Static build complete (where supported)."

.PHONY: debug
debug: CXXFLAGS := $(CXXFLAGS_BASE) $(OPTDBG)
debug: LDFLAGS  := $(LDFLAGS_BASE)
debug: $(TARGET)
	@echo "Debug build complete."

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS) $(LDLIBS)

.PHONY: clean
clean:
	$(REMOVE_CMD) $(TARGET) *.o

.PHONY: help
help:
	@echo ""
	@echo "baudsh build system"
	@echo ""
	@echo "Detected OS: $(UNAME_S)"
	@echo "Building:   $(TARGET)"
	@echo "Sources:    $(SRC)"
	@echo ""
	@echo "Targets:"
	@echo "  make          -> Release build (optimized + stripped)"
	@echo "  make static   -> Static release build (where supported)"
	@echo "  make debug    -> Debug build (no stripping)"
	@echo "  make clean    -> Remove build artifacts"
	@echo "  make help     -> Show this help"
	@echo ""
