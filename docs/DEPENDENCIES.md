# Dependency provenance

- xOBSE: https://github.com/llde/xOBSE
- Pinned revision: `5078a1dcd2d115bf1f900cfe698b6334cae61707` (22.13.0 headers)
- Submodule path: `external/xOBSE`

The submodule retains upstream source and notices. Its common library is built using the toolset selected by the OBMEFix project; the upstream project is not edited. SDK declarations are used for public interfaces, while confirmed fields missing from the SDK are isolated in `EngineInterop.h`.

The original OBME runtime is a separate requirement: https://github.com/JRoush/Oblivion-Magic-Extender . OBMEFix is a companion to OBME.dll. Neither Oblivion executables/assets nor the original OBME binaries are included in this repository.
