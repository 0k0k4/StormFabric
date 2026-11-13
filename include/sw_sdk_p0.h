#pragma once
// Append-only SDK services. Handles are loader-owned identities, never pointers.
typedef uint64_t SwHandle;
typedef enum SwResult {
    SW_OK=0, SW_INVALID_ARGUMENT=1, SW_INVALID_OWNER=2, SW_NOT_FOUND=3,
    SW_UNAVAILABLE=4, SW_WRONG_THREAD=5, SW_LIMIT_REACHED=6,
    SW_BUFFER_TOO_SMALL=7, SW_CONFLICT=8, SW_IO_ERROR=9
} SwResult;
typedef enum SwCapability {
    SW_CAP_UI_EVENTS=1ull<<0, SW_CAP_MAIN_THREAD_TASKS=1ull<<1,
    SW_CAP_KEYBOARD_ACTIONS=1ull<<2, SW_CAP_MOD_STORAGE=1ull<<3,
    SW_CAP_NATIVE_HUD=1ull<<4, SW_CAP_LUA_EXTRAS=1ull<<5,
    SW_CAP_SIMULATION_EVENTS=1ull<<6, SW_CAP_WORLD=1ull<<7,
    SW_CAP_VEHICLES=1ull<<8, SW_CAP_PLAYERS=1ull<<9, SW_CAP_WORLD_STORAGE=1ull<<10,
    SW_CAP_LUA_EVENTS=1ull<<11, SW_CAP_LUA_QOL=1ull<<12
} SwCapability;
typedef enum SwUiSurface { SW_SURFACE_MENU=0, SW_SURFACE_HUD=1, SW_SURFACE_EDITOR=2, SW_SURFACE_NONE=3 } SwUiSurface;
typedef enum SwEventType {
    SW_EVENT_UI_UPDATE=1, SW_EVENT_UI_READY=2, SW_EVENT_UI_CLOSED=3,
    SW_EVENT_ACTION=4, SW_EVENT_LUA_CREATED=5, SW_EVENT_LUA_DESTROYED=6,
    SW_EVENT_WORLD_LOADED=7, SW_EVENT_WORLD_CLOSED=8, SW_EVENT_SIMULATION_TICK=9,
    SW_EVENT_ENTITY_CREATED=10, SW_EVENT_ENTITY_DESTROYED=11, SW_EVENT_PLAYER_CONNECTED=12,
    SW_EVENT_UI_CLICK=13
} SwEventType;
#define SW_EVENT_MASK(type) (UINT64_C(1) << (type))
typedef struct SwEvent {
    uint32_t structSize;
    SwEventType type;
    SwHandle handle;
    SwUiSurface surface;
    uint32_t value; // action: 1 pressed, 0 released
    uint64_t sequence;
    double monotonicSeconds, uiDeltaSeconds; // NOT simulation time
} SwEvent;
typedef void (*SwEventCallback)(const SwEvent* event, void* userData);
typedef void (*SwTaskCallback)(void* userData);
typedef struct SwRuntimeApi {
    uint32_t structSize;
    uint64_t (*capabilities)(void);
    SwResult (*subscribe)(const SwModHost*,uint64_t eventMask,SwEventCallback,void*,SwHandle*);
    SwResult (*unsubscribe)(const SwModHost*,SwHandle);
    SwResult (*runOnGameThread)(const SwModHost*,SwTaskCallback,void*,SwHandle*);
    SwResult (*schedule)(const SwModHost*,double delaySeconds,double repeatSeconds,SwTaskCallback,void*,SwHandle*);
    SwResult (*cancelTask)(const SwModHost*,SwHandle);
    double (*getMonotonicTime)(void);
    bool (*isGameThread)(void);
    SwResult (*getSimulationTime)(double* outSeconds); // unavailable until a certified simulation binding exists
} SwRuntimeApi;
typedef struct SwActionState { bool down,pressed,released; uint64_t sequence; } SwActionState;
enum { SW_KEY_CTRL=1, SW_KEY_SHIFT=2, SW_KEY_ALT=4 };
typedef struct SwInputApi {
    uint32_t structSize;
    SwResult (*registerAction)(const SwModHost*,const char* name,SwHandle*);
    SwResult (*bindKey)(const SwModHost*,SwHandle,uint32_t windowsVirtualKey,uint32_t modifiers);
    SwResult (*getActionState)(const SwModHost*,SwHandle,SwActionState*);
    SwResult (*unregisterAction)(const SwModHost*,SwHandle);
} SwInputApi;
typedef struct SwNativeUiApi {
    uint32_t structSize;
    SwResult (*createHudRoot)(const SwModHost*,SwUiSurface,SwHandle*);
    SwResult (*createWindow)(const SwModHost*,SwHandle parent,const SwUiLayout*,const char* title,SwHandle*);
    SwResult (*text)(const SwModHost*,SwHandle parent,const SwUiLayout*,const char* utf8,uint32_t color,SwHandle*);
    // Button width/height must be positive; the caption uses the same bounded box.
    SwResult (*button)(const SwModHost*,SwHandle parent,const SwUiLayout*,const char* label,SwEventCallback,void*,SwHandle*);
    SwResult (*setText)(const SwModHost*,SwHandle,const char* utf8);
    SwResult (*setVisible)(const SwModHost*,SwHandle,bool);
    SwResult (*destroy)(const SwModHost*,SwHandle);
    SwResult (*showNotification)(const SwModHost*,uint32_t level,const char* title,const char* message);
} SwNativeUiApi;
typedef struct SwTransform { double position[3],rotation[4]; } SwTransform;
typedef struct SwWorldInfo { uint32_t structSize;SwHandle world;bool multiplayer,isServer;double simulationSeconds; } SwWorldInfo;
typedef enum SwEntityType { SW_ENTITY_VEHICLE=1,SW_ENTITY_CHARACTER=2,SW_ENTITY_OBJECT=3 } SwEntityType;
typedef struct SwEntityInfo { uint32_t structSize;SwHandle entity;SwEntityType type;int64_t nativeId;char name[128]; } SwEntityInfo;
typedef struct SwPlayerInfo { uint32_t structSize;SwHandle player,character,vehicle;bool local;char name[128]; } SwPlayerInfo;
typedef struct SwWorldApi {
    uint32_t structSize;
    SwResult (*getWorldInfo)(const SwModHost*,SwWorldInfo*);
    SwResult (*enumerateEntities)(const SwModHost*,SwEntityType,SwHandle* out,size_t capacity,size_t* count);
    SwResult (*getEntityInfo)(const SwModHost*,SwHandle,SwEntityInfo*);
    SwResult (*getTransform)(const SwModHost*,SwHandle,SwTransform*);
    SwResult (*getVelocity)(const SwModHost*,SwHandle,double outVelocity[3]);
    SwResult (*enumeratePlayers)(const SwModHost*,SwHandle* out,size_t capacity,size_t* count);
    SwResult (*getPlayerInfo)(const SwModHost*,SwHandle,SwPlayerInfo*);
} SwWorldApi;

// ---------------------------------------------------------------------------
// Native primitives (SDK v2 extension, appended to SwEngineApi).
//
// Everything here is generic: the loader never contains mod-specific logic. A
// mod asks for a *role* and the loader resolves it from its compiled discovery
// profile; every address is validated against retained instruction bytes before
// it is handed out. All primitives are fail-closed:
//   * an unresolved/ambiguous contract yields NULL / SW_NOT_FOUND / SW_CONFLICT
//     and NO memory is written;
//   * every code write validates the target pages (VirtualQuery), removes
//     PAGE_GUARD, restores the original protection and flushes the instruction
//     cache; a failed restore is reported (the hook stays fail-closed).
//   * inline hooks validate the expected prologue bytes and are always paired
//     with a trampoline that the loader owns. Handles are owner-scoped and are
//     restored automatically when the owning mod is unloaded.
//
// Handles are loader-owned identities, never pointers. Restoring twice is safe
// (idempotent) and returns SW_OK.

#define SW_NATIVE_API_VERSION 1

// One entry of the reviewed Lua binding table. The table is generated from the
// modding kit (tools/gen_lua_bindings.py) and is immutable at runtime.
//   name  : exact binding name in the game
//   table : "base" | "mc" | "mission" | "other" | "stdlib"
//   rva   : image-base-relative address (base 0x140000000); 0 means the entry is
//           a Lua value without a function pointer and must not be called
//   arity : smallest accepted argument count, -1 when not annotated
#ifndef SW_LUA_BINDING_INFO_DEFINED
#define SW_LUA_BINDING_INFO_DEFINED
typedef struct SwLuaBindingInfo {
    const char* name;
    const char* table;
    uint32_t rva;
    int8_t arity;
} SwLuaBindingInfo;
#endif

// Mirrors the engine string layout {data,len,capacity}; 16 bytes on x64.
// Only strings created with sw_engine_string_make may be freed with
// sw_engine_string_free (the engine's aligned deallocator restores them).
typedef struct SwEngineString {
    char* data;
    uint32_t len;   // length in bytes, excluding the terminating NUL
    uint32_t cap;   // capacity (engine minimum is 32)
} SwEngineString;

// One field of the game's component_physics_record (the record filled by the
// component parser @ 0x3168d0). The table is generated from the reviewed kit
// (tools/gen_component_attrs.py) and is immutable at runtime.
//   name      : exact XML key read by the parser
//   offset    : byte offset inside the component record
//   width     : proven width in bytes (flags is 8, never assume it from type)
//   type      : SwComponentAttrType
//   isElement : true for an XML element (vector), false for an XML attribute or
//               an internal field
#ifndef SW_COMPONENT_ATTR_INFO_DEFINED
#define SW_COMPONENT_ATTR_INFO_DEFINED
typedef enum SwComponentAttrType {
    SW_ATTR_FLOAT = 0,   // f32, width 4
    SW_ATTR_INT = 1,     // i32 / enum_i32, width 4
    SW_ATTR_BOOL = 2,    // bool, width 1
    SW_ATTR_STRING = 3,  // engine string_t {char*,u32,u32}, width 16
    SW_ATTR_UINT64 = 4,  // u64, width 8 (flags)
    SW_ATTR_FLOAT3 = 5,  // fvec3, width 12
    SW_ATTR_INT3 = 6      // ivec3, width 12
} SwComponentAttrType;
typedef struct SwComponentAttrInfo {
    const char* name;       // nom XML exact
    uint32_t offset;        // offset dans la structure du composant
    uint8_t  width;         // largeur en octets
    uint8_t  type;          // SwComponentAttrType
    bool     isElement;     // attribut XML vs élément
} SwComponentAttrInfo;
#endif

typedef struct SwNativeApi {
    uint32_t structSize;   // sizeof(SwNativeApi)
    uint32_t version;      // SW_NATIVE_API_VERSION

    // Resolve a generic engine symbol by role name from the compiled discovery
    // profile. Known roles (stable identifiers, not addresses):
    //   "engine.alloc"                 engine operator new(size_t)
    //   "engine.string.reserve"        engine string reserve(EStr*, uint32_t)
    //   "engine.string.free"           engine string deallocator(void*)
    //   "engine.localized"             engine localization lookup(EStr*, int)
    //   "engine.component.lookup"      component definition lookup(registry, name)
    //   "engine.component.clear"       component registry clear(registry)
    //   "engine.component.grow"        component ring grow(ring, capacity)
    //   "engine.localization.register" localization table insert(key, value)
    // Returns NULL when the role is unknown, absent from this build, ambiguous,
    // or when its retained body contract does not match. Never guesses an RVA.
    const void* (*findSymbol)(const SwModHost*, const char* role);

    // Scan the engine's executable sections for a caller-supplied masked
    // signature (bytes[i] & mask[i]). Fails closed: SW_NOT_FOUND when absent,
    // SW_CONFLICT when more than one full body matches, SW_INVALID_ARGUMENT for
    // an empty pattern or a pattern longer than 512 bytes.
    SwResult (*findSignature)(const SwModHost*, const void* bytes, const void* mask,
                              size_t size, const void** address);

    // Bounded read/write. The whole range must be committed, readable (and for
    // writes not PAGE_GUARD/PAGE_NOACCESS). Writes to executable pages use
    // PAGE_EXECUTE_READWRITE, restore the original protection and flush the
    // instruction cache. No write happens when validation fails.
    SwResult (*readMemory)(const SwModHost*, const void* address, void* out, size_t size);
    SwResult (*writeMemory)(const SwModHost*, void* address, const void* data, size_t size);

    // Inline hook: writes an absolute jump at `target` and returns a trampoline
    // that resumes at `target + prologueBytes`. `expectedBytes` (optional but
    // strongly recommended) is the exact prologue the caller validated; a
    // mismatch rejects the hook with SW_CONFLICT before any write. prologueBytes
    // must be 5..32 and must cover whole instructions.
    SwResult (*hookInline)(const SwModHost*, void* target, void* detour,
                           const void* expectedBytes, size_t prologueBytes,
                           void** trampoline, SwHandle* handle);
    SwResult (*hookRestore)(const SwModHost*, SwHandle handle);

    // Engine allocator (operator new). Blocks returned here belong to the engine:
    // they are never released by the mod (the matching engine delete is not part
    // of the certified profile). Use sw_engine_string_* for strings.
    void* (*gameAlloc)(const SwModHost*, size_t size);
    // Engine deallocator (the reviewed string/block free). Only pass pointers the
    // engine allocated (strings and engine-owned buffers), never an
    // sw_game_alloc block.
    void (*gameFree)(const SwModHost*, void* block);

    // Engine strings {data,len,capacity} allocated by the engine allocator.
    SwResult (*stringMake)(const SwModHost*, SwEngineString* out, const char* utf8);
    void (*stringFree)(const SwModHost*, SwEngineString* string);

    // Executable scratch memory (e.g. a mod-owned thunk) allocated within +/-2GB
    // of `target` when possible, released by the loader when the owner unloads.
    void* (*codeAllocNear)(const SwModHost*, const void* target, size_t size);
    void (*codeFree)(const SwModHost*, void* block);

    // Reviewed engine build identity: 1 = first certified build, 2 = second,
    // 0 = unknown (all other primitives stay inert). Layouts that changed between
    // builds must be selected by the mod from this value, never by a hardcoded
    // address.
    uint32_t (*engineBuild)(const SwModHost*);

    // Lua binding introspection (appended; check structSize before reading).
    // The table is generated from the reviewed kit and never guesses an RVA.
    //   luaBindingCount : number of entries (0 for an invalid owner)
    //   luaBindingAt    : entry at index, NULL when out of bounds
    //   luaBindingFind  : first entry with this exact name, NULL when unknown
    // Duplicate names exist in the kit (e.g. "type" in base and stdlib); Find
    // returns the first occurrence in table order, deterministically.
    uint32_t (*luaBindingCount)(const SwModHost*);
    const SwLuaBindingInfo* (*luaBindingAt)(const SwModHost*, uint32_t index);
    const SwLuaBindingInfo* (*luaBindingFind)(const SwModHost*, const char* name);

    // Component attribute introspection (appended; check structSize before
    // reading). The table is generated from the reviewed kit and never guesses
    // an offset or a width.
    //   componentAttrCount : number of fields (0 for an invalid owner)
    //   componentAttrAt    : field at index, NULL when out of bounds
    //   componentAttrFind  : first field with this exact XML name, NULL when unknown
    //   componentAttrRead  : read a scalar numeric field into *out
    //   componentAttrWrite : write a scalar numeric field from value
    // Read/Write are fail-closed: unknown name -> SW_NOT_FOUND, NULL component or
    // unreadable/unwritable range -> SW_INVALID_ARGUMENT, and a non-scalar type
    // (string or vector) is refused with SW_INVALID_ARGUMENT instead of being
    // silently converted. The width always comes from the table.
    uint32_t (*componentAttrCount)(const SwModHost*);
    const SwComponentAttrInfo* (*componentAttrAt)(const SwModHost*, uint32_t index);
    const SwComponentAttrInfo* (*componentAttrFind)(const SwModHost*, const char* name);
    SwResult (*componentAttrRead)(const SwModHost*, const void* component, const char* name, double* out);
    SwResult (*componentAttrWrite)(const SwModHost*, void* component, const char* name, double value);
} SwNativeApi;
