cmake_minimum_required(VERSION 3.18)
project(ravenbridge LANGUAGES C CXX ASM)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DYN "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/dynarmic")
set(DYN_BUILD "${CMAKE_CURRENT_SOURCE_DIR}/../build/dynarmic")

# Replaces libunity.so in the APK. Java's System.loadLibrary("unity") lands here.
add_library(unity SHARED
    src/main.cpp src/mem.cpp src/cpu.cpp src/elf.cpp src/libc.cpp src/pthread.cpp src/jni.cpp src/gl.cpp src/fx.cpp src/tramp.S)
target_include_directories(unity PRIVATE src gen "${DYN}/src" "${DYN}/externals/mcl/include")
option(RAVEN_DEV "Development override of the bridge library" OFF)
if (RAVEN_DEV)
    target_compile_definitions(unity PRIVATE RAVEN_DEV_OVERRIDE)
endif()
target_compile_options(unity PRIVATE -O2 -fno-strict-aliasing -Wall -Wno-unused-function -Wno-unused-variable -fexceptions)
target_link_libraries(unity PRIVATE
    "${DYN_BUILD}/src/dynarmic/libdynarmic.a"
    "${DYN_BUILD}/externals/fmt/libfmt.a"
    "${DYN_BUILD}/externals/mcl/src/libmcl.a"
    log dl z GLESv3 GLESv2 GLESv1_CM EGL android)
target_link_options(unity PRIVATE -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/exports.map -Wl,-z,max-page-size=16384)

# Empty stand-ins so Java's loadLibrary calls succeed.
add_library(mono SHARED src/empty.c)
add_library(lib-ouya-ndk SHARED src/empty.c)
set_target_properties(lib-ouya-ndk PROPERTIES OUTPUT_NAME "-ouya-ndk")
