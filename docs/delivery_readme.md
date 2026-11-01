# Livraison expérimentale — loader natif Windows x64

**Livraison partielle : le loader est compilé ; le mod Custom Display n'est pas
encore livré comme plugin utilisable.** Le dossier `prototype` contient uniquement
son modèle de configuration C++ et ses tests hors moteur, pas un composant à placer.

## Installer le loader

1. Fermer Stormworks et son launcher.
2. Sauvegarder tout `iphlpapi.dll` déjà présent à côté de `stormworks64.exe`.
   S'il appartient à un autre outil, ne pas l'écraser sans vérifier la compatibilité.
3. Copier le `iphlpapi.dll` de cette livraison à côté de `stormworks64.exe`.
4. Conserver le dossier `mods` existant, ses dépendances et sa configuration.
5. Lancer par Steam ou le launcher habituel. Le menu **MODS** apparaît sur les
   exécutables pris en charge. Consulter `StormworksModLoader.log` en cas de problème.

Cette livraison ne s'installe pas automatiquement et ne modifie ni les définitions
natives ni les véhicules. Pour désinstaller : jeu fermé, retirer cette DLL et
restaurer le proxy précédent s'il y en avait un. Conserver les données des mods.

## Compatibilité et validation

Deux profils authentifiés restent autorisés :

- `55914301628a31b278ff1d4aa9fd0c0446341759bf72083a8e3aa1dff859e6c3`
- `f9206d85c82f4d02fd0ac391781d19c5c68394a9ffc48accca0e1f5966db8699`

Un autre exécutable reste en diagnostic sans hooks. Aucune protection n'est
contournée. Les journaux de compilation et des tests sont inclus dans `validation`.
Le second exécutable n'était pas disponible pour cette nouvelle exécution des
tests : son profil n'a pas été retesté. Les autres cas `SKIP` sont conservés dans
le journal ; ils ne sont pas des tests réussis.

**Cette compilation n'a pas été essayée dans le jeu.** Les tests utilisent des
images PE hors exécution et/ou des objets natifs simulés. Le hash de chaque fichier
de la livraison figure dans `SHA256SUMS.txt`.

## État du Custom Display

Réalisé : modèle indépendant taille/résolution, identité canonique réversible,
contrôles de limites et budget couleur des deux cibles, tests hors moteur.

Non réalisé : enregistrement sûr des définitions, génération géométrie/collisions,
dialogue natif de l'éditeur de véhicules, résolution Lua/rendu/tactile personnalisée,
placement et rechargement d'un véhicule contenant ce composant.

Le SDK actuel ne permet pas encore ces opérations. Aucun `custom_display.dll` ni
manifeste de mod factice n'est fourni. Voir `docs/custom_displays.md` pour les
contrats à établir avant de pouvoir livrer ce mod.

Le SDK et l'archive des sources conservent les exemples et les mécanismes de
dépendances existants. Aucun exécutable du jeu ni ressource du jeu n'est distribué.
