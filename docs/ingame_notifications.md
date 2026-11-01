# Notifications natives en partie

Le menu principal conserve son affichage existant. Les notifications de `Notify`, y compris
`debug.notify` / `qol.notify` des microcontrôleurs, sont également affichées dans les interfaces
natives du HUD et de l'éditeur de microcontrôleurs. Aucun overlay de rendu externe n'est utilisé.

`src/ingame_notifications.cpp` intercepte les slots 1 (init), 2 (teardown), 3 (update) et
0 (scalar deleting destructor) de chaque vtable. Les mises à jour originales sont exécutées avant l'affichage.
Un `NotificationOverlay` distinct est conservé pour chaque instance ; ses handles sont oubliés
avant le teardown et la destruction de l'arbre par le jeu. Le contexte UI est mémorisé
uniquement depuis `init` : le deuxième argument d'`update` est l'état applicatif, **pas**
le contexte UI. Aucune entrée n'est créée depuis `update` avant un `init` correspondant.
La création est différée jusqu'à la première
notification active. La file partagée conserve les durées et les filtres INFO/WARN/ERROR
de `log.cpp` et `mods/_modloader.ini`. Aucun tick global des plugins n'est ajouté.

## Profil et preuves statiques

Profil Stormworks v1.15.18 x64, même SHA-256/build que `src/game.cpp` :

| Interface | Vtable RVA | Update RVA | Destructeur RVA |
|---|---|---|---|
| `c_ui_element_hud_interface` | `0xb423c0` | `0x5c78b0` | `0x247ed0` |
| `c_ui_microcontroller_editor_interface` | `0xb71d60` | `0x642000` | `0x643380` |

La validation préalable refuse les builds inconnus, signatures modifiées et slots de vtable
inattendus, sans installer ces hooks. Le menu peut continuer à fonctionner indépendamment.

Extractions Ghidra en lecture seule, sans exécution du jeu :
- `tools/ghidra/notifications_roots.txt` : RTTI des interfaces et composite natif.
- `tools/ghidra/notifications_lifecycle.txt` : update/destructeurs et destruction des enfants.
- `tools/ghidra/notifications_abi.txt` : slots, signatures et propagation de RCX/RDX/R8
  dans l'update composite (les paramètres ne sont pas des flottants).
- `tools/ghidra/notifications_context.txt` : init natif de l'éditeur, signatures init/teardown.
- `tools/ghidra/notifications_crash_text.txt` : init texte utilisant le contexte UI, et
  update racine passant l'état applicatif à ses enfants.

## Correction du crash signalé le 14 octobre 2026

La première implémentation confondait l'argument RDX d'update avec le contexte UI.
Le dump local `stormworks64.exe.25988.dmp` (événement Windows 11:17:50) a été analysé
hors ligne avec DbgHelp `StackWalk64`, lecture du minidump et tables unwind des PE locaux.
Il montre un arrêt secondaire `0xc0000409` à `stormworks64.exe+0x9ac535` durant le
nettoyage de l'application, et les frames antérieures :

```
stormworks64.exe+0x245bbc  // init texte : dereference de ctx+0x128
stormworks64.exe+0x8f56ec  // add_child, appel init du widget
iphlpapi.dll+0x267f1      // API widgets, DLL de la première implémentation
iphlpapi.dll+0x31679      // création de notification
iphlpapi.dll+0x32043      // hook update
stormworks64.exe+0x1dd494 // parcours update des enfants
```

Les RVA du loader ci-dessus appartiennent à la DLL avant correction. L'identification
du mauvais argument est corroborée par la décompilation du chemin update et de l'init
texte. Le correctif capture le contexte à l'init et ajoute le teardown, avec tests de
régression ; il ne masque pas une exception moteur ni ne prétend avoir été testé en jeu.

## Validation

`tests/run_tests.bat` teste le profil sur le PE relocalisé en mémoire ainsi que la création,
les couleurs de sévérité, l'expiration, l'absence de créations répétées, l'isolation des
contextes et la recréation de l'affichage sur moteur simulé. Le loader compile en x64.

**Rendu réel non encore testé.** Après redémarrage du jeu :
1. Vérifier les deux lignes `Native notifications: ... hooks installed` dans le log.
2. En partie, déclencher `debug.notify` avec `mc_lua_qol` : carte en haut à gauche.
3. Répéter dans l'éditeur de microcontrôleurs et vérifier la disparition après la durée prévue.
4. Revenir au menu, recharger une partie et rouvrir l'éditeur ; vérifier absence de widgets
   résiduels/crash et maintien des notifications du menu.
5. Vérifier le HUD masqué / la pause et un éditeur ouvert : la visibilité suit celle de
   l'interface native parente ; ces hooks ne couvrent pas toutes les autres interfaces du jeu.

La DLL compilée est `build/iphlpapi.dll`, installable via `install.bat`, jeu fermé.
