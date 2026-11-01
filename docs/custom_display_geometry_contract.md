# Custom Display — géométrie native déterministe, hors jeu

## État

`tools/custom_display_assets.py` construit une archive native contenant une
définition rectangulaire, son cadre et sa flèche. **Ce n'est pas le plugin final** :
aucun hook, dialogue, import incrémental ou réglage de résolution n'est activé.
Le dossier produit n'a volontairement pas de `mod.xml` et n'est pas installé.

L'identité `swml_cd_v1_b2x1_p480x272` encode séparément taille et pixels, selon
le même format canonique uint32 que `src/custom_display_model.h`. Les pixels ne
changent pas le mesh. **Sans le futur pont natif, le jeu produirait 64×32, pas
480×272**, pour cette taille 2×1. Le manifeste donne explicitement les deux valeurs.

## Contrat de géométrie

- Plan physique X/Z, profondeur Y inchangée, pas de mise à l'échelle du cadre.
- Convention d'ancrage entière des moniteurs natifs :
  `xmin=-floor(W/2)`, `xmax=floor((W-1)/2)`,
  `zmin=-floor((H-1)/2)`, `zmax=floor(H/2)`.
- Translation des quatre groupes de coins du cadre 1×1 de 0,25 m par bloc.
  Les triangles, couleurs, normales, largeur du biseau et profondeur sont conservés.
  Les deux points de la boîte de section du mesh sont ajustés en parallèle.
- Flèche native inchangée, uniquement translatée au centre de la nouvelle face.
- Un voxel natif complet par bloc, rotation identité, `physics_shape=0`, `flags=1`.
  Cela construit les données de collision ; cela ne valide pas les collisions en jeu.
- Surface arrière par voxel, surfaces latérales seulement sur le périmètre ;
  aucune face interne ou surface d'attache à l'avant de l'écran.
- Surfaces de flottabilité construites de la même manière que les modèles natifs.
- Nœuds vidéo, tactile, électricité et interrupteur conservés au bloc d'origine.
- Masse et valeur linéaires (4 et 500 par voxel), comme les modèles 1×1/2×2/3×3.

Le XML natif possède des attributs numériques (`physics_shape_rotation 00=…`)
qui ne sont pas du XML strict. Le générateur ne les « répare » pas : il conserve
le dialecte et remplace uniquement les blocs ciblés, avec cardinalité contrôlée.

## Contrat du mesh et provenance

Preuves statiques de l'exécutable de référence, jamais des adresses de repli :

- `FUN_140319070` choisit le blob embarqué par nom exact avant le chemin fichier ;
- `FUN_1402cbfb0` lit les flux, sommets, indices et sections du mesh ;
- `FUN_140187fe0` lit six float32 pour la boîte de section, convertis en doubles ;
- `FUN_140187d40` lit une chaîne précédée de sa longueur uint16.

Le sous-ensemble accepté est strict : signature `mesh`, version 7, un flux de
format 19, sommets de 28 octets (position float3, couleur 4 octets, normale float3),
indices uint16 avec compteur uint32, une section couvrant tous les indices,
boîte float6, deux chaînes uint16, échelle float3 unitaire, mot final nul opaque
observé dans les sources. Les variantes inconnues ne sont pas devinées.

Le générateur vérifie par SHA-256 **chacun des trois assets source relus sur disque**.
Cette restriction des assets n'est ni une nouvelle authentification de l'exécutable,
ni une certification de compatibilité d'un autre build du jeu. Aucune activation
du loader n'est modifiée. Les fichiers de preuve sont dans `tools/ghidra/` :
`custom_display_mesh_format.*` et `custom_display_mesh_records.*`.

## Bornes de l'outil et essais

La borne de 64 blocs par axe et les plafonds XML/archive/mesh protègent seulement
les allocations de l'outil hors jeu. **Ce ne sont pas des limites de placement ou
de GPU certifiées.** Une définition trop volumineuse est rejetée avant écriture.
Les pixels suivent le modèle d'identité uint32 ; leur allocation n'est pas tentée.

Depuis `StormworksModLoader` :

```text
python -m unittest discover -s tests -p custom_display_assets_tests.py -v
python tools/custom_display_assets.py --game ../Stormworks --output build/custom-display-geometry-2x1 --identity swml_cd_v1_b2x1_p480x272
```

Les douze tests couvrent : fixture binaire indépendante, toutes ses troncatures,
compteurs/layouts/indices/normales/NaN incorrects, format d'identité, ancrage,
meshes 1×1 identiques octet pour octet, comparaison des sommets et aires orientées
par plan avec les meshes 2×2/3×3 natifs, ensembles de voxels/surfaces/bornes XML,
rectangle 4×2, indépendance des pixels, archive autonome et déterministe, sources
altérées, limites, refus d'écrasement et sortie hors installation.
La comparaison par aires évite d'exiger le même ordre de sommets ou la même
diagonale de triangulation que l'exporteur des assets natifs.

Les comparaisons de sources réelles sont explicitement ignorées si l'installation
n'est pas disponible ; les tests binaires synthétiques et d'identité restent actifs.

Restent à valider : placement/collisions/fixations, huit réflexions, visuel,
sauvegarde/rechargement de ces géométries et résolution indépendante en jeu.
Le témoin 1×1 fonctionnel, le jeu, les mods et les DLL installés restent inchangés.
