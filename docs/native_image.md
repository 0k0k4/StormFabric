# Image overlay (`api->image`)

**Statut : implémenté, testé hors jeu (dessin OpenGL réel vérifié), pas encore validé en jeu.**

Un mod crée une image, y écrit des pixels **depuis n'importe quel thread**, et le loader la
dessine par-dessus la fenêtre du jeu à chaque image, juste avant la présentation. C'est la
brique d'affichage prévue pour un navigateur hors écran (CEF, Ultralight), un lecteur vidéo,
des graphiques, etc.

## Principe

- **Aucune adresse du jeu.** Le loader redirige l'import `GDI32!SwapBuffers` de
  `stormworks64.exe`, trouvé **par son nom** dans la table d'imports. Avant de rediriger,
  il vérifie que l'entrée pointe encore vers la vraie fonction (sinon il refuse : un autre
  outil l'a déjà détournée). Ça marche donc à l'identique sur les deux builds examinés.
- **Installé à la demande** : rien n'est modifié tant qu'aucun mod n'a créé d'image.
- **Rendu** : OpenGL 3.3 core (même version GLSL que les shaders du jeu), un quad par
  image, mélange en alpha prémultiplié. Tout l'état GL touché est sauvegardé puis restauré
  (programme, VAO, texture de l'unité 0, sampler, framebuffer, paramètres d'unpack, viewport,
  blend, depth/cull/scissor/stencil/sRGB, masque de couleur).
- **Envoi des pixels** : `update` copie la région dans un tampon du loader et la marque
  « sale » ; le thread de rendu n'envoie au GPU que le rectangle sale accumulé
  (`glTexSubImage2D`). Le tampon du mod est libre dès le retour de la fonction.
- **Sécurité** : une erreur pendant le dessin désactive l'overlay pour la session (journalisé),
  sans faire planter le jeu. Les textures sont toujours libérées sur le thread de rendu.

## API (`include/sw_sdk_image.h`)

| Fonction | Rôle |
|---|---|
| `create(owner, surface, w, h, format, flags, &handle)` | Image transparente, visible, en (0,0) ancre 0, opacité 1, ordre 0 |
| `update(owner, handle, region, pixels, stride)` | Copie une région (`NULL` = toute l'image) |
| `setRect(owner, handle, rect)` | Position en **pixels** de la fenêtre, +Y vers le bas, ancre 0..8 |
| `setVisible` / `setOpacity` / `setOrder` | Visibilité, opacité 0..1, ordre d'empilement |
| `destroy(owner, handle)` | Détruit l'image |
| `getScreenSize(owner, &w, &h)` | Taille de la zone client à la dernière image |
| `getDrawnRect(owner, handle, ...)` | Où l'image a été dessinée (pour ramener la souris sur l'image) |

- **Formats** : `SW_IMAGE_BGRA8_PREMULTIPLIED` (ce que donne `OnPaint` de CEF) ou
  `SW_IMAGE_RGBA8` (alpha non prémultiplié). `SW_IMAGE_FLAG_NEAREST` pour un échantillonnage
  sans lissage.
- **Placement** : `width`/`height` ≤ 0 = taille de l'image, au pixel près (le bon choix pour du
  texte et des pages web). Ancres : 0 haut-gauche, 1 haut, 2 haut-droite, 3 gauche, 4 centre,
  5 droite, 6 bas-gauche, 7 bas, 8 bas-droite.
- **Surfaces** : `SW_SURFACE_MENU`, `SW_SURFACE_HUD`, `SW_SURFACE_EDITOR` n'affichent l'image
  que pendant que cette interface est mise à jour (moins de 250 ms depuis la dernière mise à
  jour) ; `SW_SURFACE_NONE` l'affiche partout.
- **Limites** : 4096×4096 max, 64 images et 256 Mio par mod.
- **Disponibilité** : `SW_CAP_NATIVE_IMAGE` dans `runtime->capabilities()`, et
  `api->structSize` qui couvre le membre `image`.

## Entrée souris et clavier

`setInputCallback(owner, handle, callback, userData)` rend une image **interactive**
(`NULL` la rend inerte). Le loader sous-classe alors la fenêtre du jeu (`GWLP_WNDPROC`,
API Windows publique, aucune adresse du jeu) et route les messages :

- **Souris** : tant que le curseur est visible et au-dessus de l'image (l'image interactive
  la plus haute gagne), les déplacements, clics et molette vont à l'image **au lieu** du jeu.
  Un bouton enfoncé sur l'image la « capture » : elle reçoit la souris jusqu'au relâchement
  de tous les boutons, même hors de ses bords. Les coordonnées sont en **pixels de l'image**
  (mise à l'échelle comprise). Double-clic détecté (`clickCount = 2`). Le curseur caché
  (vue caméra en jeu) n'envoie rien.
- **Clavier** : un clic sur l'image lui donne le focus (`FOCUS`), un clic ailleurs le retire
  (`BLUR`), comme `setKeyboardFocus`. Avec le focus, les touches enfoncées et les caractères
  (`CHAR`, paires UTF-16 recombinées) vont à l'image au lieu du jeu ; les **relâchements**
  vont aux deux, pour qu'aucune touche ne reste bloquée côté jeu. Alt+F4 va toujours au jeu.
- **Données fournies** : de quoi alimenter un navigateur hors écran (CEF
  `SendMouseMoveEvent` / `SendMouseClickEvent` / `SendMouseWheelEvent` / `SendKeyEvent`) :
  bouton, boutons enfoncés, modificateurs, code virtuel, `lParam` brut, répétition, touche
  système.
- **Thread** : le callback tourne sur le thread des messages de la fenêtre. Il doit rester
  court et ne jamais attendre un autre thread ; les fonctions image y sont utilisables.
  Un callback qui plante perd seulement son entrée (journalisé). Au déchargement du mod, le
  loader attend la fin d'un éventuel callback en cours avant de continuer.

## Limites connues

- L'image est dessinée **au-dessus de tout**, y compris l'interface native et le curseur du
  jeu : il n'y a pas d'imbrication dans l'arbre UI natif (pas de découpe par une fenêtre native).
- Le jeu ne voit plus la souris tant qu'elle survole une image interactive : il garde la
  dernière position connue (pas de survol des boutons natifs cachés sous l'image).
- L'envoi au GPU se fait sous le verrou de l'image : un producteur qui met à jour pendant
  l'envoi attend quelques millisecondes au plus.

## Exemple

`examples/image_demo/` (`sf_image_demo.dll`) : image animée de 320×180 en haut à droite,
peinte depuis un thread du mod à environ 30 i/s ; F9 l'affiche ou la masque. Interactive :
un anneau suit le curseur, un clic laisse un point (double-clic : efface), la molette change
la vitesse, les touches tapées changent la teinte tant qu'elle a le focus (cadre blanc ;
Échap le rend).

## Tests

- `tests/image_model_tests.cpp` : validation, régions, rectangle sale, placement, surfaces.
- `tests/image_backend_tests.cpp` : contrat SDK, cloisonnement par mod, limites, déchargement ;
  puis, si la machine a un pilote OpenGL 3.3, **dessin réel** dans une fenêtre avec relecture
  des pixels (couleurs, mise à jour partielle, opacité, surfaces) et vérification de la
  restauration de l'état GL, et **entrée** par de vrais messages Windows (déplacement,
  capture, double-clic, molette, focus, caractères, Alt+F4, image du dessus, callback qui
  plante). Sans pilote (CI), cette partie est sautée.
