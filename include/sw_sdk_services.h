#pragma once
// Services between mods (appended to SwEngineApi as `services`; check structSize).
//
// A provider mod publishes a table of functions under a global name; other mods get that
// table and call it directly (same process, plain C calls, no copy). Example: an `sf_browser`
// mod publishes "browser" and UI mods use it to open pages.
//
// Lifetime rule, enforced by the loader: `get` only succeeds when the provider mod is listed,
// directly or through other mods, in the consumer's mod.json "dependencies". The loader then
// loads the provider first and unloads it last, so the table stays valid for as long as the
// consumer is loaded. A consumer that runs threads must stop them in SwModUnload.
//
// Conventions for a published table:
//   * a C struct of function pointers whose first member is `uint32_t structSize`;
//   * append members only, and bump the minor version when you do; bump the major version
//     for an incompatible change;
//   * `static const` storage in the provider DLL (it is not copied);
//   * functions callable from any thread unless the provider documents otherwise.
#ifdef __cplusplus
extern "C" {
#endif

#define SW_SERVICE_VERSION(major, minor) ((((uint32_t)(major)) << 16) | ((uint32_t)(minor) & 0xffffu))
#define SW_SERVICE_MAJOR(version) ((uint32_t)(version) >> 16)
#define SW_SERVICE_MINOR(version) ((uint32_t)(version) & 0xffffu)
#define SW_SERVICE_MAX_NAME 64u

typedef struct SwServiceApi {
    uint32_t structSize;
    // name: 1..64 characters of [a-z0-9_.-], unique in the process (SW_CONFLICT if another
    // mod published it). Publishing the same name again from the same mod replaces it.
    // tableSize: sizeof the table (at least 4). Withdrawn automatically when the mod unloads.
    // Any thread.
    SwResult (*publish)(const SwModHost*, const char* name, uint32_t version,
                        const void* table, size_t tableSize);
    SwResult (*unpublish)(const SwModHost*, const char* name);
    // Returns the table when it exists (SW_NOT_FOUND otherwise), the provider is a dependency
    // of the caller (SW_CONFLICT otherwise; also logged), and its major version equals
    // SW_SERVICE_MAJOR(minVersion) with a minor version >= SW_SERVICE_MINOR(minVersion)
    // (SW_UNAVAILABLE otherwise; *version still receives the published version).
    // version and tableSize may be NULL. Any thread.
    SwResult (*get)(const SwModHost*, const char* name, uint32_t minVersion,
                    const void** table, uint32_t* version, size_t* tableSize);
    // Appended (check structSize). For providers that keep per-consumer state (objects,
    // callbacks into the consumer): `callback` runs whenever ANOTHER mod is being unloaded,
    // before its SwModUnload and before its DLL is freed. Drop everything that belongs to
    // `released` there, and make sure none of its callbacks is running or will run again.
    // Runs on the thread that unloads (the UI thread). One callback per provider (replaces);
    // NULL removes it. Removed automatically when the provider unloads.
    SwResult (*setOwnerReleaseCallback)(const SwModHost* provider,
                                        void (*callback)(const SwModHost* released, void* userData),
                                        void* userData);
} SwServiceApi;

#ifdef __cplusplus
}
#endif
