# StormFabric

[English](README.md) | **Français**

Chargeur de mods **C++ natifs** pour **Stormworks: Build and Rescue**, sous Windows x64.
Le proxy `iphlpapi.dll` ajoute un menu **MODS**, gère les DLLs et leurs dépendances,
et fournit une API moteur centralisée pour les plugins.

**Rendu natif du jeu, sans overlay externe ni Dear ImGui.** Les adresses, layouts
et hooks du moteur restent dans le loader : les mods utilisant le SDK n'ont pas
besoin d'adresses du jeu codées en dur.

> **Projet expérimental.** Deux exécutables exacts disposent de profils validés ;
> ce n'est pas un support automatique de toutes les versions. Les services monde,
> entités, véhicules, joueurs et ticks de simulation restent indisponibles.

---

## Sommaire

- [Fonctionnalités et état actuel](#fonctionnalités-et-état-actuel)
- [Compatibilité et sécurité](#compatibilité-et-sécurité)
- [Installation](#installation)
- [Tester les exemples](#tester-les-exemples)
- [Créer un mod](#créer-un-mod)
- [Compiler et tester](#compiler-et-tester)
- [Documentation et architecture](#documentation-et-architecture)
- [Origine du projet et assistance IA](#origine-du-projet-et-assistance-ia)
- [Mods de la communauté](#mods-de-la-communauté)
- [Licence et avertissement](#licence-et-avertissement)

---

## Fonctionnalités et état actuel

- **Menu MODS natif** : liste des mods, activation/désactivation, paramètres et logs.
- **Dépendances** : tri topologique, chargement des dépendances en premier,
  déchargement en dernier et DLLs locales via `native_deps` / `AddDllDirectory`.
- **SDK v2 / ABI moteur v1** : extensions ajoutées en fin de structure pour conserver
  les préfixes existants. Vérifier `structSize`, les pointeurs et les capacités.
- **Diagnostics** : console avec logs colorés, visionneuse intégrée, notifications
  natives, progression visible de la validation et journal `StormworksModLoader.log`.
- **Rechargement des mods** depuis le menu, avec invalidation des callbacks du SDK
  avant le déchargement de leur DLL.

| API | Fonctionnalités | État / limites |
| --- | --- | --- |
| `api->ui` | Widgets natifs historiques : texte, boutons, rectangles, cases à cocher, conteneurs et défilement | API bas niveau ; respecter les durées de vie des arbres natifs |
| `api->nativeUi` | HUD/éditeur à handles opaques : racines, fenêtres statiques, texte, boutons, visibilité et notifications | Affichage et clics dans l'éditeur vérifiés ; interaction HUD en vue FPS non établie |
| `api->runtime` | Événements UI/VM Lua, tâches différées, timers et annulation | Mises à jour UI et temps monotone, **pas** ticks/temps de simulation |
| `api->input` | Actions clavier, modificateurs et états pressed/down/released | F8 vérifié ; pas de souris/manette ni de mode curseur natif exposé |
| `api->storage` | Configuration et données binaires par mod, écritures atomiques et clés contrôlées | Jusqu'à 4 Mio par fichier ; stockage lié au monde indisponible |
| `api->lua` / `lua->extras` | Bibliothèques personnalisées, 34 fonctions QoL, chaînes, nil, entiers, tables, champs et types | VM microcontrôleurs uniquement ; `raiseError` volontairement non pris en charge |
| `api->video` | Effets sur la source vidéo des moniteurs et capture asynchrone | Expérimental ; les lectures retournent la dernière capture source terminée |
| `api->world` | Types réservés pour de futurs accès au monde | **NULL** : aucune API monde/entités/véhicules/joueurs activée |

Le socle **P0 est partiel**. Les chemins exercés par la démo ont été vérifiés en jeu :
HUD lisible, bouton dans l'éditeur, tâches/timers, stockage, événements VM et tests Lua.
F8 et la persistance de visibilité ont également été confirmés.
Le compteur `PASS` cumule les exécutions : ce n'est pas le nombre de services distincts
validés. Les vérifications d'unload, certains cas de focus et les captures/effets vidéo
restent à compléter. Voir [l'état détaillé P0](docs/p0_sdk.md).

## Compatibilité et sécurité

Le loader possède actuellement deux profils d'exécutables **Windows x64** :

| Profil | Identité SHA-256 abrégée |
| --- | --- |
| Référence Stormworks v1.15.18 | `55914301628a…` |
| Exécutable plus récent testé | `f9206d85c82f…` |

Les hashes complets et les preuves sont dans [Engine discovery](docs/engine_discovery.md).
Le numéro de version affiché ne suffit pas à identifier un exécutable compatible.

Avant toute installation de hooks, le loader authentifie le SHA-256 complet du
fichier et contrôle les témoins de code, vtables et layouts du profil sélectionné.
**Un build inconnu reste en diagnostic lecture seule : aucun hook n'est installé.**
Il n'existe pas de réglage permettant de contourner cette protection.

> Une DLL native s'exécute dans le processus du jeu : ce loader n'est **pas un sandbox**.
> N'installez que des mods de confiance, sauvegardez vos sauvegardes et fermez le jeu
> avant de remplacer le loader ou une DLL de mod sur disque.

## Installation

1. **Fermez Stormworks et son launcher.** Sauvegardez un éventuel `iphlpapi.dll`
   déjà présent ; ne remplacez pas à l'aveugle le proxy d'un autre outil.
2. Compilez le loader, ou utilisez une DLL distribuée pour un build pris en charge.
3. Copiez `build/iphlpapi.dll` à côté de `stormworks64.exe`.
4. Placez chaque mod dans son propre sous-dossier de `<Stormworks>/mods/`, avec
   son manifeste `mod.json` et ses DLLs.
5. Lancez le jeu par **Steam ou le launcher habituel** (`Launcher.exe`),
   puis ouvrez **MODS** sur l'écran d'accueil.

```text
Stormworks/
├── stormworks64.exe
├── iphlpapi.dll
├── StormworksModLoader.log
└── mods/
    ├── _modloader.ini
    └── mon_mod/
        ├── mod.json
        └── mon_mod.dll
```

Alternative de copie, depuis le dossier du projet, **jeu arrêté** :

```bat
install.bat "C:\chemin\vers\Stormworks"
```

`mods/_modloader.ini` conserve les paramètres du loader et l'état d'activation des
mods. Pour désinstaller, fermez le jeu puis retirez le `iphlpapi.dll` de ce loader
(ou restaurez votre proxy précédent).

## Tester les exemples

Les DLLs compilées sont dans `build/examples/` ; elles ne sont pas automatiquement
installées. Copiez le dossier d'exemple souhaité dans `<Stormworks>/mods/`, puis
activez-le dans **MODS**.

| Exemple | Utilité |
| --- | --- |
| [`p0_demo`](examples/p0_demo/README.md) | Panneau natif, F8, stockage et vérifications PASS/FAIL/SKIP du SDK P0 |
| [`native_api_demo`](examples/native_api_demo) | Bibliothèque Lua `sw_demo`, sans adresses moteur |
| [`mc_lua_qol`](examples/mc_lua_qol) | Fonctions QoL et extensions des microcontrôleurs via le SDK |
| [`video_fx`](docs/video_fx.md) | Effets/capture vidéo natifs et script de démonstration |
| `sw_core_lib` / `radar_telemetry` | Dépendances entre mods et DLL locale |
| `simple_dllmain_mod` | DLL Win32 sans exports SDK |

## Créer un mod

### Manifeste `mod.json`

```json
{
  "id": "mon_mod",
  "name": "Mon Super Mod",
  "author": "0k0k4",
  "version": "1.0.0",
  "description": "Description affichée dans le menu MODS.",
  "main": "mon_mod.dll",
  "dependencies": [],
  "native_deps": [],
  "enabled_by_default": true
}
```

- `dependencies` : IDs des autres mods requis.
- `native_deps` : DLLs auxiliaires locales à précharger.
- `enabled_by_default` : état initial ; un choix enregistré dans `_modloader.ini`
  peut ensuite le remplacer.

### Exemple C++ sans adresses du jeu

Incluez [`sw_mod_sdk.h`](include/sw_mod_sdk.h) et utilisez `host->api` plutôt que
`gameBase + RVA`. Voici une tâche différée sur le thread des mises à jour UI :

```cpp
#include "sw_mod_sdk.h"

static const SwModHost* g_host = nullptr;

static void OnGameThread(void*) {
    g_host->log(g_host->modId, "Tache executee via le SDK !");
}

SW_MOD_EXPORT bool SwModInit(const SwModHost* host) {
    if (!host || host->sdkVersion < 2 || !host->api ||
        host->api->abiVersion != SW_ENGINE_API_VERSION ||
        host->api->structSize < offsetof(SwEngineApi, runtime)
                               + sizeof(host->api->runtime)) return false;

    const SwRuntimeApi* runtime = host->api->runtime;
    if (!runtime || runtime->structSize < sizeof(SwRuntimeApi)) return false;

    g_host = host;
    SwHandle task = 0;
    return runtime->runOnGameThread(host, &OnGameThread, nullptr, &task) == SW_OK;
}

SW_MOD_EXPORT void SwModUnload() {
    // Le loader a déjà révoqué les callbacks et ressources associés au propriétaire.
    g_host = nullptr;
}
```

## Compiler et tester

Prérequis : **Windows x64**, compilateur **MSVC** (Visual Studio 2022 ou Build Tools), outils C++ et SDK Windows.

Depuis le dossier racine du projet, dans une invite de commandes :

```bat
build.bat
examples\build_examples.bat
tests\run_tests.bat
```

- Loader : `build/iphlpapi.dll`.
- Exemples : `build/examples/`.
- Tests : `build/tests/` ; **13 suites C++** et vérifications de compilation C.

## Documentation et architecture

| Documentation | Contenu |
| --- | --- |
| [API native](docs/native_api.md) | ABI, services et propriété des callbacks |
| [SDK P0](docs/p0_sdk.md) | État réel, limites et vérifications en jeu |
| [Engine discovery](docs/engine_discovery.md) | Profils, hashes, validation et diagnostic des builds inconnus |
| [Extensions Lua](docs/p0_lua.md) | Valeurs/tables, contexte callback et erreurs |
| [Stockage](docs/p0_storage.md) | Portées, sécurité des chemins et écritures atomiques |
| [Moniteurs vidéo](docs/video_monitors.md) / [Video FX](docs/video_fx.md) | Pipeline natif, effets et captures |
| [Notifications](docs/ingame_notifications.md) | Contexte UI et cycle de vie HUD/éditeur |
| [Audit monde](docs/p0_world_evidence.md) | Fonctions retrouvées et raisons de leur non-activation |

## Origine du projet et assistance IA

Le projet vise à compléter les possibilités de modding XML/meshes et Lua de
Stormworks par un socle C++ natif, modulaire et extensible. Il sert aussi de banc
d'essai pour le reverse engineering x64, les hooks de vtables et la programmation
système Windows assistés par IA.

Plusieurs modèles ont été utilisés au cours du développement :
- **Claude Opus 5.5** et **Claude Sonnet 5.5** : prototypage, architecture initiale, repérage de l'interface native et proxy DLL.
- **Gemini 4 Argon** : dépendances, isolation des DLLs locales, cycle de vie, interface et outillage.
- **GPT 6.1 Sol** : centralisation des bindings, adaptation aux profils récents, extensions SDK P0 et tests.

## Mods de la communauté

Vous créez un mod avec ce loader ? Présentez-le via une Issue / Pull Request pour
qu'il puisse être ajouté ici.

## Licence et avertissement

- Projet open-source sous licence **MIT**. Voir [LICENSE](LICENSE).
- **Avertissement légal** : Ce projet n'est pas affilié, sponsorisé ni validé par **Geometa**. Stormworks: Build and Rescue est une marque déposée de Geometa.
- **Sécurité** : Utilisation expérimentale. Conservez des sauvegardes et n'exécutez que des DLLs de confiance.
