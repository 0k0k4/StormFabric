#pragma once
#include <cstdint>

namespace swml {
// Explicit SWML_CD_VEHICLE_OBSERVE=1, authenticated pass-through diagnostics.
// No UI, imports, input capture, placement changes or resolution bridge.
bool InstallCustomDisplayVehicleObserver(uint8_t* base);
void UninstallCustomDisplayVehicleObserver();
}
