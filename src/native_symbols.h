#pragma once
#include <cstddef>
#include <cstdint>

namespace swml {
namespace discovery { struct Image; }

// Version-guarded resolution of GENERIC engine roles for the native primitives.
//
// Two mechanisms, both fail-closed:
//   1. Roles already present in the compiled reviewed binding table (allocator,
//      engine strings, localization) are resolved through EngineRva +
//      ValidateEngineBinding: the reference RVA is mapped per reviewed build and
//      the retained body bytes are compared before the address is handed out.
//   2. Roles identified by a retained masked instruction contract
//      (component registry lookup/clear/grow, localization insert) are located
//      with discovery::Find over the executable sections of the loaded image.
//      discovery::Find only returns Status::Found for a UNIQUE full-body match;
//      missing or ambiguous contracts stay unresolved.
//
// No address is ever derived from a mod-supplied constant.
void InitializeNativeSymbols(uint8_t* base);
void ShutdownNativeSymbols();
// Absolute address for a role, or nullptr when unknown/absent/unvalidated.
const void* FindNativeSymbol(const char* role);
// Pure, testable variant used by the unit tests (no global cache).
bool FindNativeSymbolInImage(const discovery::Image& image, const char* role, uint32_t& rva);
// Role names, for diagnostics and tests.
const char* const* NativeSymbolRoles(size_t* count);
}  // namespace swml
