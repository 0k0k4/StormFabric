#pragma once

namespace swml {
// Installe les hooks (patch de slots de vtable) qui ajoutent le bouton MODS et la vue native
// de liste des mods dans le menu principal. À appeler une fois, depuis DllMain.
// Retourne false si l'exécutable n'est pas la version attendue (aucun patch n'est alors fait).
bool InstallModsUi();
}  // namespace swml
