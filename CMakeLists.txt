cmake_minimum_required(VERSION 3.20)
project(ext4win VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Link the C/C++ runtime statically, so the .exe runs without extra DLLs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

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

# --- WinFsp (optional: only needed for mounting) ---------------------------
set(_pf86 "ProgramFiles(x86)")
set(WINFSP_DIR "$ENV{${_pf86}}/WinFsp" CACHE PATH "WinFsp install directory")
if(EXISTS "${WINFSP_DIR}/inc/winfsp/winfsp.h" AND EXISTS "${WINFSP_DIR}/lib/winfsp-x64.lib")
    set(HAVE_WINFSP ON)
    message(STATUS "WinFsp SDK found: ${WINFSP_DIR}")
else()
    set(HAVE_WINFSP OFF)
    message(WARNING "WinFsp SDK not found in ${WINFSP_DIR} - building without mount support. "
                    "Install WinFsp with the 'Developer' component.")
endif()

# --- command line tool ------------------------------------------------------
add_executable(ext4win src/cli/main.cpp)
target_link_libraries(ext4win PRIVATE ext4core)
if(HAVE_WINFSP)
    target_sources(ext4win PRIVATE src/mount/winfsp_fs.cpp)
    target_include_directories(ext4win PRIVATE "${WINFSP_DIR}/inc")
    target_link_libraries(ext4win PRIVATE "${WINFSP_DIR}/lib/winfsp-x64.lib" delayimp)
    target_link_options(ext4win PRIVATE /DELAYLOAD:winfsp-x64.dll)
    target_compile_definitions(ext4win PRIVATE HAVE_WINFSP=1)
endif()
if(MSVC)
    target_compile_options(ext4win PRIVATE /W4 /permissive-)
endif()
