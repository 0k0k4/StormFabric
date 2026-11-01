#include "../include/sw_mod_sdk.h"
#include "../include/sw_sdk_lua.h"
#include <stddef.h>
_Static_assert(offsetof(SwLuaExtrasApi, structSize) == 0, "size prefix");
_Static_assert(SW_LUA_TYPE_NIL != SW_LUA_TYPE_NONE, "nil is not absent");
_Static_assert(SW_LUA_TYPE_INTEGER != SW_LUA_TYPE_NUMBER, "integer preserves all 64 bits");
int p0_lua_c_smoke(const SwLuaExtrasApi* api, const SwModHost* host, void* state) {
    return (int)api->pushString(host, state, "a\0b", 3);
}
