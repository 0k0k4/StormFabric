# Custom Display — option B, reconnaissance initiale

Date : 15 octobre 2026.

## Fonctionnement demandé

Sélectionner un composant **Custom Display** dans l'éditeur de véhicules ouvre une
fenêtre native. Elle permet de choisir largeur/hauteur physiques et résolution X/Y.
Après validation, le composant configuré est sélectionné pour placement normal.
Annuler ne doit ni placer un objet ni modifier la définition des moniteurs existants.

La taille physique et la résolution logique doivent être indépendantes par instance.
Le placement par glissement (option A) n'est pas retenu pour ce premier parcours.

## Exigence de compatibilité future

L'utilisateur demande de ne pas dépendre d'adresses codées en dur par version.

Contraintes de conception :

- aucun RVA, offset d'objet ou slot supposé stable dans le plugin ;
- privilégier définitions déclaratives et opérations natives identifiées ;
- pour les bindings privés nécessaires, rechercher et corroborer les identités
  par RTTI, chaînes, références, code décodé et relations d'appels ;
- valider aussi ABI, provenance des objets, layouts et durées de vie ; une adresse
  unique trouvée par scan n'est pas une preuve suffisante ;
- ne pas prendre le premier résultat ambigu ni réutiliser un ancien RVA en secours ;
- ne pas désactiver l'authentification ni la validation existantes pour obtenir
  artificiellement une activation sur build inconnu ;
- désactiver proprement le service si les contrats ne peuvent pas être établis.

**Limite actuelle : le loader entier reste limité à deux profils authentifiés.**
Rendre le futur plugin indépendant des RVA ne rend pas ce loader universel.
L'activation sur un futur exécutable demanderait aussi une évolution distincte du
chemin central de certification. Cette évolution n'est ni implémentée ni prouvée
par la présente reconnaissance. Aucune garantie ne peut couvrir une modification
arbitraire du moteur propriétaire.

Les adresses dans les exports Ghidra sont des **repères de preuve statique**,
pas des constantes d'implémentation à recopier dans le mod.

## Environnement effectivement vérifié

- Exécutable : `Stormworks/stormworks64.exe`, SHA-256
  `55914301628a31b278ff1d4aa9fd0c0446341759bf72083a8e3aa1dff859e6c3`.
- Ghidra : installation locale `ghidra_12.1.4_PUBLIC/`, démarrage headless réussi.
- Projet source : `<GhidraProjects>\Stormworks_decomp\Stormworks.gpr`.
- Travail sur une copie privée dans
  `%TEMP%\opencode\CustomDisplayOffline_917913b89bf545f4ba54a936f393f031`.
- Ouverture avec `-process stormworks64.exe -noanalysis -readOnly`.
- Le hash enregistré par le programme Ghidra correspond au fichier local ; voir
  `tools/ghidra/custom_display_identity.txt`.

Lors de la reconnaissance initiale : aucun jeu lancé, aucun hook installé,
aucune DLL reconstruite ou remplacée. La compilation de livraison hors jeu est
documentée dans la cinquième passe ci-dessous.
Le projet original n'a pas été ouvert par Ghidra pour cette reconnaissance.

## Premiers résultats statiques

Les noms ci-dessous ont été retrouvés dans les symboles RTTI du programme :

| Identité native | Intérêt pour la suite | Preuve |
| --- | --- | --- |
| `c_game_state_vehicle_editor` | État éditeur et dispatch d'actions à suivre | `custom_display_inventory.txt`, `custom_display_editor.txt` |
| `c_ui_vehicle_editor_interface` | Racine UI spécifique à l'éditeur de véhicules | mêmes exports |
| `c_ui_component_chooser` | Catalogue des composants | `custom_display_editor.txt` |
| `c_ui_component_chooser::chooser_panel::component_button_container::component_button` | Boutons du catalogue et chemin de sélection à suivre | `custom_display_paths.txt` |
| `c_ui_component_dragged` | Piste pour l'élément manipulé ; rôle exact non encore suivi | `custom_display_editor.txt` |
| `c_ui_component_properties_drawer` | Panneau de propriétés existant | `custom_display_paths.txt` |

Ces noms identifient des pistes pertinentes, pas encore un contrat de hook.

### Panneau de propriétés

Le constructeur exporté construit un composite et un second sous-objet virtuel.
Les deux vtables observées **ne doivent pas être prises pour deux classes ou deux
versions distinctes** : leur association apparaît dans le même constructeur.
L'init crée un fond et un conteneur de défilement ; l'update anime le tiroir.
La construction des propriétés propres au composant sélectionné reste à suivre.

### Résolution des moniteurs

La décompilation de la routine déjà identifiée dans `video_monitors.md` confirme :

1. lecture des bornes de la définition référencée par le moniteur ;
2. calcul largeur/hauteur logiques par différence inclusive des bornes, multipliée
   par 32 ;
3. stockage des dimensions dans l'instance moniteur ;
4. demande de deux ressources de rendu, dont une multipliée par un facteur 1/2/4.

Il faut donc traiter ensemble dimensions Lua, ressources GPU et tactile. Un simple
agrandissement de mesh ou changement de résolution de capture ne réalise pas la
fonctionnalité demandée.

### Chargement des définitions

Les chaînes `monitor_border` et `voxel_max` sont référencées par une même routine
du programme de référence. C'est une piste de lecteur de définition à analyser,
pas encore une API d'enregistrement dynamique ou une preuve de paramétrage par
instance.

## Verrous restant à lever avant une implémentation native sûre

1. Valider en exécution la chaîne clic catalogue → sélection → placement désormais
   retracée statiquement dans la troisième passe ci-dessous.
2. Retrouver registre, ownership et éventuels caches des définitions.
3. Déterminer si une instance peut porter ses dimensions sans muter une définition
   partagée ; sinon étudier des variantes identifiables et persistantes.
4. Suivre géométrie, occupation voxel, collisions, fixations et sélection.
5. Valider le round-trip définition/transformation retracé statiquement ; compléter
   duplication, undo/redo et apparition en monde.
6. Suivre propagation de la résolution jusqu'à Lua, overlay et coordonnées tactiles.
7. Établir un contrat de cycle de vie pour le dialogue dans **l'éditeur de véhicules**,
   distinct de l'éditeur de microcontrôleurs déjà géré par le SDK.
8. Définir et tester des contrats de découverte/validation, sans fallback d'adresses.

La fenêtre seule ne doit pas être présentée comme un Custom Display fonctionnel
tant que création, sauvegarde et reconstruction du composant ne sont pas prouvées.

## Deuxième passe — catalogue et registre

Toujours sur la copie en lecture seule du même programme. Les noms `FUN_...`
ci-dessous permettent de retrouver les preuves ; ce ne sont pas des bindings.

### Séparer le survol de la sélection pour placement

- `FUN_140611070` ajoute au modèle de catalogue une entrée de catégorie `0`
  contenant un pointeur de définition. Il réutilise l'entrée si ce pointeur est
  déjà présent ; le modèle n'est donc pas simplement une liste de noms XML.
- `FUN_14060bab0` construit le bouton natif associé à cette entrée et l'ajoute
  à deux collections du conteneur. Le bouton conserve un pointeur vers l'entrée.
- `FUN_14060b890` transmet une notification à un destinataire de l'UI.
  `FUN_140611010` utilise l'entrée active pour reconstruire le panneau d'information
  via `FUN_14060f170` lorsque son pointeur change.
- Cette dernière chaîne est une piste de mise à jour d'information/survol,
  **pas une preuve de sélection du composant à placer**. L'intercepter maintenant
  risquerait d'ouvrir la configuration sur le mauvais événement.
- `FUN_14060b8b0` propage un état à quatre sous-widgets après un test
  `FUN_140606fe0`. Ce n'est pas un point de clic démontré non plus.

Preuves : exports `custom_display_selection`, `custom_display_events`,
`custom_display_contracts`. Le dispatch d'entrées de l'état éditeur a également
été exporté dans `custom_display_placement`, sans contrat de clic certifié.

### Chargement natif et ownership des définitions

Le parcours suivant est désormais visible dans les décompilations :

```text
FUN_14046cf70 : énumération des définitions et chemins de mods
  -> FUN_14046f560 : chargement + insertion dans une collection du registre
     -> FUN_14046f7a0 : construction d'une famille de huit définitions
        -> FUN_140316a60 : remise à zéro, lecture XML, préparation des ressources
           -> FUN_1408fdcd0 / FUN_1408fdb30
              -> FUN_140317660 : champs XML, dont monitor_border et voxel_max
```

Observations utiles :

1. `FUN_14046cf70` énumère `rom/data/definitions/`, construit un chemin
   `component_mod_assets\\` et parcourt aussi des chemins `/data/definitions`
   provenant d'une collection de mods. Les conditions d'alimentation de cette
   collection et le routage des fichiers restent à établir ; ce n'est **pas**
   la preuve qu'un dossier arbitraire ajouté sera chargé.
2. `FUN_14046f7a0` construit `data/definitions/<identifiant>.xml`. Il alloue huit
   objets distincts, transmet un indice de 0 à 7 à leur chargement et copie les
   huit pointeurs dans chacun de ces objets. Leur rôle de combinaisons de reflets
   est établi dans la troisième passe ci-dessous. Une variante ne doit donc
   pas être assimilée à un seul bloc mémoire isolé.
3. Le registre conserve les huit allocations dans une collection et la définition
   principale dans une autre. Enregistrer une famille implique aussi une gestion
   de durée de vie ; copier superficiellement une définition comportant des
   ressources et conteneurs internes n'est pas une solution sûre.
4. L'identifiant sans extension est conservé dans chaque définition ; un rendu
   d'aperçu est demandé pour la principale. La visibilité du composant dépend
   également de contrôles séparés ; l'insertion brute d'un pointeur ne garantit
   pas qu'il sera proposé dans le catalogue.
5. Un deuxième chemin `FUN_140316770` lit une définition depuis du contenu fourni
   par un autre résolveur (`FUN_140315d40`), puis prépare ses ressources. Le rôle
   de ce chemin n'est pas encore établi ; ne pas le qualifier prématurément
   d'API de sauvegarde de véhicule ou d'enregistrement public.

**Conséquence pour le prototype :** étudier d'abord le chargement d'une définition
additionnelle par le mécanisme natif existant. Si des familles générées par
configuration sont nécessaires, elles devront être identifiables et recréées
avant la résolution des véhicules sauvegardés. Aucun hot-reload à la demande
depuis le dialogue n'est encore démontré.

Attention : `FUN_140206160`, qui énumère aussi les XML de définitions, construit
notamment les clés localisées `_name` et les descriptions. Ce n'est pas à lui seul
le registre de composants ; les deux parcours ne doivent pas être confondus.

### Preuve recherchée à l'issue de la deuxième passe

Suivre le clic effectif jusqu'au choix de la définition utilisée par le placement,
puis la résolution de son identifiant au rechargement d'un véhicule. Cela permettra
de choisir entre paramétrage par instance et familles de variantes générées sans
modifier les définitions partagées des moniteurs d'origine. La résolution logique
indépendante reste un chantier distinct de cette preuve de chargement.

Ces résultats sont statiques et limités au build de référence. Aucun composant
Custom Display n'a été créé ou essayé en jeu, et aucune protection du loader
n'a été modifiée.

## Troisième passe — sélection, création et identité sauvegardée

Les résultats suivants complètent les limites de la deuxième passe. Ils restent
des preuves **statiques du build de référence**, pas un contrat runtime certifié.
Les offsets cités servent uniquement à relier les observations dans les exports.

### Action effective de sélection

Dans `FUN_1407fa7a0`, l'action `0x61` :

1. consulte `FUN_140611050` sur le catalogue de l'interface éditeur ;
2. obtient l'entrée active du conteneur de boutons, et non la simple entrée du
   panneau d'information ;
3. pour une entrée de catégorie `0`, transmet sa définition à `FUN_140801340` ;
4. pour une entrée de catégorie `2`, suit un autre chemin `FUN_140801430`.

`FUN_140801340` remet l'outil dans le mode normal via `FUN_1407ff680`, réinitialise
des données de placement, conserve la définition dans l'état éditeur (champ
`+0x1580` de référence), applique son indice de transformation à la construction
temporaire (`FUN_140390ba0`) puis synchronise la sélection visuelle du catalogue.

Le traitement de souris `FUN_1407f3440` utilise cette même définition pour le
placement, notamment par `FUN_1407f7eb0`. Cette dernière appelle la fabrique
`FUN_14045eb50`. Son cas `0x2b` alloue et construit bien un
`c_scene_vehicle_component_monitor` par `FUN_140365b80` ; la fabrique affecte ensuite
la définition à l'objet et appelle son initialisation virtuelle. La création
passe également par des informations de construction et une commande conservant
les callbacks `FUN_14080a8f0` / `FUN_140809d70`.

**Conséquence :** l'action du catalogue fournit maintenant une frontière candidate
pour différer la sélection du seul Custom Display jusqu'à validation du dialogue.
Cela ne certifie pas encore le hook, son ABI ou la durée de vie du dialogue ; ne
pas confondre cette frontière avec la notification du panneau d'information.
L'undo/redo est repéré, mais son fonctionnement avec une définition générée reste
à tester, notamment après fermeture de l'éditeur ou déchargement du mod.

Preuves : `custom_display_handoff`, `custom_display_select_definition`,
`custom_display_dispatch`, `custom_display_creation`, `custom_display_persistence`.

### Les huit définitions sont des combinaisons de reflets

- `FUN_140316a60` mémorise l'indice transmis par le chargeur dans le champ `+0x40`
  de la définition, puis appelle `FUN_14031cab0`.
- Cette dernière interprète les bits `1`, `2`, `4` comme des changements de signe
  indépendants et appelle `FUN_140324a40` pour chacun des axes concernés.
- `FUN_140324a40` applique des facteurs `+1` / `-1` aux coordonnées et remet les
  bornes transformées dans l'ordre min/max ; elle transforme aussi d'autres
  données géométriques. Ce ne sont pas huit résolutions ou huit tailles.
- `FUN_140390ba0` synchronise les mêmes trois bits sur les données de construction
  utilisées lors de la sélection pour placement.

Une configuration personnalisée devra donc disposer d'une famille cohérente pour
les reflets natifs, sans copie superficielle des ressources de la définition.

Preuves : `custom_display_identity_roundtrip`, `custom_display_click` (partie
`FUN_14031cab0`), `custom_display_creation`, `custom_display_reflections`.

### Round-trip des identifiants de composants

La chaîne de sérialisation du véhicule descend désormais jusqu'aux composants :

```text
FUN_14090c430 -> FUN_1404c4ef0 (véhicule, bodies/body)
  -> FUN_1404ae3f0 (corps)
     -> FUN_1404b8a50 (XML, components/c et o)
        -> FUN_1404b9080 (identifiant de définition + indice)
     -> FUN_1404b9230 (autre format, binaire)
```

`FUN_1404b9080` lit/écrit `d` et `t` dans le format XML courant. Pour un ancien
format en lecture, elle utilise `definition` et `trans_index`. La sauvegarde
prend l'identifiant depuis la définition réellement référencée par le composant,
et l'indice depuis son champ `+0x40`. Le XML livré `starter_boat.xml` corrobore le
format compact avec des éléments `<c d="02_wedge" t="4"><o ...>`.

En lecture, `FUN_1404b8a50` parcourt les définitions du registre et exige à la fois :

- même longueur et mêmes octets pour l'identifiant ;
- même indice de transformation.

Lorsqu'une paire est trouvée, elle crée le composant selon le type de la
définition (`FUN_1404b9790`, dont le cas moniteur utilise lui aussi
`FUN_140365b80`), lui affecte cette définition, lit ses propriétés et l'insère dans
le corps par `FUN_1404ba440`. Si aucune paire ne correspond, elle conserve le nom
dans une autre collection du corps ; ce chemin ne crée pas le moniteur attendu.
Ne pas supposer qu'une définition manquante est reconstruite automatiquement.

**Conséquence :** un identifiant généré peut fournir un point d'ancrage persistant,
mais toutes ses variantes doivent déjà être enregistrées au moment où cette
lecture s'exécute. L'identifiant seul ne transporte pas les dimensions/résolutions
personnalisées : celles-ci doivent être encodées ou conservées séparément et
permettre une reconstruction déterministe. Aucun schéma personnalisé n'est encore
implémenté. Le format binaire a un chemin séparé ; sa compatibilité et celle du
multijoueur ne sont pas déduites du seul parcours XML.

Le sérialiseur propre au moniteur `FUN_1403661d0` délègue au sérialiseur de base et
traite notamment les connexions `electric`, `video`, `composite`, `active` dans un
mode de sérialisation. Aucun paramètre natif indépendant de résolution n'a été
établi par cette observation.

Preuves : `custom_display_serialization`, `custom_display_load_contract`,
`custom_display_base_serialization`, `custom_display_identity_roundtrip`,
`custom_display_body_roundtrip`, `custom_display_component_roundtrip`,
`custom_display_variant_identity`.

### Origine des chemins de mods natifs

La collection utilisée par l'énumération de `/data/definitions` n'est pas une
simple recherche de tous les dossiers voisins :

- `FUN_1401829f0` donne accès à la collection des racines ;
- `FUN_140182a60` y ajoute une chaîne si elle n'est pas déjà présente ;
- `FUN_1402962f0`, sérialiseur de paramètres de partie, lit `active_mods` /
  `mod_path`, vide cette collection en lecture puis la repeuple à partir des
  chemins sauvegardés ;
- `FUN_1405fe410` la reconstruit également à partir de deux listes dont seuls les
  éléments activés sont retenus ; l'identité précise de cette UI reste à confirmer ;
- `FUN_1408415a0` fournit un autre chemin avec `working/mods_temp/` et `mod.xml` ;
  cela ne constitue pas une preuve de support multijoueur du Custom Display ;
- `FUN_140211430` consulte `<racine>/mod.xml` ;
- `FUN_14046e090` parcourt aussi `<racine>/data/components` avant l'énumération
  des définitions par `FUN_14046cf70`.

Ajouter arbitrairement un XML dans le dossier du modloader ne suffit donc pas.
Le raccordement entre les mods DLL du loader et cette collection native n'existe
pas encore. Les ressources, les doublons de noms et les conditions de visibilité
doivent être traités avant un essai de chargement d'un composant additionnel.

Preuves : `custom_display_mod_sources`, `custom_display_mod_population`,
`custom_display_mod_entry`, `custom_display_mod_activation`.

### Visibilité et moment de population du catalogue

`FUN_140610df0` parcourt seize collections de catégories, exclut notamment les
entrées portant les bits `0x20000000` ou `0x80`, puis appelle `FUN_140611070` et
reconstruit les boutons. Les significations complètes de ces flags et les autres
conditions de disponibilité ne sont pas déduites de ce seul filtre.

`FUN_14046efd0` règle séparément un booléen de définition (`+0x270` de référence)
en comparant son identifiant à une liste reçue. Ce n'est pas une opération
d'ajout au catalogue. `FUN_14046cbb0` et `FUN_140460ee0` participent aussi à des
métadonnées du registre ; ne pas les remplacer par une simple insertion UI.

L'appel de chargement complet `FUN_14046cf70` apparaît dans une large routine
d'initialisation `FUN_1405e5320`, avant d'autres préparations du monde. La population
du catalogue `FUN_140610df0` est appelée depuis `FUN_1407ce2b0` et `FUN_1407cfce0`
avec le registre associé à ce même état de jeu. **Aucun de ces appels ne prouve un
hot-reload sûr :** le chargement complet commence par `FUN_140461830`, qui remet
en place le registre. Il ne doit pas être rappelé au milieu d'un placement alors
que des composants et commandes détiennent des pointeurs vers ses définitions.

`FUN_14031d540` poursuit la préparation de ressources de définition, notamment par
`FUN_140319190`. L'ownership et la synchronisation des ressources restent à établir
avant toute création de famille à la volée.

Preuves : `custom_display_visibility`, `custom_display_resource_lifecycle`,
`custom_display_registry_timing`.

### Contrôles hors jeu

- Aucun marqueur `decompile failed`, `no function`, `Exception` ou `ERROR` dans
  les exports `custom_display_*.txt` après cette passe.
- Lecture XML stricte des treize véhicules d'exemple livrés : douze lisibles,
  totalisant 24 880 composants et 285 identifiants distincts ; tous les indices
  `t` observés sont dans `0..7`, avec les huit valeurs représentées (absence de
  `t` comptée comme zéro). Cela corrobore le format observé, pas un test du mod.
- `missile_tracking.xml` est rejeté par `xml.etree.ElementTree` à la ligne 1,
  colonne 204. Le fichier n'a pas été corrigé ni inclus dans ces totaux. Ne pas
  conclure de cette erreur du parseur strict que le chargeur du jeu le rejette.

### État après cette passe

| Contrat | État |
| --- | --- |
| Action catalogue → définition sélectionnée → fabrique moniteur | Retracé statiquement ; non essayé en jeu |
| Identifiant + reflet sauvegardés et recherchés au chargement XML | Retracé statiquement ; non essayé avec une définition additionnelle |
| Signification des huit variantes | Combinaisons de reflets sur trois axes |
| Collection des racines de mods | Population native retracée ; raccordement loader absent |
| Enregistrement à la demande depuis le dialogue | Non établi |
| Dimensions physiques, mesh, voxels et collisions personnalisés | Non implémentés |
| Résolution indépendante Lua/rendu/tactile | Non implémentée |
| Dialogue natif dans l'éditeur de véhicules | Non implémenté |
| Certification runtime sans adresses fixes | Non implémentée |

Prochaine priorité technique : établir la préparation des ressources et la
visibilité d'une définition additionnelle, puis un premier round-trip en jeu
isolé. Ne pas modifier les moniteurs existants pour contourner ce verrou.

## Quatrième passe — résolution et modèle hors moteur

### Dimensions logiques et recréation des ressources

Les observations suivantes concernent uniquement le build de référence. Les
offsets et slots cités servent à retrouver la preuve, pas à définir un ABI futur.

- `FUN_140366010` stocke la largeur/hauteur logiques dans l'instance (`+0x4b8`,
  `+0x4bc`), puis demande deux ressources : dimensions logiques multipliées par
  le suréchantillonnage et dimensions logiques seules. Le paramètre de qualité
  natif vaut `1` → facteur `1`, `3` → facteur `4`, autre valeur → facteur `2` ;
  **un facteur n'est pas directement une valeur de ce paramètre natif**.
- `FUN_140367990` convertit une position physique en coordonnées tactiles à
  partir des bornes physiques et du bord de la définition, puis multiplie par
  les dimensions logiques de l'instance. `FUN_140367ae0` utilise ces mêmes
  dimensions pour borner les coordonnées et alimenter la sortie tactile.
- `FUN_140366a90` et `FUN_140366e90` lisent également les dimensions logiques,
  notamment pour la sortie composite et le rendu du moniteur. Le chemin Lua
  est décrit dans `video_monitors.md`. Cette cohérence est une piste pour une
  résolution indépendante, **pas une preuve qu'écrire deux champs suffit**.
- La vtable moniteur associe les slots de référence `+0x2c8` et `+0x2d0` à
  `FUN_140366010` et `FUN_140366100`. Des appels virtuels de recréation sont
  présents dans `FUN_1404a7240`, `FUN_1404bcf60` et dans les chemins exportés
  autour de `1407901ea` / `1407d02be`.
- Le dispatch `FUN_14078cfa0`, cas `0x23`, met à jour la valeur ensuite passée
  à la création des cibles et parcourt des composants pour appeler destruction
  puis création. Pour le moniteur, le test virtuel `+0x150` retourne l'objet
  via `FUN_1400cb6a0`. Une modification ponctuelle de résolution serait donc
  écrasée lors de cette recréation, qui recalcule les dimensions physiques ×32.
- `FUN_140366100` soumet des événements natifs `event_destroy_object` au
  gestionnaire différé avant de vider les deux références. Ne pas remplacer
  cette opération par une suppression GPU immédiate. La synchronisation et le
  budget transitoire anciennes/nouvelles ressources restent à établir.

Le scan d'instructions `FindInstructionUses.java` a parcouru 2 463 411 instructions
et produit 967 correspondances pour les quatre sous-chaînes recherchées, sans la
limite de 300 résultats de la commande `G` de `Dump.java`. Il inclut des champs
d'autres classes et des emplacements de pile : une correspondance d'offset n'est
pas une identification de type. Il n'exclut pas non plus les accès indirects,
les copies globales d'objets ou les écritures par une autre forme d'adressage.

Preuves : `custom_display_resolution_lifecycle`,
`custom_display_resolution_field_uses.txt`, `custom_display_target_recreation`,
`custom_display_quality_refresh`, ainsi que `video_render` et `video_execution`.
Un slot identique observé dans une classe non identifiée n'est pas assimilé à un
appel de moniteur (notamment dans l'export `FUN_1405b1480`).

### Prototype de configuration testable

`src/custom_display_model.h` ajoute un modèle C++ indépendant du moteur, sans
hook, adresse, changement du SDK ni activation dans le loader :

- quatre entiers distincts : largeur/hauteur en blocs et largeur/hauteur en pixels ;
- identifiant ASCII canonique réversible, par exemple
  `swml_cd_v1_b2x1_p480x272`, avec indice de reflet conservé séparément ;
- lecture stricte rejetant dépassements, zéros, signes, suffixes et variantes
  non canoniques, sans modifier la sortie en cas de rejet ;
- plan des deux images de rendu pour les facteurs `1`, `2`, `4`, avec contrôle
  de plage signée native et calcul de budget sans dépassement entier ;
- politique de limites explicite, désactivée par défaut. Les limites des tests
  sont fictives et ne constituent pas des capacités certifiées du jeu ou du GPU.

Le budget calculé couvre uniquement deux images couleur RGBA à quatre octets par
pixel. Ce n'est pas une mesure de mémoire GPU totale : profondeur, surcoûts du
pilote, autres écrans et destructions différées sont exclus.

Cet identifiant est un **format expérimental**, pas une sauvegarde native déjà
fonctionnelle. Sa reconstruction suppose encore de découvrir les identifiants
avant désérialisation et d'enregistrer leurs familles de définitions avec leurs
ressources. Aucun scanner de sauvegardes, générateur de définition, manifeste
persistant ou mécanisme d'enregistrement n'est ajouté ici. Une future évolution
de géométrie pourra nécessiter une nouvelle version d'identité.

Validation exécutée : `tests/run_custom_display_tests.bat`, compilation MSVC C++17
avec `/W4 /WX`, puis succès des tests `custom_display_model_tests` et
`video_model_tests`. Le premier est aussi ajouté à `tests/run_tests.bat`, mais la
suite complète n'a pas été exécutée pendant cette passe. Aucun marqueur
`decompile failed`, `no function`, `Exception` ou `ERROR` dans les exports
`custom_display_*.txt` après cette passe.

La lecture XML stricte de `monitor_1x2.xml` échoue aussi à la ligne 74, colonne 27,
dans les données `physics_shape`. Le fichier natif est laissé intact ; un futur
générateur ne doit ni supposer sa conformité à un parseur XML standard ni
« corriger » automatiquement les définitions livrées.

**Toujours non réalisé :** chargement/placement d'un moniteur additionnel en jeu,
résolution runtime personnalisée, dialogue natif et certification automatique.
La priorité reste le contrat d'enregistrement et de préparation des ressources,
sans réinitialiser le registre pendant que des objets le référencent.

## Cinquième passe — cache audio et préparation de livraison

La branche `FUN_14031d540` → `FUN_140319190` est désormais identifiée plus
précisément comme une préparation **audio**, et non comme une preuve de
chargement du mesh :

- les champs source `+0x148`, `+0x158`, `+0x168`, `+0x1c0`, `+0x1d0`, `+0x1e0`
  correspondent aux clés XML `audio_filename_start/loop/end` et leurs variantes
  `_b` dans le lecteur de définition ;
- `FUN_14027d050` recherche un nom exact dans un cache du gestionnaire, crée une
  entrée en cas d'absence, puis copie un handle géré ;
- `FUN_1408e7e90` et `FUN_1408d3f30` participent à l'affectation/libération d'un
  objet à compteur de références ; le chemin final inclut `alDeleteBuffers` ;
- ce résultat ne démontre ni propriété exclusive ni synchronisation de thread.
  Il confirme qu'une copie mémoire brute de définition ne respecte pas les
  contrats de ressources. Les ressources graphiques doivent être suivies
  séparément depuis les champs `mesh_*` et leurs consommateurs.

Preuves : `custom_display_resource_cache.commands.txt` / `.txt`,
`custom_display_resource_lifecycle` et `custom_display_variant_identity`.

Le SDK actuel ne propose aucune opération d'enregistrement d'une définition de
composant, de sélection de placement ou de définition de résolution logique.
`api->video` ne remplace pas ces opérations. Livrer une DLL utilisant uniquement
ce service ne réaliserait donc pas le mod demandé.

La livraison du loader et du prototype source est distincte d'un mod Custom
Display installable. Les contrôles hors jeu ne remplacent pas l'essai natif
chargement → placement → sauvegarde → rechargement → apparition en monde.

### Validation et livraison effectivement produites

- `build.bat` : compilation du loader réussie avec MSVC x64 ; ancienne DLL de
  travail conservée dans `build/backups/delivery-20261015-122528/`.
- `tests/run_tests.bat` : quatorze exécutables de tests et deux compilations
  smoke C ont terminé avec succès sur cet environnement. Le test du deuxième
  exécutable est explicitement `SKIP`, car ce fichier n'est pas disponible.
  Les cas SDK indisponibles restent aussi `SKIP`, sans assimilation à un succès.
- Le premier essai a échoué avant les tests de stockage, à cause d'un ancien
  chemin utilisateur codé en dur. `tests/test_temp.h` utilise désormais
  `LOCALAPPDATA/Temp/opencode` ou `SWML_TEST_TEMP_ROOT` ; les deux tests concernés
  ne dépendent plus de ce nom utilisateur. Aucun contrôle du stockage n'a été
  supprimé ou affaibli pour faire passer les tests.
- Journaux conservés : `build/delivery-build.log`,
  `build/delivery-tests.log` (échec initial) et
  `build/delivery-tests-portable.log` (suite disponible réussie).
- `tools/package_delivery.py` assemble séparément loader binaire, SDK, prototype
  source, documentation et sources du projet ; chaque archive est contrôlée par
  son CRC et accompagnée d'empreintes SHA-256. Aucun binaire ni ressource du jeu
  n'est inclus. Les exemples sources et les mécanismes de dépendances restent
  présents ; aucune DLL d'exemple préexistante n'est supprimée.

**Aucune installation dans `Stormworks/`, aucun lancement du jeu et aucun essai
runtime de cette compilation. Aucun `custom_display.dll` fonctionnel n'est livré.**

## Sixième passe — import incrémental et archives de composants

Le retour utilisateur « c'est bon » confirme son essai du loader selon le parcours
proposé. Il ne constitue pas une validation du Custom Display, toujours absent du
runtime. Le journal local consulté confirme l'authentification et la validation des
bindings ; il ne prouve pas à lui seul toutes les opérations réalisées en jeu.

Une entrée native incrémentale a été identifiée : `FUN_14046f6b0` →
`FUN_14046fc50` → `FUN_140316770`. Plusieurs importeurs natifs conservent l'archive
et ses ressources dans le registre, construisent les huit réflexions manquantes,
puis désérialisent les véhicules. Cette découverte remplace l'hypothèse « aucune
entrée incrémentale retrouvée » ; elle **ne lève pas** les exigences de thread,
ownership graphique, découverte runtime et validation en jeu.

Les preuves distinguent désormais l'identité de définition, l'identité de
configuration du prototype et l'identité MD5 de l'archive. Le chemin de sauvegarde
de l'éditeur exporte les archives utilisées en `.bin` ; l'import peut les reconstruire
avant la lecture du véhicule. Le wrapper d'ajout ne rafraîchit pas les catégories
du catalogue : cette étape et les pointeurs UI restent à traiter séparément.

Voir [le contrat d'import à certifier](custom_display_registration_contract.md)
pour les formats observés, la chaîne de sauvegarde et la prochaine preuve minimale.

Cette passe n'a changé aucun code runtime, installé aucun hook et remplacé aucune
DLL. Le travail Ghidra a conservé `-noanalysis -readOnly` sur la copie privée.
Les archives de livraison précédentes restent inchangées et ne contiennent pas
ces nouvelles preuves ; aucun nouveau binaire n'est à tester pour l'instant.

## Septième passe — témoin natif généré, à essayer en jeu

`tools/custom_display_probe.py` produit maintenant un mod natif de diagnostic,
distinct du plugin final : `swml_display_probe/mod.xml` et une archive dans
`data/components/`. L'import passe par le chargement normal des mods natifs ;
aucun binding, hook, changement du SDK ou appel manuel du registre n'est ajouté.

Le générateur utilise uniquement `monitor_1.xml` de l'installation locale et
change son nom d'affichage en **SWML Display Probe 1x1**. Le nom de définition
encodé dans l'archive est `swml_cd_probe_v1_1x1`, différent des identités du modèle
configurable. Géométrie, collisions, connexions et résolution logique native
32×32 restent inchangées. Les meshes d'origine sont référencés, non copiés.

Le codec implémente le sous-ensemble version 1 et l'enveloppe fichier observés :
contrôles de longueur, version explicite, identifiants bornés, noms de ressources,
doublons, terminaisons zéro et budget total. Ces bornes sont celles d'un outil
hors jeu, pas des capacités moteur. MD5 ne sert qu'à l'identité d'archive native ;
SHA-256 contrôle séparément l'exécutable, la source et les fichiers produits.

`tests/custom_display_probe_tests.py` : **10 tests réussis**, incluant une fixture
binaire écrite indépendamment de l'encodeur, toutes les troncatures de cette
fixture de test, versions inconnues, tailles déclarées hostiles, doublons,
préservation du dialecte XML natif et refus d'écrasement/écriture dans le jeu.
Le journal est `build/custom-display-probe-tests.log`. Ce ne sont pas des tests
d'acceptation par le moteur.

Livraison locale : `../Livraison-CustomDisplay-Probe-20261015/`. Identité de
l'archive générée : `d7b0869911c6cc3f3204060c2854c674`. Son manifeste indique
explicitement `runtime_validated: false` et `installed: false`. Le fichier natif
et le README peuvent être soumis à l'essai isolé décrit dans
[custom_display_probe.md](custom_display_probe.md).

**Non réalisé :** essai d'import natif, placement/sauvegarde/rechargement/spawn de
ce témoin, ajout à la volée, configuration de taille/résolution, dialogue natif.
Le générateur refuse les sorties dans le jeu et les dossiers de sortie existants.
Les mods locaux préexistants, dont `customScreen`, restent intacts. Aucune DLL du
loader ou du jeu n'est reconstruite/installée par cette passe.

### Retour en jeu et correction du chemin d'installation

L'utilisateur confirme que le témoin fonctionne et peut être placé. Il précise
que le jeu charge ses mods natifs depuis
`%APPDATA%\Stormworks\data\mods\swml_display_probe`
(`%APPDATA%\Stormworks\data\mods\`), et non depuis `steam_settings/mods`.
Les deux fichiers présents dans ce dossier ont été vérifiés en lecture seule :
leurs SHA-256 correspondent exactement au témoin généré et au reçu initial.

Le chemin `steam_settings/mods` suggéré auparavant était donc incorrect pour
l'installation effectivement testée. Les instructions du générateur sont
corrigées dans `custom_display_probe.md`. Les anciens manifestes et reçus restent
des instantanés historiques ; leurs mentions `runtime_validated: false` ne sont
pas réécrites en validation complète. Les anciennes archives livrées ne sont pas
modifiées, afin de conserver leurs empreintes.

**Acquis selon les retours utilisateur : import, placement et vidéo caméra du
témoin en jeu.** L'utilisateur a aussi signalé sa présence lors de l'étape de
rechargement ; le détail du redémarrage complet n'est pas établi séparément.
Le tactile et les huit réflexions restent non confirmés. Le témoin n'a toujours
ni dimensions/résolution personnalisées ni intégration avec le dossier DLL
`Stormworks/mods`.

## Huitième passe — sauvegarde observée et préparation des cibles indépendantes

### Preuve disque, sans toucher au véhicule

`tools/custom_display_probe.py verify-save` contrôle maintenant un XML de véhicule
et son archive compagnon, par rapport à une archive témoin fournie explicitement.
Les lectures sont bornées ; absence d'identité, compagnon manquant, contenu changé,
DTD/entités et encodages masquant ces déclarations sont refusés. L'outil n'écrit
pas dans les sauvegardes et ne prétend pas observer un redémarrage du jeu.

Contrôle effectué sur `test.xml` et `tmp/autosave.xml` dans le dossier utilisateur :
chacun référence `swml_cd_probe_v1_1x1` et possède l'archive
`d7b0869911c6cc3f3204060c2854c674.bin`, exactement identique au témoin livré.
SHA-256 d'archive :
`966bf567c17875c40a0d659fe004efc0bc1e81123842d39f06671c2cf99cd038`.
Le reçu `build/custom-display-save-evidence-20261015-153701-730408Z.json`
sépare ces résultats des retours utilisateur et des étapes non prouvées.

### Découverte supplémentaire, toujours sans activation

L'export `tools/custom_display_runtime_contracts.json`, issu de Ghidra en lecture
seule sur la copie privée, conserve les corps complets création/libération des
cibles, requête différée, constructeur moniteur et construction de définition
depuis une archive. Il couvre aussi les slots natifs 89/90 du moniteur et les
témoins de leurs appels directs. Ces numéros sont des preuves statiques, pas des
adresses/slots de secours exposés aux plugins.

`gen_discovery_profile.py` fusionne désormais ces preuves avec les anciennes :
identités communes strictement identiques pour corps/masques/références,
conservation des anciens groupes et slots, refus des conflits. Les nouvelles
cibles appartiennent aux groupes diagnostiques 64 (cibles) et 128 (construction
depuis archive). Cela ne déclare pas l'ABI complète d'import certifiée.

Les tests découvrent ces cibles avec ASLR, balayage forcé et métadonnées inconnues
simulées, puis refusent un slot de destruction modifié, un appel de requête
redirigé et un déplacement de champ altéré. Les builds inconnus restent bloqués
en activation ; aucun profil d'authentification n'est élargi.

### Adaptateur de file testé, non raccordé au jeu

`src/custom_display_targets.h` traduit un plan indépendant en deux requêtes
natives dans l'ordre observé : vidéo suréchantillonnée, puis cible auxiliaire à
la résolution logique. Il publie largeur/hauteur/multiplicateur avant les appels,
préserve le format/flag natifs et refuse de changer des cibles encore occupées.
Il ne possède ni objets moteur ni callbacks de plugin, et ne fait aucun appel GL.

**Cet adaptateur n'est pas encore installé ni exporté par le SDK.** Ses tests
utilisent de faux objets et une fausse file pour vérifier qualité 1/2/4, refus
atomiques, isolation des moniteurs et ordre des créations/destructions différées.
Ils ne valident pas un contexte graphique, une allocation GPU ni le thread moteur.
La libération réelle reste le chemin natif ; le budget de toutes les ressources
en vol et la certification des objets restent nécessaires avant branchement.

Résultats : 15 exécutables C++ et deux compilations C de la suite disponible,
24 tests Python, puis balayage forcé de découverte réussis. Le second exécutable
authentifié reste indisponible et explicitement sauté. Journaux :
`build/custom-display-targets-tests.log`, `build/custom-display-python-tests.log`,
`build/custom-display-forced-scan-tests.log`, `build/custom-display-targets-build.log`.
Le loader de développement a été reconstruit ; le précédent build est conservé
sous `build/backups/custom-display-targets-20261015-173630-607/`.
**Aucune DLL installée remplacée, aucun hook Custom Display ajouté, aucune archive
de livraison antérieure modifiée.** Dialogue natif, géométrie et branchement
runtime de la résolution restent à implémenter.

## Neuvième passe — géométrie paramétrique et archives déterministes

`tools/custom_display_assets.py` produit désormais une définition native de
dimensions indépendantes, avec deux ressources mesh embarquées (cadre et flèche),
occupation voxel complète, surfaces extérieures, bornes physiques et identité
canonique `swml_cd_v1_b…_p…`. Les sources stock sont authentifiées individuellement
par SHA-256 et ne sont jamais réécrites. Le dialecte XML natif est conservé.

Le cadre n'est pas étiré : les groupes de coins du 1×1 sont translatés, en
conservant biseau, couleurs, normales et indices. Les tests comparent positions,
aires orientées par plan et boîtes avec les meshes 2×2/3×3 natifs. Les ensembles de
voxels, surfaces latérales/arrière et bornes XML retrouvent aussi les modèles stock.
Un rectangle 4×2 et le recentrage de la flèche sont contrôlés séparément.

Le contrat du lecteur de mesh et de ses boîtes/chaînes est conservé dans les lots
Ghidra `custom_display_mesh_format.*` / `custom_display_mesh_records.*`, extraits
avec `-noanalysis -readOnly` sur la copie privée. Aucune adresse d'activation ou
de secours n'en est ajoutée. Détails :
[custom_display_geometry_contract.md](custom_display_geometry_contract.md).

Un exemple est construit sous `build/custom-display-geometry-2x1-480x272/` :
archive native `24ec30a203bd39dd4b01689e1790391c.bin`, SHA-256
`5512d27a5a8f118690464577eb830ec537a996b81a9d9b11426986f307d99751`.
**Ce dossier n'est pas une livraison fonctionnelle** : absence volontaire de
`mod.xml`, aucun import ni installation. Le manifeste distingue pixels demandés
480×272 et résolution que donnerait encore le jeu non modifié, 64×32.

La suite `tests/run_tests.bat` inclut maintenant aussi tous les tests Python :
**15 exécutables C++, deux compilations C et 36 tests Python réussis**, dont douze
nouveaux tests assets. Le second exécutable authentifié reste indisponible et
explicitement sauté. Journal : `build/custom-display-geometry-full-tests.log`.

**Restent non réalisés :** liaison de ces archives au loader, import incrémental
sûr et rafraîchissement du catalogue, dialogue natif de l'éditeur de véhicules,
résolution indépendante active, budgets différés et validations en jeu des
géométries, fixations, collisions, réflexions, tactile et sauvegarde/rechargement.
Le témoin, le loader installé, les sauvegardes et les anciennes livraisons ne sont
pas modifiés. Aucun nouveau binaire DLL n'a été construit par cette passe.

## Dixième passe — file différée et correction du budget natif

Le traçage du backend montre que les requêtes natives `(5, 1)` spécifient un
format couleur **et un format profondeur**, et non un format et un simple flag.
La comptabilité moteur réserve 8 + 4 octets par pixel pour chaque cible. Le
calcul préparatoire à 4 octets était donc insuffisant ; `custom_display_model.h`
compte maintenant 12 octets, avec renommage interne des champs en
`maxTargetBytes` / `targetBytes`. Aucun changement d'API SDK publiée.

Le modèle refuse aussi les dimensions dont la comptabilité par ressource
dépasserait le champ natif signé 32 bits. Tests ajoutés : ancien budget
sous-estimé refusé sans mutation/requête, frontière exacte de comptabilité,
dépassement dans les deux axes et arrondi du budget en octets.

Le chemin de destruction des wrappers et de décrément des références backend est
désormais conservé, mais vider les handles ne prouve toujours pas la libération
GPU. Le thread consommateur, le drainage et le budget global restent à certifier.
Détails et lots de preuves : [custom_display_targets_contract.md](custom_display_targets_contract.md).

**Validation : 15 exécutables C++, deux compilations C et 36 tests Python réussis**
dans `build/custom-display-native-budget-full-tests.log`. Second exécutable
authentifié indisponible, explicitement sauté. Aucun nouveau binaire DLL construit
ou installé ; témoin, sauvegardes et livraisons antérieures inchangés.
Le dialogue natif et la résolution indépendante en jeu restent non implémentés.

## Onzième passe — observateur de cycle de vie et essai réversible

Le prochain verrou ne peut pas être levé par des archives hors jeu : il faut
observer les instances, files et threads réellement utilisés. Ajout de
`custom_display_layout.{h,cpp}` et `custom_display_observer.{h,cpp}` : lecture
bornée des champs issus des opérandes natifs corroborés, puis deux trampolines
**sans changement de comportement**. Création/libération originales exactement
une fois, arguments et thread conservés. L'observation n'est installée que si
`SWML_CD_OBSERVE=1`, après authentification et validation des corps/relations/slots,
avant que les hooks UI existants soient publiés. Sans opt-in, aucune découverte
supplémentaire ni patch de moniteur. Le code ne construit aucune cible et ne
modifie aucune définition.

Les tests exercent snapshots valides/inaccessibles, chaînes trop longues ou
injectant des sauts de ligne, refus de layouts altérés, conservation des arguments
et du thread, erreurs du logger et plafond d'événements. Les scripts de test
temporaire vérifient les trois SHA-256, sauvegardent le loader/journal antérieurs,
lancent le jeu avec l'environnement du seul processus enfant, puis restaurent
atomiquement le loader initial. Un reçu persistant permet la reprise après
interruption ; aucun nouveau fichier utilisateur modifié n'est écrasé en reprise.
Neuf tests PowerShell utilisent exclusivement de faux fichiers jetables, dont un
faux exécutable volontairement invalide pour vérifier le rollback sans lancer le jeu.

**16 exécutables C++, deux compilations C et 45 tests Python réussis** :
`build/custom-display-observer-full-tests.log`. Compilation DLL réussie dans
`build/custom-display-observer-build.log` ; **313 exports proxy identiques** à
ceux de la DLL installée. Second exécutable authentifié toujours indisponible.
Le précédent build est conservé sous
`build/backups/custom-display-observer-20261015-182807-654/`.

Paquet : `build/custom-display-observation-20261015/`, DLL SHA-256
`69b0922de942d46911fb439fccda8c1dada62b0be0b1f367cd06975869c25809`.
Le prévol réel `observe.ps1 -Mode Check` réussit : aucun jeu lancé, aucun fichier
installé remplacé. Le lanceur racine `TEST-CustomDisplay.bat` ouvre cet essai.
Instructions : [custom_display_observation_readme.md](custom_display_observation_readme.md).

Correction demandée pour cet essai : démarrage via **`Stormworks/Launcher.exe`**,
jamais lancement direct de `stormworks64.exe`. Le manifeste vérifie également le
SHA-256 du lanceur, tout en conservant l'authentification de l'exécutable du jeu.
Le script attend le jeu réel et la sortie du lanceur avant restauration ; s'il
n'observe aucun jeu sous 120 secondes, il garde le reçu et demande une restauration
manuelle après fermeture, plutôt que remplacer une DLL pendant un démarrage tardif.
Les onze tests du lanceur et les **47 tests Python** passent ; prévol réel `Check`
réussi sans lancement du jeu. Journal :
`build/custom-display-launcher-correction-python-tests.log`.

**Blocage nécessitant maintenant l'utilisateur :** exécuter l'essai dans le jeu,
avec le témoin existant, les trois qualités d'écran et les retours éditeur/reload,
puis quitter complètement pour récupérer les journaux et restaurer le loader.
Ce n'est ni une validation de résolution personnalisée ni le mod terminé.
Import incrémental, dialogue natif et branchement effectif des cibles restent à
réaliser après les preuves requises. Installation, témoin et sauvegardes non
modifiés durant la préparation ; anciennes livraisons intactes.

## Douzième passe — résultat du témoin, admission multi-thread et premier pont restreint

Le test demandé a été effectué par l'utilisateur. Le run exploitable
`run-20261015-185932-ca79c36a09124319a3b53dd1674f9596` comporte **105 appels**
(48 créations/57 libérations), tous appariés et lisibles. Trois threads natifs
utilisent la même file dans cette session ; il n'existe donc pas de contrat de
thread de mod unique. Le témoin exact est observé en 32×32 aux trois qualités,
ainsi qu'en qualité native `0` (branche ×2). Les moniteurs stock 1×3 restent
96×32. Le reçu de restauration et le hash réel de la DLL installée confirment
`c882c4b1d4b8eb73f59801c82d743a874328c546f98c069cf007c0377c1c89cb`.

L'audit reproductible et ses cas de corruption/troncature sont dans
`tools/custom_display_observation_report.py` et ses tests ; résultat conservé
sous `build/custom-display-observation-runtime-report.json`. Un résultat d'audit
n'autorise jamais l'activation d'un build ou d'un hook. La file observée n'est
pas un événement de succès d'allocation/drainage et les adresses ne constituent
pas des identités de durée de vie.

Ajout de `src/custom_display_admission.h` : contribution cumulée par créations,
arbitrée atomiquement entre producteurs, sans restitution de crédit sur une
libération différée non certifiée. Tests sous contention, budgets exacts/moins un,
refus atomiques et séquences de qualités/rechargements. Ce n'est pas une mesure
GPU ni un certificat de budget natif global. Les nouveaux exports Ghidra relient
le slot de suppression graphique aux listes/drains de fin de frame sans prétendre
avoir observé leur thread ou la fin effective de leurs travaux dans cette session.

Un premier pont **extrêmement restreint et doublement opt-in** est maintenant
compilé : seul `swml_cd_v1_b1x1_p24x16`, lié à son archive exacte SHA-256 épinglée,
peut demander 24×16 dans le chemin natif de création. Type, identité, archive,
bornes 1×1 et contrats complets sont contrôlés ; les meshes restent byte-identiques
aux meshes stock 1×1. Tous les attachments sont plus petits que la baseline native
à chaque qualité. Aucun appel GL, callback de mod déplacé ou patch de définition
partagée. Le témoin et les moniteurs stock conservent le chemin original.
Destruction native inchangée. Le cap cumulé de 8 Mio propre à cet essai ne rend
aucun crédit ; un refus avec handles vides retrouve le calcul stock, et aucun
calcul ne remplace les dimensions d'une paire encore vivante.

**Ce n'est ni le pont général 480×272 ni le dialogue de configuration.** Provenance
générale des archives embarquées, synchronisation et budget global/différé ne
sont pas déclarés certifiés. L'essai est exclusivement local/hors multijoueur.
Voir [custom_display_targets_contract.md](custom_display_targets_contract.md).

**17 exécutables C++, deux compilations C et 82 tests Python réussis**, sans test
Python sauté dans cette passe ; second exécutable authentifié toujours absent.
Journaux : `build/custom-display-small-resolution-full-tests.log` et
`build/custom-display-small-resolution-build.log`. Les tests vérifient aussi le
pin SHA-256 natif, l'exclusion d'écriture et les restaurations/échecs du lanceur
sur de faux fichiers jetables, sans lancer le jeu.

Paquet : `build/custom-display-small-resolution-20261015/`, DLL SHA-256
`b4517d824ca10ac7f08bc3e63b958bd9225454243b8929525b262bd5de28ab9e` ;
**313 exports proxy identiques** au loader installé. Précédent build conservé sous
`build/backups/custom-display-small-resolution-20261015-192750-108/`.
Prévol réel réussi sans installation ni lancement. Le lanceur
`TEST-Resolution-CustomDisplay.bat` crée temporairement un mod distinct et une
copie caméra `SWML_Resolution_Test_24x16`, puis restaure le loader et ne retire que
ses fichiers inchangés. Aucun fichier utilisateur existant n'est écrasé.
La copie provient de l'autosauvegarde caméra vérifiée, SHA-256
`647667eb18e1544c61c8ecd788c2d2fdb4a1894dce74f26d28738f4a71715b7e` ;
les câbles/propriétés/positions sont conservés textuellement, seul l'identifiant
du témoin devient celui du nouvel écran.

**Prochain blocage nécessitant l'utilisateur :** charger ce véhicule isolé via
le nouveau lanceur, vérifier caméra, trois qualités, retour éditeur et respawn,
puis quitter pour récupérer les journaux et restaurer les fichiers. Aucun essai
24×16 en jeu n'est encore revendiqué. Instructions :
[custom_display_small_resolution_readme.md](custom_display_small_resolution_readme.md).

## Treizième passe — caméra 24×16 confirmée et fenêtre native retracée

L'utilisateur confirme que l'image fonctionne dans le nouvel essai. Le journal
`run-20261015-194800-d792ab877ea24722b400f2655f4ea878/observation.log`, sous
`build/custom-display-small-resolution-20261015/runs/`, SHA-256
`da8b40a1b49ec621ce62603e47ed141e46ee8540ce8a924ec5a53e9d528c0d17`,
contient **87 appels appariés : 38 créations / 49 libérations**.

L'écran exact 1×1 est créé trois fois en **24×16** : deux fois en qualité native
`3` (×4) et une fois en qualité `1` (×1). Cinq libérations du spécimen sont
observées. Les trois admissions `CDRES result=0` correspondent aux appels ;
la contribution cumulée finale vaut **165 888 octets**, sans remboursement.
Les moniteurs stock 1×3 restent 96×32 et le premier témoin reste 32×32.
Le reçu `restored.json` et le SHA-256 réel de `Stormworks/iphlpapi.dll` confirment
la restauration du loader `c882c4b1…cb` ; aucun fichier d'essai édité n'est retenu.

**Réussite de base, pas couverture complète :** caméra confirmée par l'utilisateur,
snapshots et admissions cohérents. La qualité native `2` n'est pas observée pour
le nouveau spécimen. L'audit strict reste donc `audit_passed=false` avec ce seul
manque ; `lifecycle_integrity_passed=true` distingue désormais l'intégrité des
preuves de la couverture des réglages. Aucun résultat n'est fabriqué en fusionnant
les deux expériences.

L'auditeur vérifie maintenant aussi l'activation exacte du pont épinglé et chaque
admission à l'intérieur de l'appel de création du même thread : moniteur, file,
qualité, résultat et contribution cumulée. Les snapshots atomiques de contribution
ne sont pas assimilés à des reçus par appel ni à une mesure GPU. Rapport conservé :
`build/custom-display-small-resolution-runtime-report.json`.

La reconnaissance suivante retrouve les vrais dialogues confirmer/annuler et
champs de saisie, leur ownership et le chemin UI de l'éditeur de véhicules.
La surface éditeur du SDK actuel concerne les **microcontrôleurs**, pas les
véhicules ; elle ne sera pas détournée. Les dialogues basic/non-basic ont des
comportements de capture distincts. De plus, la sélection de définition est
appelée par le catalogue, la hotbar, la souris et la restauration d'état : un hook
global ouvrirait la fenêtre au mauvais moment. Détails et preuves conservées :
[custom_display_dialog_contract.md](custom_display_dialog_contract.md).

**Aucun dialogue n'est encore activé**, aucun nouvel appel d'import ni nouvelle
DLL installée dans cette passe. Provenance générale, import à la demande, budget
global/différé, Lua/tactile et géométrie rectangulaire restent non certifiés.

**89 tests Python réussis**, sans skip :
`build/custom-display-resolution-result-python-tests.log`. L'ancien journal
pass-through conserve son audit strict réussi. Le code C++ et la DLL n'ont pas
changé ; la suite C++ n'a pas été relancée dans cette passe.

## Quatorzième passe — saisies natives, lecture sûre et observation véhicule

Le workflow B possède maintenant un modèle C++ de validation à quatre valeurs
indépendantes et une durée de vie de brouillon : annulation, teardown, jetons
périmés et confirmations doubles sont testés hors moteur. Le plan est contrôlé
au multiplicateur natif maximal ×4 avec une politique explicite ; ce n'est ni
une nouvelle limite autorisée ni une admission GPU.

L'analyse distingue le texte initial du wrapper du buffer réellement édité dans
son enfant, confirme le filtrage du collage et la consommation de chaînes moteur
par les helpers. Le lecteur C++ copie les quatre buffers sur le thread propriétaire,
après contrôle du jeton, des types et des parents, avec rejet atomique des lectures
inaccessibles/descripteurs malformés. Aucun constructeur consommant une chaîne ne
reçoit de faux descripteur STL/littéral.

Deux groupes de découverte séparés (256 widgets, 512 observation véhicule)
complètent les contrats sans remplacer les preuves existantes. Les offsets sont
extraits des opérandes et recoupés, les slots des tables retrouvées ; code, appels,
RTTI ou slots modifiés et build inconnu restent bloqués. L'analyse clavier trouve
une différence importante entre le menu principal et le handler propre au véhicule :
la capture d'un dialogue basic ne peut pas être extrapolée de l'un à l'autre.

L'observateur véhicule optionnel transmet les six appels natifs sans modification
et sans nouvelle fenêtre. Il n'a aucun rapport avec `SW_SURFACE_EDITOR`, dont la
signification SDK reste microcontrôleur. Le nouveau test réversible
`TEST-Dialogue-CustomDisplay.bat` est préparé pour vérifier en jeu deux cycles de
vie de l'éditeur et l'action catalogue stock, **pas pour répéter le test caméra**.
Il ne crée aucun mod/fixture/véhicule et désactive le pont de résolution.

**20 exécutables C++, deux compilations C et 116 tests Python réussis** dans
`build/custom-display-vehicle-observer-tests.log`, sans test Python sauté.
Le second exécutable authentifié reste indisponible ; les services SDK natifs
non certifiés conservent leurs limitations. Build DLL réussi dans
`build/custom-display-vehicle-observer-build.log`, **313 exports proxy identiques**
au loader installé. Celui-ci reste inchangé, SHA-256
`c882c4b1d4b8eb73f59801c82d743a874328c546f98c069cf007c0377c1c89cb`.
Paquet créé sans installation : `build/custom-display-vehicle-ui-20261015/`,
DLL SHA-256 `72365cd7603330741d64f8fa880c2957b68d82e2e0c0ef00a535d43acfe4f6b1`.
Le prévol réel `observe.ps1 -Mode Check` est réussi, jeu arrêté ; aucun lancement.

Les tests du lecteur et de l'observateur utilisent de faux objets/appels natifs ;
ceux de l'auditeur utilisent des traces synthétiques. **Aucun dialogue Custom
Display installé et aucune nouvelle observation véhicule réelle à ce stade.**
Les verrous d'import/provenance/persistance, de capture des entrées et de budget
global ne sont pas levés par ces tests. Détails et procédure :
[custom_display_dialog_contract.md](custom_display_dialog_contract.md) et
[custom_display_vehicle_observation_readme.md](custom_display_vehicle_observation_readme.md).

## Quinzième passe — vrais clics catalogue et interface conservée dans la partie

Les deux essais utilisateur sont conservés séparément. Le premier observe un
cycle UI complet, sans action catalogue ; le second observe **deux actions `0x61`**
sur un même état/contexte, puis un teardown/destructeur UI inclus dans le teardown
de l'état. Les deux restaurations sont complètes et le hash du loader original
a été relu sur disque. Aucun résultat de sessions distinctes n'est additionné.

L'init UI est sur un worker, l'utilisation et la destruction sur un autre thread.
L'auditeur distingue ces phases, exige la fin de l'init puis un premier update
attaché et refuse les changements de thread pendant l'utilisation. La trace du
second essai passe l'intégrité, pas la couverture de deux cycles complets :
`build/custom-display-vehicle-ui-phase-20261015/runs/run-20261015-214646-87037e4b4ef14afda88ef3452ce4a947/vehicle-phase-audit.json`.

L'analyse supplémentaire établit que l'état applicatif jeu construit et initialise
**tous** les sous-états une fois, et conserve le sous-état véhicule pendant la
partie. Le switch actif appelle les slots d'entrée/sortie, sans détruire l'UI.
**Les anciennes consignes de deux passages à l'établi étaient insuffisantes** ;
il faut un retour au menu principal puis un rechargement pour un second cycle UI.
`FUN_1407cfce0` est une entrée/activation, pas un update par frame. Son init de
catalogue ne certifie toujours pas un rafraîchissement arbitraire pendant l'édition.

Un groupe additionnel 1024 certifie les corps/relations de l'entrée, de la sortie
et du switch, les slots étant extraits de ses opérandes et recoupés avec la vtable.
Les groupes antérieurs sont préservés. L'observateur v2 ajoute deux wrappers
pass-through sans widgets/entrées/import/placement, afin de vérifier les vraies
limites d'activité où une future session de dialogue doit être invalidée.
L'auditeur compte séparément cycles UI et visites actives ; le protocole v1 ne
peut pas certifier l'activité. Le seuil des deux cycles UI n'est pas abaissé.

La suite complète passe : **20 exécutables C++, deux compilations C et 128 tests
Python**, sans saut Python (`build/custom-display-vehicle-activity-tests.log`).
Le second exécutable authentifié demeure indisponible. La DLL v2 est compilée
(`build/custom-display-vehicle-activity-build.log`), mais ses deux nouveaux hooks
n'ont **pas encore été essayés en jeu**. Aucun dialogue Custom Display n'est installé.
Les verrous de capture, d'ownership des chaînes/widgets, de provenance, de
persistance et de résolution générale restent distincts de cette observation.

Le lanceur racine pointe maintenant vers le paquet create-new
`build/custom-display-vehicle-activity-reload-20261015/`, protocole 2,
DLL SHA-256 `1e041444607b0d8bef5ea6bcefcc90f051f899fb494fdc89390ecc574f25bf75`.
Ses 313 exports sont identiques au loader original. Prévol réel réussi, jeu
arrêté, **sans installation ni lancement** ; manifeste `installed=false`,
`runtimeValidated=false`. Le message console et le LIRE-MOI demandent maintenant
explicitement menu principal puis rechargement de la partie sans quitter le jeu.
Les anciens paquets et leurs journaux restent intacts ; le premier paquet v2
`custom-display-vehicle-activity-20261015` est conservé mais n'est pas le lanceur
actif (son message console utilisait encore les anciennes consignes).

## Seizième passe — constructeur natif, contrôleur et ciblage catalogue codés

Suite à la demande utilisateur, **pas de nouvel essai manuel d'observation** :
les vérifications sont regroupées hors jeu et le travail revient au workflow B.

Le constructeur appelle les vrais helpers basic-dialog et editbox du moteur,
avec quatre valeurs initiales indépendantes, leurs libellés et le filtre digits.
Les bindings et tailles sont redécouverts/revalidés à partir du code stock.
Le helper de chaînes distingue explicitement le **basic qui emprunte** ses
deux entrées et les **editboxes/helpers qui consomment** les leurs, sans double
libération des descripteurs natifs qui restent pendants.

Le contrôleur raccorde lecture vivante et validation : erreurs sans fermer,
confirmation unique, annulation sans demande, invalidation avant sortie et
teardown, Escape natif, protection des anciens jetons et appels réentrants.
Les branches sont possédées par l'arbre du jeu ; les échecs locaux pendant la
construction laissent une branche fermée au lieu d'un faux rollback destructif.

La lecture du clic catalogue cible désormais uniquement `0x61`, en vérifiant
état/thread/interface/catalogue/ligne/définition par identité exacte. Une ligne
stock partageant une définition n'est pas reconnue comme Custom Display.
Les autres actions, y compris hotbar/restauration, ne lisent pas ce chemin.

**Limite importante :** ces chemins sont compilés mais pas raccordés aux hooks
de production. La fenêtre n'a pas encore été affichée/essayée en jeu, son layout
visuel n'est donc pas certifié. Le vrai enregistrement de la ligne, l'ouverture
différée, la provenance des actions des boutons et la capture clavier/souris
doivent être terminés avant activation. Aucun placement/import n'est ajouté.
Cela fait avancer les étapes 4/5, sans déclarer artificiellement une deuxième
étape terminée de la liste des huit étapes du résultat final.

Fichiers principaux : `custom_display_dialog_native.*`,
`custom_display_dialog_native_bindings.cpp`, `custom_display_engine_string.h`,
`custom_display_dialog_controller.*`, `custom_display_catalogue.*`.
Voir `custom_display_dialog_contract.md` pour les obligations restantes et
`tests/custom_display_dialog_native_tests.cpp` pour les tests sans jeu.

Validation finale de cette passe : **21 exécutables C++, deux compilations C,
130 tests Python réussis**, aucun saut Python. La vérification du second
exécutable authentifié reste sautée car il est indisponible. Tests de mutations
sur l'image PE mappée : tailles, opérandes catalogue, redirection de callees,
tables/RTTI, résultats fabriqués/périmés et build non revu rejetés. Faux moteur :
transferts de chaînes, ordre init/filtre, allocations nulles et chaînes manquantes,
annulation/validation/sortie/Escape/teardown, réentrance et pointeurs devenus
inaccessibles. Les tests synthétiques ne certifient pas la capture native en jeu.

Compilation DLL réussie, SHA-256
`874bd3e17d0fab1736b3ae91da6133a797c3d2b61e1de5158b52e905298fda57`,
313 exports identiques à l'original. Logs :
`build/custom-display-dialog-native-tests.log` et
`build/custom-display-dialog-native-build.log`.
Le corpus généré conserve 132 entrées primaires, 397 témoins et 15 tables RTTI ;
les anciens groupes sont préservés par merge et tests de non-régression.
**Aucun lancement/installation/fixture ni changement du lanceur racine** dans
cette passe. Le loader installé reste l'original
`c882c4b1d4b8eb73f59801c82d743a874328c546f98c069cf007c0377c1c89cb`,
hash relu sur disque. Les anciens paquets d'observation restent intacts.

## Dix-septième passe — boutons natifs à tickets immuables et drain différé

Terminée le 16 octobre 2026, sans essai manuel supplémentaire. Le basic-dialog
fournit déjà des point-buttons avec callback natif ; cette voie évite de choisir
des numéros de confirmation/annulation supposés libres dans le switch véhicule.
Les anciens chemins SDK/rectangle buttons et leur authentification sont inchangés.

- Nouveau groupe 8192 : corps/relations et RTTI, opérandes recoupés, bindings en
  lecture seule. Corpus généré : **136 primaires, 398 témoins, 16 tables RTTI**.
- Construction callback-only fermée ; `Prepare` puis `Show` séparés pour lier
  les tickets au vrai jeton **avant** de montrer la fenêtre.
- Liaison native vérifiant les deux boutons avant toute écriture, sans rebinding.
  Tickets immuables, conservés jusqu'à la fin du module, maximum 256 ouvertures
  par processus sans recyclage ; un arbre distinct par ouverture, propriété native.
- Thunk sans lecture de widget ni effet moteur ; commande différée, doublons
  fusionnés, annulation prioritaire. Le drain raccorde réellement les callbacks
  au contrôleur/lecteur/validation ; fermeture après désarmement, retry limité au
  même jeton, anciennes ouvertures inertes même après réemploi d'adresse/jeton.

**Blocage mieux identifié, pas contourné :** le handler texte natif publie une
action de soumission sur Entrée même si elle vaut zéro. Cette touche doit être
interceptée localement par la capture véhicule avant tout affichage ; zéro n'est
pas déclaré libre. La capture complète, la barrière de relâchement, le vrai pin
catalogue/enregistrement, l'ouverture hors parcours natif et le placement restent
à raccorder. Aucun hook de production n'appelle les nouveaux chemins, aucun
dialogue affiché en jeu ; aucune nouvelle étape finale déclarée terminée.

Vérifications regroupées : **21 exécutables C++, deux compilations C, 130 tests
Python réussis**. Tests de callbacks et de contrôle de provenance/lifetime sur
faux moteur, mutations de l'image PE et résultats de découverte rejetés ; second
exécutable toujours indisponible. DLL compilée, **313 exports identiques** au
loader original ; SHA-256
`6dc436edda97314d50389ecb6773be18668fb58b10872423177450b8c0df75fd`.
Logs : `build/custom-display-dialog-callback-tests.log` et
`build/custom-display-dialog-callback-build.log`. Ancienne DLL conservée sous
`build/backups/pre-dialog-callbacks-20261015-235837-687/`.

**Aucun lancement/installation/fixture ni changement du lanceur racine.** Hash
du loader installé relu :
`c882c4b1d4b8eb73f59801c82d743a874328c546f98c069cf007c0377c1c89cb`.
Le lanceur racine demeure un test d'observation v2, **pas un test du dialogue**.

## Dix-huitième passe — entrées locales et drain du contrôleur raccordés hors jeu

Terminée le 16 octobre 2026. `custom_display_dialog_input.cpp` conserve l'édition
native des quatre champs tout en dirigeant Entrée/pavé numérique vers les commandes
locales, sans action zéro dans la file véhicule. Escape utilise le binding natif
courant et reste prioritaire sur la confirmation. Le contrôleur est réellement
appelé par le drain de cette session, pas depuis le callback natif.

- Groupe authentifié **16384** ; corpus **143 primaires, 476 témoins, 16 tables RTTI**.
  Layouts extraits/recoupés, résultats périmés/fabriqués et mutations rejetés.
- `Begin` capture avant ouverture ; `Activate` attend la publication du modal
  exact et un nouveau snapshot neutre avant d'armer. Pas de fermeture fictive
  pendant l'attente du premier update basic.
- Fermeture/validation sous capture, retry après erreur, répétitions d'Entrée
  contenues, changements de focus et relâchements suivis, native-close prioritaire
  sur les confirmations en attente, drainage jusqu'au relâchement complet.
- Refus des anciens destinataires stock et du verrou pointeur encore actif.
  Le helper racine touche son ancien destinataire avant le nouveau hit-test :
  pas d'appel vers ce widget derrière la fenêtre, pas de nettoyage manuel d'offsets.
- Défaut = capture conservée jusqu'au retrait propriétaire ; teardown sans lecture
  d'objets et arbre toujours possédé par le parent natif. SDK existant inchangé.

**Pas encore un chemin activable en jeu.** Restent la source native exhaustive
des entrées maintenues, la libération native sûre du destinataire catalogue,
la transaction de toutes les routes d'entrée et le point sûr propriétaire.
La provenance/enregistrement du vrai composant, le raccordement du cas `0x61`,
le choix de définition et le placement/persistence sont toujours à terminer.
Le getter de contrôles reste ambigu sur le scan à métadonnées inconnues ; aucun
fallback ni désactivation de l'authentification. Aucune nouvelle étape finale
déclarée terminée, aucun test manuel demandé.

Vérifications finales regroupées : **21 exécutables C++, deux compilations C,
130 tests Python réussis** ; second exécutable authentifié toujours indisponible.
Logs : `build/custom-display-dialog-input-tests.log` et
`build/custom-display-dialog-input-build.log`. DLL compilée, **313 exports identiques**
à l'original ; SHA-256
`fde37c8c9bca858f37c76fe4f7082321cdd3b9096def0e14de0d4948dd1247b4`.
L'ancien build callback est conservé dans `build/backups/pre-dialog-input-*/`.

**Aucun lancement, installation, fixture ni modification du lanceur racine.**
Hash installé relu et inchangé :
`c882c4b1d4b8eb73f59801c82d743a874328c546f98c069cf007c0377c1c89cb`.
`TEST-Dialogue-CustomDisplay.bat` reste un observateur v2, pas le test du dialogue.

## Dix-neuvième passe — source native des états et retrait sous capture

16 octobre 2026. Le lecteur de snapshot n'est plus seulement une structure injectée :
`CustomDisplayNativeHeldInputReader` lit les tableaux du host/fenêtre natifs, avec
offsets et bornes extraits des instructions authentifiées. Les deux espaces de
codes sont séparés : **349 touches natives / huit boutons souris**, pas les 256
codes de l'édition texte véhicule. Les états sticky sont conservativement retenus,
les états invalides/copies tronquées/changements de host sont refusés et aucun
ancien snapshot neutre ne survit à un échec.

- Groupe **32768**, corpus **147 primaires, 481 témoins, 16 tables RTTI** ; merge sans
  affaiblissement, résultats périmés/fabriqués et mutations des bornes refusés.
- `Advance` raccorde le lecteur natif au contrôleur différé : activation, validation,
  attente de fermeture/dépublication, retrait des destinataires, libération. Un échec
  de source conserve la commande et la capture. Un nouvel instantané après callbacks
  empêche de libérer à partir d'une neutralité devenue périmée.
- La routine native de retrait focus/hover est liée sans exécution au binding.
  Son adaptateur de fermeture ne notifie que nos descendants, sous capture, avec
  verrou déjà nul ; il ne nettoie aucun champ moteur à la main.
- `Activate` revérifie l'absence de destinataire/hover stock et de verrou.
  `Release` exige aussi le modal dépublié et les destinataires natifs retirés.
  Un ancien hover stock n'est jamais envoyé au helper pointeur, même sans appui.

**Toujours pas affiché en jeu.** Le host exact, les hooks exhaustifs, le cycle de
vie et le point sûr propriétaire restent à raccorder/certifier. Le retrait stock
avant ouverture et le drainage d'un verrou natif actif restent bloquants : la
routine trouvée ne réinitialise pas ce verrou. Les doubles lectures ne certifient
pas une synchronisation, et les tableaux clavier/souris ne certifient pas les
autres sources de commandes. Enregistrement/provenance catalogue, placement et
persistance restent à terminer. Aucun test manuel supplémentaire demandé.

Preuves conservées sous `tools/ghidra/custom_display_input_*`, manifeste
`custom_display_native_input_state_symbols.json` et contrats compilés depuis
`tools/custom_display_native_input_state_contracts.json`. Aucun jeu lancé, aucune
installation ni modification du lanceur racine pendant cette passe.

Vérifications regroupées réussies : **21 exécutables C++, deux compilations C et
131 tests Python** ; tests de découverte sur image mappée, jamais exécutée. Le
second exécutable authentifié reste indisponible. Logs :
`build/custom-display-native-input-state-tests.log`,
`build/custom-display-native-input-state-build.log` et
`build/custom-display-native-input-state-evidence.log`.
DLL construite, non installée : SHA-256
`3455d31685e3d914961d1677c2cfc995b8f5f58f51a119ed8dbc5369f1d39abd`,
**313 exports identiques** à l'original. Build précédent conservé dans
`build/backups/pre-native-input-state-*/`. Hash installé relu, toujours
`c882c4b1d4b8eb73f59801c82d743a874328c546f98c069cf007c0377c1c89cb`.

## Artefacts conservés

Sous `tools/ghidra/` :

- `CustomDisplayIdentity.java` et `custom_display_identity.txt` ;
- `custom_display_inventory.commands.txt` / `.txt` ;
- `custom_display_editor.commands.txt` / `.txt` ;
- `custom_display_paths.commands.txt` / `.txt` ;
- `custom_display_selection.commands.txt` / `.txt` ;
- `custom_display_catalogue.commands.txt` / `.txt` ;
- `custom_display_placement.commands.txt` / `.txt` ;
- `custom_display_events.commands.txt` / `.txt` ;
- `custom_display_registry.commands.txt` / `.txt` ;
- `custom_display_contracts.commands.txt` / `.txt`.

La troisième passe ajoute les lots appariés `.commands.txt` / `.txt` :
`dispatch`, `click`, `actions`, `handoff`, `select_definition`, `selected_uses`,
`creation`, `persistence`, `save_slot`, `serialization`, `load_contract`,
`base_serialization`, `identity_roundtrip`, `body_roundtrip`,
`component_roundtrip`, `variant_identity`, `reflections`, `mod_sources`,
`mod_population`, `mod_entry`, `mod_activation`, `visibility`,
`resource_lifecycle`, `registry_timing` (préfixe `custom_display_`).

Les lots utilisent le script `Dump.java` existant. Ils ne génèrent ni profils
activables ni patches runtime. Cette étape est une analyse statique du seul build
local, pas un test de compatibilité future ni un essai de placement en jeu.

La quatrième passe ajoute les lots `custom_display_resolution_lifecycle`,
`custom_display_target_recreation`, `custom_display_quality_refresh`, ainsi que
`FindInstructionUses.java` et `custom_display_resolution_field_uses.txt`.
