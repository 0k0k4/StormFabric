#pragma once
#include <string>
#include <vector>

#include "mods_scan.h"

namespace swml {

// Content mods: files under mods/<id>/content/ (or the manifest's "content" folder)
// overlay the game's rom/ tree. Reading rom/<path> opens the mod's file when one
// exists, and listing a rom/ directory also returns the mods' files. Writes are
// never redirected. Implemented on the game executable's kernel32 imports, so no
// game address is involved. Installed once at startup, before the game reads rom/,
// from the mods enabled at that moment; changes apply after a restart.
// Returns true when the overlay is active (at least one enabled mod has content).
bool InstallContentOverlay(const std::vector<ModEntry>& mods);

// True when this mod's content is part of the active overlay (key: ModEntry::name,
// case-insensitive). False for mods enabled after startup.
bool ContentOverlayActive(const std::string& modName);

}  // namespace swml
