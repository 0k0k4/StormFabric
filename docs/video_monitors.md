# Stormworks — fonctionnement interne des moniteurs vidéo

**Analyse du 14 octobre 2026 — Stormworks v1.15.18, Windows x64.**

## 1. Résumé

Un moniteur est un **composant de véhicule rendu dans la scène 3D**, pas un widget
de l'interface des menus. Il reçoit un signal vidéo logique qui transporte des
références de sources et de scripts. Le moteur prépare une image caméra et une
couche de dessin Lua, puis les combine sur la surface du moniteur avec son shader
natif. Les contacts tactiles repartent séparément par une sortie composite.

Principaux résultats établis par analyse statique :

- Composant moniteur : type XML `43`, classe `c_scene_vehicle_component_monitor`.
- Vidéo : type de connexion `6` ; tactile : composite de type `5`.
- Résolution logique : **32 pixels par bloc**, indépendamment du suréchantillonnage caméra.
- Deux cibles de rendu par moniteur ; la première peut être multipliée par `1`, `2` ou `4`.
- `onDraw` est exécuté avec les dimensions du moniteur destinataire et un contexte
  de dessin temporairement actif.
- Le shader mélange `texture_video` et `texture_overlay` selon l'alpha de l'overlay.
- Deux contacts tactiles sont conservés, avec coordonnées en pixels et états d'appui.
- Le moteur possède un affichage natif d'erreurs Lua et de boucle vidéo.

**Aucun test en jeu n'a été effectué pour ce rapport.** Les cadences réelles,
le comportement réseau et les détails non suivis sont explicitement laissés ouverts.

## 2. Cible, méthode et niveau de preuve

Exécutable examiné : `Stormworks/stormworks64.exe`.

```text
SHA-256 : 55914301628a31b278ff1d4aa9fd0c0446341759bf72083a8e3aa1dff859e6c3
PE timestamp : 0x6a392154
SizeOfImage : 0xd89000
Base d'analyse Ghidra : 0x140000000
```

Méthode : lecture des ressources livrées, RTTI/vtables, références croisées et
décompilation dans le projet existant `<GhidraProjects>\Stormworks_decomp\Stormworks.gpr`,
avec Ghidra 12.1.4 en mode headless `-noanalysis -readOnly`.
Le connecteur REA n'était pas disponible dans cette session ; l'analyse a utilisé
directement les outils Ghidra déjà installés, sans installation supplémentaire.
Les sorties sont conservées dans [`../tools/ghidra/`](../tools/ghidra/).

Les adresses du rapport sont des **RVA**, relatives au module chargé. Par exemple,
`0x366010` correspond à `0x140366010` dans le projet Ghidra. Elles ne constituent
pas une ABI publique et ne sont pas garanties sur une autre version du jeu.

- **Établi** : visible dans les ressources ou dans les opérations décompilées.
- **Déduit** : interprétation du rôle d'un champ/chemin à partir de plusieurs indices.
- **Non vérifié** : nécessite un parcours supplémentaire ou une observation en jeu.

Les noms `FUN_...` et les types `undefined...` sont produits par Ghidra.
Les prototypes incomplets, notamment les paramètres de pile manquants, ne doivent
pas être copiés tels quels pour créer des hooks.

## 3. Composants et connexions

Les dix fichiers `Stormworks/rom/data/definitions/monitor*.xml` décrivent les
moniteurs physiques et les HUD de véhicule. Tous utilisent `type="43"` et les ports :

| Port | Direction (`mode`) | Type logique | Rôle |
|---|---|---:|---|
| `Video Signal` | entrée (`1`) | `6` | source vidéo et chaîne de dessins |
| `Touch Output` | sortie (`0`) | `5` | dimensions, positions et appuis |
| `Electric` | entrée (`1`) | `4` | connexion électrique |
| `Power Switch` | entrée (`1`) | `0` | commande booléenne marche/arrêt |

La fonction `0x3662b0` résout ces quatre ports par leur direction et leur type,
ce qui corrobore les XML. Les moniteurs ordinaires déclarent
`monitor_border="0.022"` et `monitor_inset="0.012"` ; les HUD déclarent `0.02` et `0.01`.
Ces paramètres concernent la géométrie de la surface, pas le nombre de pixels Lua.

Les caméras utilisent `type="42"` et une sortie vidéo `Camera Feed` de type `6`.
Les modèles livrés incluent caméras petite/moyenne, gimbal et stabilisée ; leurs
entrées disponibles varient : alimentation, infrarouge, FOV, pivot, pitch,
stabilisation ou suivi selon la définition.

**Attention :** `HUD Small`/`HUD Large` sont ici des composants de véhicule.
Ce ne sont pas les objets de l'interface générale `c_ui_element_hud_interface`.

Preuves : XML livrés ; `video_tables.txt` (RTTI) ; `video_final_trace.txt`,
fonction `0x3662b0` ; `video_pipeline.txt`, fonction `0x361f20`.

## 4. Résolutions : pixels Lua et cibles GPU

La fonction `0x366010` calcule :

```text
largeur_logique = (voxel_max.x - voxel_min.x + 1) × 32
hauteur_logique = (voxel_max.z - voxel_min.z + 1) × 32
```

L'axe vertical physique local est donc ici `z`, pas `y`.
En appliquant cette formule aux bornes des fichiers livrés :

| Définition | Nom affiché | Pixels logiques (largeur × hauteur) |
|---|---|---:|
| `monitor_1.xml` | Monitor 1x1 | 32 × 32 |
| `monitor_1x2.xml` | Monitor 1x2 | 64 × 32 |
| `monitor_1x3.xml` | Monitor 1x3 | 96 × 32 |
| `monitor_2.xml` | Monitor 2x2 | 64 × 64 |
| `monitor_2x3.xml` | Monitor 2x3 | 96 × 64 |
| `monitor_3.xml` | Monitor 3x3 | 96 × 96 |
| `monitor_5.xml` | Monitor 5x3 | 160 × 96 |
| `monitor_9.xml` | Monitor 9x5 | 288 × 160 |
| `monitor_hud_1.xml` | HUD Small | 32 × 32 |
| `monitor_hud_3.xml` | HUD Large | 96 × 96 |

Le nom « 1x2 » ne signifie donc pas automatiquement 32 × 64 : les bornes du
composant déterminent l'orientation effective.

Deux ressources de type `c_resource_object_target` sont demandées :

1. cible à `largeur × facteur`, `hauteur × facteur` ;
2. cible à la résolution logique non multipliée.

La branche examinée sélectionne le facteur `1` pour l'argument `1`, `4` pour
l'argument `3`, et `2` pour les autres valeurs. L'attribution précise de ces valeurs
aux libellés des options graphiques n'a pas été suivie.
L'association première cible = vidéo caméra, seconde = overlay est corroborée
par le paquet de rendu et l'appel de dessin vers la seconde cible.

**Conséquence :** améliorer la qualité de l'image caméra ne change pas nécessairement
`screen.getWidth()`/`screen.getHeight()`, ni les coordonnées tactiles.

Preuves : `video_execution.txt`, `0x366010` ; `video_render.txt`, `0x366e90` ;
`video_composition.txt`, `0x748ea0` ; `video_trace.txt`, `0x2e7ec0` et `0x2e7f50`.

## 5. Ce que transporte réellement le signal vidéo

### 5.1 Ce n'est pas simplement un tableau de pixels

Dans le chemin logique étudié, les connexions vidéo copient une structure contenant
des métadonnées et des références de scripts. La structure de données vidéo possède
notamment une liste de **32 entrées**, chacune comprenant un identifiant et un
pointeur, un compteur et des indicateurs d'erreur.

La fonction de copie `0x2ea8d0` copie ces champs, pas un framebuffer pixel par pixel.
La fonction de remise à zéro `0x2eac10` remet à zéro la structure.
Il faut distinguer cette représentation **logique côté moteur** des textures GPU
créées plus tard et d'un éventuel protocole réseau, non examiné ici.

### 5.2 Un bloc Lua propage la vidéo et ajoute son script

Dans `0x38a230`, le bloc `c_microprocessor_component_lua_script` :

1. copie les données de l'entrée vidéo vers sa sortie ;
2. recherche sa propre référence de script dans la liste ;
3. l'ajoute avec son identifiant lorsqu'elle n'est pas déjà présente ;
4. positionne un indicateur d'erreur si cette référence apparaît déjà ou si la
   capacité de 32 entrées est atteinte ;
5. appelle séparément `onTick` pour la logique du microcontrôleur.

**Déduction forte :** une chaîne `caméra → Lua A → Lua B → moniteur` conserve la
source et accumule les scripts à dessiner. Le détail de leur composition visuelle
et de l'ordre dans toutes les branches reste à vérifier en jeu.
La capacité 32 est celle de cette structure ; ce n'est pas une limite générale
de 32 scripts dans le véhicule.

### 5.3 Sélecteur vidéo

La classe `c_microprocessor_component_switchbox_video` possède deux entrées vidéo,
une entrée de commande et une sortie vidéo. `0x38ab00` choisit une des deux données
d'entrée selon l'état booléen et copie la structure vers la sortie.
Il s'agit d'un routage de signal, pas d'un mélange alpha des deux images.

Preuves : `video_callbacks.txt`, `0x2eb9e0`, `0x38ab00` ; `video_draw_path.txt`,
`0x2ea8d0` ; `video_execution.txt`, `0x38a230` ; `video_pipeline.txt`, `0x38aa40`.

## 6. Comment Lua dessine sur le moniteur

### 6.1 `onTick` et `onDraw` ont des chemins distincts

`onTick` est appelé depuis la mise à jour du bloc Lua (`0x38a230`).
`onDraw` est appelé depuis la préparation du rendu vidéo (`0x74aa50`), elle-même
invoquée pour les moniteurs dans `0x748ea0`.
La décompilation ne permet pas d'affirmer ici une cadence réelle fixe pour `onDraw`.

Pour chaque script du signal retenu par la liste d'identifiants du rendu, `0x74aa50` :

1. lui fournit la largeur et la hauteur du destinataire ;
2. active le contexte écran et lui affecte un batch de dessin et une police ;
3. appelle la fonction Lua nommée `onDraw`, sans argument dans ce chemin ;
4. désactive le contexte écran après l'appel ;
5. collecte les erreurs pour afficher une page d'erreur native si nécessaire.

Cela explique pourquoi les dimensions doivent être lues dans `onDraw`, et pourquoi
un même script peut produire un dessin adapté au moniteur qui reçoit son signal.
Le nombre exact d'appels lorsque plusieurs moniteurs partagent une source dépend
du parcours et des filtres de rendu ; il n'a pas été mesuré.

### 6.2 Les fonctions `screen.*` alimentent des batches natifs

Les fonctions natives utilisent le contexte contenu dans l'upvalue de leur closure :

| Offset relatif au contexte `screen` | Utilisation observée |
|---:|---|
| `+0x278` | largeur logique |
| `+0x27c` | hauteur logique |
| `+0x280` | couleur courante |
| `+0x2a4` | autorisation de dessiner pendant `onDraw` |
| `+0x2a8` | pointeur du batch de dessin |
| `+0x2b0` | référence de police |

Le contexte `screen` est situé à `script + 0x60` dans cette version : par exemple,
les dimensions sont écrites à `script + 0x2d8`/`+0x2dc` par le rendu.
**Ces offsets ne sont pas relatifs au moniteur.**

`screen.getWidth`, `screen.getHeight`, `screen.drawClear` et `screen.drawLine`
vérifient le flag actif. Hors de cette phase, les wrappers natifs examinés signalent
`attempting to use screen function outside onDraw`.
Les wrappers natifs ne se contentent donc pas tous d'ignorer silencieusement l'appel.

`drawLine` émet de la géométrie ; les triangles et glyphes sont regroupés dans des
batches. `drawClear` utilise un batch de type `5` et sa couleur de clear.
On n'observe pas dans ces primitives une boucle Lua écrivant chaque pixel CPU.

Preuves : `video_final_trace.txt`, `0x74aa50` ; `video_execution.txt`, `0x2e6050`
(enregistrement natif des closures) ; `video_trace.txt`, wrappers `0x2e7ec0`,
`0x2e7f50`, `0x2e7fe0`, `0x2e86a0` et primitive `0x2e4140` ;
`video_render.txt`, primitive de glyphes `0x2e4cc0`.
Le loader existant `src/native_lua.cpp` corrobore ces offsets, mais n'est pas utilisé
comme preuve exclusive du fonctionnement original du jeu.

## 7. Rendu caméra, overlay et surface 3D

Chaîne simplifiée, reconstituée à partir des chemins suivis :

```text
source caméra / chaîne logique vidéo
             │
             ├── informations de caméra ── rendu de scène vers cible vidéo
             │
             └── références Lua ── onDraw ── batches 2D vers cible overlay
                                                      │
                        texture_video + texture_overlay
                                                      │
                              shader additive_monitor
                                                      │
                                 surface du véhicule
```

`0x366e90` prépare un paquet contenant le moniteur, ses transformations, ses
dimensions, ses deux références de cibles et les informations de son entrée vidéo.
`0x748ea0` contient la préparation caméra/rendu de scène et la passe de dessin des
scripts vers l'overlay. La caméra exacte et tous ses modes ne sont pas reconstitués
exhaustivement dans ce rapport.

Le matériau `c_material_additive_monitor` est identifié par RTTI ; `0x688bf0` charge
`graphics/shaders/additive_monitor` et enregistre les uniforms vidéo/overlay.
Le shader livré établit précisément la combinaison :

```glsl
video = texture(texture_video, uv);
overlay = texture(texture_overlay, uv);
rgb = mix(video.rgb * video.a * alpha_video,
          overlay.rgb, overlay.a);
color = vec4(rgb, 1.0) * vertexColor * multiply_color;
color.a *= alpha;
```

- overlay alpha `0` : la couche laisse apparaître la vidéo atténuée ;
- overlay alpha `1` : elle remplace la vidéo à cet endroit ;
- alpha intermédiaire : mélange des deux ;
- la couleur des sommets et `multiply_color` modulent le résultat final.

Le vertex shader transforme la surface avec `mat_world` et `mat_view_proj`,
transmet les UV et encode la profondeur logarithmique ; le fragment shader écrit
cette profondeur. **Le moniteur reste donc intégré au rendu 3D natif.**
Le nom « additive » ne suffit pas à déterminer l'état de blend OpenGL de toute la
passe : celui-ci n'a pas été suivi jusqu'à sa configuration finale.

Preuves : `video_render.txt`, `0x366e90` ; `video_composition.txt`, `0x748ea0` ;
`video_pipeline.txt`, `0x688bf0` ; fichiers livrés
`Stormworks/rom/graphics/shaders/additive_monitor.glslv` et `.glslf`.

## 8. Retour tactile

`0x367990` convertit un point local de la surface en coordonnées écran, en tenant
compte des dimensions physiques et de la bordure. La coordonnée verticale est
inversée lors de cette conversion. Les comparaisons examinées acceptent les bornes
`0 ≤ x ≤ largeur` et `0 ≤ y ≤ hauteur`, avec bornes supérieures inclusives.
Il ne faut donc pas supposer que tous les contacts sont déjà bornés à `largeur-1`.

`0x367ae0` conserve jusqu'à **deux contacts** : recherche d'une position existante,
mise à jour de son état, ou affectation d'un emplacement libre lors d'un appui.
`0x367aa0` teste si au moins un des deux contacts est actif.
Ces chemins sont conditionnés par l'état d'erreur vidéo, avec une exception pour
la remise à jour d'un relâchement dans `0x367ae0`.

`0x366a90` renseigne la sortie composite :

| Canal | Valeur |
|---|---|
| nombre 1 | largeur logique |
| nombre 2 | hauteur logique |
| nombre 3 | X du contact 1 |
| nombre 4 | Y du contact 1 |
| nombre 5 | X du contact 2 |
| nombre 6 | Y du contact 2 |
| booléen 1 | contact 1 appuyé |
| booléen 2 | contact 2 appuyé |

La numérotation découle de la disposition composite : masque booléen puis tableau
numérique. Lorsque le moniteur est inactif, le chemin observé remet les données
composites à zéro. Lorsque seuls les contacts sont relâchés, leurs coordonnées
ne sont pas systématiquement effacées : **utiliser l'état booléen pour décider
si une position correspond à un appui actif**.

Le tactile n'arrive pas automatiquement dans Lua par le câble vidéo : il faut
relier `Touch Output` à une entrée composite du microcontrôleur et le lire dans
`onTick`. Le dessin utilise ensuite l'état mémorisé dans `onDraw`.

Preuves : `video_render.txt`, `0x367990`, `0x367ae0`, `0x367aa0`, `0x366a90` ;
`video_final_trace.txt`, résolution de la sortie composite dans `0x3662b0`.

## 9. Alimentation, erreurs et durée de vie

### Alimentation et visibilité

Les ports électriques et marche/arrêt sont établis par les XML et leur résolution
native. Le moteur possède des diagnostics `Electric not connected`,
`Missing Video Input` et `Missing On/Off` dans `0x366580`.
`0x366a90` et `0x366e90` utilisent des états d'activité et des coefficients
d'atténuation liés au composant. Le calcul complet de la tension et du seuil
d'activation n'a pas été suivi : aucune valeur de seuil n'est affirmée ici.

### Erreurs affichées à l'écran

`0x74aa50` récupère les erreurs des scripts et génère lui-même des commandes de
dessin pour une page `ERROR`. Il contient notamment les messages :

```text
A problem has been detected in one or more connected scripts.
SIGNAL_FAILURE
Video feedback loop detected
```

Le doublon de script/capacité dans les données vidéo et cette page d'erreur
corroborent une protection contre les chaînes vidéo invalides.
La distinction visuelle exacte entre toutes les erreurs n'a pas été testée.

### Ressources GPU

`0x20da70` ne crée pas directement une texture OpenGL utilisable immédiatement :
il crée une référence `c_resource_object_target` et enfile un événement
`event_create_object_target` avec dimensions et paramètres.
`0x366100` enfile des événements `event_destroy_object` puis remet à zéro les deux
références. La création/destruction est donc **différée par le gestionnaire natif**.
Le destructeur scalaire du moniteur `0x367c80` libère également des buffers CPU,
appelle le destructeur de base et libère l'objet si son flag de suppression est actif.

Preuves : `video_trace.txt`, `0x366580` ; `video_final_trace.txt`, `0x74aa50` ;
`video_draw_execute.txt`, `0x20da70` ; `video_execution.txt`, `0x366100` ;
`video_pipeline.txt`, `0x367c80`.

## 10. Conséquences pour le modloader

**Le système vidéo et l'API UI des menus sont deux sous-systèmes différents.**
Un `SwUiApi` composite/texte/bouton ne peut pas être ajouté arbitrairement à un
moniteur : celui-ci consomme des batches/ressources de rendu vidéo, pas un arbre
de widgets `mm_ui_element` de menu.

Pour dessiner nativement depuis un mod, pistes possibles — **propositions, non implémentées** :

1. Ajouter des primitives via la bibliothèque Lua, exécutées dans `onDraw` :
   c'est le point d'intégration le plus proche des fonctions déjà centralisées.
2. Exposer à terme une API distincte de dessin vidéo avec contexte temporaire,
   dimensions et primitives natives ; ne pas laisser les plugins conserver le batch.
3. Pour injecter une source complète, traiter séparément le routage logique,
   les références de ressources et la phase de rendu ; un simple pointeur de texture
   ne reproduit pas le contrat observé.

Précautions : validation de version et d'ABI, contexte actif obligatoire,
gestion différée des ressources GPU, invalidation des callbacks avant déchargement,
et séparation entre logique `onTick` et rendu `onDraw`.
Les tables d'offsets de ce rapport sont des repères d'analyse, **pas une autorisation
ni une implémentation de nouveaux hooks**.

## 11. Couverture et questions ouvertes

| Question | État |
|---|---|
| Identification des composants et ports | Répondu par XML + code |
| Résolutions et deux cibles de rendu | Répondu par code + bornes XML |
| Données vidéo, propagation Lua, sélection | Répondu sur les chemins examinés |
| Appel de `onDraw` et contexte `screen` | Répondu par code |
| Combinaison vidéo/overlay et projection 3D | Répondu ; état GPU final partiel |
| Deux contacts et contenu composite | Répondu par code |
| Caméra : tous les modes, IR, stabilisation | Partiellement suivi |
| Électricité : tension/seuil exact | Non résolu |
| Cadence, culling, pause et partage entre écrans | Non vérifié en jeu |
| Vidéo radio et sérialisation multijoueur | Non analysé au-delà de l'inventaire des définitions |
| Format GPU exact, filtres, blend final | Non résolu |
| Thread exact de chaque callback et synchronisation complète | Non résolu |

Tests conseillés pour compléter : caméra seule ; dessin Lua sans caméra ; caméra
avec overlay transparent/opaque ; chaîne de deux Lua ; deux moniteurs de tailles
différentes sur le même script ; deux contacts et bord extrême ; coupure électrique ;
boucle vidéo ; qualité graphique `1/2/3` ; pause ; réapparition/destruction du véhicule.

## 12. Index des preuves conservées

Tous les fichiers ci-dessous sont dans [`../tools/ghidra/`](../tools/ghidra/), avec
leurs entrées reproductibles correspondantes `*.commands.txt`.

| Sortie | Contenu utile |
|---|---|
| `video_inventory.txt` | symboles/RTTI/chaînes vidéo, caméra, monitor, Lua |
| `video_tables.txt` | héritages et premières vtables |
| `video_pipeline.txt` | construction moniteur, ports caméra, matériau et uniforms |
| `video_trace.txt` | diagnostics et wrappers `screen.*` |
| `video_render.txt` | paquet 3D, composite tactile, conversion des contacts, glyphes |
| `video_execution.txt` | tailles/cibles, propagation Lua et appel `onTick` |
| `video_callbacks.txt` | structure d'entrée vidéo, switch et contexte Lua |
| `video_draw_path.txt` | copie des données vidéo et références croisées |
| `video_draw_execute.txt` | création différée des cibles et vtable moniteur étendue |
| `video_screen_context.txt` | recherches d'accès aux champs de contexte |
| `video_final_trace.txt` | exécution `onDraw`, erreurs natives et ports moniteur |
| `video_composition.txt` | orchestration caméra/overlay et appels par moniteur |

Les recherches `N`/`G` du script existant sont plafonnées respectivement à 200/300
résultats ; leurs inventaires ne doivent pas être interprétés comme exhaustifs.
Certaines requêtes exploratoires décompilent la fonction contenant l'adresse
demandée, pas nécessairement une fonction commençant exactement à cette adresse.
Les conclusions ci-dessus citent les fonctions effectivement identifiées.

Une implémentation expérimentale de l'extension est désormais disponible :
[`video_fx.md`](video_fx.md). Elle est compilée et testée hors jeu, pas validée au runtime.
Les propositions et inconnues de ce rapport restent des conclusions d'analyse statique.

**Conclusion :** Stormworks reconstruit nativement l'image du moniteur à partir
d'une source de scène et d'une chaîne de callbacks Lua, puis la projette dans le
véhicule. Le tactile reste un retour logique composite indépendant. Une extension
propre doit respecter cette séparation, les phases de callback et la durée de vie
des ressources du moteur.
