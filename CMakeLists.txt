# CMakeLists.txt — Minimal 7zFM build with LKL disk image support
#
# Builds 7zFM.exe (File Manager) for Windows with LKL integration.
# Can cross-compile from Linux using MinGW:
#   mkdir build && cd build
#   cmake .. -DCMAKE_TOOLCHAIN_FILE=../mingw32-toolchain.cmake
#   cmake --build . -j$(nproc)
#
# 64-bit static build:
#   mkdir build-mingw64 && cd build-mingw64
#   cmake .. -DCMAKE_TOOLCHAIN_FILE=../mingw64-toolchain.cmake \
#     -DLKL_STATIC=ON -DMSYS2_MINGW=/opt/msys2-cross/mingw64 \
#     -DQEMU_BUILD=../qemu/build-win64-static
#   cmake --build . -j$(nproc)

cmake_minimum_required(VERSION 3.16)
project(7zFM-LKL C CXX)

# ─── Options ──────────────────────────────────────────────────────
option(LKL_STATIC "Statically link LKL into exe (needs liblkl.a)" OFF)
option(WITH_ARCHIVE "Include archive open support (7z/zip/tar)" ON)

# ─── Paths ────────────────────────────────────────────────────────
set(SEVENZIP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../7zip" CACHE PATH "7-Zip source")
set(LKL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../linux" CACHE PATH "LKL kernel source")
set(LKL_INCLUDE "${LKL_SRC}/tools/lkl/include" CACHE PATH "LKL headers")
set(LKL_LIB_DIR "${LKL_SRC}/tools/lkl/lib-win32" CACHE PATH "LKL DLLs")
set(ANYFS_SRC "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "anyfs-reader source")
set(QEMU_BUILD "${CMAKE_CURRENT_SOURCE_DIR}/../qemu/build-win32-static" CACHE PATH "QEMU build")
set(MSYS2_MINGW "/opt/msys2/mingw32" CACHE PATH "msys2 mingw prefix (mingw32 or mingw64)")

# Directories within 7-Zip source
set(CPP "${SEVENZIP_SRC}/CPP")
set(C_DIR "${SEVENZIP_SRC}/C")
set(COMMON_DIR "${CPP}/Common")
set(WIN_DIR "${CPP}/Windows")
set(7ZIP_DIR "${CPP}/7zip")
set(7ZIP_COMMON_DIR "${7ZIP_DIR}/Common")
set(AR_DIR "${7ZIP_DIR}/Archive")
set(UI_DIR "${7ZIP_DIR}/UI")
set(FM_DIR "${UI_DIR}/FileManager")
set(AGENT_DIR "${UI_DIR}/Agent")

# ─── Global settings ─────────────────────────────────────────────
add_definitions(-DUNICODE -D_UNICODE -DWIN32 -D_WIN32)
set(CMAKE_CXX_STANDARD 14)

if(NOT MSVC)
    add_compile_options(-Wno-narrowing)
    # --large-address-aware is MSVC only; not needed for 64-bit MinGW
    # Dummy htmlhelp (not available in MinGW)
    add_library(htmlhelp STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/7z-plugin/contrib/htmlhelp.c)
    target_include_directories(htmlhelp PRIVATE ${WIN_DIR})
    # Compat stubs for msys2 glib vs Debian mingw-w64 CRT differences
    add_library(win32compat STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/7z-plugin/contrib/win32_compat_stubs.c)
endif()

# ─── COMMON library (shared objects) ─────────────────────────────
add_library(COMMON OBJECT
    ${COMMON_DIR}/DynLimBuf.cpp
    ${COMMON_DIR}/IntToString.cpp
    ${COMMON_DIR}/Lang.cpp
    ${COMMON_DIR}/MyString.cpp
    ${COMMON_DIR}/MyVector.cpp
    ${COMMON_DIR}/NewHandler.cpp
    ${COMMON_DIR}/Random.cpp
    ${COMMON_DIR}/StringConvert.cpp
    ${COMMON_DIR}/StringToInt.cpp
    ${COMMON_DIR}/UTFConvert.cpp
    ${COMMON_DIR}/Wildcard.cpp
)
target_include_directories(COMMON PRIVATE ${CPP} ${ANYFS_SRC}/src/7z-plugin/compat-include)

# ─── CRC (pure C, no ASM) ────────────────────────────────────────
add_library(CRC OBJECT ${C_DIR}/7zCrc.c ${C_DIR}/7zCrcOpt.c)

# ─── 7zFM executable ─────────────────────────────────────────────
set(FM_SOURCES
    # Windows abstraction
    ${WIN_DIR}/Clipboard.cpp
    ${WIN_DIR}/CommonDialog.cpp
    ${WIN_DIR}/COM.cpp
    ${WIN_DIR}/DLL.cpp
    ${WIN_DIR}/ErrorMsg.cpp
    ${WIN_DIR}/FileDir.cpp
    ${WIN_DIR}/FileFind.cpp
    ${WIN_DIR}/FileIO.cpp
    ${WIN_DIR}/FileLink.cpp
    ${WIN_DIR}/FileName.cpp
    ${WIN_DIR}/FileSystem.cpp
    ${WIN_DIR}/MemoryGlobal.cpp
    ${WIN_DIR}/MemoryLock.cpp
    ${WIN_DIR}/Menu.cpp
    ${WIN_DIR}/Net.cpp
    ${WIN_DIR}/ProcessUtils.cpp
    ${WIN_DIR}/PropVariant.cpp
    ${WIN_DIR}/PropVariantConv.cpp
    ${WIN_DIR}/Registry.cpp
    ${WIN_DIR}/ResourceString.cpp
    ${WIN_DIR}/SecurityUtils.cpp
    ${WIN_DIR}/Shell.cpp
    ${WIN_DIR}/Synchronization.cpp
    ${WIN_DIR}/System.cpp
    ${WIN_DIR}/SystemInfo.cpp
    ${WIN_DIR}/TimeUtils.cpp
    ${WIN_DIR}/Window.cpp
    # Controls
    ${WIN_DIR}/Control/ComboBox.cpp
    ${WIN_DIR}/Control/Dialog.cpp
    ${WIN_DIR}/Control/ListView.cpp
    ${WIN_DIR}/Control/PropertyPage.cpp
    ${WIN_DIR}/Control/Window2.cpp
    # 7-Zip common
    ${7ZIP_COMMON_DIR}/CreateCoder.cpp
    ${7ZIP_COMMON_DIR}/FilePathAutoRename.cpp
    ${7ZIP_COMMON_DIR}/FileStreams.cpp
    ${7ZIP_COMMON_DIR}/FilterCoder.cpp
    ${7ZIP_COMMON_DIR}/LimitedStreams.cpp
    ${7ZIP_COMMON_DIR}/MethodProps.cpp
    ${7ZIP_COMMON_DIR}/ProgressUtils.cpp
    ${7ZIP_COMMON_DIR}/PropId.cpp
    ${7ZIP_COMMON_DIR}/StreamObjects.cpp
    ${7ZIP_COMMON_DIR}/StreamUtils.cpp
    ${7ZIP_COMMON_DIR}/UniqBlocks.cpp
    # UI Common
    ${UI_DIR}/Common/ArchiveExtractCallback.cpp
    ${UI_DIR}/Common/ArchiveName.cpp
    ${UI_DIR}/Common/ArchiveOpenCallback.cpp
    ${UI_DIR}/Common/CompressCall.cpp
    ${UI_DIR}/Common/DefaultName.cpp
    ${UI_DIR}/Common/EnumDirItems.cpp
    ${UI_DIR}/Common/ExtractingFilePath.cpp
    ${UI_DIR}/Common/HashCalc.cpp
    ${UI_DIR}/Common/LoadCodecs.cpp
    ${UI_DIR}/Common/OpenArchive.cpp
    ${UI_DIR}/Common/PropIDUtils.cpp
    ${UI_DIR}/Common/SetProperties.cpp
    ${UI_DIR}/Common/SortUtils.cpp
    ${UI_DIR}/Common/UpdateAction.cpp
    ${UI_DIR}/Common/UpdateCallback.cpp
    ${UI_DIR}/Common/UpdatePair.cpp
    ${UI_DIR}/Common/UpdateProduce.cpp
    ${UI_DIR}/Common/WorkDir.cpp
    ${UI_DIR}/Common/ZipRegistry.cpp
    # Explorer integration
    ${UI_DIR}/Explorer/ContextMenu.cpp
    ${UI_DIR}/Explorer/MyMessages.cpp
    ${UI_DIR}/Explorer/RegistryContextMenu.cpp
    # GUI helpers
    ${UI_DIR}/GUI/HashGUI.cpp
    ${UI_DIR}/GUI/UpdateCallbackGUI2.cpp
    # Archive common
    ${AR_DIR}/Common/ItemNameUtils.cpp
    ${AR_DIR}/Common/OutStreamWithCRC.cpp
    # Compress (minimal — just CopyCoder)
    ${7ZIP_DIR}/Compress/CopyCoder.cpp
    # Bench (needed for Add_LargePages_String)
    ${UI_DIR}/Common/Bench.cpp
    # C runtime
    ${C_DIR}/Alloc.c
    ${C_DIR}/CpuArch.c
    ${C_DIR}/DllSecur.c
    ${C_DIR}/Sort.c
    ${C_DIR}/Threads.c
    # File Manager UI
    ${FM_DIR}/AboutDialog.cpp
    ${FM_DIR}/AltStreamsFolder.cpp
    ${FM_DIR}/App.cpp
    ${FM_DIR}/BrowseDialog.cpp
    ${FM_DIR}/BrowseDialog2.cpp
    ${FM_DIR}/ClassDefs.cpp
    ${FM_DIR}/ComboDialog.cpp
    ${FM_DIR}/CopyDialog.cpp
    ${FM_DIR}/EditDialog.cpp
    ${FM_DIR}/EditPage.cpp
    ${FM_DIR}/EnumFormatEtc.cpp
    ${FM_DIR}/ExtractCallback.cpp
    ${ANYFS_SRC}/src/7z-plugin/FileFolderPluginOpen_lkl.cpp
    ${ANYFS_SRC}/src/7z-plugin/FilePlugins_lkl.cpp
    ${FM_DIR}/FM.cpp
    ${FM_DIR}/FoldersPage.cpp
    ${FM_DIR}/FormatUtils.cpp
    ${FM_DIR}/FSDrives.cpp
    ${FM_DIR}/FSFolder.cpp
    ${FM_DIR}/FSFolderCopy.cpp
    ${FM_DIR}/HelpUtils.cpp
    ${FM_DIR}/LangPage.cpp
    ${FM_DIR}/LangUtils.cpp
    ${FM_DIR}/LinkDialog.cpp
    ${FM_DIR}/ListViewDialog.cpp
    ${FM_DIR}/MemDialog.cpp
    ${FM_DIR}/MenuPage.cpp
    ${FM_DIR}/MessagesDialog.cpp
    ${FM_DIR}/MyLoadMenu.cpp
    ${FM_DIR}/NetFolder.cpp
    ${FM_DIR}/OpenCallback.cpp
    ${FM_DIR}/OptionsDialog.cpp
    ${FM_DIR}/OverwriteDialog.cpp
    ${FM_DIR}/Panel.cpp
    ${FM_DIR}/PanelCopy.cpp
    ${FM_DIR}/PanelCrc.cpp
    ${FM_DIR}/PanelDrag.cpp
    ${FM_DIR}/PanelFolderChange.cpp
    ${FM_DIR}/PanelItemOpen.cpp
    ${FM_DIR}/PanelItems.cpp
    ${FM_DIR}/PanelKey.cpp
    ${FM_DIR}/PanelListNotify.cpp
    ${FM_DIR}/PanelMenu.cpp
    ${FM_DIR}/PanelOperations.cpp
    ${FM_DIR}/PanelSelect.cpp
    ${FM_DIR}/PanelSort.cpp
    ${FM_DIR}/PanelSplitFile.cpp
    ${FM_DIR}/PasswordDialog.cpp
    ${FM_DIR}/ProgramLocation.cpp
    ${FM_DIR}/ProgressDialog2.cpp
    ${FM_DIR}/PropertyName.cpp
    ${FM_DIR}/RegistryAssociations.cpp
    ${FM_DIR}/RegistryPlugins.cpp
    ${FM_DIR}/RegistryUtils.cpp
    ${FM_DIR}/RootFolder.cpp
    ${FM_DIR}/SettingsPage.cpp
    ${FM_DIR}/SplitDialog.cpp
    ${FM_DIR}/SplitUtils.cpp
    ${FM_DIR}/StringUtils.cpp
    ${FM_DIR}/SysIconUtils.cpp
    ${FM_DIR}/SystemPage.cpp
    ${FM_DIR}/TextPairs.cpp
    ${FM_DIR}/UpdateCallback100.cpp
    ${FM_DIR}/VerCtrl.cpp
    ${FM_DIR}/ViewSettings.cpp
    # Agent (archive folder browsing)
    ${AGENT_DIR}/Agent.cpp
    ${AGENT_DIR}/AgentOut.cpp
    ${AGENT_DIR}/AgentProxy.cpp
    ${AGENT_DIR}/ArchiveFolder.cpp
    ${AGENT_DIR}/ArchiveFolderOpen.cpp
    ${AGENT_DIR}/ArchiveFolderOut.cpp
    ${AGENT_DIR}/UpdateCallbackAgent.cpp
    # LKL plugin (directly linked into exe)
    ${ANYFS_SRC}/src/7z-plugin/LklFolder.cpp
    ${ANYFS_SRC}/src/7z-plugin/LklFolderManager.cpp
    ${ANYFS_SRC}/src/7z-plugin/LklSession.cpp
    ${ANYFS_SRC}/src/7z-plugin/lkl_bridge.c
    ${ANYFS_SRC}/src/core/anyfs.c
    ${ANYFS_SRC}/src/core/raw_blk_backend.c
    ${ANYFS_SRC}/src/core/qemu_blk_backend.c
    ${ANYFS_SRC}/src/core/qemu_stubs.c
    # Resource
    ${FM_DIR}/resource.rc
)

add_executable(7zFM WIN32
    $<TARGET_OBJECTS:COMMON>
    $<TARGET_OBJECTS:CRC>
    ${FM_SOURCES}
)

target_include_directories(7zFM PRIVATE
    ${CPP}
    ${7ZIP_DIR}
    ${FM_DIR}
    ${UI_DIR}
    ${LKL_INCLUDE}
    ${LKL_INCLUDE}/mingw32
    ${ANYFS_SRC}/include
    ${ANYFS_SRC}/src/7z-plugin
    ${ANYFS_SRC}/src/7z-plugin/compat-include
    # QEMU headers
    ${QEMU_BUILD}
    ${QEMU_BUILD}/include
    ${QEMU_BUILD}/../include
)

target_compile_definitions(7zFM PRIVATE
    -DZ7_LANG
    -DNEW_FOLDER_INTERFACE
    -DEXTERNAL_CODECS
    -DWIN_LONG_PATH
    -DSUPPORT_DEVICE_FILE
    -DANYFS_HAS_QEMU
)

# GCC 16 on msys2-cross doesn't transitively include wchar.h
target_compile_options(7zFM PRIVATE -include wchar.h)

target_link_libraries(7zFM
    comctl32 comdlg32 mpr gdi32
    ole32 oleaut32 uuid
    ws2_32 iphlpapi
    htmlhelp
)

# LKL linking
if(LKL_STATIC)
    # QEMU static libraries (for qcow2/vmdk/vhd support)
    set(QEMU_LIBS
        "${QEMU_BUILD}/libblock.a"
        "${QEMU_BUILD}/libio.a"
        "${QEMU_BUILD}/libcrypto.a"
        "${QEMU_BUILD}/libqom.a"
        "${QEMU_BUILD}/libqemuutil.a"
        "${QEMU_BUILD}/libevent-loop-base.a"
        "${QEMU_BUILD}/libauthz.a"
    )
    # GLib and deps (needed by QEMU)
    set(GLIB_STATIC "${MSYS2_MINGW}/lib/libglib-2.0.a")
    set(INTL_STATIC "${MSYS2_MINGW}/lib/libintl.a")
    set(ICONV_STATIC "${MSYS2_MINGW}/lib/libiconv.a")
    set(PCRE2_STATIC "${MSYS2_MINGW}/lib/libpcre2-8.a")
    target_include_directories(7zFM PRIVATE
        ${MSYS2_MINGW}/include/glib-2.0
        ${MSYS2_MINGW}/lib/glib-2.0/include
    )
    target_compile_definitions(7zFM PRIVATE
        -DGLIB_STATIC_COMPILATION
    )
    target_link_libraries(7zFM
        "${LKL_SRC}/tools/lkl/liblkl.a"
        ${QEMU_LIBS}
        ${MSYS2_MINGW}/lib/libglib-2.0.a
        ${MSYS2_MINGW}/lib/libintl.a
        ${MSYS2_MINGW}/lib/libiconv.a
        ${MSYS2_MINGW}/lib/libpcre2-8.a
        ${MSYS2_MINGW}/lib/libz.a
        ${MSYS2_MINGW}/lib/libzstd.a
        win32compat
        ws2_32 iphlpapi pathcch synchronization
        mingwex ucrt
    )
else()
    target_link_directories(7zFM PRIVATE ${LKL_LIB_DIR})
    target_link_libraries(7zFM lkl anyfs-qemu)
endif()

set_target_properties(7zFM PROPERTIES OUTPUT_NAME "7zFM")

# Static link C++ runtime to avoid libstdc++-6.dll (24MB) dependency
target_link_options(7zFM PRIVATE -static-libgcc -static-libstdc++ -Wl,--image-base=0x140000000 -B/tmp/patched-ld-wrap/ -fuse-ld=bfd)
