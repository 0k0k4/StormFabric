# Essai Custom Display — observation uniquement

**Ce n'est pas encore le mod Custom Display terminé.** Cet essai observe les
appels natifs de création/libération du moniteur. Il ne modifie ni résolution,
ni définition, ni géométrie. Le témoin reste donc en **32×32**.

## À faire

1. Fermer complètement Stormworks.
2. Double-cliquer sur **TESTER.bat** et garder sa fenêtre ouverte. Il sauvegarde
   le loader actuellement installé, utilise temporairement celui de cet essai et
   démarre **Launcher.exe** (jamais directement `stormworks64.exe`), avec
   l'observation transmise au jeu par l'environnement du lanceur.
3. Charger le véhicule `test` contenant **SWML Display Probe 1x1**. Le faire
   apparaître et vérifier que la caméra fonctionne toujours. Si possible, garder
   aussi un moniteur d'origine dans ce véhicule pour comparaison.
4. Changer la qualité des écrans dans les réglages graphiques : basse, moyenne,
   haute. Revenir à l'éditeur, refaire apparaître le véhicule, puis revenir au menu
   et recharger ce véhicule une fois. Aucune nouvelle sauvegarde n'est nécessaire.
5. **Quitter complètement le jeu et son lanceur s'il reste ouvert.** Le script attend
   bien le processus du jeu même si le lanceur se ferme avant lui, puis restaure le loader
   précédent et vérifie son SHA-256. Attendre le message vert de restauration.
6. Dire simplement « test fait » à l'assistant ; les journaux sont locaux.

## Sécurité et récupération

- Aucune modification des mods natifs, sauvegardes, réglages ou DLLs de plugins
  n'est effectuée par le script. Le jeu peut naturellement écrire ses journaux
  et autosauvegardes habituels pendant son utilisation.
- Exécutable, DLL d'essai et DLL précédente vérifiés avant remplacement.
- Refus si le jeu tourne déjà, si un fichier a changé ou si un essai est en attente.
- Sauvegarde du loader et de l'ancien journal dans `runs/run-…/` avant l'essai.
- Les journaux d'observation et une preuve de restauration sont conservés au même
  endroit. Le loader normal reste inerte vis-à-vis de l'observation sans
  `SWML_CD_OBSERVE=1`.
- Si le script est interrompu : fermer le jeu puis lancer **RESTAURER.bat**.
  Ne pas copier une DLL à la main. Le reçu `active.json` conserve la transaction.
- Si aucun processus de jeu n'est observé sous 120 secondes, aucune restauration
  prématurée n'est tentée. Fermer jeu/lanceur puis lancer **RESTAURER.bat**.
- Si une autre DLL a été installée entre-temps, la récupération refuse de
  l'écraser et conserve la sauvegarde pour examen.

Les appels natifs originaux sont exécutés exactement une fois, avec les mêmes
arguments et sur le même thread. Les captures mémoire sont bornées, uniquement
en lecture et sans conservation de pointeurs de ressources après leur vie utile.
Les événements sont limités à 4 096 par processus. Ils ne prouvent pas à eux seuls
qu'une allocation GPU a réussi ou qu'une libération différée est terminée.

Le launcher et ses restaurations ont été testés sur de faux fichiers jetables,
sans lancer le jeu. Le premier essai réel reste nécessaire.
