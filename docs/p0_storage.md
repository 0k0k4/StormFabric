# P0 binary configuration and persistent storage

Implemented independently in `src/mod_storage.cpp`. No game functions, installation,
hooks, save-file mutation, or game execution are needed. This is mod-owned filesystem
storage, **not** integration with Stormworks saves or save lifecycle events.

## Parent integration (not edited by this unit)

Include `sw_sdk_storage.h` in `include/sw_mod_sdk.h` after the `SwModHost` typedef
is available and before defining `SwEngineApi`. Append, without changing old offsets:

```cpp
const SwStorageApi* storage; // check engine structSize before accessing
```

Compile/link `src/mod_storage.cpp` with the loader. In `engine_api.cpp`:

```cpp
#include "mod_storage.h"

// Namespace swml, outside its anonymous namespace:
bool IsEngineApiOwner(const SwModHost* owner) { return ValidOwner(owner); }

// During InitializeEngineApi:
g_engine.storage = GetStorageApi();

// After g_owners.insert(owner), before invoking SwModInit:
const auto storageStatus = RegisterModStorageOwner(owner);
// Log failures; storage calls for this owner then return INVALID_OWNER.

// FIRST in ReleaseEngineApiOwner, before SwModUnload and owner erasure:
ReleaseModStorageOwner(owner);
```

Owner validation must be pointer-membership-only, not dereference untrusted/stale
hosts. The parent must synchronize its engine-owner registry if ownership changes
can race calls from plugin threads. Never hold the parent registry lock while
calling Register/Release storage: storage calls validate owners while holding the
storage mutex. A membership helper may acquire the registry lock internally.
Release waits for in-flight storage I/O. The parent must stop/join plugin workers
before destroying the host or unloading its DLL. Pointer reuse after re-registration
cannot distinguish an old host reference from a new one; this pointer-based SDK is
not an adversarial native-plugin sandbox.

### World lifecycle

`SwStorageStatus SetStorageWorld(const std::string& worldId)` is an internal loader
hook, not exposed to mods. Empty disables world storage; invalid nonempty IDs return
`INVALID_KEY` **and disable** it. IDs obey the key grammar below, including no embedded
NUL. The default is unavailable. Call only with an actual, stable engine-supplied
world identifier; clear on world exit/transition. Do not derive an invented ID from
a menu state, timestamp, current directory, or map title. If the engine cannot supply
a validated ID, never enable this scope. World changes and I/O serialize together.
World files remain mod-local and do not travel with game saves automatically.

## C API and layout

The API has a `uint32_t structSize` prefix, followed by:

```c
SwStorageStatus (*readConfig)(const SwModHost*, const char* keyUtf8,
                             uint8_t* buffer, size_t capacity, size_t* outSize);
SwStorageStatus (*writeConfig)(const SwModHost*, const char* keyUtf8,
                              const uint8_t* bytes, size_t size);
SwStorageStatus (*readData)(const SwModHost*, SwStorageScope, const char* keyUtf8,
                           uint8_t* buffer, size_t capacity, size_t* outSize);
SwStorageStatus (*writeData)(const SwModHost*, SwStorageScope, const char* keyUtf8,
                            const uint8_t* bytes, size_t size);
```

All scopes are accepted by readData/writeData. Config wrappers use CONFIG.

| Scope | Path relative to snapshotted modDir |
| --- | --- |
| `SW_STORAGE_CONFIG` | `config/<key>.bin` |
| `SW_STORAGE_MOD_DATA` | `data/<key>.bin` |
| `SW_STORAGE_WORLD_DATA` | `data/world/<actual-world-id>/<key>.bin` |

`RegisterModStorageOwner(const SwModHost*)` returns `SwStorageStatus`, snapshots
the registered owner's UTF-8 modDir once (canonicalized from the opened directory
handle, not merely a lexical full-path expansion), and pins its existing absolute local-drive
root and all ancestors. Repeated registration is idempotent, not a path update.
`ReleaseModStorageOwner(const SwModHost*)` returns void and removes only bookkeeping;
it never deletes owner files. `GetStorageApi()` returns a process-lifetime const API.

Limits and validation:

- Files: at most **4 MiB**, including empty files; no implicit text encoding/NUL.
- Keys/IDs: **1..64 bytes**, ASCII letters, digits, `.`, `-`, `_`; no `..`, trailing
  dot, paths, separators, colons/ADS, whitespace, or DOS device basenames (case-insensitive,
  including extensions and COM/LPT digits). Non-ASCII names are rejected deliberately.
- Root: strict UTF-8 absolute drive path; Unicode directory names are supported.
  UNC, device namespaces, relative paths, dot segments, ADS, control/wildcard characters,
  trailing component dot/space, and directory reparse points are rejected.
- Generated paths must remain below **240 UTF-16 units**, with reserved room for
  temporary names. No long-path assumptions or truncation.
- Every touched directory is pinned without delete sharing and checked through
  its handle; child opens use a single leaf relative to the checked parent handle
  (`NtCreateFile`, OPEN_REPARSE_POINT, OBJ_DONT_REPARSE), never a fresh absolute
  traversal. Directory write sharing is needed for Windows rename operations;
  reparse changes to registered roots are checked on each call, and handle-relative
  traversal prevents a subsequent change from redirecting the operation. Existing
  file reparse points, directories and multiple hardlinks are rejected.
  An incompatible external open can produce IO_ERROR rather than UNSAFE_PATH.

Read semantics: `outSize` is required; it is zero on errors except BUFFER_TOO_SMALL,
which reports the needed size. `NULL, 0` queries size (OK for empty, otherwise
BUFFER_TOO_SMALL). NULL with nonzero capacity is invalid. Short buffers and I/O
errors leave bytes untouched. Reads hold a deny-write/delete file handle and copy
a private bounded snapshot only after a full successful read. A size query and a
later read are separate snapshots: callers must handle the size changing between them.

Write semantics: NULL is valid only with size zero. A unique, exclusively created
temporary file in the pinned target directory is flushed, then atomically committed
with handle-relative `NtSetInformationFile(FileRenameInformation)` replacement
while its exclusive source handle remains open. **This deliberately replaces the
requested MoveFileEx strategy**: denying directory write sharing prevents MoveFileEx
commits, and allowing it with absolute-path commits admits reparse redirection races.
All NT calls resolve from the already loaded `ntdll.dll`; no new linker libraries
are needed. Failure never truncates the old value; cleanup uses disposition on only
that operation's owned temporary handle, never an absolute-path deletion. Power-loss
durability remains subject to Windows/filesystem/device guarantees, not a transaction
with a game save. Crashes may leave an uncommitted `.swml-*.tmp`; no broad cleanup runs.
Directory pins protect traversal, not arbitrary hostile same-process memory writes
or arbitrary filesystem activity by another native DLL. This is not a native sandbox.

Status values: OK=0, INVALID_ARGUMENT=1, INVALID_OWNER=2, INVALID_KEY=3,
NOT_FOUND=4, BUFFER_TOO_SMALL=5, TOO_LARGE=6, UNAVAILABLE=7, UNSAFE_PATH=8,
IO_ERROR=9. The enum is independent of any parent generic result enum. Scope values
are CONFIG=0, MOD_DATA=1, WORLD_DATA=2. Invalid buffer arguments/oversize writes are
checked before ownership, so invalid calls may return those errors first.

## Standalone verification

In an x64 Visual Studio developer command prompt, from `StormworksModLoader`:

```bat
cl /nologo /EHsc /std:c++17 /W4 /DNOMINMAX /DWIN32_LEAN_AND_MEAN tests\mod_storage_tests.cpp src\mod_storage.cpp /Fo:%TEMP%\swml_tests\ /Fe:%TEMP%\swml_tests\mod_storage_tests.exe
%TEMP%\swml_tests\mod_storage_tests.exe
```

The header also compiles as C with the existing SDK smoke test:

```bat
cl /nologo /TC /W4 /WX /c /Iinclude /FIsw_mod_sdk.h /FIsw_sdk_storage.h tests\sdk_c_smoke.c /Fo:%TEMP%\swml_tests\storage_c_smoke.obj
```

The test implements only the engine-owner membership seam; storage I/O is real.
It creates a unique tree under the approved Windows temp workspace, exercises binary
config/data/empty/4 MiB roundtrips, size queries/short buffers, invalid input and keys,
world unavailability/switching, ownership release/stale hosts, path snapshots,
directory pinning, hardlinks, actual junction rejection (including a root changed
after registration), failed atomic replacement,
unchanged buffers on sharing errors, and concurrent complete snapshots. It removes
only its unique tree after explicitly removing its junctions and releasing handles.
