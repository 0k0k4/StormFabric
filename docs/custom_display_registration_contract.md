# Custom Display — contrat d'import natif à certifier

État au 15 octobre 2026 : **aucun appel d'import ajouté au loader**. Le témoin
chargé par le jeu a été placé et affiche la vidéo selon l'utilisateur ; son XML
et ses archives de sauvegarde ont été vérifiés séparément en lecture seule.
Les repères `FUN_*`, offsets et tailles ci-dessous appartiennent exclusivement
à l'exécutable de référence identifié dans `custom_displays.md`.

## Une entrée incrémentale existe, mais n'est pas encore un service sûr

Le chemin fichier `FUN_14046f560` n'est appelé directement que par le chargement
global `FUN_14046cf70` dans les références exportées. En revanche,
`FUN_14046f6b0` accepte un objet archive conservé dans le registre et appelle :

1. `FUN_14046fc50`, qui alloue huit définitions distinctes, les construit via
   `FUN_140316770`, puis relie leurs huit pointeurs de réflexion ;
2. l'ajout de la variante principale à la collection du registre ;
3. les traitements d'identité/déverrouillage et `FUN_14046efd0`.

Ce wrapper ne contient pas d'appel à la réinitialisation globale. Cela établit
l'existence d'une entrée incrémentale native, **pas** sa sûreté depuis n'importe
quel callback de mod : il modifie plusieurs collections et ne fournit ni rollback
ni synchronisation démontrés.

Le wrapper est notamment utilisé par `FUN_1404c1ed0` : recherche des fichiers
`bin`, lecture de l'archive, recherche d'un doublon, transfert du buffer dans un
objet conservé par le registre, construction des définitions manquantes, puis
lecture XML du véhicule. `FUN_140781640` suit aussi ce transfert lors d'un chemin
de l'éditeur. Les pointeurs de ressources ne sont donc pas simplement empruntés
à un buffer temporaire détruit avant la lecture du véhicule.

## Trois identités à ne pas confondre

| Identité | Observation statique | Conséquence |
| --- | --- | --- |
| Configuration du prototype | `swml_cd_v1_b…_p…` | Encode taille et résolution ; pas encore reliée au moteur. |
| Définition native | Champ `+0x288`, obtenu par `FUN_140315d40` | Sert à identifier le composant ; variante de réflexion séparée. |
| Archive | Nom à 32 chiffres hexadécimaux calculé par `FUN_140315b70` | Indexe le buffer conservé et les fichiers `.bin`, pas les dimensions. |

`FUN_140315b70` calcule MD5 sur les octets de l'archive interne : état initial,
compression à blocs de 64 octets, constantes et rotations de `FUN_1401889e0`,
finalisation `FUN_140189370`, puis hexadécimal minuscule sur 16 octets. **Ce MD5
est une identité de contenu native, pas une authentification de confiance.**
L'authentification SHA-256 des exécutables du loader reste indépendante.

`FUN_140316770` copie l'identité d'archive dans la définition (`+0x328`, longueur
`+0x330`) en plus de son identité de composant. Un identifiant de configuration
ne peut donc pas être substitué indifféremment à ces deux chaînes.

## Format interne observé et enveloppes distinctes

Pour le cas dont le premier entier vaut `1`, `FUN_140315d40` lit :

1. entier 32 bits `1` ;
2. nom de fichier terminé par zéro, dont la dernière extension est retirée pour
   produire l'identifiant de définition ;
3. texte XML terminé par zéro ;
4. nombre de ressources sur 16 bits ;
5. pour chaque ressource : nom terminé par zéro et blob précédé d'une longueur
   32 bits. Un nom dérivé est aussi calculé en mémoire par `FUN_14093b8c0`.

Le cas historique ne lit pas le préfixe `1` ni ce nom ; il conserve comme
identifiant de définition celui de l'objet archive. Il ne faut pas traiter
arbitrairement un futur préfixe inconnu comme un format compatible.

Le fichier `.bin` lu par `FUN_140315a90` enveloppe le blob dans une longueur
32 bits. Une autre enveloppe `FUN_14090c2b0` transporte une collection : nombre
16 bits puis blobs de longueur 32 bits. Ce sont des niveaux différents, pas un
format unique à interchanger. Aucun encodeur n'est ajouté sur cette seule base.

Les lecteurs natifs observés ne constituent pas une validation robuste d'entrée
hostile : certaines branches retournent sur troncature après allocation ou
laissent des compteurs locaux insuffisamment établis dans la décompilation.
Un futur service devra prévalider tailles, nombres, terminaisons, noms de
ressources, doublons et budget total avant tout appel natif.

## Ressources et visibilité : deux étapes séparées

`FUN_140319070` cherche d'abord le nom de mesh exact dans les ressources de
l'archive ; s'il est absent, il utilise le chargement fichier `FUN_1402cc670`.
Le chemin mémoire `FUN_1402cbfb0` est donc distinct du cache audio étudié dans la
cinquième passe. La construction prépare les ressources avant de libérer les
descripteurs temporaires ; les durées de vie graphiques finales restent à suivre.

Le wrapper incrémental ne reconstruit **pas** les catégories du catalogue.
`FUN_14046f1d0` vide/reconstruit uniquement leurs tableaux de pointeurs à partir
de la collection principale, contrairement à la destruction du registre entier.
Un appel natif observé dans `FUN_1407a9110` effectue le rafraîchissement puis cette
reconstruction. Cela ne prouve pas qu'un catalogue déjà affiché puisse conserver
ses boutons pendant l'opération ; son modèle UI doit être reconstruit au bon
moment, sur le thread moteur.

## Sauvegarde : progrès statique, pas preuve d'aller-retour

`FUN_1404c1b10` collecte les identités d'archive depuis les définitions utilisées,
les déduplique, retrouve les buffers conservés, puis écrit `<identité>.bin` dans
le dossier associé au véhicule. Un appel de sauvegarde de l'éditeur est visible
dans `FUN_1407d1cf0`. `FUN_1404c6280` prépare aussi une collection de blobs.

Le marqueur `+0x338`, positionné par plusieurs importeurs, est consommé par
`FUN_1404c9810`. Le lire comme un simple « composant personnalisé » universel
serait prématuré ; toutes ses conséquences doivent encore être retracées.

### Prochaine preuve minimale

- Certifier par découverte les objets registre, ressources et lecteur, leur ABI,
  leur thread et un point d'import avant consommation des définitions.
- Faire importer une **copie supplémentaire** d'un moniteur, avec identité propre,
  par le chemin natif complet ; ne pas copier ses objets mémoire.
- Établir l'activation et le rafraîchissement du catalogue sans invalider ses UI.
- Vérifier sauvegarde XML + archive, redémarrage, rechargement et spawn sur un
  véhicule isolé ; contrôler aussi les trois identités et les huit réflexions.
- Seulement ensuite ajouter géométrie paramétrique, résolution indépendante et
   dialogue. Le témoin a depuis validé import/placement/vidéo selon l'utilisateur,
   et ses fichiers de sauvegarde sont cohérents ; cela ne certifie pas un appel
   incrémental depuis notre loader ni toutes les réflexions.

## Preuves conservées

Dans `tools/ghidra/`, couples `.commands.txt` / `.txt` :
`custom_display_mesh_registry_finalize`, `custom_display_incremental_registry`,
`custom_display_memory_registration`, `custom_display_embedded_definition`,
`custom_display_archive_format`, `custom_display_archive_save`,
`custom_display_archive_contract`, `custom_display_import_gates`.
Compléments : `custom_display_archive_field_uses.txt`, `custom_display_contracts`
et `custom_display_click`.
