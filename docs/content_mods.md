# Mods de contenu

Un mod peut ajouter ou remplacer des fichiers du dossier `rom/` du jeu (définitions de
composants, meshes, audio, données…) sans le modifier. Aucune DLL n'est nécessaire.

## Structure

```text
mods/
└── mon_mod/
    ├── mod.json
    └── content/                  ← même arborescence que rom/
        ├── data/definitions/mon_composant.xml
        └── meshes/mon_mesh.mesh
```

```json
{
  "id": "mon_mod",
  "name": "Mon mod",
  "content": "content",
  "enabled_by_default": true
}
```

- `content` : dossier superposé à `rom/`, relatif au dossier du mod (`content` par défaut,
  utilisé s'il existe). Un chemin absolu, contenant `..` ou un lien symbolique est refusé.
- `main` est facultatif : un mod peut être **contenu seul**, ou combiner une DLL et du contenu.
- Un mod de contenu s'active et se désactive dans le menu **MODS** comme les autres.

## Comportement

- **Lecture** : quand le jeu lit `rom/<chemin>`, il reçoit le fichier du mod s'il existe.
  Un fichier de même nom qu'un fichier du jeu le **remplace** ; sinon il est **ajouté**.
- **Listes de dossiers** : lister un dossier de `rom/` renvoie les fichiers du jeu plus ceux
  des mods (sans doublon). Un dossier qui n'existe que dans un mod est listé aussi.
- **Écritures** : jamais redirigées. Elles vont aux fichiers du jeu.
- **Conflits** : si deux mods fournissent le même fichier, le premier dans l'ordre du menu
  MODS (ordre alphabétique) l'emporte ; le conflit est écrit dans `StormworksModLoader.log`.
- **Moment** : la superposition est construite au démarrage du jeu, avant qu'il lise `rom/`,
  à partir des mods activés à ce moment-là. Activer ou désactiver un mod de contenu prend
  effet **au prochain démarrage** ; le menu MODS affiche alors le badge `RESTART`. Un mod de
  contenu actif affiche `CONTENT`.
- **Builds** : installée seulement après l'authentification de l'exécutable par le loader
  (un build inconnu reste sans aucun hook). Le mécanisme lui-même n'utilise aucune adresse
  interne au jeu.

## Mécanisme

Le jeu lit `rom/` par `kernel32` : le `fopen` de sa CRT statique appelle `CreateFileW` par
la table d'imports de l'exécutable, et les dossiers sont listés par `FindFirstFile(Ex)W`. Le
loader redirige les entrées d'import de `CreateFileW`, `CreateFileA`, `GetFileAttributesExW`,
`FindFirstFileW`, `FindFirstFileExW`, `FindNextFileW` et `FindClose`, après avoir vérifié
que chacune contient encore la fonction de `kernel32` (sinon rien n'est écrit). Seul
l'exécutable du jeu est concerné ; les DLL (loader, mods, Steam) gardent leurs imports.

Les chemins sont résolus par `GetFullPathNameW` (chemins relatifs, `/` et `..` acceptés),
puis comparés sans tenir compte de la casse au préfixe `<jeu>\rom\`. L'index des fichiers est
construit une seule fois au démarrage et n'est plus modifié ensuite.

## Exemple

[`examples/content_demo`](../examples/content_demo) ajoute un composant « StormFabric Block ».
Aucun fichier du jeu n'est fourni dans le dépôt : `make_content.py` le fabrique à partir de
votre copie (`01_block.xml` renommé).

```bat
python make_content.py "C:\chemin\vers\Stormworks"
```

**Vérifié en jeu le 2026-11-15** (build 1.15.23) : au démarrage, le jeu a listé
`rom\data\definitions` avec le fichier du mod, l'a lu depuis le dossier du mod, et
« StormFabric Block » est apparu dans la liste des composants de l'éditeur de véhicules.
Non vérifié en jeu : le remplacement d'un fichier existant, les meshes et l'audio fournis
par un mod, et le placement du composant sur un véhicule sauvegardé puis rechargé.
