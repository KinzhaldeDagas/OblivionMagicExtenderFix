#include "RuntimeGuards.h"
#include "EngineInterop.h"
#include "obse/GameMagicEffects.h"
#include "obse/GameForms.h"
#include "obse/GameObjects.h"
#include "GuardCore.h"
#include "CheckedPatch.h"

#include <Windows.h>
#include <intrin.h>
#include <cstddef>
#include <cstring>

IDebugLog gLog;

namespace OBMEFix
{
    using namespace OBMEFixEngine;
    const char kDefaultAddSpellMessageIconPath[] = "icon_hud_default_magic.dds";

    enum MagicItemVFXHookDiagnosticState
    {
        kMagicItemVFXHookDiagnostic_None = 0,
        kMagicItemVFXHookDiagnostic_NotJump,
        kMagicItemVFXHookDiagnostic_CsiConflict,
        kMagicItemVFXHookDiagnostic_UnknownOwner,
    };

    struct DamageAVFHookSpec
    {
        const char* name;
        UInt32 vtable;
        UInt32 entry;
        UInt32 expectedTarget;
        UInt32 originalTarget;
    };

    struct RuntimeDataFileDiagnostic
    {
        const char* pathFromPluginDir;
        const char* fileName;
    };

    typedef TESForm* (__cdecl * LookupFormByIDFn)(UInt32);
    typedef void (__thiscall * DamageAVFFn)(Actor*, UInt32, float, Actor*);
    typedef EffectSetting* (__thiscall * MagicItemGetVFXEffectFn)(MagicItem*, UInt32);
    typedef EffectItem* (__thiscall * EffectItemListGetStrongestItemFn)(EffectItemList*, UInt32, bool);
    typedef const char* (__thiscall * OBMEAddSpellMessageIconFn)(EffectItemList*, UInt32, bool);

    PluginHandle s_pluginHandle = kPluginHandle_Invalid;
    UInt32 s_obmeLoadActiveEffectHook = 0;
    UInt32 s_scriptEffectIsTargetValid = 0;
    UInt32 s_obmeMagicItemVFXHook = 0;
    UInt32 s_magicModelHitEffectGetVFXCallTarget = 0;
    UInt32 s_obmeAddSpellMessageIconHook = 0;
    UInt32 s_droppedNonActorScriptEffects = 0;
    UInt32 s_blockedNonActorScriptEffects = 0;
    UInt32 s_clampedOBMEModAVDamage = 0;
    UInt32 s_substitutedNullMagicItemVFXEffects = 0;
    UInt32 s_substitutedAddSpellMessageIcons = 0;
    UInt32 s_loadBypassEvents = 0;
    UInt32 s_invalidDurationEvents = 0;
    UInt32 s_installedDamageAVFClampSlots = 0;
    UInt32 s_expectedDamageAVFClampSlots = 0;
    bool s_runtimeGuardsInstalled = false;
    bool s_loggedCsiDiagnostic = false;
    bool s_loggedCsiFileDiagnostic = false;
    bool s_loggedObmeFileDiagnostic = false;
    bool s_loggedObmeCsDiagnostic = false;
    bool s_loggedObmeCsFileDiagnostic = false;
    bool s_loggedExampleEspFileDiagnostic = false;
    bool s_loggedAddSpellMessageIconFallback = false;
    UInt32 s_lastMagicItemVFXHookDiagnosticState = kMagicItemVFXHookDiagnostic_None;
    UInt32 s_lastMagicItemVFXHookDiagnosticTarget = 0;
    DamageAVFHookSpec s_damageAVFHookSpecs[] =
    {
        { "Actor", kActorVtbl, kActorVtbl + kDamageAVFSlot, kVanillaActorDamageAVF, 0 },
        { "Character", kCharacterVtbl, kCharacterVtbl + kDamageAVFSlot, kVanillaActorDamageAVF, 0 },
        { "Creature", kCreatureVtbl, kCreatureVtbl + kDamageAVFSlot, kVanillaCreatureDamageAVF, 0 },
        { "PlayerCharacter", kPlayerCharacterVtbl, kPlayerCharacterVtbl + kDamageAVFSlot, kVanillaPlayerDamageAVF, 0 },
    };
    const RuntimeDataFileDiagnostic kOBMEExampleESPFiles[] =
    {
        { "..\\..\\OBME_CreateMgef_example.esp", "OBME_CreateMgef_example.esp" },
        { "..\\..\\OBME_DSPL_example.esp", "OBME_DSPL_example.esp" },
        { "..\\..\\OBME_EIOverrides_example.esp", "OBME_EIOverrides_example.esp" },
        { "..\\..\\OBME_MDAV_example.esp", "OBME_MDAV_example.esp" },
        { "..\\..\\OBME_Reclaim_example.esp", "OBME_Reclaim_example.esp" },
        { "..\\..\\OBME_SEFF_example.esp", "OBME_SEFF_example.esp" },
        { "..\\..\\OBME_SMAC_example.esp", "OBME_SMAC_example.esp" },
    };

    TESSaveLoadGame* GetSaveLoad()
    {
        TESSaveLoadGame** current = reinterpret_cast<TESSaveLoadGame**>(kCurrentSaveLoadAddr);
        return OBMEFixPatch::Readable(current, sizeof(*current)) ? *current : nullptr;
    }

    TESForm* LookupForm(UInt32 formID)
    {
        if (!formID)
            return nullptr;

        return reinterpret_cast<LookupFormByIDFn>(kLookupFormByIDAddr)(formID);
    }

    EffectSetting* LookupEffectSetting(UInt32 effectCode)
    {
        NiTPointerMap<EffectSetting>* effects = reinterpret_cast<NiTPointerMap<EffectSetting>*>(kEffectSettingCollectionAddr);
        if (!OBMEFixPatch::Readable(effects, sizeof(*effects)) || !effects->m_numBuckets ||
            !effects->m_buckets || effects->m_numBuckets > UINT32_MAX / sizeof(void*) ||
            !OBMEFixPatch::Readable(effects->m_buckets, effects->m_numBuckets * sizeof(void*)))
            return nullptr;
        auto entry = effects->m_buckets[effectCode % effects->m_numBuckets];
        UInt32 visited = 0;
        while (entry)
        {
            if (!OBMEFixPatch::Readable(entry, sizeof(*entry))) return nullptr;
            if (entry->key == effectCode) return entry->data;
            if (visited == effects->m_numItems) break;
            ++visited;
            entry = entry->next;
        }
        return nullptr;
    }

    UInt32 GetRelativeCallTarget(UInt32 callSite)
    {
        if (!OBMEFixPatch::Readable(reinterpret_cast<void*>(callSite), 5) || *reinterpret_cast<UInt8*>(callSite) != 0xE8)
            return 0;

        SInt32 rel = *reinterpret_cast<SInt32*>(callSite + 1);
        return callSite + 5 + rel;
    }

    UInt32 GetRelativeJumpTarget(UInt32 jumpSite)
    {
        if (!OBMEFixPatch::Readable(reinterpret_cast<void*>(jumpSite), 5) || *reinterpret_cast<UInt8*>(jumpSite) != 0xE9)
            return 0;

        SInt32 rel = *reinterpret_cast<SInt32*>(jumpSite + 1);
        return jumpSite + 5 + rel;
    }

    bool IsAddressInModule(HMODULE module, UInt32 address)
    {
        if (!module || !address) return false;
        const auto base = reinterpret_cast<const UInt8*>(module);
        if (!OBMEFixPatch::Readable(base, sizeof(IMAGE_DOS_HEADER))) return false;
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
            dos->e_lfanew > 0x100000) return false;
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (!OBMEFixPatch::Readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return false;
        const UInt32 start = reinterpret_cast<UInt32>(base);
        return address >= start && address - start < nt->OptionalHeader.SizeOfImage &&
            OBMEFixPatch::Executable(address);
    }

    bool Matches(UInt32 address, const void* bytes, size_t size)
    {
        return OBMEFixPatch::Readable(reinterpret_cast<void*>(address), size) &&
            memcmp(reinterpret_cast<void*>(address), bytes, size) == 0;
    }

    UInt32 s_protectionFaults[12] = {};
    bool PatchHealthy(UInt32 address)
    {
        for (UInt32 fault : s_protectionFaults) if (fault == address) return false;
        return true;
    }

    bool CheckedWrite(const char* name, UInt32 address, const void* expected,
        const void* replacement, size_t size, bool code, const OBMEFixPatch::API& api = OBMEFixPatch::API{})
    {
        const auto result = OBMEFixPatch::Write(reinterpret_cast<void*>(address),
            expected, replacement, size, code, api);
        if (OBMEFixPatch::NeedsAttention(result))
        {
            for (auto& fault : s_protectionFaults)
                if (!fault || fault == address) { fault = address; break; }
        }
        if (!OBMEFixPatch::Success(result))
            _ERROR("%s patch at %08X: %s; plugin remains resident%s", name, address,
                OBMEFixPatch::Describe(result), OBMEFixPatch::NeedsAttention(result) ? "; site remains unverified until restart" : "");
        return OBMEFixPatch::Success(result) && PatchHealthy(address);
    }

    bool CheckedTransfer(const char* name, UInt32 address, const UInt8* expected,
        UInt32 target, UInt8 opcode, size_t size)
    {
        UInt8 replacement[16] = {};
        if (size < 5 || size > sizeof(replacement) || !OBMEFixPatch::Executable(target)) return false;
        OBMEFixPatch::Transfer(replacement, address, target, opcode, size);
        return CheckedWrite(name, address, expected, replacement, size, true);
    }

    bool IsOBMELoadAdapter(HMODULE obme, UInt32 target)
    {
        const UInt8 prefix[] = { 0x55, 0x54, 0xFF, 0x74, 0x24, 0x0C, 0xE8 };
        const UInt8 suffix[] = { 0x83, 0xC4, 0x08, 0x5D, 0xC3 };
        return IsAddressInModule(obme, target) && Matches(target, prefix, sizeof(prefix)) &&
            Matches(target + 11, suffix, sizeof(suffix)) &&
            IsAddressInModule(obme, GetRelativeCallTarget(target + 6));
    }

    bool IsOBMEIconAdapter(HMODULE obme, UInt32 target)
    {
        const UInt8 prefix[] = { 0x83, 0xE9, 0x24, 0xE8 };
        const UInt8 suffix[] = { 0xC2, 0x08, 0x00 };
        return IsAddressInModule(obme, target) && Matches(target, prefix, sizeof(prefix)) &&
            Matches(target + 8, suffix, sizeof(suffix)) &&
            IsAddressInModule(obme, GetRelativeCallTarget(target + 3));
    }

    char* GetFileNamePart(char* path)
    {
        char* filePart = path;
        for (char* cursor = path; *cursor; ++cursor)
        {
            if (*cursor == '\\' || *cursor == '/')
                filePart = cursor + 1;
        }

        return filePart;
    }

    bool BuildPluginDirectoryPath(char* path, DWORD pathSize)
    {
        HMODULE self = GetModuleHandleA("OBMEFix.dll");
        if (!self || !path || !pathSize)
            return false;

        DWORD length = GetModuleFileNameA(self, path, pathSize);
        if (!length || length >= pathSize)
            return false;

        char* filePart = GetFileNamePart(path);
        *filePart = 0;
        return true;
    }

    bool BuildSiblingPluginPath(const char* fileName, char* path, DWORD pathSize)
    {
        if (!BuildPluginDirectoryPath(path, pathSize))
            return false;

        char* filePart = GetFileNamePart(path);
        const DWORD remaining = static_cast<DWORD>(pathSize - (filePart - path));
        lstrcpynA(filePart, fileName, remaining);
        if (lstrcmpiA(filePart, fileName) != 0)
            return false;

        return true;
    }

    bool DoesSiblingPluginFileExist(const char* fileName)
    {
        char path[MAX_PATH] = {};
        if (!BuildSiblingPluginPath(fileName, path, sizeof(path)))
            return false;

        const DWORD attributes = GetFileAttributesA(path);
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    bool DoesPluginRelativeFileExist(const char* pathFromPluginDir)
    {
        char path[MAX_PATH] = {};
        if (!BuildSiblingPluginPath(pathFromPluginDir, path, sizeof(path)))
            return false;

        const DWORD attributes = GetFileAttributesA(path);
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    void OpenOBMEFixLog()
    {
        char logPath[MAX_PATH] = {};
        if (BuildSiblingPluginPath("OBMEFix.log", logPath, sizeof(logPath)))
        {
            gLog.Open(logPath);
            _MESSAGE("OBMEFix log path: %s", logPath);
            return;
        }

        gLog.Open("OBMEFix.log");
        _WARNING("OBMEFix could not resolve its plugin directory; writing OBMEFix.log to the process working directory");
    }

    const char* ActiveStatus(bool active)
    {
        return active ? "active" : "not active";
    }

    const char* VerifiedStatus(bool verified)
    {
        return verified ? "verified" : "not verified";
    }

    const char* FileStatus(bool present)
    {
        return present ? "present" : "missing";
    }

    void LogRuntimeEnvironment(const OBSEInterface* obse)
    {
        _MESSAGE("Runtime versions: OBSE=%08X required>=%08X Oblivion=%08X required=%08X editorMode=%u",
            obse ? obse->obseVersion : 0,
            OBSE_VERSION_INTEGER,
            obse ? obse->oblivionVersion : 0,
            kOblivionVersion,
            obse ? obse->isEditor : 0);

        char exePath[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, exePath, sizeof(exePath)))
            _MESSAGE("Runtime executable: %s", exePath);

        char cwd[MAX_PATH] = {};
        DWORD cwdLength = GetCurrentDirectoryA(sizeof(cwd), cwd);
        if (cwdLength && cwdLength < sizeof(cwd))
            _MESSAGE("Process working directory: %s", cwd);

        if (obse && obse->obseVersion >= OBSE_VERSION_INTEGER && obse->GetOblivionDirectory)
            _MESSAGE("OBSE Oblivion directory: %s", obse->GetOblivionDirectory());

        char pluginDir[MAX_PATH] = {};
        if (BuildPluginDirectoryPath(pluginDir, sizeof(pluginDir)))
            _MESSAGE("OBMEFix plugin directory: %s", pluginDir);

        char path[MAX_PATH] = {};
        if (BuildSiblingPluginPath("OBMEFix.dll", path, sizeof(path)))
            _MESSAGE("OBMEFix.dll: %s", path);

        if (BuildSiblingPluginPath("OBME.dll", path, sizeof(path)))
            _MESSAGE("Original OBME.dll: %s (%s)", path, FileStatus(DoesSiblingPluginFileExist("OBME.dll")));

        if (BuildSiblingPluginPath("OBME_CS.dll", path, sizeof(path)))
            _MESSAGE("Construction Set OBME_CS.dll: %s (%s; gameplay does not need this DLL)", path, FileStatus(DoesSiblingPluginFileExist("OBME_CS.dll")));

        if (BuildSiblingPluginPath("CSI.dll", path, sizeof(path)))
            _MESSAGE("Custom Spell Icons CSI.dll: %s (%s; OBME v1.0 has a known hard hook conflict with CSI v3.1)", path, FileStatus(DoesSiblingPluginFileExist("CSI.dll")));
    }

    bool GetLoadedModulePath(HMODULE module, char* path, DWORD pathSize)
    {
        if (!module || !path || !pathSize)
            return false;

        DWORD length = GetModuleFileNameA(module, path, pathSize);
        return length && length < pathSize;
    }

    bool VerifyLoadedOriginalOBMEModule(HMODULE obme)
    {
        char expectedPath[MAX_PATH] = {};
        char loadedPath[MAX_PATH] = {};

        if (!BuildSiblingPluginPath("OBME.dll", expectedPath, sizeof(expectedPath)))
        {
            _ERROR("Could not resolve the expected sibling OBME.dll path for OBMEFix");
            return false;
        }

        if (!GetLoadedModulePath(obme, loadedPath, sizeof(loadedPath)))
        {
            _ERROR("Could not resolve the loaded OBME.dll module path");
            return false;
        }

        if (lstrcmpiA(expectedPath, loadedPath) != 0)
        {
            _ERROR("Loaded OBME.dll path does not match the OBME.dll beside OBMEFix.dll; this usually means the active OBSE/mod-manager deployment path is not the one being inspected. loaded='%s' expected='%s'",
                loadedPath,
                expectedPath);
            return false;
        }

        return true;
    }

    bool RequireOriginalOBMEPluginFile()
    {
        if (DoesSiblingPluginFileExist("OBME.dll"))
            return true;

        _ERROR("OBMEFix requires the original OBME.dll beside OBMEFix.dll. OBMEFix is only a runtime patch layer and does not replace OBME.dll.");
        return false;
    }

    const char* NormalizeAddSpellMessageIconPath(const char* path)
    {
        return OBMEFixCore::NormalizeIcon(path);
    }

    bool IsUsableAddSpellMessageIconPath(const char* path)
    {
        return path && *path;
    }

    const char* GetEffectSettingIconPath(EffectSetting* effectSetting)
    {
        if (!effectSetting || !effectSetting->texture.ddsPath.m_dataLen)
            return nullptr;

        return effectSetting->texture.ddsPath.m_data;
    }

    const char* GetVanillaStrongestEffectIconPath(EffectItemList* effectList, UInt32 range, bool requireArea)
    {
        if (!effectList)
            return nullptr;

        EffectItem* effectItem = reinterpret_cast<EffectItemListGetStrongestItemFn>(kVanillaEffectItemListGetStrongestItem)(effectList, range, requireArea);
        if (!effectItem)
            return nullptr;

        return GetEffectSettingIconPath(effectItem->setting);
    }

    const char* GetSEFFMessageIconPath()
    {
        EffectSetting* seff = LookupEffectSetting(kSEFFHandlerCode);
        return GetEffectSettingIconPath(seff);
    }

    void LogAddSpellMessageIconFallback(const char* source, const char* path)
    {
        ++s_substitutedAddSpellMessageIcons;
        if (s_loggedAddSpellMessageIconFallback)
            return;

        s_loggedAddSpellMessageIconFallback = true;
        _WARNING(
            "OBME AddSpell notification icon path was null or empty; using %s fallback '%s' to prevent stale previous-item icons in the top-left message",
            source ? source : "unknown",
            path ? path : "");
    }

    const char* __cdecl ResolveAddSpellMessageIconPath(EffectItemList* effectList, UInt32 range, UInt32 requireArea)
    {
        const bool mustHaveArea = requireArea != 0;
        const char* path = nullptr;

        if (s_obmeAddSpellMessageIconHook)
        {
            path = reinterpret_cast<OBMEAddSpellMessageIconFn>(s_obmeAddSpellMessageIconHook)(effectList, range, mustHaveArea);
            path = NormalizeAddSpellMessageIconPath(path);
            if (IsUsableAddSpellMessageIconPath(path))
                return path;
        }

        path = NormalizeAddSpellMessageIconPath(GetVanillaStrongestEffectIconPath(effectList, range, mustHaveArea));
        if (IsUsableAddSpellMessageIconPath(path))
        {
            LogAddSpellMessageIconFallback("vanilla strongest-effect icon", path);
            return path;
        }

        path = NormalizeAddSpellMessageIconPath(GetSEFFMessageIconPath());
        if (IsUsableAddSpellMessageIconPath(path))
        {
            LogAddSpellMessageIconFallback("SEFF effect icon", path);
            return path;
        }

        LogAddSpellMessageIconFallback("default magic HUD icon", kDefaultAddSpellMessageIconPath);
        return kDefaultAddSpellMessageIconPath;
    }

    void __declspec(naked) AddSpellMessageIconHook()
    {
        __asm
        {
            push    [esp + 8]
            push    [esp + 8]
            push    ecx
            call    ResolveAddSpellMessageIconPath
            add     esp, 0x0C
            retn    8
        }
    }

    bool IsRuntimeActor(TESObjectREFR* parent)
    {
        return parent && parent->IsActor();
    }

    TESObjectREFR* GetNonActorTargetParent(MagicTarget* target)
    {
        if (!target)
            return nullptr;

        TESObjectREFR* parent = target->GetParent();
        if (!parent || IsRuntimeActor(parent))
            return nullptr;

        return parent;
    }

    EffectItemList* GetEffectListForForm(TESForm* form)
    {
        if (!form)
            return nullptr;

        switch (form->typeID)
        {
        case kFormType_Enchantment:
        case kFormType_Spell:
            return &reinterpret_cast<MagicItemForm*>(form)->magicItem.list;

        case kFormType_Ingredient:
        case kFormType_AlchemyItem:
            return &reinterpret_cast<MagicItemObject*>(form)->magicItem.list;

        case kFormType_SigilStone:
            return &reinterpret_cast<TESSigilStone*>(form)->effectList;

        default:
            return nullptr;
        }
    }

    EffectItem* GetEffectItemBySavedIndex(EffectItemList* list, SInt8 effectIndex)
    {
        return list ? OBMEFixCore::ItemAt(&list->effectList, effectIndex) : nullptr;
    }

    EffectItem* GetSavedEffectItem(UInt32 formID, SInt8 effectIndex)
    {
        return GetEffectItemBySavedIndex(GetEffectListForForm(LookupForm(formID)), effectIndex);
    }

    bool GetRecordExtent(TESSaveLoadGame* saveLoad, TESObjectREFR* parent,
        UInt32 blockStart, UInt16 blockLength, UInt32& end)
    {
        const SaveLoadView view(saveLoad);
        if (!view.IsReadable() || !parent || !view.Changes() ||
            !OBMEFixPatch::Readable(view.FormHeader(), 12)) return false;
        UInt32 headerForm = 0;
        UInt16 headerLength = 0;
        memcpy(&headerForm, view.FormHeader(), 4);
        memcpy(&headerLength, view.FormHeader() + 10, 2);
        if (headerForm != parent->refID || view.FormHeader()[9] != view.Version()) return false;
        const auto change = reinterpret_cast<FindChangeFn>(kChangesMapFindAddr)(view.Changes(), headerForm);
        if (!OBMEFixPatch::Readable(change, sizeof(ChangeRecord)) ||
            !OBMEFixPatch::Readable(change->data, 4)) return false;
        UInt16 length = 0;
        memcpy(&length, change->data, 2);
        if (length != headerLength || change->data[2] != view.FormHeader()[4] ||
            change->data[3] != view.Version()) return false;
        // Native deferred allocation at 0x4667AF uses saved length + 4.
        const UInt32 start = reinterpret_cast<UInt32>(change->data);
        const UInt32 cursor = reinterpret_cast<UInt32>(view.Cursor());
        if (start > UINT32_MAX - 4 || length > UINT32_MAX - start - 4) return false;
        end = start + 4 + length;
        if (cursor < start + 4 || cursor > end ||
            !OBMEFixPatch::Readable(change->data, static_cast<size_t>(length) + 4)) return false;
        if (OBMEFixCore::UsesBlocks(view.Version()))
        {
            if (blockStart < start + 4 || blockStart > end || blockLength > end - blockStart) return false;
            const UInt32 blockEnd = blockStart + blockLength;
            if (cursor < blockStart + 2 || cursor > blockEnd) return false;
            end = blockEnd;
        }
        return true;
    }

    bool ResolveSavedFormID(TESSaveLoadGame* saveLoad, UInt32 encoded, UInt32& resolved)
    {
        resolved = encoded;
        const SaveLoadView view(saveLoad);
        if (!view.IsReadable()) return false;
        if (!view.UsesIrefs() || !encoded || encoded >= 0xFF000000) return true;
        const auto table = view.Irefs();
        if (!OBMEFixPatch::Readable(table, sizeof(*table)) || !table->data ||
            encoded > table->firstFreeEntry || encoded >= table->capacity ||
            encoded > (UINT32_MAX / sizeof(UInt32)) - 1) return false;
        const UInt32 address = reinterpret_cast<UInt32>(table->data);
        const UInt32 offset = encoded * sizeof(UInt32);
        if (offset > UINT32_MAX - address ||
            !OBMEFixPatch::Readable(reinterpret_cast<void*>(address + offset), sizeof(UInt32))) return false;
        resolved = table->data[encoded];
        return resolved != 0; // native LoadFormID reports true for this missing-map case
    }

    bool LoadCallerContextMatches()
    {
        const UInt8 caller[] = {0x8B,0x7C,0x24,0x2C,0x57};
        const UInt8 countReload[] = {0x0F,0xB7,0x4C,0x24,0x10,0x83,0xC5,0x01,0x3B,0xE9};
        return Matches(kLoadCallerContext, caller, sizeof(caller)) &&
            Matches(kLoadCountReload, countReload, sizeof(countReload));
    }

    bool __cdecl TryDropCurrentEffect(TESObjectREFR* parent, UInt32* entryStack, SInt32* index)
    {
        if (!parent) return false;
        // Another plugin can change the caller after the last message. Do not use
        // our extra native-frame fields under an unverified stack contract.
        if (!LoadCallerContextMatches())
        {
            if (OBMEFixCore::ShouldLogEvent(s_loadBypassEvents))
                _WARNING("SEFF salvage bypass: caller stack contract changed; delegating without reading frame metadata");
            return false;
        }
        if (IsRuntimeActor(parent)) return false;
        auto saveLoad = GetSaveLoad();
        const SaveLoadView view(saveLoad);
        if (!view.IsReadable()) return false;
        const auto context = LoadCallContext::FromEntry(entryStack);
        const UInt16 total = context.totalRecords;
        const UInt16 blockLength = context.blockLength;
        const UInt32 blockStart = context.blockStart;
        UInt32 end = 0;
        if (*index < 0 || static_cast<UInt32>(*index) >= total ||
            !GetRecordExtent(saveLoad, parent, blockStart, blockLength, end))
        {
            if (OBMEFixCore::ShouldLogEvent(s_loadBypassEvents))
                _WARNING("SEFF salvage bypass: no verified record extent/count; delegating to OBME (events=%u)", s_loadBypassEvents);
            return false;
        }
        const UInt32 start = reinterpret_cast<UInt32>(view.Cursor());
        OBMEFixCore::Record record;
        const auto result = OBMEFixCore::ParseRecord(view.Cursor(), end - start,
            view.Version(), total - *index, record);
        if (result != OBMEFixCore::ParseStatus::Valid)
        {
            if (OBMEFixCore::ShouldLogEvent(s_loadBypassEvents))
                _WARNING("SEFF salvage bypass: unsupported/invalid record (%u), upstream loader still applies (events=%u)",
                    static_cast<unsigned>(result), s_loadBypassEvents);
            return false;
        }
        if (!OBMEFixCore::LongDuration(record.duration)) return false;
        UInt32 formID = 0;
        bool scriptEffect = record.hasConversion && record.conversion.handlerCode == kSEFFHandlerCode;
        if (!record.hasConversion && ResolveSavedFormID(saveLoad, record.encodedFormID, formID))
        {
            const auto effectItem = GetSavedEffectItem(formID, record.effectIndex);
            scriptEffect = effectItem && effectItem->effectCode == kSEFFHandlerCode;
        }
        if (!scriptEffect) return false;
        // Commit exactly one validated parse. OBME's own read/remap helpers were not called.
        view.SetCursor(view.Cursor() + record.consumedBytes);
        *index += record.consumedRecords - 1;
        if (OBMEFixCore::ShouldLogEvent(s_droppedNonActorScriptEffects))
            _MESSAGE("Dropped non-actor SEFF: parent=%08X duration=%.2f records=%u bytes=%u total=%u",
                parent->refID, record.duration, record.consumedRecords,
                static_cast<unsigned>(record.consumedBytes), s_droppedNonActorScriptEffects);
        return true;
    }

    void __declspec(naked) LoadActiveEffectHook()
    {
        __asm
        {
            mov     eax, esp
            push    ebp
            mov     ecx, esp
            push    ecx
            push    eax
            push    [eax + 4]
            call    TryDropCurrentEffect
            add     esp, 12
            pop     ebp
            test    al, al
            jnz     dropped
            jmp     [s_obmeLoadActiveEffectHook]
        dropped:
            xor     eax, eax
            retn
        }
    }

    bool InstallNonActorScriptEffectLoadGuard(HMODULE obme)
    {
        const UInt32 current = GetRelativeCallTarget(kLoadActiveEffectCallSite);
        const UInt32 hook = reinterpret_cast<UInt32>(&LoadActiveEffectHook);
        if (!PatchHealthy(kLoadActiveEffectCallSite)) return false;
        if (!LoadCallerContextMatches())
        {
            _WARNING("SEFF load caller/loop context changed; guard cannot be verified");
            return false;
        }
        if (current == hook) return IsOBMELoadAdapter(obme, s_obmeLoadActiveEffectHook);
        if ((s_obmeLoadActiveEffectHook && s_obmeLoadActiveEffectHook != current) || !IsOBMELoadAdapter(obme, current))
        {
            _WARNING("Load guard conflict/unrecognized OBME adapter at %08X; target=%08X", kLoadActiveEffectCallSite, current);
            return false;
        }
        UInt8 expected[5];
        OBMEFixPatch::Transfer(expected, kLoadActiveEffectCallSite, current, 0xE8, 5);
        s_obmeLoadActiveEffectHook = current; // published before the hook, stable thereafter
        return CheckedTransfer("SEFF load", kLoadActiveEffectCallSite, expected, hook, 0xE8, 5);
    }

    bool __fastcall ScriptEffect_IsTargetValid(ActiveEffect* activeEffect, void*, MagicTarget* target)
    {
        bool previousResult = true;
        if (s_scriptEffectIsTargetValid)
        {
            previousResult = reinterpret_cast<bool (__thiscall *)(ActiveEffect*, MagicTarget*)>(s_scriptEffectIsTargetValid)(activeEffect, target);
            if (!previousResult)
                return false;
        }

        if (!activeEffect) return previousResult;
        if (!std::isfinite(activeEffect->duration))
        {
            if (OBMEFixCore::ShouldLogEvent(s_invalidDurationEvents))
                _WARNING("Non-finite script-effect duration; preserving previous validator (events=%u)", s_invalidDurationEvents);
            return previousResult;
        }
        if (!OBMEFixCore::LongDuration(activeEffect->duration)) return previousResult;

        TESObjectREFR* parent = GetNonActorTargetParent(target);
        if (!parent)
            return previousResult;

        const UInt32 refID = parent->refID;
        const UInt8 baseType = parent->baseForm ? parent->baseForm->typeID : 0;

        if (OBMEFixCore::ShouldLogEvent(s_blockedNonActorScriptEffects)) _MESSAGE(
            "Blocked duration SEFF active effect on non-actor target: parent=%08X baseType=%02X duration=%.2f totalBlocked=%u",
            refID,
            baseType,
            activeEffect->duration,
            s_blockedNonActorScriptEffects);

        return false;
    }

    bool InstallNonActorScriptEffectCreationGuard()
    {
        const UInt32 hook = reinterpret_cast<UInt32>(&ScriptEffect_IsTargetValid);
        if (!OBMEFixPatch::Readable(reinterpret_cast<void*>(kScriptEffectIsTargetValidEntry), 4)) return false;
        const UInt32 current = *reinterpret_cast<UInt32*>(kScriptEffectIsTargetValidEntry);
        const UInt8 validator[] = {0xB0,0x01,0xC2,0x04,0x00};
        if (!Matches(kVanillaAlwaysTrueIsTargetValid, validator, sizeof(validator))) return false;
        if (!PatchHealthy(kScriptEffectIsTargetValidEntry)) return false;
        if (current == hook) return s_scriptEffectIsTargetValid == kVanillaAlwaysTrueIsTargetValid;
        if (current != kVanillaAlwaysTrueIsTargetValid ||
            (s_scriptEffectIsTargetValid && s_scriptEffectIsTargetValid != current))
        {
            _WARNING("Creation validator conflict at %08X; target=%08X. Chain retained, slot untouched.",
                kScriptEffectIsTargetValidEntry, current);
            return false;
        }
        s_scriptEffectIsTargetValid = current;
        return CheckedWrite("SEFF creation", kScriptEffectIsTargetValidEntry, &current, &hook, 4, false);
    }

    void ApplyDamageWithClamp(Actor* actor, UInt32 actorValue, float amount, Actor* source,
        DamageAVFHookSpec& spec, UInt32 caller)
    {
        if (actor && amount < 0.0f && actorValue != kActorValueFatigue &&
            IsAddressInModule(GetModuleHandleA("OBME.dll"), caller))
        {
            const float currentValue = actor->GetAV_F(actorValue);
            const float requested = amount;
            if (OBMEFixCore::ClampDamageAmount(actorValue, currentValue, amount))
            {
                if (OBMEFixCore::ShouldLogEvent(s_clampedOBMEModAVDamage))
                    _MESSAGE("Clamped OBME AV damage: av=%u requested=%.2f current=%.2f total=%u",
                        actorValue, requested, currentValue, s_clampedOBMEModAVDamage);
            }
        }
        const UInt32 original = spec.originalTarget ? spec.originalTarget : spec.expectedTarget;
        reinterpret_cast<DamageAVFFn>(original)(actor, actorValue, amount, source);
    }

    template<unsigned Index>
    void __declspec(noinline) __fastcall DamageAVFEntry(Actor* actor, void*, UInt32 av, float amount, Actor* source)
    {
        const UInt32 caller = reinterpret_cast<UInt32>(_ReturnAddress());
        ApplyDamageWithClamp(actor, av, amount, source, s_damageAVFHookSpecs[Index], caller);
    }

    bool InstallOBMEModAVDamageClamp()
    {
        const UInt32 hooks[] = {
            reinterpret_cast<UInt32>(&DamageAVFEntry<0>), reinterpret_cast<UInt32>(&DamageAVFEntry<1>),
            reinterpret_cast<UInt32>(&DamageAVFEntry<2>), reinterpret_cast<UInt32>(&DamageAVFEntry<3>) };
        UInt32 installed = 0;
        s_expectedDamageAVFClampSlots = 4;
        for (unsigned i = 0; i != 4; ++i)
        {
            auto& spec = s_damageAVFHookSpecs[i];
            if (!PatchHealthy(spec.entry) || !OBMEFixPatch::Readable(reinterpret_cast<void*>(spec.entry), 4) ||
                !OBMEFixPatch::Executable(spec.expectedTarget)) continue;
            const UInt32 current = *reinterpret_cast<UInt32*>(spec.entry);
            if (current == hooks[i])
            {
                if (spec.originalTarget == spec.expectedTarget) ++installed;
                continue;
            }
            if (current != spec.expectedTarget || (spec.originalTarget && spec.originalTarget != current))
            {
                _WARNING("%s DamageAV conflict: %08X; keeping existing owner", spec.name, current);
                continue;
            }
            spec.originalTarget = current;
            if (CheckedWrite(spec.name, spec.entry, &current, &hooks[i], 4, false)) ++installed;
        }
        s_installedDamageAVFClampSlots = installed;
        return installed == 4;
    }

    void LogDeploymentDiagnostics()
    {
        if (GetModuleHandleA("CSI.dll") && !s_loggedCsiDiagnostic)
        {
            s_loggedCsiDiagnostic = true;
            _WARNING("CSI.dll is loaded with OBME.dll. OBME v1.0 and Custom Spell Icons v3.1 patch the same path and are a known hard conflict.");
        }

        if (DoesSiblingPluginFileExist("CSI.dll") && !s_loggedCsiFileDiagnostic)
        {
            s_loggedCsiFileDiagnostic = true;
            _WARNING("CSI.dll is present beside OBMEFix.dll. Keep one owner for the OBME/CSI MagicItem::GetVFXEffect hook; OBMEFix leaves unknown owners untouched.");
        }

        if (GetModuleHandleA("OBME_CS.dll") && !s_loggedObmeCsDiagnostic)
        {
            s_loggedObmeCsDiagnostic = true;
            _WARNING("OBME_CS.dll is loaded in the game runtime. OBME_CS.dll is for Construction Set authoring, not gameplay.");
        }

        if (DoesSiblingPluginFileExist("OBME_CS.dll") && !s_loggedObmeCsFileDiagnostic)
        {
            s_loggedObmeCsFileDiagnostic = true;
            _WARNING("OBME_CS.dll is installed beside OBMEFix.dll. OBME_CS.dll is for Construction Set authoring and should not be deployed for gameplay.");
        }

        if (!s_loggedExampleEspFileDiagnostic)
        {
            UInt32 exampleCount = 0;
            const char* firstExample = nullptr;

            for (UInt32 i = 0; i < (sizeof(kOBMEExampleESPFiles) / sizeof(kOBMEExampleESPFiles[0])); ++i)
            {
                if (!DoesPluginRelativeFileExist(kOBMEExampleESPFiles[i].pathFromPluginDir))
                    continue;

                if (!firstExample)
                    firstExample = kOBMEExampleESPFiles[i].fileName;

                ++exampleCount;
            }

            if (exampleCount)
            {
                s_loggedExampleEspFileDiagnostic = true;
                _WARNING(
                    "Detected %u OBME example ESP(s) in Data (first: %s). These bundled demo/test plugins are not required for gameplay and should stay disabled or out of player deployments unless intentionally testing OBME features.",
                    exampleCount,
                    firstExample);
            }
        }
    }

    void LogOriginalOBMEDiagnostic(HMODULE obme)
    {
        if (obme || s_loggedObmeFileDiagnostic)
            return;

        s_loggedObmeFileDiagnostic = true;
        if (DoesSiblingPluginFileExist("OBME.dll"))
        {
            _WARNING("OBME.dll is present beside OBMEFix.dll but is not loaded. Check obse_loader.log and the mod-manager virtualized plugin path for an OBME Query/Load failure.");
        }
        else
        {
            _ERROR("OBME.dll is not present beside OBMEFix.dll. Install the original OBME.dll in the same OBSE Plugins directory; OBMEFix only patches OBME at runtime.");
        }
    }

    EffectSetting* __fastcall ResolveMagicItemVFXWithFallback(MagicItem* magicItem, void*, UInt32 minRange)
    {
        auto effect = s_magicModelHitEffectGetVFXCallTarget ?
            reinterpret_cast<MagicItemGetVFXEffectFn>(s_magicModelHitEffectGetVFXCallTarget)(magicItem, minRange) : nullptr;
        if (effect) return effect;
        auto fallback = LookupEffectSetting(kSEFFHandlerCode);
        if (OBMEFixCore::ShouldLogEvent(s_substitutedNullMagicItemVFXEffects))
            _WARNING("Null VFX result: %s (total=%u)", fallback ? "using SEFF" : "skipping model path",
                s_substitutedNullMagicItemVFXEffects);
        return fallback;
    }

    UInt32 s_vfxNonNullContinuation = kVFXNonNullContinuation;
    UInt32 s_vfxNullContinuation = kVFXNullContinuation;
    void __declspec(naked) MagicModelHitEffectVFXHook()
    {
        __asm
        {
            call    ResolveMagicItemVFXWithFallback
            test    eax, eax
            jz      noPath
            mov     edx, [eax + 0x18]
            jmp     [s_vfxNonNullContinuation]
        noPath:
            jmp     [s_vfxNullContinuation]
        }
    }

    bool InstallMagicModelHitEffectNullVFXGuard()
    {
        if (!PatchHealthy(kMagicModelHitEffectGetVFXCallSite)) return false;
        UInt8 original[8], installed[8];
        OBMEFixPatch::Transfer(original, kMagicModelHitEffectGetVFXCallSite, kMagicItemGetVFXEffectHookSite, 0xE8, 8);
        original[5]=0x8B; original[6]=0x50; original[7]=0x18;
        OBMEFixPatch::Transfer(installed, kMagicModelHitEffectGetVFXCallSite,
            reinterpret_cast<UInt32>(&MagicModelHitEffectVFXHook), 0xE9, 8);
        const UInt8 continuation[] = {0x8D,0x48,0x18,0x8B,0x42,0x14,0xFF,0xD0,0x89,0x46,0x2C,0x88,0x5E,0x28};
        if (!Matches(kVFXNonNullContinuation, continuation, sizeof(continuation))) return false;
        if (Matches(kMagicModelHitEffectGetVFXCallSite, installed, sizeof(installed)))
            return s_magicModelHitEffectGetVFXCallTarget == kMagicItemGetVFXEffectHookSite;
        if (!Matches(kMagicModelHitEffectGetVFXCallSite, original, sizeof(original)))
        {
            _WARNING("MagicModelHitEffect caller is owned by another patch; leaving it untouched");
            return false;
        }
        s_magicModelHitEffectGetVFXCallTarget = kMagicItemGetVFXEffectHookSite;
        return CheckedWrite("VFX null branch", kMagicModelHitEffectGetVFXCallSite, original, installed, 8, true);
    }

    bool InstallAddSpellMessageIconGuard(HMODULE obme)
    {
        if (!PatchHealthy(kAddSpellMessageIconCallSite)) return false;
        UInt8 installed[11];
        OBMEFixPatch::Transfer(installed, kAddSpellMessageIconCallSite,
            reinterpret_cast<UInt32>(&AddSpellMessageIconHook), 0xE8, sizeof(installed));
        if (Matches(kAddSpellMessageIconCallSite, installed, sizeof(installed)))
            return IsOBMEIconAdapter(obme, s_obmeAddSpellMessageIconHook);
        // A jump directly to our own call-compatible adapter has a known repair.
        // Require the entire padded span and the original ABI cache; foreign jumps stay untouched.
        UInt8 ownJump[11];
        OBMEFixPatch::Transfer(ownJump, kAddSpellMessageIconCallSite,
            reinterpret_cast<UInt32>(&AddSpellMessageIconHook), 0xE9, sizeof(ownJump));
        if (Matches(kAddSpellMessageIconCallSite, ownJump, sizeof(ownJump)) &&
            IsOBMEIconAdapter(obme, s_obmeAddSpellMessageIconHook))
        {
            _WARNING("Correcting verified jump-to-OBMEFix icon adapter back to its required CALL");
            return CheckedWrite("AddSpell own-jump repair", kAddSpellMessageIconCallSite,
                ownJump, installed, sizeof(installed), true);
        }
        const UInt32 target = GetRelativeCallTarget(kAddSpellMessageIconCallSite);
        if ((s_obmeAddSpellMessageIconHook && s_obmeAddSpellMessageIconHook != target) || !IsOBMEIconAdapter(obme, target))
        {
            _WARNING("AddSpell icon patch conflict/unrecognized adapter; no reassertion at %08X", kAddSpellMessageIconCallSite);
            return false;
        }
        UInt8 expected[11];
        OBMEFixPatch::Transfer(expected, kAddSpellMessageIconCallSite, target, 0xE8, sizeof(expected));
        if (!Matches(kAddSpellMessageIconCallSite, expected, sizeof(expected))) return false;
        s_obmeAddSpellMessageIconHook = target;
        return CheckedWrite("AddSpell icon", kAddSpellMessageIconCallSite, expected, installed, sizeof(installed), true);
    }

    bool VerifyMagicItemVFXHook(HMODULE obme)
    {
        const UInt32 target = GetRelativeJumpTarget(kMagicItemGetVFXEffectHookSite);
        const bool recognized = IsAddressInModule(obme, target) &&
            (!s_obmeMagicItemVFXHook || s_obmeMagicItemVFXHook == target);
        if (recognized) s_obmeMagicItemVFXHook = target;
        if (!recognized && (s_lastMagicItemVFXHookDiagnosticTarget != target ||
            s_lastMagicItemVFXHookDiagnosticState == kMagicItemVFXHookDiagnostic_None))
            _WARNING("OBME VFX entry owner cannot be verified: target=%08X; CSI presence does not authorize overwrite", target);
        s_lastMagicItemVFXHookDiagnosticTarget = target;
        s_lastMagicItemVFXHookDiagnosticState = recognized ? kMagicItemVFXHookDiagnostic_None : kMagicItemVFXHookDiagnostic_UnknownOwner;
        return recognized;
    }

    bool InstallRuntimeGuards()
    {
        s_runtimeGuardsInstalled = false;
        s_installedDamageAVFClampSlots = 0;
        s_expectedDamageAVFClampSlots = 4;
        HMODULE obme = GetModuleHandleA("OBME.dll");
        LogDeploymentDiagnostics();
        LogOriginalOBMEDiagnostic(obme);
        static UInt32 lastMask = UINT32_MAX;
        UInt32 mask = 0;
        if (obme && VerifyLoadedOriginalOBMEModule(obme))
        {
            if (VerifyMagicItemVFXHook(obme)) mask |= 1;
            if (InstallMagicModelHitEffectNullVFXGuard()) mask |= 2;
            if (InstallAddSpellMessageIconGuard(obme)) mask |= 4;
            if (InstallNonActorScriptEffectLoadGuard(obme)) mask |= 8;
            if (InstallNonActorScriptEffectCreationGuard()) mask |= 16;
            if (InstallOBMEModAVDamageClamp()) mask |= 32;
        }
        s_runtimeGuardsInstalled = mask == 63;
        const UInt32 snapshot = mask | (s_installedDamageAVFClampSlots << 8);
        if (lastMask != snapshot)
        {
            _MESSAGE("OBMEFix hook status: VFX owner=%s null branch=%s icon=%s bounded SEFF load=%s creation=%s damage=%u/4; %s",
                VerifiedStatus((mask&1)!=0), ActiveStatus((mask&2)!=0), ActiveStatus((mask&4)!=0),
                ActiveStatus((mask&8)!=0), ActiveStatus((mask&16)!=0), s_installedDamageAVFClampSlots,
                s_runtimeGuardsInstalled ? "all sites verified; runtime input limits still apply" : "partial/deferred protection");
            lastMask = snapshot;
        }
        return s_runtimeGuardsInstalled;
    }

    void MessageHandler(OBSEMessagingInterface::Message* message)
    {
        if (!message)
            return;

        if (message->type == OBSEMessagingInterface::kMessage_PostLoad ||
            message->type == OBSEMessagingInterface::kMessage_PostPostLoad ||
            message->type == OBSEMessagingInterface::kMessage_PreLoadGame)
        {
            _MESSAGE("OBMEFix received OBSE message %u; verifying runtime guard installation", message->type);
            InstallRuntimeGuards();
        }
    }

    bool Query(const OBSEInterface* obse, PluginInfo* info)
    {
        info->infoVersion = PluginInfo::kInfoVersion;
        info->name = "OBMEFix";
        info->version = 2;

        OpenOBMEFixLog();
#ifdef _DEBUG
        gLog.SetLogLevel(IDebugLog::kLevel_DebugMessage);
#else
        gLog.SetLogLevel(IDebugLog::kLevel_Message);
#endif

        _MESSAGE("OBMEFix Query started. OBMEFix is a runtime patch layer; the original OBME.dll must remain installed beside OBMEFix.dll.");
        LogRuntimeEnvironment(obse);

        if (obse->isEditor)
        {
            _MESSAGE("OBMEFix Query stopped: editor mode detected. OBMEFix is for the Oblivion game runtime, not the Construction Set.");
            return false;
        }

        if (obse->obseVersion < OBSE_VERSION_INTEGER)
        {
            _ERROR("OBSE version %08X, expected >= %08X", obse->obseVersion, OBSE_VERSION_INTEGER);
            return false;
        }

        if (obse->oblivionVersion != kOblivionVersion)
        {
            _ERROR("Oblivion version %08X, expected %08X", obse->oblivionVersion, kOblivionVersion);
            return false;
        }

        if (!RequireOriginalOBMEPluginFile())
            return false;

        _MESSAGE("OBMEFix Query passed: OBSE version, Oblivion version, and sibling OBME.dll file check are valid.");
        return true;
    }

    bool Load(const OBSEInterface* obse)
    {
        s_pluginHandle = obse->GetPluginHandle();
        _MESSAGE("OBMEFix Load started. Registering OBSE messages for deferred installation.");

        OBSEMessagingInterface* messaging = reinterpret_cast<OBSEMessagingInterface*>(obse->QueryInterface(kInterface_Messaging));
        if (!messaging || messaging->version < OBSEMessagingInterface::kVersion || !messaging->RegisterListener)
        {
            _ERROR("OBSE messaging interface is not available; cannot safely defer OBMEFix installation");
            return false;
        }

        if (!messaging->RegisterListener(s_pluginHandle, "OBSE", MessageHandler))
        {
            _ERROR("Failed to register OBMEFix OBSE message listener");
            return false;
        }

        // Registering the listener publishes a pointer into this DLL. There is no
        // messaging unregister API, so no failure return is permitted after success.
        _MESSAGE("OBMEFix resident; installation deferred to PostLoad/PostPostLoad and rechecked at PreLoadGame.");
        return true;
    }
}
