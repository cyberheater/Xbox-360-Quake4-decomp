cmake_minimum_required(VERSION 3.20)
project(Quake4Recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# Dev builds: favour compile speed over optimisation for the ~160 generated files.
set(CMAKE_CXX_FLAGS_RELEASE "/O1 /DNDEBUG")

set(PPC_DIR       "C:/Quake4/ppc"           CACHE PATH "XenonRecomp output directory")
set(XENONRECOMP   "C:/Quake4/XenonRecomp"   CACHE PATH "XenonRecomp checkout")
set(XENONRECOMP_BUILD "${XENONRECOMP}/out"  CACHE PATH "XenonRecomp build directory")
set(GAME_DIR      "C:/Quake4/game"          CACHE PATH "Extracted game files")

# ---- recompiled guest code ---------------------------------------------------
file(GLOB PPC_SOURCES CONFIGURE_DEPENDS "${PPC_DIR}/ppc_recomp.*.cpp" "${PPC_DIR}/ppc_func_mapping.cpp")

add_library(ppc_recomp OBJECT ${PPC_SOURCES})
target_include_directories(ppc_recomp PUBLIC "${PPC_DIR}" "${XENONRECOMP}/thirdparty/simde")
target_compile_definitions(ppc_recomp PRIVATE NOMINMAX)
target_compile_options(ppc_recomp PRIVATE /w /arch:AVX)

# ---- host runtime ------------------------------------------------------------
add_executable(quake4
    src/main.cpp
    src/memory.cpp
    src/guest_alloc.cpp
    src/kobject.cpp
    src/thread.cpp
    src/imports.cpp
    src/kernel.cpp
    src/kernel_memory.cpp
    src/kernel_threading.cpp
    src/kernel_video.cpp
    src/gpu_cp.cpp
    src/texture_dump.cpp
    src/probe.cpp
    src/renderer.cpp
    src/xenos_shader.cpp
    src/kernel_system.cpp
    src/kernel_audio_net.cpp
    src/kernel_file.cpp
    src/kernel_content.cpp
    src/kernel_string.cpp
    src/vfs.cpp
    src/xex_headers.cpp
    src/backtrace.cpp
    src/watch.cpp
    src/debug_hooks.cpp
    src/import_log.cpp
    src/import_stubs.cpp
)

target_include_directories(quake4 PRIVATE
    src
    "${PPC_DIR}"
    "${XENONRECOMP}/XenonUtils"
    "${XENONRECOMP}/thirdparty/simde"
    "${XENONRECOMP}/thirdparty/disasm"
    "${XENONRECOMP}/thirdparty/fmt/include"
)
target_compile_definitions(quake4 PRIVATE NOMINMAX GAME_DIR="${GAME_DIR}")
target_compile_options(quake4 PRIVATE /arch:AVX)

# Recompiled guest code is deeply recursive and the main guest thread runs on the process stack.
target_link_options(quake4 PRIVATE /MAP:quake4.map /MAPINFO:EXPORTS /STACK:0x4000000)

target_link_libraries(quake4 PRIVATE
    ppc_recomp
    "${XENONRECOMP_BUILD}/XenonUtils/XenonUtils.lib"
    "${XENONRECOMP_BUILD}/thirdparty/disasm/disasm.lib"
    "${XENONRECOMP_BUILD}/thirdparty/fmt/fmt.lib"
    psapi.lib
    d3d11.lib
    dxgi.lib
    d3dcompiler.lib
)
