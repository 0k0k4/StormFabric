# Test du cycle de vie de l'éditeur de véhicules

Ce test **n'ajoute pas encore la fenêtre Custom Display**. Il enregistre seulement
les créations, entrées/sorties actives, mises à jour échantillonnées, actions et destructions natives de
l'éditeur de véhicules. Il ne change ni les composants, ni la résolution, ni les
entrées clavier/souris. Aucun mod natif, véhicule ou archive de test n'est installé.

## Manipulation nécessaire

1. Fermer complètement Stormworks et son Launcher, puis lancer `TESTER.bat`
   (ou `TEST-Dialogue-CustomDisplay.bat` à la racine).
2. Charger une partie locale et entrer dans l'éditeur de véhicules. Cliquer sur
    un bloc **stock dans le catalogue complet des composants**, pas seulement
    dans la barre de raccourcis en bas ; inutile de le poser.
3. Ouvrir la fenêtre **Charger** du véhicule, la laisser ouverte cinq secondes,
   puis l'annuler. Ne pas enregistrer ni écraser de véhicule.
4. Fermer l'éditeur pour revenir au personnage devant l'établi, puis utiliser
   de nouveau l'établi pour rentrer dans l'éditeur. Fermer simplement Charger
   ne compte pas comme une sortie de l'éditeur. Cliquer de nouveau sur un bloc
   dans le catalogue complet.
5. **Revenir au menu principal sans fermer le jeu**, puis recharger la même
   partie. Rentrer à nouveau dans l'éditeur et sélectionner un bloc du catalogue.
   Cette étape est nécessaire : l'interface est conservée pendant toute une
   partie, même quand on quitte l'éditeur pour revenir au personnage.
6. Quitter complètement le jeu et le Launcher. Laisser la console du test
   terminer la restauration.

Le lanceur passe exclusivement par `Stormworks/Launcher.exe`. Il vérifie les
hashes, sauvegarde le loader installé et le remet à l'identique après le test.
Si la console signale une session incomplète : fermer jeu et Launcher, puis
lancer `RESTAURER.bat`. Ne pas remplacer les DLL manuellement.

Le journal est conservé dans `runs/run-…/observation.log`, la restauration dans
`restored.json`. L'ancien journal est conservé séparément dans `previous.log`.
Une DLL modifiée par ailleurs n'est jamais écrasée automatiquement.

## Ce que les résultats prouveront — ou pas

Deux chargements de partie dans une seule session permettent d'observer le recyclage des objets et l'ordre de
fermeture sur les threads natifs. La construction peut être faite sur un
worker distinct ; le premier update attaché fixe le thread d'utilisation
observé, qui doit rester identique jusqu'à la destruction. Cela ne certifie
pas la synchronisation de publication ni un transfert d'ownership du dialogue.
Le clic catalogue doit apparaître comme
action `0x61` (97). Les événements ne contiennent pas le texte des saisies.

L'auditeur `tools/custom_display_vehicle_report.py` distingue intégrité de la
trace et couverture demandée. Un échantillon de pointeur modal non nul **ne
prouve pas** que le clavier et tous les clics sont capturés. Même une trace
complète n'autorise pas l'import d'archives, la nouvelle fenêtre ou le placement
de configurations arbitraires. L'authentification du moteur reste inchangée.

Le paquet actuel annonce le protocole **v2** et observe huit slots. Les sorties
à l'établi sont des désactivations du sous-état, pas des destructions d'UI.
L'auditeur les compte séparément : il ne remplace jamais deux cycles complets
par deux clics ou deux ouvertures d'éditeur, et ne fusionne pas plusieurs jeux.

## Premier résultat réel (15 octobre, 20:57)

Une durée de vie attachée complète, init sur le thread 44280 puis
updates/actions/teardown/destruction sur 54980 ; contexte et parent stables.
Le contrôle initial supposait à tort un thread commun à ces deux phases.
L'auditeur corrigé accepte leur séparation mais refuse les changements de
thread pendant l'utilisation, une action avant le premier update attaché,
ou un update avant la fin de l'init. Les 121 tests Python passent.

La couverture reste **incomplète** : une seule entrée dans l'éditeur, actions
8 et 18 uniquement, aucun `0x61`, aucun modal échantillonné. Ne pas interpréter
ces absences comme la preuve que l'utilisateur n'a pas cliqué ou ouvert Charger.
Le loader original a été restauré et son hash vérifié. Le prochain passage
doit suivre explicitement les étapes 2 et 4 ci-dessus ; aucun test caméra requis.

## Second résultat réel (15 octobre, 21:46)

Deux clics catalogue `0x61` confirmés, contexte/parent stables et teardown
complet ; init sur 31392, utilisation/destruction sur 35452. Intégrité valide,
couverture stricte incomplète (une durée de vie UI). Le loader original est
restauré et son hash vérifié. Le code natif confirme que cette durée de vie
couvre la partie entière : les anciennes consignes ne pouvaient pas produire
les deux destructions demandées. **Ce n'est pas une erreur de manipulation.**

Le prochain paquet ajoute les véritables transitions d'activité à l'observation.
Les nouveaux hooks sont testés hors jeu, pas encore validés en jeu ; la fenêtre
Custom Display reste désactivée. La nouvelle étape 5 force un second cycle
de création/destruction sans changer de sauvegarde ni créer de véhicule.
