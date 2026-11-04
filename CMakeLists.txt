cmake_minimum_required(VERSION 3.20)
project(ext4easy VERSION 0.4.0 LANGUAGES C CXX RC)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Link the C/C++ runtime statically, so the .exe runs without extra DLLs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    # source files are UTF-8 (umlauts in UI strings); not for the resource compiler
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>)
endif()

# --- lwext4 (vendored, see third_party/lwext4/PATCHES.md) -------------------
file(GLOB LWEXT4_SOURCES ${CMAKE_SOURCE_DIR}/third_party/lwext4/src/*.c)
add_library(lwext4 STATIC ${LWEXT4_SOURCES})
target_include_directories(lwext4 PUBLIC ${CMAKE_SOURCE_DIR}/third_party/lwext4/include)
target_compile_definitions(lwext4 PUBLIC
    CONFIG_USE_DEFAULT_CFG=1
    CONFIG_HAVE_OWN_OFLAGS=1
    CONFIG_HAVE_OWN_ERRNO=0
    CONFIG_HAVE_OWN_ASSERT=0
    CONFIG_DEBUG_PRINTF=1
    CONFIG_DEBUG_ASSERT=1
    CONFIG_BLOCK_DEV_CACHE_SIZE=1024
    CONFIG_EXT4_BLOCKDEVS_COUNT=26
    CONFIG_EXT4_MOUNTPOINTS_COUNT=26)
if(MSVC)
    # lwext4 is third-party C code; keep its warnings quiet.
    target_compile_options(lwext4 PRIVATE /W1 /wd4244 /wd4267 /wd4996)
endif()

# --- core: disk access, partitions, ext4 volume ----------------------------
add_library(ext4core STATIC
    src/core/disk_source.cpp
    src/core/partitions.cpp
    src/core/ext4_volume.cpp
    src/core/names.cpp
    src/core/text.cpp)
target_include_directories(ext4core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ext4core PUBLIC lwext4)
if(MSVC)
    target_compile_options(ext4core PRIVATE /W4 /permissive-)
endif()

configure_file(res/version.h.in ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)

# --- WinFsp (required) ------------------------------------------------------
set(_pf86 "ProgramFiles(x86)")
set(WINFSP_DIR "$ENV{${_pf86}}/WinFsp" CACHE PATH "WinFsp install directory")
if(NOT EXISTS "${WINFSP_DIR}/inc/winfsp/winfsp.h" OR NOT EXISTS "${WINFSP_DIR}/lib/winfsp-x64.lib")
    message(FATAL_ERROR "WinFsp SDK not found in ${WINFSP_DIR}. "
                        "Install WinFsp with the 'Developer' component.")
endif()

# --- IPC: shared by service, CLI and tray app ------------------------------
add_library(ext4ipc STATIC src/ipc/pipe.cpp)
target_include_directories(ext4ipc PUBLIC ${CMAKE_SOURCE_DIR}/src)

# --- ext4easy.exe: CLI, mount, background service ---------------------------
add_executable(ext4easy
    src/cli/main.cpp
    src/mount/winfsp_fs.cpp
    src/service/device_watcher.cpp
    src/service/log.cpp
    src/service/pipe_server.cpp
    src/service/service_main.cpp
    src/service/settings.cpp
    src/service/volume_manager.cpp
    res/ext4easy.rc)
target_include_directories(ext4easy PRIVATE "${WINFSP_DIR}/inc" ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(ext4easy PRIVATE ext4core ext4ipc "${WINFSP_DIR}/lib/winfsp-x64.lib"
    delayimp cfgmgr32 shell32 advapi32)
target_link_options(ext4easy PRIVATE /DELAYLOAD:winfsp-x64.dll)

# --- ext4easy-tray.exe ----------------------------------------------------------
add_executable(ext4easy-tray WIN32
    src/tray/tray.cpp
    src/tray/strings.cpp
    src/tray/tray.manifest
    res/tray.rc)
target_include_directories(ext4easy-tray PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(ext4easy-tray PRIVATE ext4ipc comctl32 shell32 comdlg32 ole32)

if(MSVC)
    target_compile_options(ext4easy PRIVATE /W4 /permissive-)
    target_compile_options(ext4easy-tray PRIVATE /W4 /permissive-)
    target_compile_options(ext4ipc PRIVATE /W4 /permissive-)
endif()

# --- tests ----------------------------------------------------------------------
add_executable(ext4stress tests/stress.cpp)
target_link_libraries(ext4stress PRIVATE ext4core bcrypt)
if(MSVC)
    target_compile_options(ext4stress PRIVATE /W4 /permissive-)
endif()
add_executable(ext4scenario tests/scenario.cpp)
target_link_libraries(ext4scenario PRIVATE ext4core)
