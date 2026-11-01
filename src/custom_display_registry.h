#pragma once
#include <string>
#include <utility>
#include <vector>

namespace swml {

// Initializes resolution cache from %APPDATA%/Stormworks/data/mods/swml_custom_display/resolutions.txt
void InitializeCustomDisplayRegistry();

// Sets and persists resolution for a specific definition ID (e.g. "swml_cd_2x1")
void SetCustomDisplayResolution(const std::string& defId, int px, int py);

// Finds or assigns an available slot definition name for a given (w, h) and (px, py).
// For example, if "swml_cd_2x2" already has (256, 256) and user asks for (64, 64),
// it will return "swml_cd_2x2_s2".
std::string GetOrAllocateDisplayDefinitionId(int w, int h, int px, int py);

// Gets the stored resolution for a definition ID. Returns true if found in registry.
bool GetCustomDisplayResolution(const std::string& defId, int& px, int& py);

// Remembers last configured values for dialog prefilling
void SetLastDialogValues(int w, int h, int px, int py);
void GetLastDialogValues(int& w, int& h, int& px, int& py);

// Returns all registered custom displays for pre-initialization
std::vector<std::pair<std::string, std::pair<int, int>>> GetAllRegisteredDisplays();

} // namespace swml
