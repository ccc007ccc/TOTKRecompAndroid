# GAMELIB_ID for Android (runtime/gamelib.h runtime/game_profile.h): a hash of the generator,
# the headers the generated code is compiled against, and its flags. Included by the app's CMakeLists.txt
# (libmain.so checks it) and android/gamelib/CMakeLists.txt (totkgame.so carries it), so the two always agree.
# Needs R (the repository). Sets GAMELIB_ID and GAMELIB_FLAGS (the generated code's compile flags, as a list).
include(${R}/android/build_profiles.cmake)
# The dev profile retains the bring-up settings. Performance profiles optimize every generated unit.
set(GAMELIB_FLAGS ${TOTK_GAME_OPT_FLAGS} -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -fPIC)
if(NOT TOTK_BUILD_PROFILE STREQUAL "dev" AND TOTK_DISABLE_LICM_PROMOTION)
    list(APPEND GAMELIB_FLAGS -mllvm -disable-licm-promotion)
endif()
set(_gamelib_srcs tools/gen.py tools/lifter.py tools/analyze.py tools/nso.py tools/passes.py tools/patches.py
    tools/simd_c.py tools/gamelib_entry.c runtime/hle/hle_symbols.txt
    runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h runtime/game_profile.h)
set(_id_text "")
foreach(f ${_gamelib_srcs})
    file(READ ${R}/${f} _t)
    string(APPEND _id_text "${_t}")
    # (a change re-runs the configure: the app's id followed only CMakeLists edits, and a new patches.py gave the game
    # code an id the installed app rejected - the launcher took the game for missing)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${R}/${f})
endforeach()
string(SHA256 _id "${_id_text}${GAMELIB_FLAGS} --simd asm --dyn-base")
string(SUBSTRING ${_id} 0 16 GAMELIB_ID)
