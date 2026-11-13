# API moteur native — SDK v2, ABI v1

Les mods incluent uniquement `include/sw_mod_sdk.h` et utilisent `host->api`.
Les RVA, offsets, allocations du CRT du jeu, vtables et appels internes sont désormais
la responsabilité du **loader**, pas des plugins. `mc_lua_qol` a été migré vers cette API.
`gameBase` reste présent pour la compatibilité avec les anciens plugins, mais son utilisation
directe n'est pas couverte par l'API stable.

## Compatibilité réelle

- Les champs SDK v1 conservent leurs offsets ; `api` est ajouté à la fin de `SwModHost`.
- Vérifier `sdkVersion >= 2`, `api`, `abiVersion == SW_ENGINE_API_VERSION` et les capacités
  nécessaires (`api->lua` peut être nul). Les tables ont un `structSize` pour leur extension.
- **Cela ne rend pas le loader universel après une mise à jour.** Le profil demeure celui de
  Stormworks v1.15.18 x64. Un portage doit adapter et valider la couche moteur centralisée ;
  les plugins utilisant exclusivement l'API n'ont alors plus de RVA à corriger.
- UI : contrôles de build, signatures et vtables dans `src/game.cpp`.
- Lua : 58 empreintes de 32 octets, masquant les relocations ASLR, plus les slots de vtable,
  dans `src/engine_profile.cpp` et `src/lua_profile.generated.h`.
- La génération des empreintes refuse un SHA-256 différent de l'exécutable de référence :
  `python tools/gen_lua_profile.py "chemin\stormworks64.exe"`.
- Aucun scan de signature ne prétend rendre automatiquement compatible un build inconnu.
- Vidéo : extension optionnelle `api->video`, préfixe ABI préservé ; vérifier `api->structSize`
  avant d'accéder au champ. Profil, API et limites : [`video_fx.md`](video_fx.md).

## UI : `host->api->ui`

Widgets disponibles : composite, texte, rectangle, bouton rond, bouton rectangulaire,
checkbox, heading et scroll container. Opérations : ajouter/retirer un enfant, ajouter un
item défilant, texte/localisation, visibilité, alpha, activation, état coché, position/ancrage.

Le loader alloue les widgets et chaînes avec **les allocateurs du jeu**. Les chaînes passées
à l'API sont copiées ; le plugin n'a pas à connaître `EStr` ni la taille des widgets.
Le parent peut être nul pour construire un item avant `scrollAdd`/`addChild`.

Exemple, dans une vue native dont le plugin maîtrise le cycle de vie :

```cpp
void OnClick(void* context, void* userData) {
    // Les mutations complexes peuvent être différées au prochain tick de la vue.
}

SwUiHandle button = host->api->ui->button(
    host, context, parent, 0.0f, -0.2f, 0,
    "cube_wrench", "Mon bouton", 0.14f, OnClick, nullptr);
host->api->ui->setAlpha(context, button, 255);
```

- Coordonnées natives : hauteur écran = 2 unités ; Y positif vers le haut.
- Ancres/alignements : grille 3×3, indices 0..8. Couleurs : ABGR.
- Les widgets appartiennent à l'arbre du jeu une fois attachés. Ne pas appeler `free`/`delete`.
- Les handles/contextes sont empruntés : ils deviennent invalides lorsque la vue est détruite.
  Le plugin doit oublier ses handles lors d'un changement de contexte ; il ne faut jamais
  réutiliser un handle d'une ancienne interface.
- Les callbacks des boutons sont des relais loader-owned. Avant le déchargement du plugin,
  leurs tokens sont invalidés : une ancienne vue ne peut plus appeler la DLL libérée.
  Les widgets ne sont **pas automatiquement supprimés** : les retirer explicitement tant que
  leur arbre est valide, ou laisser le jeu détruire l'arbre.
- Appeler l'API UI sur le thread du jeu, hors `DllMain`. Les fonctions supposent des handles
  du type approprié (`setText` pour du texte, `setChecked` pour une checkbox…).
- Les IDs de localisation doivent être des IDs connus du jeu ; pas des entiers arbitraires.

## Lua : `host->api->lua`

### Fonctionnalités existantes

`enableQol(host)` active le service historique : lectures/écritures composite groupées,
pulse/toggle, copie/clear, texte aligné, arcs/jauges/polygones/boutons tactiles,
couleurs Hex/HSV, maths, lissage, PID, debug/log/notifications, extension de la limite
de scripts, timeout et documentation dans l'éditeur.

Le service est partagé : `disableQol(host)` ne le désactive pas pour les autres propriétaires.
La libération du propriétaire est automatique avant le déchargement, y compris si
`SwModInit` échoue après avoir utilisé l'API.

`lua->qol` expose également **chacune** des 34 fonctions existantes comme une fonction C
`int(void* luaState)`, pour les réutiliser depuis un callback Lua natif sans manipuler
les structures input/screen du moteur. Les arguments/résultats restent ceux de la fonction Lua
correspondante, décrits dans le Help QoL. Les accès input/output exigent `onTick`, les accès
screen exigent `onDraw` comme auparavant.

### Ajouter une bibliothèque Lua

`registerLibrary(host, name, functions, count)` copie les noms et enregistre des callbacks.
La bibliothèque est installée dans les **nouvelles VM de microcontrôleurs**. Les tables natives
`input`, `output`, `screen`, `math`, `debug`, `qol`, `_G`, `string` et `table` sont réservées.
Le nom d'une bibliothèque active est unique. Aucun RVA n'est nécessaire :

```cpp
static const SwModHost* g_host;

int Sum(void* L, void*) {
    const SwLuaApi* lua = g_host->api->lua;
    double value = lua->argNumber(L, 1, 0) + lua->argNumber(L, 2, 0);
    if (!lua->checkStack(L, 1)) return 0;
    lua->pushNumber(L, value);
    return 1;
}

// Dans SwModInit, après les vérifications de version/capacité :
g_host = host;
SwLuaFunction funcs[] = {{"sum", Sum, nullptr}};
bool ok = host->api->lua->registerLibrary(host, "my_mod", funcs, 1);
// Puis en Lua : value = my_mod.sum(10, 20)
```

API de pile : `getTop`, `argNumber`, `argBool`, `argString`, `checkStack`, `pushNumber`,
`pushBool`. Pour les chaînes, le plugin fournit son buffer ; la valeur retournée est la
longueur complète, hors NUL, même si la copie est tronquée.

Exemple compilable : `examples/native_api_demo`, désactivé par défaut, enregistre
`sw_demo.sum(...)` et `sw_demo.clamp(x,min,max)` sans activer les autres changements QoL.

### Déchargement et limites

Les closures Lua pointent vers le loader, avec un token propre à chaque enregistrement.
Avant `FreeLibrary`, le loader attend la fin d'un callback Lua en cours puis invalide les
callbacks du propriétaire. Les anciennes closures retournent ensuite zéro résultat, même
si un nouveau plugin réutilise la même adresse d'hôte lors d'un hot-reload.

Les hooks et tokens restent dans le loader pendant la vie du processus : ils peuvent être
encore référencés par des VM/vues. Après désactivation, les VM existantes conservent leurs
extensions loader-owned jusqu'à leur recréation ; les nouvelles VM sont sans QoL. Les
callbacks personnalisés ne sont pas remplacés à chaud dans les VM déjà créées. Les anciens
compteurs de hook CPU/limites d'un éditeur déjà ouvert ne sont pas instantanément restaurés.

Les appels `SwModTick` restent actuellement liés à l'update du **menu principal**, pas à
un tick global en partie. Les callbacks Lua fonctionnent via leurs propres hooks moteur.

Les notifications disposent aussi de hooks UI natifs du HUD et de l'éditeur de
microcontrôleurs : voir [`ingame_notifications.md`](ingame_notifications.md).

Les DLL restent du code natif non isolé : cette API n'est pas une sandbox. Les contrôles SEH
ne garantissent pas la récupération après une corruption mémoire ou une erreur fatale Lua.

## Vérifications effectuées pour cette migration

- Compilation x64 du loader et de tous les exemples.
- Compilation du SDK depuis C et C++.
- Tests avec moteur simulé : préfixe ABI v1, validation propriétaire, callbacks UI inertes
  après libération, réutilisation d'adresse lors du hot-reload, référence partagée QoL et
  rejet d'un profil Lua invalide. Lancer `tests\run_tests.bat`.
- Validation du profil sur une copie mémoire du PE de référence avec relocations ASLR
  simulées (sans exécuter le jeu) : rejet d'une fonction modifiée, d'un slot de vtable
  modifié et d'un horodatage de build différent.
- **Pas de test en jeu de cette migration** : vérifier le menu, les widgets et les callbacks
  Lua/custom, le Help et le hot-reload avant de considérer l'intégration validée au runtime.

## Primitives natives : `host->api->native` (extension ajoutée)

`SwNativeApi` est **ajoutée à la fin** de `SwEngineApi` (`docs/native_api.md`).
`abiVersion` reste **1** : un mod compilé avec l'ancien en-tête continue de fonctionner,
et un mod récent doit vérifier la présence du champ avant de l'utiliser :

```cpp
if (host->api->structSize < offsetof(SwEngineApi, native) + sizeof(host->api->native) ||
    !host->api->native ||
    host->api->native->structSize < sizeof(SwNativeApi) ||
    host->api->native->version != SW_NATIVE_API_VERSION) return false;
```

Ces primitives sont **génériques** : le loader ne contient aucune logique de mod. Chaque
adresse vient de son profil de découverte compilé (table de bindings revue + contrats
d'octets retenus) et est revalidée avant d'être rendue. **Fail-closed** : un contrat
absent, ambigu ou modifié renvoie `NULL` / `SW_NOT_FOUND` / `SW_CONFLICT` et **aucune
écriture n'est effectuée**.

### Signatures C exactes

```c
typedef struct SwEngineString { char* data; uint32_t len; uint32_t cap; } SwEngineString;

typedef struct SwLuaBindingInfo {
    const char* name;   /* nom exact du binding dans le jeu */
    const char* table;  /* "base" | "mc" | "mission" | "other" | "stdlib" */
    uint32_t rva;       /* RVA (base 0x140000000), 0 = valeur Lua sans fonction */
    int8_t arity;       /* plus petit nombre d'arguments accepté, -1 = inconnu */
} SwLuaBindingInfo;

typedef struct SwNativeApi {
    uint32_t structSize;   /* sizeof(SwNativeApi) */
    uint32_t version;      /* SW_NATIVE_API_VERSION */

    const void* (*findSymbol)(const SwModHost*, const char* role);
    SwResult (*findSignature)(const SwModHost*, const void* bytes, const void* mask,
                              size_t size, const void** address);
    SwResult (*readMemory)(const SwModHost*, const void* address, void* out, size_t size);
    SwResult (*writeMemory)(const SwModHost*, void* address, const void* data, size_t size);
    SwResult (*hookInline)(const SwModHost*, void* target, void* detour,
                           const void* expectedBytes, size_t prologueBytes,
                           void** trampoline, SwHandle* handle);
    SwResult (*hookRestore)(const SwModHost*, SwHandle handle);
    void* (*gameAlloc)(const SwModHost*, size_t size);
    void  (*gameFree)(const SwModHost*, void* block);
    SwResult (*stringMake)(const SwModHost*, SwEngineString* out, const char* utf8);
    void  (*stringFree)(const SwModHost*, SwEngineString* string);
    void* (*codeAllocNear)(const SwModHost*, const void* target, size_t size);
    void  (*codeFree)(const SwModHost*, void* block);
    uint32_t (*engineBuild)(const SwModHost*);

    /* Ajouté : introspection des bindings Lua (vérifier structSize). */
    uint32_t (*luaBindingCount)(const SwModHost*);
    const SwLuaBindingInfo* (*luaBindingAt)(const SwModHost*, uint32_t index);
    const SwLuaBindingInfo* (*luaBindingFind)(const SwModHost*, const char* name);
} SwNativeApi;
```

### Rôles résolus par `findSymbol` (version-gardés)

| Rôle | Contrat |
|---|---|
| `engine.alloc` | `operator new(size_t)` du jeu (binding revu) |
| `engine.string.reserve` | réserve de chaîne moteur `(EStr*, uint32_t)` |
| `engine.string.free` | désallocateur de chaîne/bloc moteur |
| `engine.localized` | table de localisation `(EStr*, int)` |
| `engine.component.lookup` | lookup de définition de composant (contrat d'octets) |
| `engine.component.clear` | `ComponentRegistry::Clear` (contrat d'octets) |
| `engine.component.grow` | croissance d'un ring de registre (contrat d'octets) |
| `engine.localization.register` | insertion dans la table de localisation (contrat d'octets) |

Un rôle inconnu, absent du build, ambigu (plusieurs corps identiques) ou dont les octets
retenus ne correspondent plus renvoie `NULL`. Aucune RVA n'est devinée.

`engineBuild` renvoie `1`/`2` pour les deux builds d'exécutable revus, `0` sinon (toutes
les autres primitives restent alors inertes). Les dispositions qui ont changé entre les
deux builds (par ex. la taille d'une structure) doivent être sélectionnées par le mod à
partir de cette valeur.

`findSignature` scanne les sections exécutables de l'image pour `bytes[i] & mask[i]`
(motif 8..512 octets, masque strictement `0x00`/`0xFF`) via `discovery::Find` :
`SW_OK` + adresse seulement sur une occurrence **unique**, `SW_NOT_FOUND` si absente,
`SW_CONFLICT` si ambiguë. C'est le mécanisme à utiliser pour un site propre à un mod.

### Introspection des bindings Lua

La table `kLuaBindings` est **générée à la compilation** depuis le kit de rétro-ingénierie
(`stormworks-modding-kit/lua-api/lua_bindings.json` + `annotated-bindings/`). Elle ne
contient aucune adresse devinée et ne dépend pas de l'image chargée : ces trois primitives
répondent même quand `engineBuild` vaut `0`. Elles servent à un mod à résoudre et valider
ses propres appels (nom → RVA → arité), pas à appeler les fonctions du jeu.

```c
uint32_t (*luaBindingCount)(const SwModHost*);
const SwLuaBindingInfo* (*luaBindingAt)(const SwModHost*, uint32_t index);
const SwLuaBindingInfo* (*luaBindingFind)(const SwModHost*, const char* name);
```

- `luaBindingCount(host)` : nombre d'entrées de la table (427 pour le kit actuel).
  Renvoie `0` si `host` est invalide (fail-closed).
- `luaBindingAt(host, index)` : pointeur vers l'entrée `index`, ou `NULL` si `index`
  est hors bornes (`index >= luaBindingCount`) ou si `host` est invalide. L'index `0`
  est valide. Le pointeur appartient au loader et reste valide pendant toute la vie
  du processus ; ne pas le libérer.
- `luaBindingFind(host, name)` : première entrée dont le `name` est **exactement**
  égal (comparaison sensible à la casse), ou `NULL` si `name` est `NULL`, vide,
  inconnu, ou si `host` est invalide. Les doublons du kit sont résolus de façon
  déterministe : c'est la première occurrence dans l'ordre de la table qui gagne.

Sémantique des champs :

- `rva` est relative à l'image (base `0x140000000`). `rva == 0` marque une entrée
  **valeur Lua** sans pointeur de fonction (`_G`, `_VERSION`, `pi`, `huge`, …) :
  elle ne doit pas être appelée.
- `arity` est le **plus petit** nombre d'arguments accepté. Le kit annote parfois
  plusieurs arités (`1 | 4`, `13 | 17`) ; une seule valeur `int8_t` ne peut pas
  représenter un ensemble, donc la table stocke le minimum. `-1` signifie « non
  annoté » (ou `variable`) : ne pas en déduire une arité.

**Incohérence connue du kit, à ne pas masquer** : les 427 entrées ne portent que
391 noms distincts. 34 noms sont dupliqués (33 deux fois, `type` quatre fois),
par exemple `ipairs`/`next`/`pairs` dans `base`, `multiply` dans `mission`, ou
`type` dans `base` et `stdlib`. La table générée conserve **toutes** les entrées
(le total doit rester 427) ; `luaBindingFind` renvoie la première. Un mod qui a
besoin de toutes les occurrences doit parcourir `luaBindingAt`.

Régénération de la table (le kit est en lecture seule) :

```bat
python tools\gen_lua_bindings.py
```

Le script est **idempotent** : relancé, il produit un fichier identique (même
SHA-256). Il écrit aussi les `static_assert` de contrôle (total, comptage par
table, nombre d'entrées typées) ; une modification du kit qui ne correspondrait
plus aux constantes casse la compilation au lieu de passer inaperçue. Ne jamais
éditer `src/lua_bindings.generated.h` à la main.

### Sécurité des primitives

- **Lecture/écriture** : la plage entière doit être `MEM_COMMIT`, sans `PAGE_GUARD` ni
  `PAGE_NOACCESS` (taille bornée à 256 Mo). Une écriture sur page exécutable utilise
  `PAGE_EXECUTE_READWRITE`, **restaure la protection d'origine**, puis
  `FlushInstructionCache`. Échec de validation ⇒ `SW_INVALID_ARGUMENT`, **aucune écriture**.
- **Hook inline** : `target` doit être une page exécutable validée, `prologueBytes` ∈ [5, 32]
  et couvrir des instructions entières. `expectedBytes` (recommandé) est comparé octet par
  octet ; toute différence ⇒ `SW_CONFLICT` **avant** écriture. Le loader construit un
  trampoline (prologue copié + saut absolu vers `target + prologueBytes`) qui reste sa
  propriété : ne pas le libérer, ne pas le modifier. La publication du saut est ordonnée
  (queue d'instruction d'abord, opcode en dernier).
- **Restauration** : `hookRestore` réécrit le prologue d'origine (protection restaurée,
  cache d'instruction vidé) ; l'appel est **idempotent** (`SW_OK` même si déjà restauré).
  Les hooks d'un mod sont automatiquement restaurés par le loader **avant**
  `FreeLibrary` du plugin. L'ordre de démontage est : hooks du mod d'abord, puis le
  loader (`DLL_PROCESS_DETACH`), en ordre inverse d'installation.
- **Mémoire** : `gameAlloc` = `operator new` du jeu. Le bloc appartient au moteur : un mod
  ne le libère pas (le `operator delete` correspondant n'est pas certifié). `gameFree`
  appelle le désallocateur de chaîne/bloc revu : ne lui passer que des pointeurs alloués
  par le moteur. Pour les chaînes, utiliser `stringMake` / `stringFree` (le moteur les
  libère lui-même dans ses destructeurs).
- **Isolation** : les handles sont des identités du loader, jamais des pointeurs. Un handle
  d'un autre propriétaire est refusé (`SW_INVALID_OWNER`) et un handle inconnu renvoie
  `SW_NOT_FOUND`. `codeAllocNear` alloue dans ±2 Go de la cible (utile à un `jmp rel32`) et
  libère automatiquement les blocs du mod à son déchargement.
