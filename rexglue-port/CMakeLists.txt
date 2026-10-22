# q4 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(q4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(Q4_SOURCES
    src/main.cpp
    src/q4_hooks.cpp
    src/q4_menu.cpp
    src/q4_fps.cpp
    src/q4_tick.cpp
    src/q4_tick_hooks_ai.cpp
    src/q4_tick_hooks_phys.cpp
    src/q4_tick_hooks_engine.cpp
    src/q4_quit_menu.cpp
    src/q4_graphics.cpp
    src/q4_perf.cpp
    src/q4_tagscope.cpp
)

if(WIN32)
    add_executable(q4 WIN32 ${Q4_SOURCES})
else()
    add_executable(q4 ${Q4_SOURCES})
endif()

rexglue_setup_target(q4)

# Recompiled game code: allow AVX2 (VEX-encoded SSE for the VMX128 emulation, wider
# auto-vectorisation). No FMA, so floating-point results stay as before. Needs an AVX2 CPU
# (Intel Haswell / AMD Zen or newer); configure with -DQ4_AVX2=OFF for older machines.
option(Q4_AVX2 "Build the recompiled game code with AVX2 (measured: no gain over SSE4.1)" OFF)
if(Q4_AVX2 AND TARGET q4_recomp AND NOT MSVC)
    target_compile_options(q4_recomp PRIVATE -mavx2)
endif()
if(WIN32)
    target_link_libraries(q4 PRIVATE shell32 comdlg32)
endif()

# --- q4hle: native (HLE) renderer work. Off by default; the q4 target above is unaffected. ---
# A second executable that links the same recompiled objects (q4_recomp) plus src/hle/*.
option(Q4_HLE "Also build q4hle (native renderer work in progress)" OFF)
if(Q4_HLE)
    file(GLOB Q4_HLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/hle/*.cpp")
    # src/hle/hle_app.cpp provides the entry point in place of src/main.cpp
    set(Q4_HLE_BASE_SOURCES ${Q4_SOURCES})
    list(REMOVE_ITEM Q4_HLE_BASE_SOURCES src/main.cpp)
    if(WIN32)
        add_executable(q4hle WIN32 ${Q4_HLE_BASE_SOURCES} ${Q4_HLE_SOURCES})
    else()
        add_executable(q4hle ${Q4_HLE_BASE_SOURCES} ${Q4_HLE_SOURCES})
    endif()
    target_link_libraries(q4hle PRIVATE q4_recomp)
    add_dependencies(q4hle q4_codegen)
    target_include_directories(q4hle PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${REXGLUE_ENTRYPOINT_INCLUDE_DIR})
    target_link_libraries(q4hle PRIVATE rex::runtime)
    target_compile_definitions(q4hle PRIVATE Q4_HLE=1)
    if(WIN32)
        # symbol map for the profiler's host-code addresses (Q4_PROF=1, see src/hle/profiler.cpp)
        target_link_options(q4hle PRIVATE "-Wl,/MAP:${CMAKE_CURRENT_BINARY_DIR}/q4hle.map")
    endif()
    rexglue_configure_target(q4hle)
    if(WIN32)
        target_link_libraries(q4hle PRIVATE shell32 comdlg32)
    endif()
endif()
