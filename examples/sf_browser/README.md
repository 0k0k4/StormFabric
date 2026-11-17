# sf_browser — Chromium dans Stormworks

**Statut : fonctionne hors jeu (test de bout en bout), à valider en jeu.**

`sf_browser` affiche des pages web (Chromium, via CEF 156) par-dessus le jeu et les offre aux
autres mods sous forme du service `browser` (`sf_browser_api.h`). Il s'appuie sur trois
briques du loader : l'overlay d'images (`api->image`), l'entrée sur les images et les
services entre mods (`api->services`).

## Architecture

```text
stormworks64.exe                                 sf_browser_host.exe (+ sous-processus CEF)
┌──────────────────────────────┐   pipe _cmd     ┌───────────────────────────────┐
│ mod consommateur             │ ──────────────▶ │ CEF, rendu hors écran          │
│   └─ service "browser"       │                 │  OnPaint → mémoire partagée    │
│ sf_browser.dll               │ ◀────────────── │  événements (titre, messages…) │
│   ├─ image + entrée (loader) │   pipe _evt     └───────────────────────────────┘
│   └─ thread « pompe » ◀──── mémoire partagée (pixels BGRA, rectangle sale)
└──────────────────────────────┘
```

- **Chromium tourne dans son propre processus.** Aucune DLL CEF n'est chargée dans le jeu
  (règle 7 respectée : CEF livre `d3dcompiler_47.dll` et `vulkan-1.dll`, noms de System32),
  un plantage de Chromium ne fait pas tomber le jeu, et le processus est rattaché à un *job*
  Windows : il meurt avec le jeu.
- **Pixels** : CEF peint dans `OnPaint`, l'hôte copie les rectangles sales dans une section
  de mémoire partagée (une par navigateur et par taille) et signale un événement ; le thread
  « pompe » du mod envoie la région sale à l'image du loader.
- **Entrée** : le callback d'entrée de l'image transmet souris et clavier à l'hôte
  (`SendMouse*Event`, `SendKeyEvent`). Les menus déroulants (`<select>`) sont composés dans
  l'image.
- **Messages page ↔ mod** : `window.sfBrowser.send(texte)` côté page (via `cefQuery`),
  `postMessage(texte)` côté mod → `window.sfBrowser.onmessage(texte)`.
- Les fenêtres popup (`target=_blank`, `window.open`) s'ouvrent dans le même navigateur.

## Utiliser le service depuis un mod

1. `"dependencies": ["sf_browser"]` dans le `mod.json`.
2. Récupérer la table :

```cpp
const void* table = nullptr;
host->api->services->get(host, SF_BROWSER_SERVICE, SF_BROWSER_VERSION, &table, nullptr, nullptr);
auto* browser = static_cast<const SfBrowserService*>(table);
```

3. Créer un navigateur :

```cpp
char url[1024];
browser->fileUrl(host, "page.html", url, sizeof(url)); // fichier du dossier de TON mod
SfBrowserOptions o = {sizeof(o)};
o.url = url; o.width = 900; o.height = 560;
o.surface = SW_SURFACE_NONE; o.rect = {0, 0, 0, 0, 4}; // centré, 1 pixel de page = 1 pixel écran
o.interactive = true; o.transparent = true;
SfBrowserHandle page = 0;
browser->create(host, &o, &OnEvent, nullptr, &page);
```

Fonctions : `loadUrl`, `executeJavaScript`, `postMessage`, `navigate` (précédent, suivant,
recharger, arrêter), `resize`, `setRect`, `setVisible`, `setOrder`, `setOpacity`,
`setFocus`, `destroy`. Les événements (`SF_BROWSER_EVENT_*`) arrivent sur le thread IPC de
sf_browser : rester bref. Les navigateurs d'un mod sont fermés automatiquement à son
déchargement.

Exemple complet : `demo/` (`browser_demo`, page `demo.html`, F10 pour afficher/masquer).

## Construire

1. Télécharger la **Minimal Distribution** Windows 64 bits de CEF depuis
   <https://cef-builds.spotifycdn.com/index.html> (testé : 156.0.30, Chromium 156.0.8078.96)
   et la décompresser dans `third_party/cef` (dossier ignoré par git).
2. `examples\sf_browser\build.bat` : compile la bibliothèque C++ de CEF au premier passage
   (`build_cef_wrapper.bat`, cl uniquement, sans CMake), puis `sf_browser_host.exe`,
   `sf_browser.dll` et `browser_demo`, et copie le runtime CEF.
3. `examples\sf_browser\test\run_e2e.bat` : test de bout en bout hors jeu (fenêtre OpenGL,
   vrai overlay et vrais services) : rendu, clic, clavier, messages, redimensionnement,
   déchargement.

Sortie : `build\examples\sf_browser\` (≈ 400 Mo avec le runtime CEF) et
`build\examples\browser_demo\`.

## Installer

Jeu fermé, copier `build\examples\sf_browser\` vers `<Stormworks>\mods\sf_browser\` et
`build\examples\browser_demo\` vers `<Stormworks>\mods\browser_demo\`, puis les activer.
Le cache de Chromium va dans `mods\sf_browser\cache\`, son journal dans
`mods\sf_browser\cef.log`.

## Limites actuelles

- Pas de saisie IME (langues asiatiques), pas de son contrôlé par le jeu, pas de
  téléchargements de fichiers, pas de boîtes de dialogue JavaScript natives.
- Pas encore d'accès depuis Lua (microcontrôleurs, addons) ni d'affichage sur les moniteurs
  de véhicule.
- Le curseur affiché reste celui du jeu (les formes de curseur de la page ne sont pas reprises).
