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
