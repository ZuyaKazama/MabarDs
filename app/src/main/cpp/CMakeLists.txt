cmake_minimum_required(VERSION 3.22)
project(emunds C CXX ASM)

# ---------------------------------------------------------------------------
# Optimization flags (applied to everything, including the melonDS core)
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# CMake 3.22 misreads the NDK r28 version and forces gold for LTO; gold is gone since r26, lld is the default
set(CMAKE_C_LINK_OPTIONS_IPO "")
set(CMAKE_CXX_LINK_OPTIONS_IPO "")

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(EMU_OPT_FLAGS "-O3 -fomit-frame-pointer -fno-semantic-interposition -ffunction-sections -fdata-sections")
if (ANDROID_ABI STREQUAL "arm64-v8a")
    # ARMv8.2-A baseline is safe for practically every 64-bit phone sold since 2018,
    # but keep v8-a so older Cortex-A53 devices still run.
    set(EMU_OPT_FLAGS "${EMU_OPT_FLAGS} -march=armv8-a")
endif()
set(CMAKE_C_FLAGS_RELEASE   "${CMAKE_C_FLAGS_RELEASE} ${EMU_OPT_FLAGS}")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${EMU_OPT_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections -Wl,--icf=all -Wl,-z,max-page-size=16384")

set(EXTERNAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external)
set(MELONDS_DIR  ${EXTERNAL_DIR}/melonDS)
set(ENET_DIR     ${EXTERNAL_DIR}/enet)

if (NOT EXISTS ${MELONDS_DIR}/src/NDS.cpp)
    message(FATAL_ERROR "melonDS source missing. Run ./scripts/fetch_deps.sh first.")
endif()
if (NOT EXISTS ${ENET_DIR}/include/enet/enet.h)
    message(FATAL_ERROR "ENet source missing. Run ./scripts/fetch_deps.sh first.")
endif()

# ---------------------------------------------------------------------------
# melonDS core (no Qt frontend, no desktop OpenGL renderer, no GDB stub)
# ---------------------------------------------------------------------------
set(BUILD_QT_SDL        OFF CACHE BOOL "" FORCE)
set(ENABLE_OGLRENDERER  ON  CACHE BOOL "" FORCE)   # ported to GLES 3.2, see gles/
set(ENABLE_GDBSTUB      OFF CACHE BOOL "" FORCE)
set(ENABLE_JIT          ON  CACHE BOOL "" FORCE)
set(ENABLE_LTO_RELEASE  ON  CACHE BOOL "" FORCE)
set(USE_VCPKG           OFF CACHE BOOL "" FORCE)
add_subdirectory(${MELONDS_DIR} melonds EXCLUDE_FROM_ALL)

# OpenGL renderer on GLES 3.2: our compatibility header replaces glad
target_compile_definitions(core PUBLIC MELONDS_GLES "MELONDS_GL_HEADER=\"emunds_gles.h\"")
# EmuNDS core additions (patches/melonds-gles.patch): software-renderer frameskip
target_compile_definitions(core PUBLIC EMUNDS_FRAMESKIP)
target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/gles)

# ---------------------------------------------------------------------------
# ENet (reliable UDP used by melonDS LAN multiplayer)
# ---------------------------------------------------------------------------
add_library(enet STATIC
    ${ENET_DIR}/callbacks.c
    ${ENET_DIR}/compress.c
    ${ENET_DIR}/host.c
    ${ENET_DIR}/list.c
    ${ENET_DIR}/packet.c
    ${ENET_DIR}/peer.c
    ${ENET_DIR}/protocol.c
    ${ENET_DIR}/unix.c)
target_include_directories(enet PUBLIC ${ENET_DIR}/include)
target_compile_definitions(enet PRIVATE
    HAS_FCNTL=1 HAS_POLL=1 HAS_GETADDRINFO=1 HAS_GETNAMEINFO=1
    HAS_GETHOSTBYNAME_R=1 HAS_GETHOSTBYADDR_R=1 HAS_INET_PTON=1 HAS_INET_NTOP=1
    HAS_MSGHDR_FLAGS=1 HAS_SOCKLEN_T=1)

# ---------------------------------------------------------------------------
# melonDS networking: LAN multiplayer + slirp (emulated internet access point)
# ---------------------------------------------------------------------------
add_subdirectory(${MELONDS_DIR}/src/net/libslirp slirp EXCLUDE_FROM_ALL)

set(NET_DIR ${MELONDS_DIR}/src/net)
add_library(net-utils STATIC
    ${NET_DIR}/Net.cpp
    ${NET_DIR}/Net_Slirp.cpp
    ${NET_DIR}/PacketDispatcher.cpp
    ${NET_DIR}/LocalMP.cpp
    ${NET_DIR}/LAN.cpp
    ${NET_DIR}/MPInterface.cpp)
target_include_directories(net-utils PUBLIC ${NET_DIR} ${MELONDS_DIR}/src)
target_link_libraries(net-utils PUBLIC core enet slirp)

# ---------------------------------------------------------------------------
# JNI frontend
# ---------------------------------------------------------------------------
add_library(emunds SHARED
    jni_bridge.cpp
    EmuCore.cpp
    Platform_android.cpp
    AudioOut.cpp
    VideoOut.cpp
    PerfTuning.cpp
    gles/emunds_gles.cpp)

target_include_directories(emunds PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MELONDS_DIR}/src
    ${MELONDS_DIR}/src/frontend
    ${NET_DIR})

target_link_libraries(emunds PRIVATE
    net-utils core
    android log aaudio GLESv3 EGL)

set_target_properties(emunds PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
