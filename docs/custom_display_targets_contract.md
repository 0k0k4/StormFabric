# Custom Display — cibles vidéo indépendantes (expérimental)

## Contrat statique observé

Repères de l'exécutable de référence uniquement ; aucun de ces nombres ne doit
servir d'adresse de secours sur un autre build.

- `FUN_140366010` reçoit moniteur, file native de ressources différées, qualité.
  La qualité `1` donne ×1, `3` donne ×4, toute autre valeur donne ×2.
  Il écrit largeur/hauteur logiques à `+0x4b8/+0x4bc`, le multiplicateur à
  `+0x4c0`, puis demande deux cibles via `FUN_14020da70` :
  `(largeur × multiplicateur, hauteur × multiplicateur, 5, 1, +0x4c8)` et
  `(largeur, hauteur, 5, 1, +0x4d8)`.
- `FUN_14020da70` reçoit la file, quatre entiers 32 bits et un pointeur de handle.
   Les entiers finaux sont les formats couleur et profondeur, pas deux flags.
   Il ne fait rien si le premier pointeur du handle est déjà non nul : ce n'est
  **pas** une opération de redimensionnement. Sinon, il mémorise la file dans le
  second pointeur, crée un wrapper moteur et met en file un événement de création.
  Son retour est `void` ; un handle publié n'est pas une texture GPU déjà prête.
- `FUN_140366100` met les wrappers en file pour destruction native puis efface les
  deux pointeurs de chaque handle. Cela ne signifie pas que la mémoire graphique
  a déjà été libérée. Répéter libération/création sans vider la file peut augmenter
  la mémoire en vol au-delà du budget des deux nouvelles images.

Il ne faut donc ni modifier les dimensions après avoir créé les cibles d'origine,
ni remplacer ces handles par des identifiants GL, ni libérer les wrappers avec le
CRT du loader. Il faut intervenir à la création native, sur la file/thread moteur
corrects, avec l'ancienne paire libérée selon le contrat natif.

## Consommateurs trouvés, essais personnalisés encore absents

- `FUN_140366a90` publie les dimensions logiques depuis `+0x4b8/+0x4bc` dans un
  état de signal ; il reste à certifier leur chemin complet jusqu'au Lua.
- `FUN_140366e90` copie ces champs dans les données de rendu.
- `FUN_140367990` et `FUN_140367ae0` convertissent un point physique vers ces
  dimensions ; les comparaisons natives autorisent notamment la borne inclusive.
  Il ne faut pas inventer un clamp différent sans valider le comportement tactile.

Ces observations ne valident ni `screen.getWidth/getHeight`, ni le dessin Lua, ni
le tactile à une résolution personnalisée. Le test caméra utilisateur porte sur
le témoin inchangé de 32×32.

## Formats, comptabilité et libération différée (dixième passe)

Le chemin `14020cf60 → 1401b4390 → 1401b0290 → 1401afeb0` établit, sur
l'exécutable de référence, que `(5, 1)` demande **deux attachments par cible** :
couleur de format interne `0x881a` (8 octets/pixel dans la comptabilité moteur)
et profondeur `0x1902` (4 octets/pixel comptabilisés). Le second entier `1`
n'est donc pas un simple flag. La comptabilité minimale des deux cibles est :

`12 × pixelWidth × pixelHeight × (supersampling² + 1)`.

Le modèle préparatoire utilisait auparavant 4 octets/pixel et sous-estimait ce
total par trois. Il est corrigé : `maxTargetBytes` / `targetBytes` remplacent
les noms internes `maxRgbaBytes` / `rgbaBytes`. Pour 480×272 en qualité ×4,
le total comptabilisé est 26 634 240 octets, hors surcoûts et anciennes ressources.
Ce n'est pas une mesure de mémoire GPU : le stockage réel de la profondeur
non dimensionnée dépend notamment du pilote.

`1401b0290` accumule couleur puis profondeur dans un champ signé 32 bits par
ressource. Le plan refuse désormais tout dépassement de ce compteur avant les
requêtes (`NativeAccountingRange`), même si la politique uint64 autorise plus.
Le compteur global natif est lui aussi sur 32 bits : le contrôle par cible ne
certifie donc toujours pas un budget global sûr.

La destruction suit `14020cc40 → wrapper.vtable[1] → 14020cbf0 → 1408d29b0`.
Cette dernière décrémente la référence de la ressource backend et la déplace ou
la retire des listes du gestionnaire suivant son état. Effacer les handles du
moniteur ne prouve ni l'exécution de cet événement ni la suppression des textures.
`1401b03e0` effectue les suppressions graphiques et retire les octets comptabilisés ;
son déclenchement effectif doit encore être relié au drainage de la file et au
thread propriétaire. Aucun callback de fin de libération n'est inventé.

Preuves : lots `custom_display_queue_{ownership,dispatch,execution,backend,release}`,
`custom_display_backend_{lifetime,cleanup}`, `custom_display_attachment_formats`
et inventaire `custom_display_queue_inventory.txt`. Ces éléments restent statiques,
extraits du projet privé en lecture seule ; ils ne certifient pas une ABI runtime.

## Code disponible et barrières restantes

`custom_display_targets.h` implémente l'ordre de requêtes indépendamment des
offsets natifs. Il exige une politique explicite, refuse les handles occupés ou
les champs aliasés et ne modifie rien sur refus de validation. `Queued` signifie
uniquement que les deux fonctions de requête ont été appelées sans retour de
statut ; ce n'est pas une preuve de création GPU. Les tests ne simulent pas une
garantie de rollback que le moteur ne fournit pas.

Le futur pont devra encore :

1. authentifier l'exécutable et les contrats de découverte, puis certifier les
   layouts, instances de moniteur/définition, identité de configuration et file ;
2. s'installer au bon moment sans conflit avec des slots déjà patchés et sans
   retenir de callbacks dans une DLL de mod déchargeable ;
3. cibler exclusivement les nouvelles définitions Custom Display, jamais les
   moniteurs d'origine ou un identifiant de même nom non vérifié ;
4. couvrir création, changement de qualité, rechargement et destruction, avec
   limites GPU et budget global/différé établis ;
5. valider Lua, rendu, caméra et tactile avant d'exposer une API stable.

La découverte supplémentaire (groupes 64/128) est en lecture seule et n'installe
pas ce pont. L'authentification, le SDK et les hooks vidéo existants sont inchangés.
Les archives configurées peuvent désormais être construites **hors jeu** avec
`tools/custom_display_assets.py` (voir `custom_display_geometry_contract.md`).
Leur identité encode les pixels, mais ne change pas à elle seule le calcul natif
de 32 pixels par bloc : le présent pont reste nécessaire.
Preuves : `tools/ghidra/custom_display_target_backend.*`,
`tools/ghidra/custom_display_runtime_symbols.json`,
`tools/custom_display_runtime_contracts.json`.

## Observation runtime préparée, premier essai requis

`custom_display_layout.cpp` extrait les déplacements depuis les opérandes des
instructions exportées, uniquement après correspondance complète des corps,
témoins d'appels et slots RTTI. Il croise les accès création/libération et les
relations entre champs. Aucun offset d'objet de secours ni activation de build
inconnu n'est ajouté. Les tests rejettent aussi un résultat de découverte devenu
obsolète après mutation du code ou d'un slot.

`custom_display_observer.cpp` peut installer **deux trampolines pass-through**
sur création/libération, avec authentification complète avant publication et
opt-in strict `SWML_CD_OBSERVE=1`. Il observe thread, file reçue, pile native,
identités de définition/archive, dimensions et pointeurs des deux handles.
Chaque fonction native est appelée exactement une fois avec ses arguments
inchangés. Aucune routine de queue n'est appelée par l'observateur lui-même,
aucun wrapper n'est déréférencé après libération, aucune résolution n'est changée.
Les copies utilisent `ReadProcessMemory`, des chaînes ASCII bornées et un plafond
de 4 096 événements. Aucune API SDK ni callback dans un plugin déchargeable.

État initial de cette passe : code packagé dans
`build/custom-display-observation-20261015/`, avec lanceur temporaire et
restauration vérifiée du loader initial. Le prévol réel en mode `Check` réussit
sans remplacement de DLL ni lancement du jeu. Voir
[custom_display_observation_readme.md](custom_display_observation_readme.md).
Le test utilisateur doit produire les appels réels de placement, changement de
qualité, retour éditeur, rechargement et destruction avant tout remplacement du
calcul de résolution. Ces observations ne suffiront pas à prétendre certifier
un budget GPU global ou la fin effective d'une libération différée.

## Résultat runtime du témoin et essai de résolution restreint (douzième passe)

L'utilisateur a effectué l'essai. Le journal valide est
`build/custom-display-observation-20261015/runs/run-20261015-185932-ca79c36a09124319a3b53dd1674f9596/observation.log`
(SHA-256 `d8e864566f9635aba4add4ac71c4d9d8a507dc23151917675fd418d4e6cae554`).
Le premier run conservé ne contient aucun événement exploitable ; il n'est pas
fusionné avec le second pour fabriquer une couverture.

Audit reproductible : `tools/custom_display_observation_report.py <journal>`.
Rapport : `build/custom-display-observation-runtime-report.json`. Il vérifie
chaque paire avant/après, thread/file/identité stables dans l'appel, corps de
handle vide à la création, managers publiés égaux à la file et effacement natif
à la libération. Résultat : **105 appels, 48 créations, 57 libérations**, aucune
paire manquante ni snapshot illisible ; trois threads (11260, 30680, 43896), une
file observée. Le témoin exact est créé huit fois et libéré neuf fois, avec
qualités `0/1/2/3` et résolution inchangée `32×32`, multiplicateurs `2/1/2/4`.
Les moniteurs `monitor_1x3` restent à `96×32`. Dix libérations entrent déjà vides :
il serait faux de les compter comme dix allocations GPU détruites.

Ces mesures certifient le comportement observé des slots, **pas** l'exclusivité
d'un thread unique, une absence de concurrence inter-appels, la vidange du backend
ou un succès d'allocation graphique. Les adresses peuvent être réutilisées après
destruction ; aucun pointeur ne devient une identité persistante par cet audit.

Compléments statiques en lecture seule : `custom_display_backend_drain`,
`custom_display_backend_drain_followup`, `custom_display_frame_resource_order`.
La vtable `mm_resource_target` expose création à `+8` et suppression graphique à
`+0x10`. `FUN_1401b3cc0` consomme notamment la liste de cibles à `+0xf0`, appelle
le slot `+0x10`, puis retire/détruit l'objet via `FUN_1401aef30`. Le chemin de
fin de frame `FUN_140897290` rejoint ce drain après `FUN_1401b4bf0`. Cela relie
statiquement une suppression aux listes du backend ; aucun événement runtime de
fin de suppression n'a été instrumenté. Ces adresses restent des preuves, jamais
des adresses de secours.

### Pourquoi le prochain essai est 24×16 et non 480×272

Le pont général reste non certifié. Un **second opt-in**,
`SWML_CD_SMALL_RESOLUTION=1` avec `SWML_CD_OBSERVE=1`, autorise uniquement un
composant local fixe :

- identité `swml_cd_v1_b1x1_p24x16` ; archive native
  `26f8c812f42cd41a9173ae6030881eb9` ; SHA-256 du fichier
  `c809a2f3a5277d7048b287920dbb4b6c8bea71a834f06f83e0b2aea267d48e56` ;
- fichier exact vérifié SHA-256 au démarrage et conservé ouvert sans partage
  d'écriture/suppression ; les tests natifs vérifient aussi cette exclusion ;
- type moniteur exact, identité de définition/archive exacte et bornes physiques
  1×1 relues depuis les déplacements extraits des instructions corroborées ;
- cibles `24×16×qualité` / `24×16`, toujours plus petites sur chaque axe que la
  paire native `32×32×qualité` / `32×32`. Forme 1×1 et deux meshes byte-identiques
  aux originaux. Pas de géométrie rectangulaire expérimentale dans ce paquet ;
- appels aux requêtes natives sur la file et le thread reçus du slot natif,
  formats `(5,1)`, ordre vidéo/overlay et destruction native conservés ;
- registre cumulatif atomique `CustomDisplayAdmission` : chaque création reste
  facturée jusqu'à la fin du processus, y compris une requête interrompue. Aucun
  crédit à la disparition des handles. Cap expérimental **8 Mio de comptabilité
  cumulée**, pas une limite GPU certifiée ni un budget global ;
- si cap épuisé sur une paire vide, retour au calcul natif 32×32 avec diagnostic ;
  si paire occupée, aucun nouveau calcul ne corrompt ses dimensions vivantes.

Le fichier SHA-256 épinglé n'authentifie pas des sauvegardes embarquées hostiles
MD5-collidantes. **Essai strictement local et hors multijoueur**, pas une API de
production et pas une validation de provenance générale. Ni le cap ni les tailles
fixes ne doivent être élargis pour contourner les barrières du pont général.

Paquet distinct : `build/custom-display-small-resolution-20261015/`. Prévol réel
`Check` réussi, aucun fichier installé remplacé ni fixture créée pendant la
préparation. Lanceur racine : `TEST-Resolution-CustomDisplay.bat`, via Launcher.exe.
Il crée seulement ses chemins neufs sous mods/vehicles, restaure l'ancienne DLL,
retire les fichiers d'essai inchangés et conserve tout fichier édité.
Le véhicule caméra séparé est une copie de l'autosauvegarde vérifiée ; seul son
identifiant de moniteur change, pas les positions/propriétés/câbles.

### Résultat du premier essai 24×16

Run `run-20261015-194800-d792ab877ea24722b400f2655f4ea878` : l'utilisateur confirme
l'image caméra. Les 87 appels (38 créations, 49 libérations) sont appariés.
Le spécimen exact est créé trois fois en 24×16, aux qualités natives `3/1/3`,
avec trois admissions réussies et une contribution cumulée finale de 165 888 octets.
La destruction reste native. Le reçu et le hash réel du loader confirment sa
restauration, sans fixture éditée conservée.

Rapport : `build/custom-display-small-resolution-runtime-report.json`, reproduit
par `tools/custom_display_observation_report.py <journal> --small-experiment`.
L'intégrité des appels et admissions passe ; **l'audit complet ne passe pas**,
car la qualité native `2` manque pour ce spécimen. Cette couverture partielle
n'est pas une erreur de résolution ou une validation de tous les réglages.
Lua/tactile, résolution supérieure, provenance runtime générale et dialogue natif
restent à réaliser. Aucune limite expérimentale élargie après ce résultat.
