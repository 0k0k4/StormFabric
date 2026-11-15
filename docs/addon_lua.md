# Lua des addons : `lua->registerAddonLibrary`

Un mod natif peut installer une bibliothèque dans les **VM des addons/missions**
(`script.lua`), là où le script dispose de l'API du jeu (`server.*`, `matrix.*`).
C'est le pont entre un mod C++ et le gameplay : le script Lua appelle le jeu, et
appelle le mod par une table globale.

> **Build récent uniquement.** La capacité est certifiée pour l'exécutable
> v1.15.23 (SHA-256 `f9206d85…`). Sur tout autre build, y compris l'ancien build
> de référence, elle est absente : `SW_CAP_LUA_ADDON` vaut 0, l'enregistrement
> renvoie `false`, et le Lua des microcontrôleurs n'est pas affecté.
>
> **Vérifié en jeu le 2026-11-15** (build récent, lancement par `Launcher.exe`) :
> profil validé et hooks posés au démarrage, 17 VM créées au chargement d'un
> monde puis détruites à sa fermeture, et un addon de test a appelé la
> bibliothèque native depuis `onCreate` (`ping -> pong #1.0` dans le log du
> loader) ; la commande `?sf` a répondu dans le chat. Non vérifié : déchargement
> du mod en cours de partie, `?reload_scripts`, multijoueur.

## Utilisation

```cpp
static const SwModHost* g_host;

static int Ping(void* L, void*) {
    const SwLuaApi* lua = g_host->api->lua;
    if (!lua->checkStack(L, 1)) return 0;
    lua->pushNumber(L, 42);
    return 1;
}

// Dans SwModInit, après les vérifications de version :
const SwLuaApi* lua = host->api->lua;
if (lua->structSize < offsetof(SwLuaApi, registerAddonLibrary) + sizeof(lua->registerAddonLibrary) ||
    !(host->api->runtime->capabilities() & SW_CAP_LUA_ADDON)) return false;
g_host = host;
SwLuaFunction funcs[] = {{"ping", &Ping, nullptr}};
bool ok = lua->registerAddonLibrary(host, "my_mod", funcs, 1);
```

Puis dans le `script.lua` d'un addon :

```lua
function onCreate(is_world_create)
    if my_mod then server.announce("my_mod", tostring(my_mod.ping())) end
end
```

Le test `if my_mod then` garde l'addon utilisable sans StormFabric. Exemple
complet : [`examples/addon_lua_demo`](../examples/addon_lua_demo).

## Contrat

- Même contrat que `registerLibrary` : noms et callbacks copiés, 1 à 256
  fonctions, identifiants `[A-Za-z0-9_]` de 63 caractères au plus, nom unique
  parmi les bibliothèques addon actives. Les deux portées sont indépendantes :
  une bibliothèque addon n'apparaît pas dans les microcontrôleurs, et inversement.
- La bibliothèque est installée dans les **nouvelles** VM, après les tables du jeu
  et avant le chargement du script : elle est visible dès la première ligne.
  Une VM déjà créée n'est pas modifiée ; recharger les scripts la recrée.
- Noms refusés : les tables du jeu (`server`, `matrix`, `property`, `debug`,
  `async`, `g_savedata`), les bibliothèques standard, les fonctions de base
  (`pairs`, `print`, `pcall`…), `httpReply`, et tout nom de la forme `onXxx`
  (le jeu cherche ces globales comme callbacks).
- Dans un callback, l'API de pile habituelle fonctionne : `getTop`, `arg*`,
  `checkStack`, `push*` et `lua->extras`. Les fonctions `lua->qol` et l'API vidéo
  sont propres aux microcontrôleurs : appelées depuis une VM addon, elles ne font
  rien et renvoient zéro résultat.
- Un callback natif peut appeler lui-même les fonctions du jeu avec
  `extras->getGlobal` et `extras->call` (voir plus bas). Il reste lié à un appel
  venu du script : hors callback, ces fonctions renvoient `INVALID_CONTEXT`.
- Déchargement : comme pour les microcontrôleurs, les closures pointent vers le
  loader ; après le déchargement du mod elles renvoient zéro résultat.

## Appeler le jeu depuis le C++

Deux membres ajoutés en fin de `SwLuaExtrasApi` (vérifier `structSize`) :

- `getGlobal(owner, L, name)` pousse la globale `name` (nil si absente). Refusé
  (`SW_LUA_UNSUPPORTED`) si `_G` porte une métatable.
- `call(owner, L, nargs, nresults, errorOut, errorCapacity)` est un appel protégé.
  La pile doit contenir la fonction puis ses `nargs` arguments ; tout est consommé.
  `SW_LUA_OK` laisse exactement `nresults` résultats. `SW_LUA_CALL_FAILED` ne laisse
  rien et copie le message d'erreur s'il s'agit d'une chaîne. Les deux compteurs
  vont de 0 à 255. Une erreur Lua ne traverse jamais le code du mod.

```cpp
// Dans un callback : server.announce("Mon mod", "bonjour")
const SwLuaExtrasApi* x = lua->extras;
char error[256];
if (lua->checkStack(L, 6) && x->getGlobal(host, L, "server") == SW_LUA_OK) {
    if (x->getField(host, L, -1, "announce") == SW_LUA_OK) {
        x->pushString(host, L, "Mon mod", 7);
        x->pushString(host, L, "bonjour", 7);
        if (x->call(host, L, 2, 0, error, sizeof(error)) != SW_LUA_OK) host->log(host->modId, error);
    }
    x->pop(host, L, 1); // server
}
```

`call` s'appuie sur `lua_pcallk` (`0x989a0`, 516 octets avec ses blocs de fin),
identifié comme la cible de l'appel fait par le `pcall` de la bibliothèque de base
et vérifié octet par octet au démarrage. Build récent uniquement : ailleurs, `call`
renvoie `SW_LUA_UNSUPPORTED`. Il fonctionne aussi dans une VM de microcontrôleur.

Vérifié en jeu le 2026-11-15 : depuis la commande `?sf`, le mod de démo a appelé
`server.announce` et lu le résultat de `server.getPlayerName(0)`. Non vérifié en
jeu : le chemin d'erreur (fonction Lua qui lève une erreur), couvert seulement par
les tests hors jeu.

## Événements

`SW_EVENT_ADDON_LUA_CREATED` et `SW_EVENT_ADDON_LUA_DESTROYED` s'abonnent par
`runtime->subscribe`. Ils portent une génération opaque dans `handle`, sont livrés
à la prochaine mise à jour UI et ne donnent aucun accès à la pile. Un rechargement
de script détruit puis recrée la VM : **ce ne sont pas des événements de
chargement ou de fermeture du monde**. Sans la capacité, l'abonnement renvoie
`SW_UNAVAILABLE`.

## Preuve statique

| Élément | RVA (build récent) | Rôle |
|---|---|---|
| vtable `c_playlist_lua_script` | `0xb11290` | identifiée par RTTI exact, occurrence unique |
| slot 1 | `0x1e1e80` (48 octets) | ferme l'état Lua stocké en `self+8`, puis y écrit zéro |
| slot 3 | `0x599990` (6374 octets) | lit l'état en `self+8`, enregistre les tables du jeu |

Le slot 3 appelle quatre fois l'assistant d'enregistrement de table déjà certifié
(`0x1e22d0`). Le jeu appelle slot 1, crée l'état, puis slot 3, puis charge le
script (`0x5d0660`). C'est le même protocole que la classe des microcontrôleurs.

`tools/gen_addon_lua_profile.py` refuse tout exécutable dont le SHA-256 diffère,
vérifie ces témoins et écrit `src/addon_lua_profile.generated.h` (ne pas éditer à
la main ; le script est idempotent). Au démarrage, le loader exige le build
récent, les corps complets des deux méthodes et des slots de vtable intacts ;
sinon aucun hook addon n'est posé.

```bat
set STORMWORKS_NEWER_EXE=C:\chemin\vers\stormworks64.exe
python tools\gen_addon_lua_profile.py
```

## Limites

- Seule la classe `c_playlist_lua_script` est instrumentée. Si le jeu utilise une
  autre classe pour certains scripts, ils ne reçoivent pas les bibliothèques.
- Aucun contrôle multijoueur : un client et un serveur avec des mods différents
  exécutent des scripts aux globales différentes.
- Le serveur dédié (`server64.exe`) n'est pas pris en charge.
