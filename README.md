# OBMEFix 2.0.1

OBMEFix is a companion plugin for the original OBME.dll on **Oblivion 1.2.0416, 32-bit**. The current source requires **xOBSE major version 22 or later**, using the local 22.13.0 headers. PluginInfo reports version 2. Keep the original OBME.dll installed.

## Install

Copy `Output/Release/OBMEFix.dll` to the active game's `Data/OBSE/Plugins` directory beside OBME.dll, with the game closed. The release ZIP contains the same directory structure.

OBMEFix registers its listener first and installs during PostLoad/PostPostLoad. It rechecks its sites at PreLoadGame. A failed or partial installation keeps the DLL resident so callbacks and engine hooks cannot point into unloaded code.

## Protections and compatibility

- Prevents finite duration SEFF effects over one second on non-actor targets while preserving a prior validator rejection.
- Drops matching saved records only after a bounded, single-pass parse proves their extent, duration, and record budget.
- Restores the native negative non-Fatigue damage floor for calls originating in OBME. Each patched slot forwards to its own captured original, including after another plugin copies the vtable.
- Handles a null VFX result with SEFF fallback, then a no-model-path constructor branch if the fallback is also unavailable.
- Preserves usable OBME AddSpell icons and falls back through the vanilla strongest-effect icon, SEFF icon, and default HUD icon. Returned paths match the normalized value checked.
- Checks patch ownership and complete byte spans. Foreign hooks are left untouched. A fully recognized jump directly to OBMEFix's own AddSpell adapter can be repaired to its required CALL.
- Checks memory-protection changes and code-cache flushing; reports write failures and partial states.
- Keeps a site unverified if rollback cache flushing or page-protection restoration fails, including when both fail.
- Rechecks the load caller's stack contract on revalidation and before reading the additional frame fields; a changed caller bypasses salvage without altering its record cursor.
- Limits recurring gameplay diagnostics while retaining totals.

CSI.dll presence does not establish ownership of an overwritten hook and does not authorize OBMEFix to replace it. Resolve conflicts using the log's observed state. A recognized OBME load/icon adapter must match the supported x86 shape and executable module range.

## Save-salvage limits

The supported native record-layout range is **0x2A through 0x7D**; IDA confirms that this executable initializes its current record version to 0x7D. The older RSH comment saying 0x71 was stale. OBME conversion metadata is limited to the recognized unversioned/1.0 formats in GuardCore.h.

Salvage currently requires the active parent's deferred ChangesMap buffer, whose four-byte prefix must agree with the temporary loading-form header and record version. Its declared allocation span and readable memory are checked. Legacy BLOK bounds, when present, further restrict that span; the serialized record count is checked independently.

If the format, conversion version, parent context, record extent, count, or duration cannot be verified, OBMEFix logs a bounded bypass event and leaves loading to OBME. **This does not make OBME's upstream reader safe for malformed or incomplete files.** Immediate-load contexts without a matching deferred buffer may bypass salvage. No damaged save is rewritten by the plugin.

## Build and targeted checks

Clone with the pinned SDK dependency, then build from the repository root with Visual Studio 2022 C++ and Windows SDK tools installed:

```powershell
git clone --recurse-submodules https://github.com/KinzhaldeDagas/OblivionMagicExtenderFix.git
cd OblivionMagicExtenderFix
```

For an existing clone, run `git submodule update --init --recursive`. The projects use `external/xOBSE` and forward the selected platform toolset to its common library. See [dependency provenance](docs/DEPENDENCIES.md).

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" "OBMEFix.vcxproj" /p:Configuration=Release /p:Platform=Win32 /t:Build /verbosity:minimal
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" "OBMEFixTests.vcxproj" /p:Configuration=Release /p:Platform=Win32 /t:Build /verbosity:minimal
& "TestOutput\Release\OBMEFixTests.exe"
```

The test executable includes the production implementation with isolated address fixtures. It covers parser truncations and versions, record/index/cursor handling, native load/VFX adapters, lifetime, validator chains, copied vtables, ownership conflicts, patch failures, icon normalization, and bounded event logging. It does not run or modify Oblivion.

The latest correction pass finished with **220 checks passed, zero failed in each of Release and Debug**. Gameplay is a separate user check. See the [current verification record](<docs/VERIFICATION.md>).

For Debug, use `/p:Configuration=Debug` and `TestOutput/Debug`. To run the isolated compiler analysis without changing normal build outputs:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" "OBMEFix.vcxproj" /p:Configuration=Release /p:Platform=Win32 /p:BuildProjectReferences=false /p:OBMEFixAnalysis=true /t:ClCompile
```

`main.cpp` retains the two exported OBSE entry points. `RuntimeGuards.cpp` contains installation and runtime behavior; `EngineInterop.h` isolates verified native addresses/field access while using the pinned SDK's `TESSaveLoadGame`, `ChangesMap`, and actor-value definitions. The SDK does not name all serialization fields, so those byte offsets remain documented at that boundary rather than presented as a complete private loader class. `GuardCore.h` and `CheckedPatch.h` retain the parsing and patch policies. Tests include the production implementation as an isolated native fixture, not a live game integration.

## Gameplay checklist

Gameplay verification is reserved for the user.

1. Use the new DLL and retain the original OBME.dll. Inspect the sibling OBMEFix.log for the current hook-status line and any conflicts or patch failures.
2. Check short-duration/non-duration SEFF and actor targets still behave normally; check long-duration SEFF on a door/container is rejected and logged.
3. Load a copy of an affected save. Note whether the log records a dropped effect or a salvage bypass; a bypass is not a successful salvage.
4. Check damage to depleted Magicka, Fatigue behavior, and recoverable effects.
5. Add spells with normal, empty, prefixed, and custom icons. Confirm the notification does not reuse a stale item icon.
6. Check ordinary impact visuals and a missing-VFX case. Confirm no constructor crash, then save/load and remove the effect normally.
