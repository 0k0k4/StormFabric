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
    SW_CAP_LUA_EVENTS=1ull<<11
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
