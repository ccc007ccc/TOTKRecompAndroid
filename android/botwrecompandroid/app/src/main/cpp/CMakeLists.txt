# libmain.so: the TOTKRecompAndroid runtime for Android, with the
# Vulkan backend and the ImGui UI. The game code is not in it: totkgame.so, generated with gen.py --dyn-base and built
# from the player's own game files, is loaded at run time (docs/las/ANDROID-PORT.md).
cmake_minimum_required(VERSION 3.22)
project(botwrecompandroid C CXX)

set(R ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..)   # the repository
get_filename_component(R ${R} ABSOLUTE)
set(DEPS ${R}/android/deps/out)                        # android/deps/build_deps.sh
if(NOT EXISTS ${DEPS}/lib/libSDL3.so)
    message(FATAL_ERROR "run android/deps/build_deps.sh first")
endif()
if(NOT BOTW_GEN_DIR)
    message(FATAL_ERROR "BOTW_GEN_DIR (the Gradle build's generated files: smaa_hlsl.inc) is not set")
endif()

include(${R}/android/gamelib_id.cmake)  # (GAMELIB_ID: the same as totkgame.so's, android/gamelib)
message(STATUS "GAMELIB_ID ${GAMELIB_ID}")

add_subdirectory(${R}/third_party/libadrenotools ${CMAKE_BINARY_DIR}/adrenotools)
foreach(native_target adrenotools linkernsbypass hook_impl main_hook file_redirect_hook gsl_alloc_hook)
    target_compile_options(${native_target} PRIVATE ${TOTK_RUNTIME_FLAGS})
    totk_link_profile(${native_target})
endforeach()

add_library(SDL3 SHARED IMPORTED)
set_target_properties(SDL3 PROPERTIES IMPORTED_LOCATION ${DEPS}/lib/libSDL3.so)
add_library(shaderc_shared SHARED IMPORTED)
set_target_properties(shaderc_shared PROPERTIES IMPORTED_LOCATION ${DEPS}/lib/libshaderc_shared.so)

file(GLOB RT_SRC CONFIGURE_DEPENDS ${R}/runtime/*.c ${R}/runtime/hle/*.c ${R}/runtime/services/*.c ${R}/runtime/nvn/*.c
     ${R}/runtime/nvn/maxwell/*.c)
# FFmpeg (android/deps/build_ffmpeg.sh): the game's movies and the start screen's background; without it, neither
set(HAVE_FFMPEG FALSE)
if(EXISTS ${DEPS}/lib/libavformat.so)
    set(HAVE_FFMPEG TRUE)
    foreach(l avcodec avformat avutil swscale)
        add_library(${l} SHARED IMPORTED)
        set_target_properties(${l} PROPERTIES IMPORTED_LOCATION ${DEPS}/lib/lib${l}.so)
    endforeach()
else()
    list(REMOVE_ITEM RT_SRC ${R}/runtime/hle/movie.c)
endif()
file(GLOB VK_SRC CONFIGURE_DEPENDS ${R}/runtime/nvn/vulkan/*.c ${R}/runtime/nvn/vulkan/*.cpp)
file(GLOB UI_SRC CONFIGURE_DEPENDS ${R}/runtime/ui/*.mm)
set(IMGUI ${R}/third_party/imgui)
set(IMGUI_SRC ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_sdl3.cpp ${IMGUI}/backends/imgui_impl_vulkan.cpp)

add_library(main SHARED ${RT_SRC} ${R}/runtime/platform/android.c ${VK_SRC} ${UI_SRC} ${IMGUI_SRC}
            ${DEPS}/volk/volk.c ${R}/android/common/guest_window.c)
# the UI sources are plain C++ apart from __APPLE__ blocks (compiled as C++ on Android)
set_source_files_properties(${UI_SRC} PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-xc++")
set_source_files_properties(${R}/runtime/gamelib.c PROPERTIES COMPILE_DEFINITIONS
    "GAMELIB_ID=\"${GAMELIB_ID}\";GAMELIB_BUILD_PROFILE=\"${TOTK_BUILD_PROFILE}\"")
set_source_files_properties(${R}/runtime/gamebuild.c PROPERTIES COMPILE_DEFINITIONS "GAMELIB_ID=\"${GAMELIB_ID}\"")
# Use the same profile when the player builds on the phone. A high-performance APK must not create O1/optnone
# game code carrying an O3 identity merely because the desktop CMake target was configured correctly.
string(JOIN " " _phone_game_flags ${GAMELIB_FLAGS})
set(_phone_cflags "-target aarch64-linux-musl ${_phone_game_flags} -mllvm -dag-maps-huge-region=500 -fno-sanitize=undefined -fno-stack-protector -g0")
if(TOTK_BUILD_PROFILE STREQUAL "dev" AND TOTK_DISABLE_LICM_PROMOTION)
    string(APPEND _phone_cflags " -mllvm -disable-licm-promotion")
endif()
set(_phone_link_flags "-target aarch64-linux-musl -shared -nostdlib -Wl,-z,max-page-size=16384")
set(_phone_performance 0)
if(NOT TOTK_BUILD_PROFILE STREQUAL "dev")
    set(_phone_performance 1)
    # Zig 0.17 rejects LLD's --lto-O3/--thinlto-jobs/-mllvm linker arguments. Its driver selects the LTO
    # optimization level with -O3. The compile-time LICM workaround remains in GAMELIB_FLAGS.
    string(APPEND _phone_link_flags " -O3 -flto=thin")
endif()
set_property(SOURCE ${R}/runtime/gamebuild.c APPEND PROPERTY COMPILE_DEFINITIONS
    "GAMELIB_ANDROID_CFLAGS=\"${_phone_cflags}\"" "GAMELIB_ANDROID_SHARED=\"${_phone_link_flags}\""
    "GAMELIB_PERFORMANCE=${_phone_performance}")

target_include_directories(main PRIVATE ${R}/runtime ${R}/runtime/nvn ${IMGUI} ${IMGUI}/backends ${DEPS}/include
                           ${DEPS}/volk ${BOTW_GEN_DIR} ${R}/third_party/smaa/Textures ${R}/android/common)
target_compile_definitions(main PRIVATE
    VK_NO_PROTOTYPES VK_USE_PLATFORM_ANDROID_KHR IMGUI_IMPL_VULKAN_USE_VOLK   # Vulkan from the loaded driver (volk)
    VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)
if(HAVE_FFMPEG)
    target_link_libraries(main PRIVATE avcodec avformat avutil swscale)
else()
    target_compile_definitions(main PRIVATE LARECOMP_NO_FFMPEG)
endif()
# Debug stays usable at -O2; release profiles use -O3 and ThinLTO while preserving guest floating-point semantics.
target_compile_options(main PRIVATE ${TOTK_RUNTIME_FLAGS} -g -fno-strict-aliasing -Wall -Wno-unused-function -Wno-format  # (as on Linux: %llx for uint64_t)
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++17>)
totk_link_profile(main)
message(STATUS "TotK runtime profile: ${TOTK_BUILD_PROFILE}; flags: ${TOTK_RUNTIME_FLAGS}")
target_link_libraries(main PRIVATE SDL3 shaderc_shared adrenotools android nativewindow log dl m z)
# (runtime/hle.h, ipc.h, main.c: tables in named sections found through __start_ / __stop_ symbols)
target_link_options(main PRIVATE -Wl,-z,nostart-stop-gc)
