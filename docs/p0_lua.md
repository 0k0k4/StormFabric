# SDK Lua P0 (microcontrôleurs uniquement)

`include/sw_sdk_lua.h` ajoute une table indépendante `SwLuaExtrasApi`. Le préfixe
existant de `SwLuaApi` ne change pas : l'intégrateur ajoute uniquement
`const SwLuaExtrasApi* extras` **après** `qol`, avec la déclaration anticipée
`typedef struct SwLuaExtrasApi SwLuaExtrasApi;` avant `SwLuaApi` et l'inclusion du
nouveau header après les définitions core `SwLuaApi` / `SwModHost`, déclare `swml::NativeLuaExtras()` dans
`src/native_lua.h`, puis affecte `g_lua.extras = NativeLuaExtras()`.
Les plugins vérifient `SwLuaApi.structSize` avant de lire `extras`, puis la taille
de cette table avant de lire un membre ajouté ultérieurement.

## Opérations réelles

Toutes renvoient `SwLuaStatus` : `pushString`, `pushNil`, `pushInteger`,
`createTable`, `getField`, `setField`, `getType`, `pop`, `copyString`.
`raiseError` existe mais renvoie **`SW_LUA_UNSUPPORTED`** : aucune erreur Lua
n'est levée au travers des frames C++ d'un plugin. Retourner zéro résultat, ou
retourner `(nil, message)` avec `pushNil` / `pushString`, est la voie sûre.

Les chaînes sont des octets Lua : UTF-8 conservé sans transcodage, NUL internes
acceptés, longueur explicite limitée à 1 Mio. `copyString` ne convertit jamais
un nombre/nil en texte et signale toute copie tronquée ; `(NULL, 0)` demande
seulement la longueur. L'ancien `argString` conserve ses conversions, mais lit
désormais la vraie longueur short/long TString et ne lit pas au-delà de 1 Mio.
`getType` distingue absence, nil, entier signé 64 bits, flottant et les autres
types Lua. `pushInteger` ne passe jamais par `double`.

Indices strictement positifs/négatifs de la pile courante ; zéro, indice absent,
registry et upvalues refusés. `pop` refuse les nombres négatifs/underflow.
`createTable(arraySlots)` alloue une table et une capacité d'array (0..65536),
pas des valeurs ; les slots sont nil. Les clés de champs font 1..255 octets,
terminés par NUL ; les tables avec métatable sont refusées pour ne pas appeler
un metamethod de plugin. `getField` pousse aussi nil pour un champ absent ;
`setField` consomme exactement la valeur supérieure en cas de succès.

Chaque opération vérifie propriétaire, état exact, thread/callback courant,
génération VM et capacité de pile **avant** l'accès natif. Les wrappers existants
`getTop/arg*/checkStack/push*/qol` rejettent aussi les appels hors callback
enregistré. `checkStack` accepte au maximum 4096 slots par demande.

## Allocations, erreurs et durée de vie

Les trois nouveaux bindings doivent être intégrés à la validation centrale.
Sinon les opérations qui allouent renvoient `UNSUPPORTED` et `registerLibrary`
refuse l'enregistrement (pas de fallback par adresse).

`luaD_rawrunprotected` encadre uniquement une feuille POD native, **jamais le
callback de plugin**. Un longjmp d'allocation saute cette feuille sans destructeur
C++ et revient dans le trampoline moteur avant de retourner au wrapper. La pile
est restaurée via un offset, y compris après déplacement du stack. Les chaînes,
copies de clés et locks sont hors de cette portée. Les nouvelles tables sont
enracinées sur la pile avant leur redimensionnement. L'installation des closures
et les deux builders de tables QoL utilisent également des feuilles protégées.
Les exceptions C++/SEH de callback et nombres de résultats invalides désactivent
le token, restaurent la pile et ne laissent pas un contexte TLS actif.

Les tokens loader ne sont jamais libérés alors qu'une closure peut les garder.
`ReleaseNativeLuaOwner` attend le callback en cours via un mutex puis efface
owner/callback/userData. Recharger un plugin à la même adresse ne réactive pas
ses anciens tokens. Les init/destroy VM retirent les anciennes générations.
**L'intégrateur doit différer un unload demandé depuis le callback lui-même** :
un mutex récursif ne peut pas empêcher `FreeLibrary` du code en cours. Aucune
garantie de sandbox pour une DLL native malveillante ; les pointeurs de buffer
plugin doivent être valides selon le contrat C habituel.

## Pont événements VM pour l'intégrateur parent

Intégration réalisée : les hooks init/reinit/destroy envoient la génération opaque
à `QueueModLuaEvent`. Le runtime livre ces notifications sur la prochaine mise à
jour UI ; aucun pointeur Lua n'est exposé et aucun accès pile n'est accordé. Les
tests couvrent l'ordre destroy/create lors d'une réinitialisation et le chemin
QoL désactivé. Les trois contrats et témoins callee sont fusionnés par
`gen_engine_bindings.py` ; `lua->extras` est branché dans `engine_api.cpp`.

Pas de nouveau symbole de dispatcher imposé : l'implémentation parent possède
son dispatcher stable. Points d'intégration exacts dans `src/native_lua.cpp` :

* `HkMcLuaInit` : invalider les anciens enregistrements avant `g_origMcLuaInit` ;
  nouveau `L = *(uintptr_t*)(self+8)` enregistré avec une génération après
  l'init originale. `ApplyRegisteredLibraries(self)` vient ensuite.
  Émettre « microcontroller VM created » après l'installation des bibliothèques
  et du QoL : traiter **aussi le chemin `!g_enabled`** (qui retourne tôt).
* `HkMcLuaDestroy` : émettre « microcontroller VM destroying » sur le thread du
  hook **avant** `g_origMcLuaDestroy(self)` ; conserver le L avant cet appel.
  La map de générations doit être invalidée avant d'autoriser la destruction.
* Ces événements n'accordent pas eux-mêmes de droit d'accès à la pile : seules
  les closures enregistrées activent le contexte SDK Lua. L'événement peut
  fournir un identifiant opaque/génération, pas une promesse de pointeur durable.

Les VM addon ne sont ni découvertes ni instrumentées ; ne pas annoncer leur
support. Les bibliothèques sont appliquées aux **nouvelles** VM seulement.

## Preuve statique / intégration des mappings

REA indisponible pour cette sous-tâche ; export direct du projet Ghidra retenu,
`-readOnly -noanalysis`, et comparaison PE sans exécution :

| clé privée | fonction | RVA référence | RVA nouveau |
|---|---|---|---|
| `0x097e90` | `lua_pushlstring(L, bytes, length)` | `0x097e90` | `0x097e90` |
| `0x098130` | `getfield(L, TValue*, key)` | `0x098130` | `0x098130` |
| `0x0a26f0` | `luaD_rawrunprotected(L, leaf, data)` | `0x0a26f0` | `0x0a26f0` |

`tools/p0_lua_binding_evidence.json` contient les deux SHA-256 exécutables,
les RVAs, les corps complets/masks/rel32, les hashes d'instructions pour chaque
version et les hashes des exports Ghidra. `tools/p0_lua_contracts.json` contient
les trois contrats et sept témoins callee. Aucun mapping central généré modifié
par cette sous-tâche ; le parent doit les fusionner puis régénérer/valider.
`tools/review_p0_lua.py` reproduit la comparaison offline pour les chemins des
deux PE indiqués dans ce dépôt.

Observé statiquement : lecture longueur TString +0xb / +0x10, caractères +0x18 ;
trampoline protège/restaure `L+0x58` et `L+0xc6`, invoque `leaf(L,data)`, puis
retourne le statut ; `luaD_throw` utilise ce jump buffer. Les getters/setters
utilisent un **TValue*** (pas un index entier) ; métatable Table à +0x28.
Les tests ne prouvent pas l'exécution du jeu : comportement runtime réel,
réentrance d'éventuels finalizers moteur et événements addon restent inconnus.

## Tests sans jeu

Compiler avec MSVC x64 et un environnement VS initialisé (pas de modification
des scripts/builds globaux) :

```bat
cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /DNOMINMAX /DWIN32_LEAN_AND_MEAN tests\p0_lua_backend_tests.cpp /Fe:p0_lua_backend_tests.exe
p0_lua_backend_tests.exe
cl /nologo /TC /std:c11 /W4 /c tests\p0_lua_c_smoke.c
python tools\review_p0_lua.py
```

Le test inclut le **vrai** `native_lua.cpp`, remplace uniquement les natives et
fonctions de validation par des fakes, et exerce : mauvais owner/state/thread,
bornes/underflow, entier 64 bits, types/nil distincts, short/long strings
binaires, troncature, champs/table, métatable rejetée, stack déplacé, vrai
setjmp/longjmp fake pour OOM (avant toute frame fake C++ possédant un destructeur),
TLS restauré, résultat/exception callback invalide, token périmé et unload
concurrent. MSVC C4611 est désactivé localement pour le setjmp intentionnel du
fake POD de test ; pas d'utilisation de setjmp dans le code SDK C++ de production.
