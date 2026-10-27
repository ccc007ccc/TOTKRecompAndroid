# totkgame.so for Android: the game code generated from the player's own game files with gen.py --dyn-base
# (android/build_gamelib.sh runs the generator and this build). The runtime functions it calls are libmain.so's: it
# names libmain.so as a dependency, which is how Android's linker finds them when the runtime loads it (dlopen).
#   LAS_GEN: the generated code (sdk/, main/)   LAS_LIBMAIN: the app's libmain.so (to link against)
cmake_minimum_required(VERSION 3.22)
project(totkgame C)

set(R ${CMAKE_CURRENT_SOURCE_DIR}/../..)
get_filename_component(R ${R} ABSOLUTE)
include(${R}/android/gamelib_id.cmake)
message(STATUS "GAMELIB_ID ${GAMELIB_ID}")

file(GLOB GEN_SRC ${LAS_GEN}/main/*.c ${LAS_GEN}/sdk/*.c ${LAS_GEN}/subsdk0/*.c ${LAS_GEN}/rtld/*.c)
list(LENGTH GEN_SRC n)
if(n EQUAL 0)
    message(FATAL_ERROR "no generated code in ${LAS_GEN}")
endif()
if(NOT EXISTS ${LAS_LIBMAIN})
    message(FATAL_ERROR "LAS_LIBMAIN (the app's libmain.so) not found: ${LAS_LIBMAIN}")
endif()

add_library(main SHARED IMPORTED)
set_target_properties(main PROPERTIES IMPORTED_LOCATION ${LAS_LIBMAIN} IMPORTED_NO_SONAME FALSE)

add_library(totkgame SHARED ${GEN_SRC} ${R}/tools/gamelib_entry.c)
target_include_directories(totkgame PRIVATE ${R}/runtime)
target_compile_options(totkgame PRIVATE ${GAMELIB_FLAGS})
option(TOTK_FAST_COMPILE "Compile all functions cold for first-boot debugging (not a performance build)" OFF)
option(TOTK_DISABLE_LICM_PROMOTION "Avoid pathological LLVM MemorySSA updates in generated loops" ON)
if(TOTK_FAST_COMPILE)
    target_compile_options(totkgame PRIVATE -O0)
    message(STATUS "TotK first-boot build: all guest functions at -O0")
elseif(TOTK_DISABLE_LICM_PROMOTION AND CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # NDK r30: main_1011.c spent >15 minutes in MemorySSA rename during LICM
    # promotion; disabling only promotion compiled it in 51s at -O1. Keep
    # inlining and the remaining O1 passes, instead of making whole units O0.
    target_compile_options(totkgame PRIVATE -mllvm -disable-licm-promotion)
endif()
set_source_files_properties(${R}/tools/gamelib_entry.c PROPERTIES COMPILE_DEFINITIONS "GAMELIB_ID=\"${GAMELIB_ID}\"")
# Files with a gigantic function (heavy.txt, android/build_gamelib.sh: over 15,000 labels - a jump table's function
# gets one per instruction) at -O0: clang spent hours on each at -O1 / -O2 (v1.6.0), seconds at -O0
if(EXISTS ${LAS_GEN}/heavy.txt)
    file(STRINGS ${LAS_GEN}/heavy.txt HEAVY)
    foreach(f ${HEAVY})
        set_source_files_properties(${LAS_GEN}/${f} PROPERTIES COMPILE_OPTIONS "-O0")
    endforeach()
endif()
# Explicit exceptions for measured compiler failures below the label threshold.
# Keep these separate from shared generated inputs, and record them in the cache
# so a performance result can identify exactly which units remained unoptimized.
set(TOTK_EXTRA_HEAVY_SOURCES "" CACHE STRING "Additional generated units requiring O0, relative to LAS_GEN")
foreach(f IN LISTS TOTK_EXTRA_HEAVY_SOURCES)
    if(NOT "${LAS_GEN}/${f}" IN_LIST GEN_SRC)
        message(FATAL_ERROR "Not a generated source: ${f}")
    endif()
    set_source_files_properties(${LAS_GEN}/${f} PROPERTIES COMPILE_OPTIONS "-O0")
    message(STATUS "Additional compiler-resource exception at -O0: ${f}")
endforeach()
target_link_libraries(totkgame PRIVATE main)
set_target_properties(totkgame PROPERTIES PREFIX "")   # totkgame.so (runtime/gamelib.h GAMELIB_NAME)
file(WRITE ${CMAKE_BINARY_DIR}/totkgame.so.id "${GAMELIB_ID}\n")
