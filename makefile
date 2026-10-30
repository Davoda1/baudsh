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

# A single invocation writes one output file. Mixing build modes (or cleaning
# while building with -j) would otherwise produce misleading or racy results.
BUILD_GOALS := $(filter all release static debug baudsh baudsh.exe,$(MAKECMDGOALS))
REQUESTED_MODES := $(sort $(if $(filter all release baudsh baudsh.exe,$(BUILD_GOALS)),release) $(filter static debug,$(BUILD_GOALS)))
ifneq ($(word 2,$(REQUESTED_MODES)),)
  $(error Choose one build mode per invocation: release, static, or debug)
endif
ifneq ($(filter clean,$(MAKECMDGOALS)),)
  ifneq ($(BUILD_GOALS),)
    $(error Run make clean separately from build targets)
  endif
endif
BUILD_MODE := $(if $(REQUESTED_MODES),$(REQUESTED_MODES),release)

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
  HEADERS := baudsh_win.hpp
else
  SRC := $(SRC_POSIX)
  HEADERS := baudsh_posix_features.hpp
endif

# Common flags
CXXSTD  := -std=c++17
WARN    := -Wall -Wextra -pedantic
OPTREL  := -O2
OPTDBG  := -O0 -g

CXXFLAGS_BASE := $(CXXSTD) $(WARN)
LDFLAGS_BASE  :=
LDLIBS        ?=

# Platform-specific libraries
ifeq ($(IS_WIN),0)
  ifeq ($(UNAME_S),Linux)
    LDLIBS += -lutil
  endif
endif

# Strip command differences
STRIP_CMD := $(STRIP) --strip-all
ifeq ($(UNAME_S),Darwin)
  STRIP_CMD := $(STRIP) -x
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

# MSYS, MinGW and Cygwin make normally run under a Unix-like shell; del is
# a cmd.exe builtin and does not work there.  Use it only for native cmd.exe.
ifeq ($(UNAME_S),Windows)
  CLEAN_CMD := if exist $(TARGET) del /f /q $(TARGET) & if exist *.o del /f /q *.o
else
  CLEAN_CMD := rm -f $(TARGET) *.o
endif

# --------------------------------------------------------------
# Targets
# --------------------------------------------------------------

.PHONY: all
all: release

.PHONY: release
release: $(TARGET)
	@echo "Stripping binary..."
	@$(STRIP_CMD) $(TARGET)
	@echo "Release build complete."

.PHONY: static
static: $(TARGET)
	@echo "Stripping binary..."
	@$(STRIP_CMD) $(TARGET)
	@echo "Static build complete (where supported)."

.PHONY: debug
debug: $(TARGET)
	@echo "Debug build complete."

# The modes share the same output filename.  Always rebuild it so switching
# modes or changing compiler flags cannot silently reuse the wrong executable.
# This project is small enough that a full rebuild is inexpensive.
.PHONY: FORCE
FORCE:

$(TARGET): $(SRC) $(HEADERS) FORCE
	$(CXX) $(CPPFLAGS) $(CXXFLAGS_BASE) $(if $(filter debug,$(BUILD_MODE)),$(OPTDBG),$(OPTREL)) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS_BASE) $(LDFLAGS) $(if $(filter static,$(BUILD_MODE)),$(STATIC_FLAGS)) $(LDLIBS)

.PHONY: clean
clean:
	$(CLEAN_CMD)

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
