# Verification — OBMEFix 2.0.1

Supported runtime: original Oblivion 1.2.0416, x86, with original OBME.dll and xOBSE major 22 or later. PluginInfo version remains 2.

The 2026-10-13 correction pass preserved the two plugin exports and existing save-salvage scope. It corrected ignored rollback cache-flush failures, preserved combined restoration failures, and checked the native caller signature before trusting a parent pointer or additional frame fields. Native entry points and SDK field access were separated into focused components.

## Evidence from the implementation pass

| Check | Result |
| --- | --- |
| Baseline Release/Debug native harnesses | PASS — 206 checks each |
| Four regression assertions before the behavior corrections | FAIL as expected |
| Final Release/Debug builds and native harnesses | PASS — 220 checks each |
| MSVC NativeRecommendedRules analysis | PASS — zero warnings/errors |
| PE architecture and exports | PASS — x86; OBSEPlugin_Query and OBSEPlugin_Load |
| Selected supported-target instructions | PASS — native IDA inspection and disk byte comparison |
| Gameplay and third-party plugin combinations | NOT RUN — user-owned verification |

These are isolated native x86 fixtures, compiler analysis, and binary inspection, not a live game session. Invalid/unsupported save contexts still delegate to OBME; this does not make its upstream reader safe for malformed files. No atomicity, general race-freedom, or performance claim is made.

The independently deployed Release DLL had SHA256 `E02759765DDCAFA24798ACA149794000266439BA72CECDC2B5BC2393198FC292`. Rebuilding from another checkout can produce a different binary hash (for example, debug paths and linker metadata differ).

## Reproducing checks

Build `OBMEFix.vcxproj` and `OBMEFixTests.vcxproj` with `Configuration=Release` or `Debug` and `Platform=Win32`, then run `TestOutput/<Configuration>/OBMEFixTests.exe`. Use `OBMEFixAnalysis=true`, `BuildProjectReferences=false`, and target `ClCompile` for isolated compiler analysis. The README contains full commands.

## Publication-checkout verification

The repository layout itself was rebuilt on 2026-10-13 using the pristine pinned submodule, without patching its project file. Both Release and Debug builds succeeded and both native harness runs passed 220 checks. The toolset is forwarded through ProjectReference metadata; the upstream source remains unchanged. The upstream Debug common-library build emits its existing deprecated /Gm warning.
