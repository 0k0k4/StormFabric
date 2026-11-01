// Include after the SwModHost typedef (or include sw_mod_sdk.h first).
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SW_STORAGE_MAX_BYTES (4u * 1024u * 1024u)
#define SW_STORAGE_MAX_KEY_BYTES 64u

typedef enum SwStorageStatus {
    SW_STORAGE_OK = 0,
    SW_STORAGE_INVALID_ARGUMENT = 1,
    SW_STORAGE_INVALID_OWNER = 2,
    SW_STORAGE_INVALID_KEY = 3,
    SW_STORAGE_NOT_FOUND = 4,
    SW_STORAGE_BUFFER_TOO_SMALL = 5,
    SW_STORAGE_TOO_LARGE = 6,
    SW_STORAGE_UNAVAILABLE = 7,
    SW_STORAGE_UNSAFE_PATH = 8,
    SW_STORAGE_IO_ERROR = 9
} SwStorageStatus;

typedef enum SwStorageScope {
    SW_STORAGE_CONFIG = 0,
    SW_STORAGE_MOD_DATA = 1,
    SW_STORAGE_WORLD_DATA = 2
} SwStorageScope;

// Keys are UTF-8 strings restricted to 1..64 ASCII letters/digits/._-.
// No "..", trailing dot, or DOS device basename is accepted. Data is binary.
// Reads require outSize; set it to the snapshot size on OK/BUFFER_TOO_SMALL,
// or zero on other errors. A short buffer is never partially modified.
// NULL buffer + zero capacity is a size query (OK for an empty file).
// Writes accept NULL bytes only for size zero. No trailing NUL is stored.
typedef struct SwStorageApi {
    uint32_t structSize;
    SwStorageStatus (*readConfig)(const SwModHost* owner, const char* keyUtf8,
                                 uint8_t* buffer, size_t capacity, size_t* outSize);
    SwStorageStatus (*writeConfig)(const SwModHost* owner, const char* keyUtf8,
                                  const uint8_t* bytes, size_t size);
    // All three scopes are accepted; WORLD_DATA requires a loader-provided world ID.
    SwStorageStatus (*readData)(const SwModHost* owner, SwStorageScope scope,
                               const char* keyUtf8, uint8_t* buffer,
                               size_t capacity, size_t* outSize);
    SwStorageStatus (*writeData)(const SwModHost* owner, SwStorageScope scope,
                                const char* keyUtf8, const uint8_t* bytes, size_t size);
} SwStorageApi;

#ifdef __cplusplus
}
#endif
