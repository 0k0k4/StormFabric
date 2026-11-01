# Security Policy

## Scope & Execution Model

StormFabric operates as an in-process native Windows DLL (`iphlpapi.dll`).

- **No Sandbox**: Mods compiled as native Windows dynamic link libraries have full user-level execution privileges within the game's process.
- **Safety Warning**: Only install plugins and mods from developers you trust. StormFabric validates game executable hashes to prevent improper hooking, but cannot sandbox arbitrary native code loaded by mods.

## Reporting a Vulnerability

If you discover a security vulnerability or exploit in StormFabric (e.g. buffer overflows in IPC/storage, memory corruption in the mod loader core):

1. **Do not** report security vulnerabilities via public GitHub issues.
2. Please open a private security advisory through GitHub's Security Advisories tab or contact the repository owner directly.
3. Include detailed reproduction steps and affected versions.
