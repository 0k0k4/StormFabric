# Custom Display — fenêtre native dans l'éditeur de véhicules

État au 16 octobre 2026 : découverte, constructeur natif à quatre champs,
contrôleur, callbacks différés, adaptateur local d'entrées et lecture ciblée du catalogue codés/testés hors jeu ;
**aucun dialogue installé ni activé en jeu**.
Les adresses ci-dessous sont des repères dans l'exécutable de référence, jamais
des fallbacks runtime. Les lots `custom_display_dialog_*` ont été extraits
avec `-noanalysis -readOnly` sur la copie privée du projet Ghidra.

## Ne pas confondre les deux éditeurs

`SW_SURFACE_EDITOR`, dans `mod_native_ui.cpp` et les hooks actuels, désigne
l'éditeur de **microcontrôleurs** (`microcontroller_editor`, init `FUN_140640e80`).
Il ne donne pas accès à l'éditeur de véhicules. Ne pas changer la signification
de cette surface SDK pour y greffer le nouveau composant.

Le véhicule possède deux objets distincts :

| Objet | Vtable de référence | Fonctions observées |
| --- | --- | --- |
| `c_ui_vehicle_editor_interface` | `140b99940` | init `FUN_14062ac80`, update `FUN_14062c530`, destruction `FUN_14062ed20` |
| `c_game_state_vehicle_editor` | `140b44838` | init `FUN_1407c91f0`, teardown `FUN_1407cf750`, entrée `FUN_1407cfce0`, sortie `FUN_1407d0770`, actions `FUN_1407fa7a0` |

L'update UI ne remplace pas le dispatch de l'état. Les durées de vie du contexte,
de la racine UI et de l'état doivent être certifiées ensemble avant de publier
un callback de confirmation. Le teardown d'élément UI partagé est distinct du
deleting-destructor ; le premier ne doit pas être oublié.

## Widgets natifs effectivement retrouvés

Deux classes de dialogue ne sont **pas interchangeables** :

- `c_ui_element_confirm_cancel_dialog`, construit par `FUN_140263270` ;
  les appels de l'init véhicule allouent `0xf0` octets. Il construit ses propres
  boutons `action_tick` / `action_cross` et reçoit deux entiers d'action.
- `c_ui_element_confirm_cancel_dialog_basic`, construit par `FUN_14025a020` ;
  les appels observés allouent `0xe8` octets. Son update `FUN_14025a990` publie
  le dialogue actif dans le contexte (`+0x1a0` de référence). Son gestionnaire
  clavier `FUN_14025a9e0` vérifie cette propriété avant fermeture.

Les deux héritent de `c_ui_element_view_container`. L'animation et le hit-test
consomment l'état d'ouverture (`+0x81`), pas seulement le flag visible de base.
**Un rectangle dessiné au-dessus du catalogue n'est donc pas une fenêtre modale.**
Le dialogue non-basic n'hérite pas du même update spécialisé que basic ; ne pas
lui attribuer automatiquement la capture clavier observée pour basic.

L'init de l'interface véhicule utilise déjà ces deux classes. L'attachement natif
insère l'enfant dans la collection du parent, appelle son init puis affecte son
parent et sa notification d'attachement. Le moteur possède l'arbre résultant ;
ne pas libérer un enfant comme une allocation indépendante après attachement.

### Champs de saisie

`c_ui_element_editbox` : constructeur `FUN_14024f560`, init `FUN_14024f690`.
L'init construit un `editbox_content` distinct et le conserve comme enfant.
`FUN_14024f8a0`, appelé depuis l'init véhicule avec une chaîne de caractères
autorisés, est une piste pour restreindre les quatre saisies aux chiffres.
Les lots `dialog_text`, `dialog_input` et `dialog_dispatch_complete` établissent
maintenant les chemins statiques suivants (pas encore un essai en jeu) :

- Le wrapper conserve son **texte initial** en `+0x58` et son enfant en `+0x78`.
  Le buffer édité est la chaîne moteur de cet enfant en `+0xb8` ; lire `+0x58`
  après frappe renverrait donc l'ancienne valeur. `FUN_1401df640` modifie le buffer
  de l'enfant et sa longueur en `+0xc0`.
- Le filtre `FUN_14024f8a0` copie la liste dans l'enfant en `+0x108` puis **libère
  aussi la chaîne d'entrée**. Il doit être appelé après l'init créant l'enfant.
  `FUN_14024e5c0` filtre les caractères frappés ; `FUN_14024e2e0` filtre aussi
  chaque octet du collage avant insertion. La validation finale ne doit malgré
  tout jamais faire confiance au filtre graphique.
- Le maximum de caractères est dans l'enfant en `+0xcc` (défaut observé : 128).
  La validation du mod refuse plus de dix caractères ; aucune écriture de cette
  limite native n'est actuellement installée.
- `FUN_14024f910` remplace le texte via `FUN_1401e0070` et consomme également son
  entrée. Le destructeur du contenu libère la liste autorisée puis le buffer
  édité ; celui du wrapper libère séparément le texte initial.

**Attention à l'ownership :** le constructeur **editbox** prend une chaîne moteur,
la copie dans ses champs puis libère le buffer d'entrée. Le constructeur du
dialogue **basic**, lui, copie ses deux entrées **sans les libérer** : les appels
véhicule les libèrent explicitement ensuite. Ne jamais leur passer une chaîne
STL ou un littéral au moyen d'un faux descripteur moteur. Il faut distinguer
emprunt et consommation, même si les deux signatures C++ prennent `EStr*`.

Le groupe de découverte **256, `custom_display_dialog_widgets`**, ajoute
29 contrats primaires, 37 témoins de callees et trois vtables RTTI au corpus.
Le merge conserve les corps, masques, relations et groupes préexistants.
`custom_display_dialog_layout.cpp` extrait les opérandes depuis les fonctions
retrouvées, puis recoupe les champs, longueurs, alignements et l'allocation de
l'enfant. Il refait une découverte sur l'image courante pour rejeter un résultat
périmé, un appel redirigé ou une RTTI/vtable altérée. Les nombres ci-dessus ne
sont pas des offsets de secours. **Ce groupe ne certifie ni l'état véhicule, ni
l'authentification du fichier, ni une autorisation d'installer des hooks.**

### Capture native : racine et contexte sont deux vues du même état

`FUN_1401e1c20`, appelé au début du teardown véhicule, passe la racine et
`racine+0x50` au teardown des enfants. Le contexte véhicule est donc ici
`état+0x448`, la racine `état+0x3f8`. Le champ modal `contexte+0x1a0` est aussi
`racine+0x1f0`. Chercher seulement le premier déplacement manque ses consommateurs.

- `FUN_1401e20b0` et `FUN_1401e2250` limitent la recherche des candidats à l'arbre
  du dialogue modal, en remontant les parents pour calculer ses coordonnées.
- `FUN_1401e1cb0` remet le pointeur modal à zéro **avant** l'update des enfants ;
  l'update basic le republie. Ne pas le traiter comme un verrou permanent ni le
  conserver à travers un démontage d'arbre.
- `FUN_1401e2af0` route d'abord le gestionnaire du dialogue puis celui de l'élément
  capturé. Le gestionnaire basic peut fermer par le slot **37 / `+0x128`**, puis
  renvoie faux : cette fermeture ne constitue pas une consommation globale de la
  touche et ne produit pas à elle seule un callback d'annulation du mod.
- `FUN_1401e2d40` est appelé avant le placement par le handler souris véhicule ;
  celui-ci s'arrête uniquement quand l'appel indique une entrée consommée.

Les lots `dialog_modal` contiennent aussi des candidats écartés :
`FUN_1401d8d20` manipule une autre collection, pas un contexte UI ; la première
recherche `Dump G` était limitée à 300 résultats. Les scans complets
`dialog_modal_uses` et `dialog_root_uses` conservent leurs compteurs de fin.
Ces analyses ne certifient pas encore l'absence de clic traversant, l'absence de
raccourci véhicule résiduel ni le thread d'ownership en jeu.

Le lot `dialog_vehicle_boundary` confirme que le handler véhicule
`FUN_1407f0af0` route l'élément capturé via `état+0x5d8` et son slot `+0xa0`,
mais modifie aussi certains états/raccourcis avant et après cet appel. Les xrefs
directes de `FUN_1401e2af0` ne contiennent pas ce handler. Le lot
`dialog_keyboard_boundary` conserve notamment `FUN_14083b810` comme
**contre-exemple menu principal** (`+0xbd6a0`), pas comme preuve de routage
clavier de l'éditeur de véhicules. Publier le pointeur modal d'un basic ne
suffit donc toujours pas à certifier sa capture clavier dans cet éditeur.

## Construction native et contrôleur (implémentés, non activés)

`custom_display_dialog_native.cpp` construit maintenant un vrai
`c_ui_element_confirm_cancel_dialog_basic` avec les boutons natifs, puis ses
quatre `c_ui_element_editbox`, via les constructeurs et l'attachement du jeu.
Les valeurs initiales sont `1 / 1 / 480 / 272` ; le filtre autorise les chiffres.
Le corps comporte les quatre libellés. **Le layout visuel reste à vérifier en
jeu** ; il n'est pas déclaré validé parce que les faux widgets passent.

`custom_display_engine_string.h` alloue uniquement avec `str_reserve` du jeu,
sépare `Borrow` et `Consume`, et désarme le propriétaire **avant** un appel
consommant. Les consommateurs natifs peuvent laisser `data` pendant alors
qu'ils ont libéré le buffer : le helper ne tente pas de le libérer une seconde
fois. Une entrée empruntée est au contraire conservée et libérée par le caller.
Les littéraux/descripteurs STL ne sont jamais présentés comme buffers moteur.

Le groupe **2048, `custom_display_dialog_construction`**, authentifie les
constructeurs, allocateurs, attachement, ouverture et deux callers stock. Les
tailles d'allocation dialog/editbox sont extraites des MOV précédant leurs
allocations dans les callers retrouvés (`dialog_build`, `dialog_fields`), sans
fallback de taille. Les trois tables recoupent init/destructor/ouverture.
`BindCustomDisplayDialogNativeApi` revalide l'image et fournit les bindings
**sans exécuter de fonction ni autoriser une installation**. L'authentification
du fichier et la certification des entrées restent obligatoires au raccordement.

Une branche est attachée **une seule fois** ; `add_child` appelle déjà l'init.
La fenêtre reste fermée pendant toute la construction et après succès, jusqu'au
binding du lecteur et à l'ouverture explicite par son propriétaire. En cas de
retour d'échec local (allocation nulle/chaîne indisponible), la branche partielle
reste fermée, possédée par son parent natif, jusqu'au teardown normal. Pas de
retry sur ce résultat partiel, pas de suppression manuelle d'enfants attachés.
Cela ne prétend pas récupérer une exception/erreur fatale interne au moteur.

Cette politique évite un faux rollback : `FUN_1401dd7e0` ne nettoie le focus/hover
que si ces pointeurs valent **exactement** l'enfant retiré, pas un descendant.
Il n'est donc pas un détachement sûr d'un dialogue contenant un editbox actif.

`custom_display_dialog_controller.cpp` raccorde le lecteur vivant au brouillon :
ouverture après contrôle des quatre types/parents, validation à la confirmation,
fenêtre maintenue ouverte sur erreur, demande de configuration unique puis
fermeture après invalidation des jetons. Annulation, sortie d'éditeur, fermeture
native par Escape et début de teardown rendent les anciens jetons inertes.
Une fermeture native n'est pas rappelée après Escape ; le teardown n'appelle
aucune fonction sur l'arbre. Le contrôleur ne peut plus rouvrir après teardown.
Les mauvais propriétaires/threads et les callbacks réentrants sont rejetés avant
toute lecture ou fonction native. Une demande validée n'est **pas** une permission
de placement : aucun import, choix de définition ni changement de sélection ici.

Dans la variante historique à entiers, les actions confirm/cancel/edit doivent être réservées par l'adaptateur véhicule.
Des entiers positifs fournis au constructeur ne prouvent pas qu'ils sont libres.
Il reste notamment à garantir leur provenance/durée de vie : une action native
différée d'une ancienne ouverture ne doit pas être attribuée au jeton courant.
Ne pas raccorder des actions recyclées au contrôleur en lisant simplement son
jeton actuel, ni considérer les valeurs synthétiques des tests comme réservées.

### Variante à callbacks natifs (codée, toujours non activée)

Les lots `dialog_callback_route/dispatch/fields/zero` établissent une voie sans
choisir des entiers supposés libres. Le basic construit deux **point buttons**,
pas des rectangle buttons : confirmation `dialog+0xd0`, annulation `+0xd8` dans
la référence. Leur handler release `FUN_140249900` appelle le callback
`button+0xd8(context, button+0xe0)`, même si l'action release `+0x90` vaut zéro.
Le handler press ne publie une action `+0x94` que si elle est non nulle ; le
release vérifie également l'action de répétition `+0x98`. Ce ne sont pas les
offsets rectangle `+0xb8/+0xc0` du SDK existant.

Le nouveau groupe **8192, `custom_display_dialog_callbacks`**, authentifie les
corps/relations, le constructeur et la table RTTI point-button (slots 17/18),
l'init basic et le handler clavier texte. Les positions des boutons, leurs
tailles, callback/userdata et actions sont extraits des opérandes puis recoupés
entre constructeur et consommateur ; aucune valeur runtime de secours.
Le corpus de cette passe contenait **136 primaires, 398 témoins et 16 tables** ;
le groupe d'entrées ci-dessous porte le total à **143 / 476 / 16**.
`BindCustomDisplayDialogCallbackAccess` est strictement une validation/binding
en lecture seule ; ce n'est ni une authentification du fichier ni un hook.

`BuildCustomDisplayCallbackDialog` construit une branche fermée avec trois
actions nulles. `Prepare` du contrôleur valide les editboxes et réserve le jeton
**sans ouvrir** ; `Show` est séparé, et une session préparée ne peut ni confirmer
ni annuler via une action. Sortie/teardown invalident aussi la préparation.

`custom_display_dialog_callbacks.cpp` attache les callbacks après contrôle des
deux types/parents, de la fermeture et des actions/callbacks/userdata initialement
nuls. Tous les contrôles précèdent les écritures. Une branche ne peut être liée
qu'une fois. Les tickets retiennent propriétaire/thread/contexte/jeton, avec une
identité distincte par ouverture, jamais recyclée même après réemploi des mêmes
adresses et du même numéro de jeton par un autre contrôleur. L'arène garde au plus
**256 ouvertures par processus**, y compris celles retirées ; épuisement = refus,
pas réemploi. Chaque ouverture nécessite donc un nouvel arbre, fermé et conservé
par son parent jusqu'au teardown normal. Pas de suppression/détachement manuel.

Le thunk ne lit aucun widget et ne valide/ferme/place rien : il compare le ticket
avant d'accéder à ses données internes, vérifie thread/contexte, puis mémorise une
commande. Les doublons sont fusionnés ; l'annulation prime avant le drain.
Les callbacks restent désarmés jusqu'à l'armement explicite après la barrière de
relâchement. `DispatchCustomDisplayDialogCallback` raccorde le drain différé au
contrôleur : désarmement avant les appels/fermetures natifs, réarmement du même
jeton seulement après validation échouée, retrait définitif après succès/annulation.
Le futur adaptateur doit observer Escape **avant** ce drain et retirer les tickets
avant sortie/refresh/teardown. Le mutex protège seulement l'arène du loader :
il ne certifie aucune synchronisation/publication des objets moteur.

**Piège bloquant confirmé :** `FUN_1401df720` publie l'action de soumission du
texte même si celle-ci vaut **zéro**, sur Entrée/Entrée pavé numérique (codes
de référence `0x3d/0x5e`). Zéro n'est donc pas un identifiant inoffensif à laisser
parvenir au dispatcher véhicule. Il reste obligatoire d'intercepter ces touches
localement, ainsi que les raccourcis/placements, tout en conservant l'édition
native et Escape. Cette variante ne doit pas être montrée avant ce raccordement.

Tests hors jeu : binding réel sur faux arbres avec offsets décalés, absence
d'écritures partielles, mauvais parent/action/thread/contexte, callback inconnu,
drain/validation/annulation/réentrance à la fermeture, anciennes ouvertures après
destruction et réemploi d'identité, impossibilité de rebinder un arbre, plafond.
Mutations du code, opérandes, RTTI/table et résultats fabriqués/périmés rejetées.
Ces tests ne certifient toujours ni modalité ni affichage en jeu.

### Adaptateur local des entrées (codé, non raccordé aux hooks)

`custom_display_dialog_input.cpp` relie désormais les aides UI natives au même
ticket immuable et au contrôleur différé, sans appeler les handlers véhicule :

- caractères et édition/collage : uniquement les quatre contenus editbox dont
  types/parents sont vérifiés ; les chaînes initiales ne sont pas modifiées ;
- Entrée et Entrée pavé numérique : commandes locales de confirmation, jamais
  transmises au handler texte qui publierait l'action zéro. Une touche maintenue
  ne confirme pas à nouveau après une validation échouée ;
- Échap : comparaison avec le binding de contrôle natif courant, commande locale
  d'annulation prioritaire ; aucune constante de touche Escape supposée stable ;
- souris/pointeur : helpers de la racine native, retours faux quand même consommés,
  pas de fallback placement/raccourci ; coordonnées copiées et finies ;
- changement de champ pendant une touche maintenue : pas de répétition vers le
  nouveau champ, relâchement au destinataire initial encore valide ;
- propriétaire/thread/jeton erronés : aucune lecture native. Réentrance et état
  défectueux : aucune entrée transmise au véhicule.

Le groupe **16384, `custom_display_dialog_input`**, authentifie les cinq routes
véhicule, les helpers racine/contenu, le getter de contrôles et leurs relations,
en réutilisant les tables sans affaiblir leurs groupes existants. Layouts extraits
des opérandes puis recoupés : racine/contexte/file, destinataire capturé, hover,
verrou pointeur, offsets du binding Escape et codes de soumission. Les repères des
lots `dialog_vehicle_input` / `dialog_input_operands` ne sont pas des fallbacks.
`BindCustomDisplayDialogInputApi` n'exécute aucun natif. Sur métadonnées inconnues,
le getter de contrôles reste ambigu dans le scan synthétique : pas de premier
candidat choisi, pas d'activation, pas d'abaissement des contrôles.

Séquence interne prévue : `Prepare` contrôleur → réservation/attachement du ticket
→ `Begin` entrées sur arbre fermé et snapshot complet neutre → `Show` → update
basic natif publiant le modal → `Activate` avec un nouveau snapshot neutre.
`Activate` revérifie aussi destinataire, hover et verrou pointeur avant d'armer
le ticket : l'état neutre lors de `Begin` n'autorise pas une contamination pendant
l'update natif. Entre `Begin` et `Activate`,
les entrées sont consommées sans appels/lectures de widgets par les routes de
saisie. La fermeture native n'est donc pas confondue avec l'attente d'ouverture.

`Drain` s'exécute seulement au futur point sûr propriétaire, hors parcours natif.
Il observe d'abord une fermeture native, qui invalide le contrôleur et prime sur
une confirmation en attente. Pendant validation/fermeture, toutes les routes
d'entrées restent capturées, y compris une réentrance native. Une erreur de valeurs
reprend l'édition sur le même jeton ; succès/annulation restent en drainage jusqu'à
`Release` sur snapshot complet neutre **et** dialogue fermé, modal dépublié,
destinataire/hover nuls et verrou pointeur nul. Un défaut garde la capture jusqu'au retrait
par le propriétaire ; aucune libération automatique risquant un clic traversant.
Sortie/refresh/teardown doivent retirer entrées **et** contrôleur avant démontage.

**Le raccordement runtime reste à implémenter**, pas simulé en production :
le lecteur natif ci-dessous a besoin du véritable host de fenêtre, du cycle de vie
et du point sûr, et l'ancien destinataire catalogue/verrou pointeur doit être retiré
avant ouverture. `Begin` refuse un destinataire ou hover extérieur au dialogue et
un verrou pointeur encore actif. Le helper souris appelle son destinataire retenu
**avant** le hit-test, même à un nouvel appui : l'adaptateur refuse donc de lui
transmettre appui ou relâchement si ce destinataire est stock. Aucune remise à zéro
directe d'un champ moteur n'a été ajoutée pour contourner ce contrat.

### Source native clavier/souris et retrait des destinataires (19e passe)

Le groupe **32768, `custom_display_native_input_state`**, authentifie quatre corps
supplémentaires et leurs callees sans changer les contrats précédents. Le corpus
contient **147 primaires, 481 témoins et 16 tables RTTI**. Les opérandes du host de
fenêtre et des boucles natives de perte de focus donnent les tableaux et bornes
clavier/souris ; les deux bornes de chaque boucle sont recoupées. Les champs de
retrait racine sont recoupés avec les layouts UI déjà authentifiés.

Sur le seul exécutable de référence, ces boucles couvrent **349 touches et huit
boutons**, distincts des codes texte véhicule limités à 256. Le snapshot possède
donc 512 bits de capacité, sans prendre cette capacité pour une taille moteur.
`CustomDisplayNativeHeldInputReader` épingle host/fenêtre/propriétaire/thread,
copie tous les états deux fois, rejette une dérive ou un changement de fenêtre,
et efface tout ancien résultat au premier échec. Valeurs natives 0/1 reconnues ;
la valeur sticky 3 reste conservativement maintenue et n'est jamais consommée.
Une autre valeur ou une copie tronquée ne publie pas `complete=true`.
Le retrait propriétaire ne lit aucun objet natif ; une instance retirée ne se
rebind pas. Ce lecteur ne fait aucun polling clavier OS et n'appelle pas de getter
qui viderait les états sticky.

`Advance` raccorde ce lecteur à la session locale : attente/activation du modal,
drain du contrôleur, retrait natif puis libération. Un échec de source conserve
capture et commande en attente ; un nouvel instantané est lu **après** les callbacks
natifs de fermeture/retrait, jamais réutilisé depuis avant ces appels pour libérer.
Cette méthode reste à appeler depuis le véritable point sûr : elle n'installe pas
de hooks et n'autorise ni enregistrement ni placement.

La routine native `FUN_1401e2e20` notifie capture-loss et hover-leave puis retire
les pointeurs retenus, **sans remettre à zéro le verrou pointeur**. Son binding
ne l'exécute pas. `RetireRecipients` l'utilise seulement en drainage, sur arbre
fermé/dépublié, avec snapshot neutre et verrou déjà nul, et seulement si les deux
destinataires sont descendants du dialogue. Elle conserve la capture pendant une
réentrance et vérifie les postconditions. Le helper pointeur refuse aussi un hover
stock même si aucun bouton n'est maintenu : le natif peut lui envoyer hover-leave.

**Non certifiés :** provenance runtime du host fourni par les futurs hooks,
synchronisation/publication inter-thread, état des autres sources de commandes,
retirement du destinataire stock avant `Begin` et drainage natif d'un verrou
encore actif. Deux copies identiques ne prouvent pas une synchronisation, et
`complete` décrit ici les tableaux clavier/souris natifs, pas un permis d'activation
ou une preuve de neutralité de toutes les routes de contrôle.

Tests regroupés : offsets décalés, attente du modal, barrières incomplètes/maintenues,
soumission locale et répétitions, Escape prioritaire, anciennes cibles stock,
fermeture pendant réentrance, validation échouée puis corrigée, fermeture native
avant drain, défaut et teardown sans lecture. **Ce sont des faux natifs** : capture
réelle, focus visuel, rendu et absence de raccourcis résiduels restent non certifiés.
La transaction de tous les hooks d'entrée (y compris wheel/drag/autres commandes),
le point sûr et la provenance catalogue restent obligatoires avant affichage.

## Lecture ciblée du clic catalogue (pas encore un hook installé)

`custom_display_catalogue.cpp` lit maintenant la chaîne de sélection du **cas
`0x61`**, avec copie mémoire bornée injectée. Le groupe **4096,
`custom_display_catalogue_route`**, rend explicites les contrats du getter
`FUN_140611050`, du dispatcher et de son slot. Le getter était auparavant un
témoin de callee, pas un symbole publiable : le code ne réutilise pas son RVA
comme adresse si sa découverte manque. Les sept offsets viennent des opérandes
du cas catalogue et du getter ; code/relations/résultat périmés sont rejetés.

La décision `OpenRequested` exige l'identité exacte de l'état, du thread, de
l'interface, du catalogue, de la **ligne native** et de la définition épinglée,
ainsi qu'un record composant de type 0. Une autre ligne partageant la même
définition reste stock. Aucun nom affiché/MD5 ne sert de raccourci d'identité.
Les autres actions — dont hotbar/restauration — ne provoquent même pas une
lecture de sélection. Les données manquantes restent un résultat distinct,
pas une autorisation d'ouvrir ou de sélectionner.

L'épinglage doit encore venir d'un véritable enregistrement propre au loader,
puis être invalidé lors du refresh/sortie/teardown. Ces structures ne le créent
pas. Le futur hook consommera uniquement le cas correspondant et différera la
création/ouverture hors des traversées natives. **Aucun de ces nouveaux chemins
n'est appelé depuis les hooks du jeu à ce stade.**

## Validation et durée de vie du brouillon (implémentées hors jeu)

`src/custom_display_dialog_model.h` valide quatre valeurs décimales indépendantes,
refuse vide/zéro/signes/espaces/caractères étrangers/NUL/dépassement, normalise les
zéros initiaux dans l'identité et vérifie le plan **×4**, pas seulement la qualité
courante. Une politique nulle reste désactivée. Ce calcul ne remplace pas
l'admission cumulative ni la validation de géométrie/provenance.

Le brouillon possède un jeton d'ouverture, invalide une validation précédente
échouée, rejette les confirmations anciennes/doubles et ne rend aucune demande
après annulation ou teardown. Il ne conserve aucun pointeur moteur et n'effectue
ni import ni placement. Ses jetons sont propres à la durée de vie de l'instance ;
l'adaptateur natif devra en plus gérer l'identité et la destruction du propriétaire,
sur son thread, avant de publier un callback. Cette logique n'est **pas** une
fenêtre déjà raccordée au catalogue.

`custom_display_dialog_text.cpp` implémente maintenant la lecture bornée des
quatre buffers édités. Il vérifie jeton, propriétaire et thread **avant** toute
lecture, puis types natifs, parents, longueur/capacité et terminaison. Il copie
les quatre valeurs sans tronquer et ne publie aucun résultat partiel. Le test
utilise des offsets décalés exprès, des buffers initiaux inaccessibles et des
descripteurs malformés. Ce lecteur n'est pas encore lié à de vrais widgets :
son `Bind` est une étape interne, pas une authentification ni une permission de
créer une fenêtre. L'adaptateur propriétaire devra fournir une lecture mémoire
native et invalider la session avant destruction de l'arbre.

## Observation runtime préparée : six slots, aucun changement de comportement

Le groupe **512, `custom_display_vehicle_observer`**, exporte six contrats
primaires, 269 témoins et deux vtables (certains contrats recoupent le corpus
existant sans l'affaiblir). Il couvre les slots init/update/teardown/destructor
de l'interface véhicule et teardown/actions de son état. L'offset racine est
extrait de l'instruction précédant son teardown ; les six indices de slots
viennent des tables retrouvées, pas d'un fallback runtime.

`custom_display_vehicle_observer.cpp` est désactivé sauf
`SWML_CD_VEHICLE_OBSERVE=1`. L'installation exige l'authentification SHA-256
existante, les groupes complets 256/512, une nouvelle validation de l'image
courante et une transaction des six slots. Il n'ajoute aucun widget, ne consomme
aucune entrée et n'importe aucune définition. Il transmet les paramètres,
retours et thread originaux ; une erreur du logger ou le plafond de 4096
événements ne saute ni ne répète l'appel natif. Les updates sont échantillonnées
(16 premières puis une sur 60), pas une trace complète des frames/entrées.
Aucune lecture d'objet n'a lieu après un deleting-destructor ou un dispatch
d'action susceptible de changer d'état. Aucun texte de saisie n'est journalisé.

Le nouveau type de paquet du lanceur ne pose **aucun fichier dans les données
natives**. Il retire les opt-ins moniteur/résolution et active seulement
l'observateur véhicule ; les autres modes retirent explicitement cet opt-in.
Voir `custom_display_vehicle_observation_readme.md` : deux entrées/sorties de
l'éditeur, clic catalogue stock, fenêtre Charger puis annulation, sortie du jeu.
Ce n'est pas une répétition du test caméra. L'auditeur d'une seule session
sépare intégrité des paires/threads/durées de vie et couverture demandée ; ses
tests synthétiques ne remplacent pas l'observation réelle ci-dessous.

### Premier passage réel et séparation des threads

Le journal `run-20261015-205718-3f015a662d774daf809f6abab3b53c30`
(SHA-256 `316e2534b1e95aeb85cf466f8f4ab389bf48fdaff44c19ecd957345e7c1fc46e`)
contient une durée de vie attachée complète. Init avant/après sur 44280,
updates/actions/teardown/destruction sur 54980, même contexte et même parent
attaché. Le teardown UI et la destruction sont inclus dans le teardown état.
Le loader original a été restauré à l'identique et son hash relu sur disque.

Les lots statiques `custom_display_vehicle_worker_dispatch` et
`custom_display_vehicle_worker_publication` expliquent cette séparation :
la vtable `thread_task_state_create` mène à `FUN_1408e98b0`, qui appelle le
slot `+0x10` de l'**état applicatif**, pas celui du sous-état véhicule ;
`FUN_1408ea600` construit cette tâche, lui affecte l'état applicatif en `+0xc8`
et l'enfile via `FUN_14017d670`. L'état applicatif jeu (`c_application_state_game`)
a son init `FUN_140845d20` à ce slot ; cet init construit le sous-état véhicule
puis appelle les slots `+8` de tous les sous-états. Ce sont des preuves
statiques de chemin, **pas** un contrat authentifié de synchronisation/publication.

L'auditeur distingue donc le thread d'init du thread fixé au premier update
attaché après init terminé. Actions, updates suivants, teardown et destruction
doivent rester sur ce second thread avec le même contexte. Il ne transfère
aucune session du lecteur texte et n'autorise aucun widget. Une action seule
ne peut pas fixer ce thread. Les tests couvrent les rejets de changements de
thread, d'init retardée et d'actions sans update attaché.

Intégrité de cette trace : **valide** selon ce modèle de phases ; couverture :
**incomplète** (une durée de vie, actions 8/18, pas de `0x61`, pas de modal
échantillonné). Ne pas fusionner avec un futur journal pour atteindre les deux
durées de vie. La fenêtre Custom Display reste désactivée.

### Second passage réel : catalogue confirmé, interface conservée

Le journal `run-20261015-214646-87037e4b4ef14afda88ef3452ce4a947`
(SHA-256 `a8ceebde06aecf4d8d510bdd0ccef019ffce8881d1adb1c06347a1151de34390`)
contient deux actions catalogue `0x61` sur le même état/contexte, un init sur
31392 puis les updates/actions/teardown/destruction sur 35452. Son intégrité
v1 est valide ; la couverture stricte reste incomplète : un seul cycle UI,
aucun modal échantillonné. Rapport conservé séparément dans ce run.

Les lots `vehicle_application_boundary/lifetime`, `vehicle_active_dispatch`
et `vehicle_activity` expliquent pourquoi une sortie à l'établi ne détruit pas
l'UI : `FUN_140845d20` alloue le véhicule et le conserve dans `app+0xce0f8`
et dans la collection `app+0x338`. `FUN_140848d20` change le sous-état actif
en appelant l'ancien slot **4 / +0x20**, puis le nouveau slot **3 / +0x18**.
Il ne détruit pas les éléments de cette collection. Le teardown applicatif
`FUN_1408473b0` quitte le sous-état actif puis appelle les teardowns de toute
la collection. Il faut donc quitter la **partie** pour observer un autre cycle
UI, pas simplement fermer l'éditeur. Les consignes précédentes étaient insuffisantes.

**Correction de nomenclature :** `FUN_1407cfce0` est une entrée/activation,
pas un update par frame. Elle repeuple notamment le catalogue via
`FUN_140610df0` et `FUN_140610f40`. `FUN_1407d0770` est la sortie/désactivation.
Une session de dialogue devra être invalidée **avant cette sortie**, même si
la racine, les widgets et le contexte restent vivants. L'observation actuelle
ne certifie pas encore ces nouveaux hooks ni leurs threads en jeu.

### Observateur d'activité v2 préparé (pas encore essayé en jeu)

Le groupe additionnel **1024, `custom_display_vehicle_activity`**, conserve
trois contrats primaires (entrée, sortie, switch applicatif), 39 témoins locaux
et les slots 3/4 de la vtable véhicule. Le merge préserve les six contrats
antérieurs et leur groupe 512 ; le corpus total contient 130 contrats primaires,
398 témoins et 14 vtables RTTI. Les deux nouveaux indices sont extraits des
déplacements des CALL virtuels du switch authentifié et recoupés avec la table.
Une instruction, relation, RTTI, table ou résultat périmé/modifié est rejeté.

L'opt-in véhicule reste inchangé ; la transaction passe à huit slots et exige
les groupes 256/512/1024. Les deux nouveaux wrappers transmettent les quatre
registres d'arguments à l'identique, sur le thread natif, sans changer le jeu,
même si le logger échoue ou atteint son plafond. Aucune lecture de l'arbre
après le retour d'une entrée/sortie. Le marqueur annonce `version=2`.

L'auditeur conserve le seuil de **deux véritables cycles UI**, distincts des
visites actives. En v2 il vérifie en plus des entrées/sorties appariées, un état
et un thread stables, aucun update/action hors visite active et la fin de la
sortie avant teardown. La couverture demande deux visites closes et au moins
une réentrée du même propriétaire UI ; les journaux v1 ne peuvent pas la
certifier. Aucun compteur de visites ne remplace le nombre de cycles UI.

## Interception de la sélection : conserver le comportement stock

Les références à `FUN_140801340` ne proviennent pas uniquement du clic catalogue :
init/restauration de l'état (`FUN_1407ce2b0`), hotbar (`FUN_140801550`), souris
(`FUN_1407f3440`) et actions (`FUN_1407fa7a0`) l'utilisent aussi.
Un hook inconditionnel ouvrirait donc la fenêtre lors de restaurations ou de
sélections ordinaires. La frontière candidate reste l'action catalogue `0x61`,
après lecture de l'entrée active, uniquement pour l'identité Custom Display.

`FUN_140610df0` peuple les entrées du catalogue à partir des catégories ;
`FUN_140611070` réutilise une entrée si le pointeur de définition existe déjà.
Cela ne prouve pas qu'un changement de catégories avec une UI affichée soit sûr.

## Contrats encore nécessaires avant activation

1. Découverte complète des corps, relations, RTTI/slots et opérandes utilisés ;
   authentification inchangée. Les noms/adresses ci-dessus ne l'autorisent pas.
2. Cycle de vie véhicule observé : thread de création, update, fermeture,
   destruction et invalidation des callbacks avant celle des enfants.
3. Raccordement des callbacks à la vraie durée de vie véhicule, ouverture/drain
   hors traversée, barrière de relâchement et capture de la soumission texte ;
   aucune action arbitraire ajoutée au switch natif. La variante à entiers
   nécessiterait toujours une réservation indépendante.
4. Capture souris/clavier/modalité, fermeture Échap et restauration de la sélection
   sans placement quand l'utilisateur annule.
5. Validation des quatre valeurs avant génération ; import natif, provenance,
   budget et sélection de la définition enregistrée avant retour au placement.

Le test caméra 24×16 ne certifie aucun de ces contrats. Le pont de résolution
restreint ne doit pas être élargi pour afficher une fenêtre prématurément active.
