# Video FX — mod natif expérimental

## État et installation

Implémentation compilée x64 et tests **hors jeu** réussis. **Rendu et captures non encore
validés en partie.** Compatible avec le profil Stormworks **v1.15.18 x64** uniquement.
Le profil refuse un build inconnu ou des points de hook déjà modifiés.

- Loader : `Stormworks/iphlpapi.dll`, produit par `build.bat`.
- Mod : `Stormworks/mods/video_fx/video_fx.dll` et `mod.json`.
- Exemple : `examples/video_fx/demo.lua`, également copié dans le dossier du mod.
- Construction : `examples/build_examples.bat`, puis copier `build/examples/video_fx/`
  dans `<Stormworks_Root>/mods/video_fx/`. Ne pas remplacer les DLL pendant que le jeu tourne.

Activer **Native Video FX (experimental)** dans MODS avant de faire apparaître le véhicule.
La table `video` est ajoutée aux **nouvelles VM Lua** : après activation/rechargement du
plugin, refaire apparaître le véhicule/recréer les microcontrôleurs.
Brancher **caméra → bloc Lua → moniteur de véhicule alimenté**.

### Rejet au chargement avec l'ancien QoL

Un premier lancement a révélé que la DLL installée `mods/mc_lua_qol/mc_lua_qol.dll`
était encore la version autonome à hooks internes. Chargée avant `video_fx`, elle
modifie les vtables Lua : le service central refuse alors son démarrage. Utiliser la
version migrée produite par `examples/build_examples.bat` (message de chargement
`Lua QoL enabled through the loader API`), puis **redémarrer complètement le jeu**.
Ne pas simplement recharger le plugin dans un processus où les anciens hooks existent.
Le loader et le plugin journalisent désormais chaque étape de refus d'initialisation.

## Bibliothèque Lua `video`

Toutes les fonctions exigent `onDraw` d'un **moniteur de véhicule**. `onTick`, les écrans
HUD et la prévisualisation d'éditeur non couverte par le hook ne sont pas supportés.
Les setters retournent `true` si leur demande est acceptée, pas une garantie que le GPU
a rendu l'effet ; `false` indique un refus (phase, propriétaire, valeur invalide).

| Fonction | Sémantique |
|---|---|
| `video.setOffset(x, y)` | Translation en pixels logiques ; X droite, Y bas. Défaut `(0,0)`, bornes ±65536. |
| `video.setScale(x [, y])` | Échelle autour du centre ; Y vaut X par défaut. Défaut `(1,1)`, facteurs 0,01–100. |
| `video.setTint(r,g,b [,a])` | Multiplicateurs RGBA 0–255 ; défaut 255. |
| `video.setFilter(grayscale, invert)` | Intensités 0–1 : niveaux de gris, puis inversion. Défaut `(0,0)`. |
| `video.reset()` | Réinitialise les effets du passage courant. |
| `video.requestCapture([enabled])` | Demande une capture source pour ce passage, défaut `true` ; `false` annule la demande. |
| `video.getPixel(x,y)` | Retourne `r,g,b,a,ready,sequence`, couleurs 0–255 ; demande aussi une capture. |

Les effets sont **réinitialisés à chaque passage du moniteur** : les rappeler chaque
`onDraw` pour les conserver. Une erreur native de dessin annule les effets du passage.
La translation suit l'échelle ; les zones hors vidéo sont noires. Les dessins `screen.*`,
le tactile et ses coordonnées restent inchangés. L'effet n'altère pas le signal logique
transmis entre microcontrôleurs ; il agit au moment de l'affichage du moniteur.

Un script connecté à deux moniteurs dispose de deux passages/captures indépendants.
Dans une chaîne de scripts sur un moniteur, les setters modifient **l'état partagé du
passage** : le dernier setter d'une propriété gagne, sans pile de filtres par script.

```lua
function onDraw()
    video.setOffset(4, 0)
    video.setScale(1.25)
    video.setTint(160, 255, 160)
    video.setFilter(0.5, 0)
    local r,g,b,a,ready,seq = video.getPixel(16,16)
    screen.setColor(255,255,255)
    screen.drawText(1,1,ready and ("R=" .. r) or "Capture pending")
end
```

### Lecture de pixels

La capture lit la **texture vidéo native avant effets et avant overlay Lua**, pas la
couleur finale du moniteur 3D ni la fenêtre du jeu. Coordonnées logiques de
`screen.getWidth()/getHeight()`, origine supérieure gauche, bornes `[0,width-1]`,
`[0,height-1]`. Les fractions positives sont tronquées. Pour une cible haute résolution,
le texel correspondant au coin du pixel logique est sélectionné, sans moyenne/interpolation.

Plusieurs `getPixel` lisent la même copie CPU de l'image entière, sans appel GPU par pixel.
Continuer à appeler `getPixel`/`requestCapture` chaque `onDraw` pour l'actualiser.

- Les premières lectures retournent `(0,0,0,0,false,0)` ; même résultat hors phase/bornes,
  sans source ou sans support GPU.
- `sequence` identifie une capture source soumise, pas un tick Lua. Une capture peut dater
  d'une ou plusieurs images selon la file native/GPU. `ready=true` ne garantit pas sa
  fraîcheur : comparer `sequence`.
- Deux PBO par moniteur, fences consultées avec délai **zéro**, mapping après signalement.
  Aucun `glFinish` ni attente explicite CPU/GPU. Le pilote peut néanmoins introduire des
  coûts/stalls lors de soumission/mapping ; performances non encore mesurées.
- Limites : 64 moniteurs suivis, texture ≤1024×1024, 4 Mio par capture, budget PBO **32 Mio
  par contexte GL**. Atteindre une limite diffère/refuse la capture sans bloquer le rendu.
  Les PBO inactifs/détruits sont supprimés sur le thread de rendu.

## API et backend

Le plugin `examples/video_fx/video_fx.cpp` inclut seulement le SDK. Aucun RVA, offset de
structure du jeu, hook ou import OpenGL. Bibliothèque Lua via `host->api->lua`, fonctions
vidéo via `host->api->video`.

`SwVideoApi` est ajouté à la fin de `SwEngineApi`, préfixes UI/Lua et SDK v1 préservés.
ABI moteur toujours 1, SDK toujours 2. Avant de lire le champ, vérifier
`api->structSize >= offsetof(SwEngineApi,video)+sizeof(api->video)`, puis sa présence et
son `structSize`. L'API C utilise des couleurs normalisées 0–1. `enable/disable` suit le
propriétaire ; aucune adresse interne n'est exposée par l'API vidéo.

- `src/video_profile.h` / `video_profile.generated.h` : RVA et cinq empreintes exactes,
  build PE et vtable. `tools/gen_video_profile.py` refuse un autre SHA-256 et vérifie les CALL.
- `src/native_video.cpp` : relais près du module pour deux CALL rel32, suivi des moniteurs
  et destructeur. Pas de trampoline à longueur d'instruction devinée.
- L'IAT `glDrawElements` est relayée ; les autres dessins sont inchangés. Seule la phase
  de composition native d'un moniteur peut sélectionner les FX.
- `src/video_shaders.h` : projection, profondeur logarithmique et mélange vidéo/overlay
  des shaders livrés, plus uniforms FX. Compilation dans le **contexte GL natif** ; aucun
  fichier ROM modifié et aucun overlay externe/ImGui.
- Le renderer original configure matrices, couleurs, textures, buffers et attributs.
  Le relais copie ses uniforms dans le programme FX, dessine **la même géométrie native**,
  puis restaure le programme d'origine. Les bindings pixel-pack, texture et texture active
  sont restaurés après capture. Identité/échec shader : rendu original conservé.
- Le loader détient les leases, invalidées à la désactivation sans résurrection à la
  réutilisation d'adresse d'hôte. Les callbacks Lua sont invalidés avant `FreeLibrary`.
  Les hooks restent installés mais passifs. Destruction/redimensionnement du moniteur
  oublient les anciennes associations et captures ; cleanup GL sur le thread de rendu.

Les programmes GL/relais sont conservés jusqu'à la fin du processus. Le backend suppose
le thread de rendu natif ; pas d'accès PBO inter-contextes. La perte/recréation de contexte
et l'alternance de contextes pour un moniteur ne sont **pas** validées.

## Preuves statiques retenues

Complément à [`video_monitors.md`](video_monitors.md) ; fichiers sous `tools/ghidra/`,
avec commandes reproductibles. Ces preuves statiques ne montrent **pas** le nouveau mod
en cours d'exécution.

| Preuve | Utilisation |
|---|---|
| `video_fx_compile.txt`, CALL `0x74a538` | Passage moniteur, tailles, signal `monitor+0x1a8`. |
| `video_render.txt`, `0x366e90` | Le paquet stocke les **adresses** de `monitor+0x4c8` et `+0x4d8`, pas les objets GL. |
| `video_final_trace.txt`, `0x74aa50` | Déréférencement du stockage, contexte Lua actif et callbacks. |
| `video_fx_gpu.txt`, `0x20cf60` | Objet GPU placé à `resource+8`. |
| `video_fx_compile.txt`, CALL `0x6d87d0` | Objets vidéo/overlay via `wrapper+8`, alpha vidéo/alpha sur pile. |
| `video_fx_gpu.txt`, `0x688ec0` | Texture à `target+0x48`, unités 0/1, attributs et draw GL. |
| `video_fx_compile.txt`, vtable `0xb70ea0[0]` | Destructeur `0x367c80`. |
| Shaders ROM `additive_monitor.*`, `depth_utils.glslh` | Projection, profondeur et composition reproduites. |

Les recherches `video_fx_shader.txt` / `video_fx_link.txt` sont exploratoires : le backend
final ne hooke **pas** `glShaderSource` et ne relinke aucun programme du jeu.

## Tests et checklist runtime

`tests/run_tests.bat` : SDK C, préfixes ABI, propriétaires, profil vidéo sous ASLR simulé
et rejet de code/vtable/build altérés, bornes/NaN/infini, conversion pixel logique/texel.
Le **backend réel avec faux appels natifs** est testé : stockage→wrapper→objet, phase
`onDraw`, isolation, reset, redimensionnement, destruction, unload/reload.
Ces tests ne créent aucun contexte GL ni processus de jeu.

À exécuter en partie :
1. Charger une carte ; vérifier aussi la régression notifications/HUD du loader précédent.
2. Véhicule caméra→Lua→moniteur avec `demo.lua` ; chercher `FX shader ready` dans le log.
3. Identité/reset, offset X/Y, scale, tint/filter ; texte/tactile inchangés. Confirmer l'axe
   Y avec une source asymétrique connue (rendu et capture).
4. Vérifier pixels/couleurs, `sequence`, latence et plusieurs tailles/moniteurs.
5. Chaîne de scripts, erreur Lua, source absente, moniteur éteint, despawn/respawn,
   retour menu/rechargement carte, désactivation/réactivation du mod.
6. Budget/multi-captures, coût CPU/GPU, résolution vidéo, perte de contexte et autres pilotes.
