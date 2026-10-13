# Changelog

All notable changes to `OBMEFix.dll`.

## 2.0.1 - 2026-10-13

### Fixed
- Distinguish failed rollback cache/readback from a recoverable write failure, retain combined rollback/protection-restoration failures, and keep uncertain sites unverified until restart.
- Validate the SEFF load caller/loop signature even after the hook is installed, and bypass metadata reads when another patch changes that stack contract.

### Internal interfaces and verification
- Keep public entry points in main.cpp and move guard behavior into RuntimeGuards.cpp.
- Use the pinned SDK loader type with a narrow verified field-access boundary, typed ChangesMap receivers, SDK actor-value constants, and compile-time ABI checks. No allocator or object ownership changes.
- Keep supported game/extender versions, the two exports, PluginInfo version 2, and the existing conservative salvage policy.
- Baseline Release and Debug both passed 206 checks on the host. Four added regression assertions failed before the behavior fixes; final Release and Debug both passed 220 checks, including invalid-parent and valid-actor ordering cases.
- Add an opt-in compiler-analysis configuration with its own intermediate directory. The shared xOBSE dependency project is unchanged by this pass.

## 2.0.0 - 2026-10-08

### Fixed
- Keep the plugin resident after listener registration; defer installation to post-load messages.
- Preserve creation-validator chains and refuse unknown owners instead of introducing recursion.
- Remove broad reassertion over CSI/other plugin hooks. Repair only a fully recognized jump to the plugin's own icon adapter.
- Add a caller-side VFX null branch when both the original result and SEFF fallback are absent; handle an unavailable effect collection safely.
- Parse saved effects once with declared deferred-buffer bounds, record budgets, checked lengths, raw conversion markers, supported versions, and finite durations.
- Preserve OBME's node-index semantics, including null list nodes.
- Bind each damage callback to its original slot so copied vtables continue forwarding exactly once.
- Validate complete patch spans, memory-protection operations, and instruction-cache flushes. Retain partial status on protection-restoration faults.
- Refresh all guard states independently and limit repeated gameplay log events.
- Return the same normalized icon path that was validated; identify the strongest-item boolean correctly as requireArea.

### Compatibility and verification
- The native record-version ceiling is 0x7D, decoded at 0x0045B648; the old local 0x71 comment was stale.
- Unverified save contexts, unsupported versions, and malformed record candidates bypass salvage and continue through OBME. Upstream malformed-file handling is outside this guard's guarantee.
- Release Win32 build and 206 isolated x86 checks passed. Gameplay testing is reserved for the user.
- PluginInfo version is 2. The original OBME.dll is still required; current source requires xOBSE major version 22 or later.

## 1.0.0 - 2026-05-26

### Added
- Added a load-salvage path for `OBME.dll` load-order variance.
- Added OBSE post-load revalidation so runtime guards are checked again on both `kMessage_PostLoad` and `kMessage_PostPostLoad`.
- Added runtime diagnostics for hard and soft hook conflicts, including `CSI.dll` and `OBME_CS.dll`.

### Fixed
- Fixed crash-prone active-effect load behavior for non-actor `SEFF` conversion records with duration greater than 1 second by dropping only the invalid saved non-actor conversion records while preserving list index behavior.
- Fixed non-actor `SEFF` creation guard by patching `ScriptEffect::IsTargetValid` only when OBME/vanilla assumptions are still valid.
- Fixed unsafe installation scenarios by refusing to patch when:
  - `OBME.dll` is not loaded.
  - `MagicItem::GetVFXEffect` hook target is still vanilla/unexpected.
  - `MagicItem::GetVFXEffect` hook target is outside `OBME.dll`.

### Changed
- Hook installation now mirrors OBME/IDA behavior more strictly for active-effect load:
  - reads conversion headers with `LoadFormID` like OBME,
  - preserves saved `recordSize`/index accounting,
  - and advances the buffer cursor to the end of the consumed real record.
- Runtime guard setup now verifies current function ownership before installing patches and tracks prior states to avoid duplicate logs unless ownership changes.
- Diagnostic messages now include plugin presence, hook-target address, and explicit known-conflict notices.

### Verified
- Source and runtime behavior were cross-checked against:
  - OBME source paths (`_ActiveEffects.cpp`, conversion/load routines).
  - Relevant Oblivion hook points from IDA (`0x0068F035`, `0x006A2A60`, `0x00A76AE0`, etc.).
  - OBSE message lifecycle behavior for reliable post-load retry logic.
- Release build produced:
  - `Output/Release/OBMEFix.dll` (size: `15872`, SHA256: `138CFD1268731FCBB7A000EBC9648AC89E705AE95B692B85EA1981FCB5F27C12`).
  - `Oblivion/Data/OBSE/Plugins/OBMEFix.dll` with identical size/hash.

### Known limits
- No in-game repro run was performed in this pass.
- External tooling integration conflicts (xEdit/TES4Edit/Wrye Bash/CBash/PBash/Bashed Patch) are tracked as environment issues, not runtime `OBMEFix.dll` runtime fixes.
