# Stormworks 64-bit Engine Function & Structure Mapping

Ce document répertorie le mapping des fonctions, structures et offsets rétro-conçus (reverse engineering) du binaire `stormworks64.exe` (Steam x64, base par défaut `0x140000000`).

---

## 1. Informations Générales & ABI

- **Architecture :** x86-64 (AMD64)
- **Convention d'appel :** Microsoft x64 Calling Convention
  - Paramètres entiers/pointeurs : `RCX`, `RDX`, `R8`, `R9`, puis pile (`[RSP + 0x20]`, `[RSP + 0x28]`, etc.). Shadow space de 32 octets (0x20) obligatoire réservé par l'appelant.
  - Paramètres flottants : `XMM0`, `XMM1`, `XMM2`, `XMM3`.
  - Registres non-volatiles (préservés par l'appelé) : `RBX`, `RBP`, `RSI`, `RDI`, `R12`, `R13`, `R14`, `R15`.
- **Moteur Graphique :** OpenGL 3.3 Core (contexte partagé géré via `ResourceManager` / `graphicsContext`).

---

## 2. Structures de Données Clés

### `SwString` / `SwEngineString` (Taille: 16 octets / 0x10)
Représentation interne d'une chaîne de caractères dynamique dans le moteur.
| Offset | Type | Nom | Description |
|---|---|---|---|
| `+0x00` | `const char*` | `data` | Pointeur vers la chaîne ASCIIZ / UTF-8 |
| `+0x08` | `uint32_t` | `length` | Longueur en octets (hors terminateur nul) |
| `+0x0c` | `uint32_t` | `capacity` | Capacité allouée du tampon |

---

### `DefRingBuffer` (Taille: 24 octets / 0x18)
Tampon circulaire générique utilisé dans tout le moteur pour stocker les tableaux dynamiques (définitions, voxels, surfaces).
| Offset | Type | Nom | Description |
|---|---|---|---|
| `+0x00` | `void*` | `data` | Tampon d'éléments / tableau de pointeurs |
| `+0x08` | `uint32_t` | `capacity` | Capacité maximale actuelle du tampon |
| `+0x0c` | `int32_t` | `head` | Indice de tête circulaire |
| `+0x10` | `uint32_t` | `count` | Nombre d'éléments actifs |
| `+0x14` | `uint32_t` | `pad` | Alignement / padding |

---

### `ComponentRegistry` (`world + 0xbb670`)
Registre central de tous les composants existants dans une instance de monde (éditeur ou monde simulation gameplay). Contient 4 ring buffers imbriqués :
| Offset | Type | Nom | Description |
|---|---|---|---|
| `+0xc8` | `void**` | `variantDefsBuffer` | **Critique pour le Spawn :** Tampon circulaire de TOUTES les variantes individuelles (8 par composant). Utilisé par le désérialiseur de véhicules (`0x1404b8a50`) lors du spawn pour retrouver la définition exacte du bloc posé (`idStr` + `variantIndex`). |
| `+0xd0` | `uint32_t` | `variantDefsCapacity` | Capacité du ring buffer des variantes (ex: 2048) |
| `+0xd4` | `int32_t` | `variantDefsHead` | Indice de tête du buffer des variantes |
| `+0xd8` | `uint32_t` | `variantDefsCount` | Nombre total de variantes enregistrées |
| `+0xe0` | `void**` | `xmlDescriptorsBuffer`| Tampon circulaire des descripteurs XML (structures de 32 octets `{ idStr, idLen, idCap, xmlData, xmlLen, pad }`). Interrogé par `FindDefinitionDescriptorByName` (`0x14046f2f0`) et `0x1404c6280` lors de la création du paquet de spawn. |
| `+0xe8` | `uint32_t` | `xmlDescriptorsCapacity`| Capacité du ring buffer des descripteurs XML |
| `+0xec` | `int32_t` | `xmlDescriptorsHead` | Indice de tête des descripteurs XML |
| `+0xf0` | `uint32_t` | `xmlDescriptorsCount`| Nombre de descripteurs XML enregistrés |
| `+0xf8` | `void**` | `allDefsBuffer` | Tampon circulaire de tous les composants (`ComponentDefinition*` variant 0). Utilisé par `FindComponentDefinitionByName` (`0x14046f380`). |
| `+0x100` | `uint32_t` | `allDefsCapacity` | Capacité du ring buffer global (ex: 1024) |
| `+0x104` | `int32_t` | `allDefsHead` | Indice de tête |
| `+0x108` | `uint32_t` | `allDefsCount` | Nombre total de composants enregistrés |
| `+0x110 + cat * 24` | `DefRingBuffer` | `categoryBuckets[16]` | Tampons circulaires filtrés par catégorie (ex: cat 6 = Displays, utilisé par l'UI du catalogue) |


---

### `MeshResource` (Taille: 160 octets / 0xa0)
Structure contenant les données CPU et les identifiants VBO/VAO GPU d'un maillage 3D.
| Offset | Type | Nom | Description |
|---|---|---|---|
| `+0x00` | `uint8_t` | `isLoaded` | 1 si le maillage est initialisé et prêt à être rendu |
| `+0x08` | `uint16_t*` | `indicesBuffer` | Tampon CPU des indices de faces (triangles) |
| `+0x10` | `uint32_t` | `indicesCount` | Nombre d'indices (ex: 150 pour monitor_1 = 50 triangles) |
| `+0x18` | `uint8_t*` | `vertexBuffer` | Tampon CPU des sommets (28 octets par sommet) |
| `+0x20` | `uint32_t` | `vertexCount` | Nombre de sommets (ex: 84 sommets pour un écran) |
| `+0x28` | `void*` | `graphicsContext` | Pointeur vers le contexte graphique OpenGL du moteur |
| `+0x30` | `void*` | `gpuBuffers` | Pointeur vers la structure VAO / VBO OpenGL allouée sur le GPU |
| `+0x38` | `void*` | `graphicsContext2` | Contexte secondaire |
| `+0x40` | `void*` | `submeshes` | Tampon de sous-maillages |
| `+0x48` | `uint32_t` | `submeshCapacity`| Capacité du tableau de sous-maillages |

---

### `ComponentDefinition` (Taille d'une variante: 0x2848 octets, 8 variantes consécutives = 0x14240 octets)
Définition complète d'un bloc / composant du jeu. Alloué en 8 variantes (indices 0 à 7 représentant les 8 combinaisons de symétrie/miroir XYZ).
| Offset | Type | Nom | Description |
|---|---|---|---|
| `+0x00..+0x38` | `void*[8]` | `variants` | Tableau de 8 pointeurs vers chaque variante (0..7) |
| `+0x40` | `uint8_t` | `variantIndex` | Index de la variante actuelle (0 à 7) |
| `+0x48` | `DefRingBuffer` | `surfaces` | Surfaces de contact 6 faces (oriente: Back, Front, Left, Right, Top, Bottom) |
| `+0x60` | `DefRingBuffer` | `buoyancySurfaces` | Surfaces étanches pour la flottabilité et physique des fluides |
| `+0x78` | `DefRingBuffer` | `logicNodes` | Noeuds logiques (Composite, Vidéo, Nombre, Booléen, Électricité) |
| `+0xa8` | `DefRingBuffer` | `voxels` | Tampon circulaire des voxels physiques (`VoxelEntry`, 60 octets par voxel) |
| `+0xc0` | `int32_t[3]` | `voxelMin` | Coordonnées voxel minimales `[minX, minY, minZ]` |
| `+0xcc` | `int32_t[3]` | `voxelMax` | Coordonnées voxel maximales `[maxX, maxY, maxZ]` |
| `+0xd8` | `int32_t[3]` | `physicsMin` | Boîte englobante physique minimale |
| `+0xe4` | `int32_t[3]` | `physicsMax` | Boîte englobante physique maximale |
| `+0x110` | `float[3]` | `centerOfMass` | Centre de gravité `[cx, cy, cz]` en mètres |
| `+0x288` | `const char*` | `idStr` | Identifiant technique unique (ex: `"swml_cd_16x2"`, `"monitor_1"`) |
| `+0x290` | `uint32_t` | `idLen` | Longueur de l'identifiant |
| `+0x298` | `const char*` | `nameStr` | Nom d'affichage UI (ex: `"Custom Display 16x2"`) |
| `+0x2a0` | `uint32_t` | `nameLen` | Longueur du nom d'affichage |
| `+0x2a8` | `int32_t` | `category` | Catégorie de composant (6 = Displays / Écrans) |
| `+0x2ac` | `int32_t` | `type` | Type interne de factory (43 = `ComponentMonitor`) |
| `+0x2b0` | `float` | `mass` | Masse physique du bloc (ex: 4.0f * W * H) |
| `+0x2b4` | `int32_t` | `price` | Prix en monnaie de jeu (ex: 500 * W * H) |
| `+0x2b8` | `uint64_t` | `flags` | **Critique :** Doit valoir `0`. Si bit 7 (0x80) actif, déclenche le destructeur XML virtuel au delete et crash! |
| `+0x340` | `float[3]` | `constraintOffset`| Point de contrainte physique |
| `+0x3a0` | `MeshResource` | `mainMesh` | Maillage 3D principal affiché en jeu (OpenGL VBO/VAO) |
| `+0x440` | `MeshResource` | `meshSlot1` | Deuxième emplacement de maillage |
| `+0x4e0` | `MeshResource` | `meshSlot2` | Troisième emplacement de maillage |
| `+0x580` | `MeshResource` | `meshSlot3` | Quatrième emplacement de maillage |
| `+0x620` | `MeshResource` | `editorArrowMesh` | Maillage de flèche d'orientation dans l'éditeur |
| `+0x6c0` | `MeshResource` | `meshSlot5` | Sixième emplacement de maillage |
| `+0x760` | `const char*` | `meshDataName` | Chemin d'accès au fichier maillage (ex: `"rom/meshes/component_screen_1.mesh"`) |
| `+0x790` | `const char*` | `meshEditorName` | Chemin du maillage d'éditeur (ex: `"rom/meshes/arrow_screen_1.mesh"`) |

---

### `VoxelEntry` (Taille: 60 octets / 0x3c)
Représente un voxel cubique d'un composant (dimensions d'un voxel = 0.25m x 0.25m x 0.25m).
| Offset | Type | Nom | Description |
|---|---|---|---|
| `+0x00` | `int32_t` | `x` | Coordonnée X relative en blocs |
| `+0x04` | `int32_t` | `y` | Coordonnée Y relative en blocs |
| `+0x08` | `int32_t` | `z` | Coordonnée Z relative en blocs |
| `+0x0c` | `uint32_t` | `flags` | 1 = voxel physique actif |
| `+0x10` | `uint32_t` | `physicsShape` | Forme géométrique physique (0 = cube plein) |
| `+0x14` | `int32_t[9]` | `rot` | Matrice de rotation 3x3 entière (identité `{1,0,0, 0,1,0, 0,0,1}`) |
| `+0x38` | `uint32_t` | `buoyPipes` | Canalisations et étanchéité interne |

---

## 3. Répertoire des Fonctions Moteur (Symbol Mapping)

### Système de Définition & Registre

#### `FindComponentDefinitionByName`
- **RVA :** `0x46f380` | **VA :** `0x14046f380`
- **Signature :**
  ```cpp
  ComponentDefinition* FindComponentDefinitionByName(void* registry, const SwEngineString* name);
  ```
- **Paramètres :**
  - `RCX` : `registry` — Pointeur vers le registre de composants (`world + 0xbb670`).
  - `RDX` : `name` — Pointeur vers la structure `SwEngineString` contenant l'ID du composant recherché.
- **Retour :** `RAX` — Pointeur vers la variante 0 du composant, ou `nullptr` si non trouvé.
- **Description :**
  Parcourt le tampon circulaire global (`registry + 0xf8`). Compare le nom avec `def->idStr` (`+0x288`). Utilisé par 35 systèmes du moteur : spawner de véhicules, éditeur, chargement de sauvegardes, réseau, LUA.
#### `FindDefinitionDescriptorByName`
- **RVA :** `0x46f2f0` | **VA :** `0x14046f2f0`
- **Signature :**
  ```cpp
  DefinitionFileDescriptor* FindDefinitionDescriptorByName(void* registry, const SwEngineString* name);
  ```
- **Paramètres :**
  - `RCX` : `registry` — Pointeur vers le registre de composants (`world + 0xbb670`).
  - `RDX` : `name` — Pointeur vers `SwEngineString` (`data` et `length`).
- **Retour :** `RAX` — Pointeur vers la structure descripteur de 32 octets (`+0x00`: idStr, `+0x08`: idLen, `+0x10`: xmlData, `+0x18`: xmlLen), ou `nullptr`.
- **Description :**
  Parcourt le tampon circulaire des descripteurs XML (`registry + 0xe0`). Utilisé par le packager de spawn (`0x1404c6280`) et les sous-systèmes réseau pour récupérer les définitions XML brutes. Hooké par SWML pour peupler dynamiquement les descripteurs des écrans créés en mémoire.

---

### Système de Spawn & Cycle de Vie Véhicule

#### `VehicleComponent::DeserializeBlocks`
- **RVA :** `0x4b8a50` | **VA :** `0x1404b8a50`
- **Signature :**
  ```cpp
  void DeserializeVehicleComponents(void* body, void** registryPtr, void* xmlReader, void* outComponents, int bodyIndex);
  ```
- **Paramètres :**
  - `RCX` : `body` — Instance de corps de véhicule (`[rcx + 0x70]` = `world`).
  - `RDX` : `registryPtr` — Pointeur vers `ComponentRegistry*` (`*registryPtr` = `ComponentRegistry`).
  - `R8`  : `xmlReader` — Lecteur XML du fichier de véhicule.
  - `R9`  : `outComponents` — Tableau recevant les composants instanciés.
  - `[RSP + 0x28]` : `bodyIndex` — Indice du sous-corps physique.
- **Description & Analyse Assembleur :**
  Fonction au cœur de l'instanciation des véhicules au spawn et au chargement. Parcourt les nœuds `<c d="...">` (composants) du véhicule et recherche chaque variante directement dans le ring buffer des variantes du registre : `registry + 0xc8`.
  ```x86asm
  48 8B 4D 48          mov rcx, [rbp + 0x48]  ; rcx = registryPtr
  48 8B 01             mov rax, [rcx]         ; rax = *registryPtr (ComponentRegistry)
  8B 90 D8 00 00 00    mov edx, [rax + 0xd8]  ; variantDefsCount
  8B 88 D0 00 00 00    mov ecx, [rax + 0xd0]  ; variantDefsCapacity
  4C 8B 80 C8 00 00 00 mov r8, [rax + 0xc8]   ; variantDefsBuffer
  44 8B B8 D4 00 00 00 mov r15d, [rax + 0xd4] ; variantDefsHead
  ; Boucle sur chaque variante du ring buffer:
  49 8B 3C D0          mov rdi, [r8 + rdx * 8]  ; rdi = variante candidate
  44 3B AF 90 02 00 00 cmp r13d, [rdi + 0x290]  ; compare idLen
  75 26                jne NEXT_VARIANT
  48 8B 97 88 02 00 00 mov rdx, [rdi + 0x288]  ; rdx = idStr
  E8 8A 36 4E 00       call memcmp              ; compare idStr
  85 C0                test eax, eax
  75 09                jne NEXT_VARIANT
  0F B6 47 40          movzx eax, byte ptr [rdi + 0x40] ; variantIndex
  39 45 D0             cmp [rbp - 0x30], eax   ; compare variantIndex (0..7)
  74 53                je VARIANT_FOUND
  ```
  Si la variante n'est pas trouvée dans `registry + 0xc8`, le composant est abandonné et n'apparaît pas dans le monde.

#### `Vehicle::PackageDefinitionsForSpawn`
- **RVA :** `0x4c6280` | **VA :** `0x1404c6280`
- **Signature :**
  ```cpp
  void PackageDefinitionsForSpawn(void* vehicleComponents, void* registry, void* outDefinitionBundle);
  ```
- **Description :**
  Prépare le bundle des définitions XML requises par un véhicule avant son spawn en appelant `FindDefinitionDescriptorByName` (`0x14046f2f0`) sur `registry + 0xe0`. Permet au serveur/client de vérifier et synchroniser les définitions.


#### `SpawnVehicleComponent`
- **RVA :** `0x7f4cc0` | **VA :** `0x1407f4cc0`
- **Signature :**
  ```cpp
  void* SpawnVehicleComponent(void* spawnerContext, void* componentRef);
  ```
- **Paramètres :**
  - `RSI` : Contexte du spawner du monde (`[rsi + 0x70]` = instance du monde serveur/gameplay, `[rsi + 0x1580]` = référence variante).
  - `RDX` : Référence du composant à instancier (`[rdx + 0x368]` = `SwEngineString` avec l'ID).
- **Description & Analyse Assembleur :**
  Fonction critique instanciant un composant lors du clic sur "Spawn" ou lors du spawn scripté/sauvegarde :
  1. Résolution de la définition de composant :
     ```x86asm
     48 8B 4E 70          mov rcx, [rsi + 0x70]      ; rcx = world
     48 81 C1 70 B6 0B 00 add rcx, 0xbb670           ; rcx = world + 0xbb670 (ComponentRegistry)
     48 81 C2 68 03 00 00 add rdx, 0x368             ; rdx = component ID string
     E8 A2 A6 C7 FF       call FindComponentDefinitionByName ; 0x14046f380
     48 85 C0             test rax, rax
     0F 84 D1 01 00 00    jz SKIP_COMPONENT          ; saute et ignore si nullptr
     ```
  2. Sélection de la variante (0..7) :
     ```x86asm
     48 8B 8E 80 15 00 00 mov rcx, [rsi + 0x1580]
     0F B6 51 40          movzx edx, byte ptr [rcx + 0x40] ; variantIndex
     4C 8B 2C D0          mov r13, [rax + rdx * 8]   ; r13 = ComponentDefinition variant
     ```
  3. Vérification des buffers GPU VBO / IBO :
     ```x86asm
     49 8B 85 A0 03 00 00 mov rax, [r13 + 0x3A0]    ; MeshResource de la variante
     48 85 C0             test rax, rax
     74 4B                jz SKIP_MESH
     48 8B 48 08          mov rcx, [rax + 8]         ; c_resource_object_geometry / geom
     48 85 C9             test rcx, rcx
     74 43                jz SKIP_MESH
     8B 41 60             mov eax, [rcx + 0x60]      ; geom->vbo
     85 C0                test eax, eax
     74 3B                jz SKIP_MESH               ; SAUTE L'AFFICHAGE DU MAILLAGE 3D SI VBO == 0 !
     8B 41 64             mov eax, [rcx + 0x64]      ; geom->ibo
     85 C0                test eax, eax
     74 33                jz SKIP_MESH               ; SAUTE L'AFFICHAGE DU MAILLAGE 3D SI IBO == 0 !
     ```
  Si `vbo == 0` ou `ibo == 0`, le moteur n'affiche aucun maillage 3D pour le bloc instancié dans le monde physique.

#### `DeleteVehicleComponentBlock`
- **RVA :** `0x7f4648` | **VA :** `0x1407f4648`
- **Signature :**
  ```cpp
  void DeleteVehicleComponentBlock(void* editorContext, void* block);
  ```
- **Description :**
  Gère la suppression d'un bloc dans l'éditeur de véhicule. Vérifie les bits à `definition + 0x2b8`. Si le bit `0x80` est présent, le moteur tente d'appeler le destructeur virtuel XML, provoquant un crash d'accès mémoire. Doit avoir `flags = 0`.

---

### Système de Rendu & Maillages 3D (OpenGL)

#### `MeshResource::LoadFromMemory`
- **RVA :** `0x2cbfb0` | **VA :** `0x1402cbfb0`
- **Signature :**
  ```cpp
  void MeshResource::LoadFromMemory(MeshResource* dst, const MemBuffer* fileBuf, void* graphicsContext);
  ```
- **Paramètres :**
  - `RCX` : `dst` — Emplacement du `MeshResource` (`definition + 0x3a0` ou `+0x620`).
  - `RDX` : `fileBuf` — Tampon mémoire `{ const void* data, uint32_t size }` du fichier `.mesh` binaire.
  - `R8`  : `graphicsContext` — Pointeur de contexte graphique (`baseMon + 0x3c8`).
- **Description :**
  Nettoie l'ancien maillage via `MeshResource::Clear`, alloue les sommets et indices en RAM, et appelle `CreateOpenGLMeshBuffers` (`0x14020d9a0`) pour créer et uploader les VBO/VAO OpenGL sur la carte graphique. Permet de charger un maillage paramétrique calculé en direct en C++ sans toucher au disque.

#### `MeshResource::LoadFromFile`
- **RVA :** `0x2cc670` | **VA :** `0x1402cc670`
- **Signature :**
  ```cpp
  bool MeshResource::LoadFromFile(MeshResource* dst, void* fileSystem, void* graphicsContext, const SwString* filename);
  ```
- **Description :**
  Ouvre un maillage depuis le système de fichiers / archive du jeu via `0x1401836f0`, puis transmet le tampon mémoire résultant à `MeshResource::LoadFromMemory` (`0x1402cbfb0`).

#### `MeshResource::Clear`
- **RVA :** `0x2cbef0` | **VA :** `0x1402cbef0`
- **Signature :**
  ```cpp
  void MeshResource::Clear(MeshResource* dst);
  ```
- **Description :**
  Libère les tampons CPU des indices (`+0x08`), sommets (`+0x18`) et sous-maillages (`+0x40`) s'ils sont non-nuls. Avant d'appeler `LoadFromMemory` sur une définition clonée, il est indispensable de faire un `memset(dst, 0, 0xa0)` pour éviter que `Clear` ne libère les tampons de la définition source (`baseMon`).

#### `CreateOpenGLMeshBuffers`
- **RVA :** `0x20d9a0` | **VA :** `0x14020d9a0`
- **Signature :**
  ```cpp
  void CreateOpenGLMeshBuffers(void* graphicsContext, void** outGpuBuffers);
  ```
- **Description :**
  Alloue un `c_resource_object_geometry` (taille 16 octets, vtable `0x140b6a9c0`) dans `outGpuBuffers` (`meshRes + 0x30`), puis instancie un événement différé `c_resource_manager_deferred::event_create_object_geometry` (taille 16 octets, vtable `0x140b5af78`, cible = `*outGpuBuffers`) et l'ajoute au ring buffer d'événements de `graphicsContext` (`+0x00`: events, `+0x08`: cap, `+0x0c`: head, `+0x10`: count).

#### `DeferredEventCreateGeometry::Exec`
- **RVA :** `0x20cf40` | **VA :** `0x14020cf40`
- **Signature :**
  ```cpp
  void DeferredEventCreateGeometry::Exec(void* event, void* graphicsContext);
  ```
- **Description :**
  Méthode virtuelle `vtable[1]` de l'événement de création géométrique. Appelle `AllocateMmResourceGeometry` (`0x1401b4a70`) et affecte le pointeur `mm_resource_geometry*` résultant dans `*(event->target + 8)` (soit `*(c_resource_object_geometry + 8)`).

#### `DeferredEventCreateGeometry::Dtor`
- **RVA :** `0x20e800` | **VA :** `0x14020e800`
- **Signature :**
  ```cpp
  void DeferredEventCreateGeometry::Dtor(void* event, uint32_t flags);
  ```
- **Description :**
  Destructeur virtuel `vtable[0]` de l'événement différé. Libère la mémoire de l'événement via `operator delete(event, 0x10)`.

#### `AllocateMmResourceGeometry`
- **RVA :** `0x1b4a70` | **VA :** `0x1401b4a70`
- **Signature :**
  ```cpp
  mm_resource_geometry* AllocateMmResourceGeometry(void* graphicsContext);
  ```
- **Description :**
  Alloue une structure `mm_resource_geometry` de 0x68 octets (vtable `0x140b6abf8`), incrémente son compteur de références et l'enregistre dans la table de ressources du contexte graphique à `graphicsContext + 0x180`. Contient les identifiants OpenGL finaux : `+0x5c`: index count, `+0x60`: VBO (`GL_ARRAY_BUFFER`), `+0x64`: IBO (`GL_ELEMENT_ARRAY_BUFFER`).

#### `DrawGeometryBuffers` (Site du crash établi 0x14068d71c / nvoglv64.dll)
- **RVA :** `0x68d5b0` | **VA :** `0x14068d5b0`
- **Signature :**
  ```cpp
  void DrawGeometryBuffers(void* shaderCtx, mm_resource_geometry* geom, uint64_t indexOffset, int indexCount, ...);
  ```
- **Symptôme & Trace Minidump :**
  Crash à `0x00007FFE206FD14E` dans `nvoglv64.dll` (`RVA 0xB8D14E`, `Code 0xC0000005`, lecture de `0x0000000000000000`).
- **Description & Analyse de Cause Racine :**
  Fonction OpenGL de rendu bas-niveau effectuant :
  ```x86asm
  mov edx, dword ptr [rsi + 0x60] ; geom->vbo
  mov ecx, 0x8892                 ; GL_ARRAY_BUFFER
  call glBindBuffer
  mov edx, dword ptr [rsi + 0x64] ; geom->ibo
  mov ecx, 0x8893                 ; GL_ELEMENT_ARRAY_BUFFER
  call glBindBuffer
  ; Configure glVertexAttribPointer (stride 28, pos=0, color=12, normal=16)
  mov edx, dword ptr [rsi + 0x5c] ; geom->indexCount
  call glDrawElements             ; glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_SHORT, (void*)0)
  call glBindBuffer(0x8892, 0)
  call glBindBuffer(0x8893, 0)
  ```
  Le moteur n'utilise pas de VAO dans cette routine, mais lie directement `geom->vbo` et `geom->ibo`.
  Si le maillage a des identifiants VBO/IBO nuls (`0`), `glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0)` délie l'IBO, et Nvidia tente de lire `(void*)0` comme un pointeur mémoire client CPU, provoquant un plantage immédiat.
  Si le maillage pointe vers le `geom` d'un moniteur 2x2 (`monitor_2`), OpenGL dessine le maillage 2x2 quelle que soit la taille physique déclarée.
- **Solution Finale :**
  1. Calcul mathématique direct des coordonnées des 84 sommets et 150 indices pour toute taille `W x H` via `BuildCustomDisplayMeshBuffers(w, h, frameBuf, arrowBuf)`.
  2. Allocation directe sur le GPU via `glGenBuffers` et `glBufferData` de VBO/IBO dédiés pour chaque taille `W x H`.
  3. Clonage de la structure `c_resource_object_geometry` (16 octets) et `mm_resource_geometry` (0x68 octets) avec les handles GPU personnalisés et mise en cache dans `s_gpuMeshCache`.
  4. Application à toutes les variantes (0..7) de la définition du composant dans le registre : l'écran s'affiche aux dimensions exactes tant dans l'éditeur que dans le monde simulation.

---

### Usine de Composants & Types

#### `ComponentFactory`
- **RVA :** `0x45eb50` | **VA :** `0x14045eb50`
- **Description :**
  Switch central sur `definition + 0x2ac` (type). Type 43 instancie un `ComponentMonitor` (taille 0x610 octets, constructeur à `0x140365b80`, vtable `0x140b70ea0`).

---

### Interface & Éditeur de Véhicule

| RVA | VA | Nom Usuel | Rôle |
|---|---|---|---|
| `0x801340` | `0x140801340` | `VehicleEditor::SelectDefinitionInHand` | Met un composant dans la main du joueur dans l'éditeur |
| `0x611070` | `0x140611070` | `CatalogueWidget::AddItem` | Insère dynamiquement un nouvel item dans le catalogue |
| `0x611050` | `0x140611050` | `CatalogueWidget::GetSelectedItem` | Récupère l'élément sélectionné dans le catalogue |
| `0x60a470` | `0x14060a470` | `ToolbarWidget::GetActiveSlot` | Récupère le slot actif de la barre d'outils |
| `0x60bd10` | `0x14060bd10` | `LayoutGrid::Layout` | Recalcule le layout en grille du catalogue |
| `0x24b650` | `0x14024b650` | `ScrollPane::Refresh` | Met à jour le défilement et la zone visible d'une vue déroulante |
| `0x25aa60` | `0x14025aa60` | `Widget::SetChildrenOpen` | Active/désactive récursivement les widgets enfants |
| `0x24f7d0` | `0x14024f7d0` | `Editbox::SetAlpha` | Règle l'opacité et l'activation d'un champ de saisie |
| `0x8dd150` | `0x1408dd150` | `RingBuffer::Grow` | Réalloue et agrandit un `DefRingBuffer` saturé |

---

## 4. Vtables & Cycles de Vie de l'Éditeur de Véhicule

- Table de Vtable UI Éditeur : `stormworks64.exe + 0xb99940`
  - `uiInitSlot` (RVA `0x62ac80`) : Initialisation de l'UI
  - `uiUpdateSlot` (RVA `0x62c530`) : Boucle de frame de l'UI
  - `uiTeardownSlot` (RVA `0x241bc0`) : Fermeture de l'UI
  - `uiDestroySlot` (RVA `0x62ed20`) : Destruction du conteneur UI
- Table de Vtable State Véhicule : `stormworks64.exe + 0xb44838`
  - `stateEnterSlot` (RVA `0x7cfce0`) : Entrée dans l'éditeur de véhicule
  - `stateLeaveSlot` (RVA `0x7d0770`) : Sortie de l'éditeur de véhicule
  - `stateActionsSlot` (RVA `0x7fa7a0`) : Dispatch des actions utilisateur (sélection catalogue, raccourcis, clic)
  - `stateTeardownSlot` (RVA `0x7cf750`) : Nettoyage d'état

---

## 5. Gestion de la Mémoire & Destructeurs du Registre

#### `ComponentRegistry::Clear`
- **RVA :** `0x5f1fd0` | **VA :** `0x1405f1fd0`
- **Signature :**
  ```cpp
  void ComponentRegistry::Clear(void* registry);
  ```
- **Description :**
  Nettoie et réinitialise tous les buffers d'un registre de composants (`world + 0xbb670`) lors de la transition d'un monde ou de l'entrée dans l'établi.
  1. Vide les catégories de composants (`registry + 0x110`).
  2. Libère le tampon global `registry + 0xf8` via `_aligned_free` (`0x1409B15A0`).
  3. Appelle le destructeur des descripteurs XML (`0x1408B8EE0`) sur `registry + 0xe0`.
  4. Appelle le destructeur des variantes (`0x1408B8FB0`) sur `registry + 0xc8`.

#### `DestructXmlDescriptors`
- **RVA :** `0x8b8ee0` | **VA :** `0x1408b8ee0`
- **Description :**
  Parcourt le ring buffer des descripteurs XML (`registry + 0xe0`), et libère individuellement pour chaque entrée :
  - `desc->xmlData` (+0x10) via `_aligned_free` (`0x1409B15A0`)
  - `desc->idStr` (+0x00) via `_aligned_free` (`0x1409B15A0`)
  - `desc` via `operator delete(desc, 32)` (`0x14095706C`)
  > [!CAUTION]
  > Tout descripteur alloué par le runtime d'une DLL externe passé à ce destructeur provoquera un crash instantané `STATUS_HEAP_CORRUPTION` (`0xc0000374`). Le mod intercepte `ComponentRegistry::Clear` pour retirer et libérer ses propres descripteurs avant l'appel moteur.

#### `DestructVariantDefs`
- **RVA :** `0x8b8fb0` | **VA :** `0x1408b8fb0`
- **Description :**
  Parcourt le ring buffer des variantes (`registry + 0xc8`), et appelle pour chaque variante :
  - Destructeur de variante : `0x14087EE00`
  - `operator delete(var, 0x2848)` (`0x14095706C`)
  Le hook du mod compacte et retire toutes les variantes virtuelles `"swml_"` avant l'exécution du destructeur moteur afin d'éviter tout conflit de tas.
