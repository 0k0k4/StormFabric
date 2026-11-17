# Services entre mods (`api->services`)

**Statut : implémenté et testé hors jeu.**

Un mod **fournisseur** publie une table de fonctions sous un nom ; les mods **consommateurs**
récupèrent cette table et appellent ses fonctions directement (même processus, appels C,
aucune copie). C'est ce qui permettra à un mod `sf_browser` d'offrir « le navigateur » aux
autres mods (interfaces, microcontrôleurs…).

## Règle de durée de vie

`get` ne réussit que si le fournisseur figure, directement ou par l'intermédiaire d'autres
mods, dans les `dependencies` du `mod.json` du consommateur. Le loader charge alors le
fournisseur **avant** et le décharge **après** le consommateur (y compris s'il est désactivé :
ses dépendants sont déchargés d'abord). La table reste donc valide tant que le consommateur
est chargé, sans comptage de références. Un consommateur qui lance des threads doit les
arrêter dans `SwModUnload`.

Sans cette dépendance, `get` renvoie `SW_CONFLICT` et le loader l'écrit dans le journal.

## API (`include/sw_sdk_services.h`)

| Fonction | Rôle |
|---|---|
| `publish(owner, name, version, table, size)` | Publie (ou remplace, pour le même mod) ; `SW_CONFLICT` si un autre mod a déjà ce nom |
| `unpublish(owner, name)` | Retire ; automatique au déchargement du mod |
| `get(owner, name, minVersion, &table, &version, &size)` | Récupère la table |
| `setOwnerReleaseCallback(provider, callback, userData)` | Le fournisseur est prévenu quand un **autre** mod est déchargé (avant son `SwModUnload` et avant la libération de sa DLL), pour supprimer ses objets et ses callbacks |

- **Nom** : 1 à 64 caractères `[a-z0-9_.-]`, unique dans le processus.
- **Version** : `SW_SERVICE_VERSION(majeure, mineure)`. `get` exige la même majeure et une
  mineure au moins égale à celle demandée (`SW_UNAVAILABLE` sinon ; `version` reçoit quand
  même la version publiée).
- Toutes les fonctions sont utilisables depuis n'importe quel thread.

Un fournisseur qui garde des objets ou des callbacks par consommateur (comme `sf_browser`)
**doit** utiliser `setOwnerReleaseCallback` : après le retour de son callback, plus aucun
code du consommateur ne doit être appelé.

## Conventions pour une table

- Une `struct` C de pointeurs de fonction dont le premier membre est `uint32_t structSize`.
- Stockage `static const` dans la DLL du fournisseur : la table n'est pas copiée.
- Ajouts **à la fin** seulement, avec une mineure incrémentée ; le consommateur vérifie
  `structSize` avant d'utiliser un membre ajouté. Changement incompatible : nouvelle majeure.
- Le fournisseur livre un en-tête qui décrit la table, son nom et sa version.
- Fonctions appelables depuis n'importe quel thread, sauf mention contraire du fournisseur.

## Exemple

`examples/service_demo/` :

- `service_provider` (`sf_service_provider.dll`) publie `demo.counter` v1.0
  (`demo_service.h`) ;
- `service_consumer` (`sf_service_consumer.dll`) déclare `"dependencies": ["service_provider"]`,
  récupère la table dans `SwModInit` et écrit dans le journal :
  `service_provider says: counter = 42 (service v1.0)`.

## Tests

`tests/mod_services_tests.cpp` : noms, arguments, conflit entre mods, dépendance directe et
transitive, refus sans dépendance, versions, remplacement, retrait au déchargement.
