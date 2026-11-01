// Append-only Lua extension. Included by sw_mod_sdk.h after SwModHost is declared.
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef struct SwModHost SwModHost;

typedef enum SwLuaStatus {
    SW_LUA_OK = 0,
    SW_LUA_INVALID_ARGUMENT = 1,
    SW_LUA_INVALID_CONTEXT = 2,
    SW_LUA_OUT_OF_RANGE = 3,
    SW_LUA_TYPE_MISMATCH = 4,
    SW_LUA_STACK_LIMIT = 5,
    SW_LUA_UNSUPPORTED = 6,
    SW_LUA_ENGINE_ERROR = 7,
    SW_LUA_BUFFER_TOO_SMALL = 8
} SwLuaStatus;

typedef enum SwLuaType {
    SW_LUA_TYPE_NONE = -1,
    SW_LUA_TYPE_NIL = 0,
    SW_LUA_TYPE_BOOLEAN = 1,
    SW_LUA_TYPE_LIGHTUSERDATA = 2,
    SW_LUA_TYPE_NUMBER = 3,
    SW_LUA_TYPE_STRING = 4,
    SW_LUA_TYPE_TABLE = 5,
    SW_LUA_TYPE_FUNCTION = 6,
    SW_LUA_TYPE_USERDATA = 7,
    SW_LUA_TYPE_THREAD = 8,
    SW_LUA_TYPE_INTEGER = 9
} SwLuaType;

#define SW_LUA_MAX_STRING_BYTES ((size_t)1048576)
#define SW_LUA_MAX_ARRAY_SLOTS 65536u
#define SW_LUA_MAX_STACK_SLOTS 4096

// Every operation requires the current loader-registered callback, on its thread,
// with that callback's owner and exact state. Plugins must not retain the state;
// string buffers are copied synchronously and native Lua string pointers are never exported.
// Indices: positive or negative stack indices only; no registry/upvalue pseudo-indices.
typedef struct SwLuaExtrasApi {
    uint32_t structSize;
    // Lua strings are byte strings: UTF-8 and embedded NUL are preserved, no transcoding.
    SwLuaStatus (*pushString)(const SwModHost* owner, void* state, const char* bytes, size_t length);
    SwLuaStatus (*pushNil)(const SwModHost* owner, void* state);
    SwLuaStatus (*pushInteger)(const SwModHost* owner, void* state, int64_t value);
    SwLuaStatus (*createTable)(const SwModHost* owner, void* state, uint32_t arraySlots);
    // Bounded NUL-terminated keys (1..255 bytes). Tables with metatables are unsupported.
    // getField pushes one value (including nil); setField consumes the top value on success.
    SwLuaStatus (*getField)(const SwModHost* owner, void* state, int index, const char* key);
    SwLuaStatus (*setField)(const SwModHost* owner, void* state, int index, const char* key);
    SwLuaStatus (*getType)(const SwModHost* owner, void* state, int index, SwLuaType* type);
    SwLuaStatus (*pop)(const SwModHost* owner, void* state, int count);
    // Never calls lua_error/longjmp: returns SW_LUA_UNSUPPORTED without changing the stack.
    SwLuaStatus (*raiseError)(const SwModHost* owner, void* state, const char* bytes, size_t length);
    // String only, no conversion. Returns full byte length. Copies at most capacity-1,
    // terminates if capacity>0, preserves embedded NUL, reports truncation explicitly.
    SwLuaStatus (*copyString)(const SwModHost* owner, void* state, int index,
                             char* out, size_t capacity, size_t* length);
} SwLuaExtrasApi;
#ifdef __cplusplus
}
#endif
