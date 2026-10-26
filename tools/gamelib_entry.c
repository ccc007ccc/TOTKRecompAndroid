// Entry of the game code library (runtime/gamelib.h), compiled into it with the generated code: the function
// tables, found by name when the app loads the library. Everything else in it stays hidden.
#include "gamelib.h"

extern const uint64_t main_fn_addrs[], sdk_fn_addrs[], main_fn_count, sdk_fn_count;
extern const GuestFn main_fn_ptrs[], sdk_fn_ptrs[];
extern const uint64_t subsdk0_fn_addrs[], rtld_fn_addrs[], subsdk0_fn_count, rtld_fn_count;
extern const GuestFn subsdk0_fn_ptrs[], rtld_fn_ptrs[];
extern const uint8_t main_build_id[], sdk_build_id[], subsdk0_build_id[], rtld_build_id[];

GAMELIB_EXPORT const GameLibTables *totkgame_tables(void) {
    static const GameLibTables t = {GAMELIB_ABI,  GAMELIB_ID,   main_fn_addrs,  sdk_fn_addrs,
                                    main_fn_ptrs, sdk_fn_ptrs,  &main_fn_count, &sdk_fn_count,
                                    subsdk0_fn_addrs, rtld_fn_addrs, subsdk0_fn_ptrs, rtld_fn_ptrs,
                                    &subsdk0_fn_count, &rtld_fn_count,
                                    {main_build_id, sdk_build_id, subsdk0_build_id, rtld_build_id}};
    return &t;
}
