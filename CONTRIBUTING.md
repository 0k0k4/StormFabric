# Contributing to StormFabric

Thank you for your interest in contributing to StormFabric!

## Code of Conduct

Please be respectful and constructive in all issues, pull requests, and discussions.

## Development Prerequisites

- **Windows 10/11 x64**
- **MSVC v143+** (Visual Studio 2022 Community or Build Tools with C++ workload)
- **Windows 10/11 SDK**
- **Python 3.9+** (optional, for contract validation and test runners)
- **Git**

## Building the Project

Open an x64 Native Tools Command Prompt or standard Command Prompt:

```cmd
build.bat
```

This compiles:
- `build/iphlpapi.dll` (The main proxy loader)
- `build/mods/CustomDisplay/custom_display.dll`

To build the example plugins:

```cmd
examples\build_examples.bat
```

To run offline unit and regression tests:

```cmd
tests\run_tests.bat
```

## Architectural Guidelines

1. **Clean-Room SDK**:
   - Mods using the public SDK (`include/sw_mod_sdk.h`) must never depend on hardcoded game memory addresses or offsets.
   - Any engine-specific hooks or offsets belong inside the loader core (`src/engine_bindings.cpp`, `src/engine_discovery.cpp`).
2. **Binary Interface (ABI) Stability**:
   - The SDK C structures use explicit `structSize` members.
   - New members must always be appended to the end of structures to maintain backward compatibility.
   - Never pass heap-allocated CRT objects (e.g. `std::string`, `std::vector`) across DLL boundaries; use raw C types or opaque handles.
3. **Safety & Process Integrity**:
   - StormFabric does not hook unknown builds by default. Every hook requires SHA-256 and instruction witness validation.

## Submitting Pull Requests

1. Fork the repository and create your feature branch: `git checkout -b feature/my-feature`.
2. Ensure `build.bat`, `examples\build_examples.bat`, and `tests\run_tests.bat` all pass cleanly.
3. Commit with clear, descriptive commit messages.
4. Push your branch and open a Pull Request describing your changes and testing methodology.
