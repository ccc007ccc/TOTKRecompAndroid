# Shared by the APK and generated game library; optimization flags are part of GAMELIB_ID.
include_guard(GLOBAL)
set(TOTK_BUILD_PROFILE "dev" CACHE STRING "Native profile: dev, performance, snapdragon-8elite")
set_property(CACHE TOTK_BUILD_PROFILE PROPERTY STRINGS dev performance snapdragon-8elite)
set(TOTK_LTO_JOBS "2" CACHE STRING "Maximum parallel ThinLTO backends")
set(TOTK_COMPILE_JOBS "2" CACHE STRING "Maximum simultaneous native compilations")
option(TOTK_DISABLE_LICM_PROMOTION "Avoid pathological LLVM MemorySSA updates in generated loops" ON)
if(NOT TOTK_BUILD_PROFILE MATCHES "^(dev|performance|snapdragon-8elite)$")
    message(FATAL_ERROR "Unknown TOTK_BUILD_PROFILE: ${TOTK_BUILD_PROFILE}")
endif()
if(NOT TOTK_LTO_JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "TOTK_LTO_JOBS must be a positive integer")
endif()
if(NOT TOTK_COMPILE_JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "TOTK_COMPILE_JOBS must be a positive integer")
endif()
# Ninja is invoked separately by AGP; Gradle's worker count alone does not bound native compiler memory.
set_property(GLOBAL APPEND PROPERTY JOB_POOLS "totk_compile=${TOTK_COMPILE_JOBS}" totk_link=1)
set(CMAKE_JOB_POOL_COMPILE totk_compile)
set(CMAKE_JOB_POOL_LINK totk_link)

# Keep the existing dev flags byte-for-byte compatible with installed game libraries.
set(TOTK_RUNTIME_FLAGS -O2)
set(TOTK_GAME_OPT_FLAGS -O1)
set(TOTK_CPU_FLAGS "")
if(NOT TOTK_BUILD_PROFILE STREQUAL "dev")
    # SM8750 exposes Armv8.7-A features. NDK r30 supports only the oryon-1 scheduling model,
    # not a distinct second-generation Oryon model; do not substitute a Cortex CPU or use -march=native.
    if(TOTK_BUILD_PROFILE STREQUAL "snapdragon-8elite")
        set(TOTK_CPU_FLAGS -march=armv8.7-a+crypto+fp16 -mtune=oryon-1)
    endif()
    set(TOTK_RUNTIME_FLAGS -O3 -flto=thin -fno-fast-math ${TOTK_CPU_FLAGS})
    set(TOTK_GAME_OPT_FLAGS ${TOTK_RUNTIME_FLAGS})
endif()

function(totk_link_profile target)
    if(NOT TOTK_BUILD_PROFILE STREQUAL "dev")
        if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
            message(FATAL_ERROR "Performance profiles require Clang with ThinLTO")
        endif()
        if(TOTK_BUILD_PROFILE STREQUAL "snapdragon-8elite" AND NOT ANDROID_ABI STREQUAL "arm64-v8a")
            message(FATAL_ERROR "snapdragon-8elite requires Android arm64-v8a")
        endif()
        target_link_options(${target} PRIVATE -flto=thin -Wl,--lto-O3
            "-Wl,--thinlto-jobs=${TOTK_LTO_JOBS}"
            "-Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/thinlto-cache")
    endif()
endfunction()
