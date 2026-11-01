# Essai réel de résolution — 24×16 pixels

**Expérience locale, pas le mod terminé.** La forme physique reste celle d'un
moniteur 1×1 ; sa résolution logique demandée est **24×16 au lieu de 32×32**.
Les moniteurs d'origine et le témoin précédent ne sont pas modifiés.

## Test simple

1. Fermer Stormworks et son launcher.
2. Lancer **TESTER.bat** (ou `TEST-Resolution-CustomDisplay.bat` à la racine).
   Le jeu démarre via **Launcher.exe**. Garder la fenêtre du test ouverte.
3. Dans l'éditeur, charger **SWML_Resolution_Test_24x16** : une copie séparée du
   véhicule caméra de l'autosauvegarde, préparée sans modifier l'original.
4. Faire apparaître le véhicule. Vérifier que la caméra s'affiche sur le petit
   écran ; l'image peut être plus pixelisée/déformée, c'est attendu.
5. Passer la qualité des écrans en basse, moyenne puis haute ; revenir à
   l'éditeur et faire réapparaître le véhicule une fois.
6. Quitter complètement le jeu et le launcher, attendre le message vert de
   restauration, puis dire **« test résolution fait »** et si l'image fonctionnait.

Pas besoin de modifier les câbles, de créer un véhicule, d'envoyer les journaux
ou de sauvegarder. **Ne pas sauvegarder par-dessus un véhicule personnel.**

## Ce qui est temporairement installé

- La DLL d'essai remplace temporairement le loader, avec sauvegarde et contrôle
  des SHA-256 avant remplacement/restauration.
- Un mod natif distinct `swml_cd_resolution_test` et le véhicule séparé
  `SWML_Resolution_Test_24x16` sont créés uniquement si leurs chemins sont libres.
- L'archive exacte est vérifiée SHA-256 et maintenue ouverte en lecture seule
  pendant le jeu. L'identité native d'archive n'est pas confondue avec ce SHA-256.
- Les fichiers d'essai inchangés sont retirés à la fermeture. Tout fichier changé
  par l'utilisateur est conservé, jamais écrasé ou supprimé automatiquement.
- Le jeu peut produire ses réglages, journaux et autosauvegardes habituels.
- En cas d'interruption : fermer jeu et launcher, puis **RESTAURER.bat** dans ce
  paquet. Les sauvegardes et reçus sont sous `runs/run-…/`.

## Limites de cette expérience

Le calcul personnalisé intervient dans le chemin natif de création, sur son
thread et sa file. Destruction native inchangée ; pas d'overlay ni d'ImGui.
Chaque attachment demandé est plus petit que le moniteur 1×1 natif, pour chaque
qualité. Un budget cumulatif conservateur ne rend aucun crédit lors des
libérations différées ; s'il est épuisé, l'écran revient explicitement à son
calcul natif 32×32 sur la prochaine création sans ressource vivante.

L'essai est réservé à ce véhicule local neuf, **hors multijoueur**. Le contrôle
du fichier de référence ne certifie pas des archives embarquées hostiles ou des
collisions MD5, ni le budget GPU global. La résolution 480×272, les géométries
rectangulaires, Lua/tactile et le dialogue natif de configuration ne sont pas
encore livrés. Le premier essai en jeu de cette résolution reste à effectuer.
