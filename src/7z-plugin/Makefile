# Makefile for 7z-lkl.dll — LKL plugin for 7-Zip File Manager
#
# Builds a single DLL that implements IFolderFolder/IFolderManager using LKL.
#
# Two modes:
#   make MODE=dynamic  — links against liblkl.dll + libanyfs-qemublk.dll (default)
#   make MODE=static   — links liblkl.a + QEMU block .a + everything into one DLL
#
# Prerequisites:
#   - 7-Zip source: ~/7zip (or set SEVENZIP_SRC)
#   - LKL headers:  ~/linux/tools/lkl/include (or set LKL_INCLUDE)
#   - anyfs headers: ~/anyfs-reader/include (or set ANYFS_INCLUDE)
#   - Cross compiler: i686-w64-mingw32-g++ (or set CXX/CC)
#
# For static mode, also need:
#   - liblkl.a (Win32): rebuild with gen_lkl_config.sh + make ARCH=lkl
#   - QEMU block libs: ~/qemu/build-win32/*.a

MODE ?= dynamic

# Toolchain
CROSS ?= i686-w64-mingw32-
CXX = $(CROSS)g++
CC  = $(CROSS)gcc
AR  = $(CROSS)ar
WINDRES = $(CROSS)windres
STRIP = $(CROSS)strip

# Paths
SEVENZIP_SRC ?= $(HOME)/7zip
LKL_SRC      ?= $(HOME)/linux
LKL_INCLUDE  ?= $(LKL_SRC)/tools/lkl/include
ANYFS_INCLUDE ?= $(HOME)/anyfs-reader/include
ANYFS_SRC    ?= $(HOME)/anyfs-reader/src/core
QEMU_BUILD   ?= $(HOME)/qemu/build-win32
MSYS2_MINGW  ?= /opt/msys2/mingw32
LKL_LIB_WIN32 ?= $(LKL_SRC)/tools/lkl/lib-win32

OUTPUT = 7z-lkl.dll

# 7-Zip include paths (mimic the structure expected by 7z headers)
# 7z headers use includes like: "Common/MyCom.h", "Windows/PropVariant.h"
# and "../../UI/FileManager/IFolder.h", "../../PropID.h"
SEVENZIP_CPP = $(SEVENZIP_SRC)/CPP
SEVENZIP_INC = -I$(SEVENZIP_CPP) -I$(SEVENZIP_CPP)/7zip

CXXFLAGS = -O2 -std=c++14 -DWIN32 -D_WIN32 -DUNICODE -D_UNICODE \
           $(SEVENZIP_INC) \
           -fexceptions -fno-rtti

CFLAGS = -O2 -DWIN32 -D_WIN32 \
         -I$(LKL_INCLUDE) -I$(LKL_INCLUDE)/mingw32 -I$(ANYFS_INCLUDE)

# Plugin C++ sources
PLUGIN_SRCS = StdAfx.cpp ClassDefs.cpp LklSession.cpp LklFolder.cpp LklFolderManager.cpp DllExports.cpp

# Bridge C sources (avoids C++ keyword conflicts in LKL headers)
BRIDGE_SRCS = lkl_bridge.c

# 7-Zip support sources (COM framework, PropVariant, MyString, etc.)
SEVENZIP_SRCS = \
    $(SEVENZIP_CPP)/Windows/PropVariant.cpp \
    $(SEVENZIP_CPP)/Common/MyString.cpp \
    $(SEVENZIP_CPP)/Common/MyVector.cpp \
    $(SEVENZIP_CPP)/Common/IntToString.cpp \
    $(SEVENZIP_CPP)/Common/StringConvert.cpp \
    $(SEVENZIP_CPP)/Common/NewHandler.cpp

# Anyfs core sources (compiled as C)
ANYFS_SRCS = \
    $(ANYFS_SRC)/anyfs.c \
    $(ANYFS_SRC)/raw_blk_backend.c

PLUGIN_OBJS = $(PLUGIN_SRCS:.cpp=.o)
BRIDGE_OBJS = $(BRIDGE_SRCS:.c=.o)
SEVENZIP_OBJS = $(notdir $(SEVENZIP_SRCS:.cpp=.o))
ANYFS_OBJS = $(notdir $(ANYFS_SRCS:.c=.o))

ALL_OBJS = $(PLUGIN_OBJS) $(BRIDGE_OBJS) $(SEVENZIP_OBJS) $(ANYFS_OBJS)

# ─── Dynamic mode: link against existing DLLs ─────────────────────
ifeq ($(MODE),dynamic)

# Generate import libs from DLLs
LDFLAGS = -shared -Wl,--out-implib,lib7z-lkl.a \
          -L$(LKL_LIB_WIN32) \
          -Wl,--enable-stdcall-fixup

LDLIBS = -llkl -lws2_32 -liphlpapi -lole32 -loleaut32 -luuid

# If libanyfs-qemublk.dll exists, link against it
ifneq ($(wildcard $(LKL_LIB_WIN32)/libanyfs-qemu.dll),)
LDLIBS += -L$(LKL_LIB_WIN32) -lanyfs-qemu
CFLAGS += -DANYFS_HAS_QEMU
CXXFLAGS += -DANYFS_HAS_QEMU
ANYFS_SRCS += $(ANYFS_SRC)/qemu_blk_backend.c
endif

endif # dynamic

# ─── Static mode: link everything into one DLL ────────────────────
ifeq ($(MODE),static)

LKL_STATIC_A ?= $(LKL_SRC)/tools/lkl/liblkl.a

# QEMU block libraries (order matters for link resolution)
QEMU_LIBS = \
    $(QEMU_BUILD)/libblock.a \
    $(QEMU_BUILD)/libio.a \
    $(QEMU_BUILD)/libqemuutil.a \
    $(QEMU_BUILD)/libqom.a \
    $(QEMU_BUILD)/libcrypto.a \
    $(QEMU_BUILD)/libauthz.a \
    $(QEMU_BUILD)/libevent-loop-base.a

CFLAGS += -DANYFS_HAS_QEMU
CXXFLAGS += -DANYFS_HAS_QEMU
ANYFS_SRCS += $(ANYFS_SRC)/qemu_blk_backend.c

LDFLAGS = -shared -static-libgcc -static-libstdc++ \
          -Wl,--out-implib,lib7z-lkl.a \
          -Wl,--whole-archive $(LKL_STATIC_A) -Wl,--no-whole-archive \
          $(QEMU_LIBS) \
          -Wl,--enable-stdcall-fixup

LDLIBS = -lws2_32 -liphlpapi -lglib-2.0 -lintl -lz -lzstd

endif # static

# ─── Build rules ──────────────────────────────────────────────────

all: $(OUTPUT)
	@echo "Built $(OUTPUT) [mode=$(MODE)]"
	@$(STRIP) -s $(OUTPUT)
	@ls -la $(OUTPUT)

$(OUTPUT): $(ALL_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $(ALL_OBJS) $(LDLIBS)

# Plugin C++ objects
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Bridge C objects
lkl_bridge.o: lkl_bridge.c
	$(CC) $(CFLAGS) -c -o $@ $<

# 7-Zip support objects (explicit rules)
PropVariant.o: $(SEVENZIP_CPP)/Windows/PropVariant.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<
MyString.o: $(SEVENZIP_CPP)/Common/MyString.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<
MyVector.o: $(SEVENZIP_CPP)/Common/MyVector.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<
IntToString.o: $(SEVENZIP_CPP)/Common/IntToString.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<
StringConvert.o: $(SEVENZIP_CPP)/Common/StringConvert.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<
NewHandler.o: $(SEVENZIP_CPP)/Common/NewHandler.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Anyfs C objects
anyfs.o: $(ANYFS_SRC)/anyfs.c
	$(CC) $(CFLAGS) -c -o $@ $<
raw_blk_backend.o: $(ANYFS_SRC)/raw_blk_backend.c
	$(CC) $(CFLAGS) -c -o $@ $<
QEMU_CFLAGS = -I$(HOME)/qemu/include -I$(QEMU_BUILD) \
              -I$(MSYS2_MINGW)/include/glib-2.0 \
              -I$(MSYS2_MINGW)/lib/glib-2.0/include

qemu_blk_backend.o: $(ANYFS_SRC)/qemu_blk_backend.c
	$(CC) $(CFLAGS) $(QEMU_CFLAGS) -c -o $@ $<

clean:
	rm -f *.o $(OUTPUT) lib7z-lkl.a

# ─── Helper: rebuild LKL for Win32 (static mode) ─────────────────
rebuild-lkl:
	cd $(HOME)/anyfs-reader && ./scripts/gen_lkl_config.sh --cross=i686-w64-mingw32
	cd $(LKL_SRC)/tools/lkl && $(MAKE) -j$$(nproc) -C ../.. ARCH=lkl \
		CROSS_COMPILE=i686-w64-mingw32- CC=i686-w64-mingw32-gcc
	@echo "liblkl.a rebuilt for Win32"
	@file $(LKL_STATIC_A)

.PHONY: all clean rebuild-lkl
