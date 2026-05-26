#include "obse/PluginAPI.h"
#include "obse/GameMagicEffects.h"
#include "obse/GameObjects.h"
#include "obse/obse_common/SafeWrite.h"

#include <Windows.h>

IDebugLog gLog("OBMEFix.temp");

namespace
{
    constexpr UInt32 kOblivionVersion = OBLIVION_VERSION;
    constexpr UInt32 kLoadActiveEffectCallSite = 0x0068F035;
    constexpr UInt32 kVanillaLoadActiveEffectTarget = 0x0068EDA0;
    constexpr UInt32 kCurrentSaveLoadAddr = 0x00B33B00;
    constexpr UInt32 kLoadDataAddr = 0x004534D0;
    constexpr UInt32 kLoadFormIDAddr = 0x0045BA00;
    constexpr UInt32 kSEFFHandlerCode = 'FFES';
    constexpr UInt32 kMagicItemGetVFXEffectHookSite = 0x00419B30;
    constexpr UInt32 kScriptEffectVtbl = 0x00A76AAC;
    constexpr UInt32 kScriptEffectIsTargetValidEntry = kScriptEffectVtbl + 0x34;
    constexpr UInt32 kVanillaAlwaysTrueIsTargetValid = 0x006FDFF0;
    constexpr float kStuckScriptEffectDurationThreshold = 1.0f;

    enum MagicItemVFXHookDiagnosticState
    {
        kMagicItemVFXHookDiagnostic_None = 0,
        kMagicItemVFXHookDiagnostic_NotJump,
        kMagicItemVFXHookDiagnostic_CsiConflict,
        kMagicItemVFXHookDiagnostic_UnknownOwner,
    };

    struct TESSaveLoadGameMinimal
    {
        UInt8 pad000[0x14];
        UInt32 bufferOffset;
    };

    struct ActiveEffectConversionData
    {
        UInt32 version;
        UInt32 handlerCode;
        UInt32 mgefCode;
        SInt32 mgefRelativeIndex;
        UInt32 reserved[4];
    };

    struct ActiveEffectRecordMetadata
    {
        ActiveEffectConversionData conversion;
        UInt16 recordSize;
        UInt32 formID;
        SInt8 effectIndex;
        float duration;
        bool hasConversion;
    };

    typedef void (__thiscall * LoadDataFn)(TESSaveLoadGameMinimal*, void*, UInt32);
    typedef bool (__thiscall * LoadFormIDFn)(TESSaveLoadGameMinimal*, void*, UInt32);

    PluginHandle s_pluginHandle = kPluginHandle_Invalid;
    UInt32 s_obmeLoadActiveEffectHook = 0;
    UInt32 s_droppedNonActorScriptEffects = 0;
    UInt32 s_blockedNonActorScriptEffects = 0;
    bool s_runtimeGuardsInstalled = false;
    bool s_loggedCsiDiagnostic = false;
    bool s_loggedObmeCsDiagnostic = false;
    UInt32 s_lastMagicItemVFXHookDiagnosticState = kMagicItemVFXHookDiagnostic_None;
    UInt32 s_lastMagicItemVFXHookDiagnosticTarget = 0;

    TESSaveLoadGameMinimal* GetSaveLoad()
    {
        TESSaveLoadGameMinimal** current = reinterpret_cast<TESSaveLoadGameMinimal**>(kCurrentSaveLoadAddr);
        return current ? *current : nullptr;
    }

    void LoadData(void* buffer, UInt32 size)
    {
        TESSaveLoadGameMinimal* saveLoad = GetSaveLoad();
        if (!saveLoad)
            return;

        reinterpret_cast<LoadDataFn>(kLoadDataAddr)(saveLoad, buffer, size);
    }

    bool LoadFormID(void* buffer, UInt32 size)
    {
        TESSaveLoadGameMinimal* saveLoad = GetSaveLoad();
        if (!saveLoad)
            return false;

        return reinterpret_cast<LoadFormIDFn>(kLoadFormIDAddr)(saveLoad, buffer, size);
    }

    UInt32 GetRelativeCallTarget(UInt32 callSite)
    {
        if (*reinterpret_cast<UInt8*>(callSite) != 0xE8)
            return 0;

        SInt32 rel = *reinterpret_cast<SInt32*>(callSite + 1);
        return callSite + 5 + rel;
    }

    UInt32 GetRelativeJumpTarget(UInt32 jumpSite)
    {
        if (*reinterpret_cast<UInt8*>(jumpSite) != 0xE9)
            return 0;

        SInt32 rel = *reinterpret_cast<SInt32*>(jumpSite + 1);
        return jumpSite + 5 + rel;
    }

    bool IsAddressInModule(HMODULE module, UInt32 address)
    {
        if (!module || !address)
            return false;

        const UInt8* base = reinterpret_cast<const UInt8*>(module);
        const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;

        const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return false;

        const UInt32 moduleBase = reinterpret_cast<UInt32>(base);
        const UInt32 moduleEnd = moduleBase + nt->OptionalHeader.SizeOfImage;
        return address >= moduleBase && address < moduleEnd;
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

    bool ReadSavedActiveEffectDuration(UInt16 recordSize, float& outDuration)
    {
        if (recordSize < 0x0F)
            return false;

        float timeElapsed = 0.0f;
        UInt8 applied = 0;
        UInt8 terminated = 0;
        UInt8 pad13 = 0;
        float magnitude = 0.0f;

        LoadData(&timeElapsed, sizeof(timeElapsed));
        LoadData(&applied, sizeof(applied));
        LoadData(&terminated, sizeof(terminated));
        LoadData(&pad13, sizeof(pad13));
        LoadData(&magnitude, sizeof(magnitude));
        LoadData(&outDuration, sizeof(outDuration));

        return true;
    }

    bool PeekCurrentActiveEffectRecord(ActiveEffectRecordMetadata& outRecord)
    {
        TESSaveLoadGameMinimal* saveLoad = GetSaveLoad();
        if (!saveLoad)
            return false;

        const UInt32 startOffset = saveLoad->bufferOffset;

        UInt16 recordSize = 0;
        UInt32 formID = 0;
        SInt8 effectIndex = -1;

        LoadData(&recordSize, sizeof(recordSize));
        LoadFormID(&formID, sizeof(formID));
        LoadData(&effectIndex, sizeof(effectIndex));

        if (recordSize == sizeof(ActiveEffectConversionData) && formID == 0 && effectIndex == -1)
        {
            outRecord.hasConversion = true;
            LoadData(&outRecord.conversion, sizeof(outRecord.conversion));

            LoadData(&outRecord.recordSize, sizeof(outRecord.recordSize));
            LoadFormID(&outRecord.formID, sizeof(outRecord.formID));
            LoadData(&outRecord.effectIndex, sizeof(outRecord.effectIndex));
            ReadSavedActiveEffectDuration(outRecord.recordSize, outRecord.duration);
        }

        saveLoad->bufferOffset = startOffset;
        return outRecord.hasConversion;
    }

    bool ShouldDropCurrentActiveEffect(TESObjectREFR* parent)
    {
        if (!parent || IsRuntimeActor(parent))
            return false;

        ActiveEffectRecordMetadata record = {};
        if (!PeekCurrentActiveEffectRecord(record))
            return false;

        return record.conversion.handlerCode == kSEFFHandlerCode &&
            record.duration > kStuckScriptEffectDurationThreshold;
    }

    void LogDroppedActiveEffect(TESObjectREFR* parent, const ActiveEffectConversionData& conversion, UInt16 recordSize, float duration)
    {
        const UInt32 refID = parent ? parent->refID : 0;
        const UInt8 baseType = parent && parent->baseForm ? parent->baseForm->typeID : 0;

        ++s_droppedNonActorScriptEffects;
        _MESSAGE(
            "Dropped stuck non-actor SEFF active effect: parent=%08X baseType=%02X mgef=%08X duration=%.2f size=%u totalDropped=%u",
            refID,
            baseType,
            conversion.mgefCode,
            duration,
            recordSize,
            s_droppedNonActorScriptEffects);
    }

    void* DropCurrentActiveEffect(TESObjectREFR* parent, SInt32& activeEffectIndex)
    {
        TESSaveLoadGameMinimal* saveLoad = GetSaveLoad();
        if (!saveLoad)
            return nullptr;

        UInt16 recordSize = 0;
        UInt32 formID = 0;
        SInt8 effectIndex = -1;
        ActiveEffectConversionData conversion = {};
        bool hasConversion = false;
        float duration = 0.0f;

        LoadData(&recordSize, sizeof(recordSize));
        LoadFormID(&formID, sizeof(formID));
        LoadData(&effectIndex, sizeof(effectIndex));

        if (recordSize == sizeof(ActiveEffectConversionData) && formID == 0 && effectIndex == -1)
        {
            LoadData(&conversion, sizeof(conversion));
            hasConversion = true;

            LoadData(&recordSize, sizeof(recordSize));
            LoadFormID(&formID, sizeof(formID));
            LoadData(&effectIndex, sizeof(effectIndex));
            ++activeEffectIndex;
        }

        const UInt32 payloadOffset = saveLoad->bufferOffset;
        if (hasConversion)
            ReadSavedActiveEffectDuration(recordSize, duration);

        saveLoad->bufferOffset = payloadOffset + recordSize;

        if (hasConversion)
            LogDroppedActiveEffect(parent, conversion, recordSize, duration);
        else
            _WARNING("Dropped non-actor active effect without OBME conversion header: parent=%08X size=%u",
                parent ? parent->refID : 0,
                recordSize);

        return nullptr;
    }

    void __declspec(naked) LoadActiveEffectHook()
    {
        __asm
        {
            mov     eax, [esp + 4]
            push    eax
            call    ShouldDropCurrentActiveEffect
            add     esp, 4
            test    al, al
            jnz     dropEffect
            jmp     [s_obmeLoadActiveEffectHook]

        dropEffect:
            push    ebp
            push    esp
            push    [esp + 0x0C]
            call    DropCurrentActiveEffect
            add     esp, 0x8
            pop     ebp
            retn
        }
    }

    bool InstallNonActorScriptEffectLoadGuard()
    {
        UInt32 currentTarget = GetRelativeCallTarget(kLoadActiveEffectCallSite);
        if (!currentTarget)
        {
            _ERROR("Expected call instruction not found at %08X", kLoadActiveEffectCallSite);
            return false;
        }

        if (currentTarget == reinterpret_cast<UInt32>(&LoadActiveEffectHook))
            return true;

        if (currentTarget == kVanillaLoadActiveEffectTarget)
        {
            _ERROR("OBME active-effect load hook is not installed; OBMEFix requires OBME.dll to load first");
            return false;
        }

        HMODULE obme = GetModuleHandleA("OBME.dll");
        if (!IsAddressInModule(obme, currentTarget))
        {
            _ERROR("Active-effect load hook target %08X is not inside OBME.dll; refusing to chain an unknown hook", currentTarget);
            return false;
        }

        s_obmeLoadActiveEffectHook = currentTarget;
        WriteRelCall(kLoadActiveEffectCallSite, reinterpret_cast<UInt32>(&LoadActiveEffectHook));

        _MESSAGE("Installed non-actor SEFF load guard at %08X; chaining OBME hook %08X",
            kLoadActiveEffectCallSite,
            s_obmeLoadActiveEffectHook);

        return true;
    }

    bool __fastcall ScriptEffect_IsTargetValid(ActiveEffect* activeEffect, void*, MagicTarget* target)
    {
        if (!activeEffect || activeEffect->duration <= kStuckScriptEffectDurationThreshold)
            return true;

        TESObjectREFR* parent = GetNonActorTargetParent(target);
        if (!parent)
            return true;

        const UInt32 refID = parent->refID;
        const UInt8 baseType = parent->baseForm ? parent->baseForm->typeID : 0;

        ++s_blockedNonActorScriptEffects;
        _MESSAGE(
            "Blocked duration SEFF active effect on non-actor target: parent=%08X baseType=%02X duration=%.2f totalBlocked=%u",
            refID,
            baseType,
            activeEffect->duration,
            s_blockedNonActorScriptEffects);

        return false;
    }

    bool InstallNonActorScriptEffectCreationGuard()
    {
        UInt32 currentTarget = *reinterpret_cast<UInt32*>(kScriptEffectIsTargetValidEntry);

        if (currentTarget == reinterpret_cast<UInt32>(&ScriptEffect_IsTargetValid))
        {
            _MESSAGE("Non-actor SEFF creation guard is already installed");
            return true;
        }

        if (currentTarget != kVanillaAlwaysTrueIsTargetValid)
        {
            _WARNING(
                "ScriptEffect::IsTargetValid entry is %08X, expected vanilla %08X; skipping creation guard to avoid clobbering another patch",
                currentTarget,
                kVanillaAlwaysTrueIsTargetValid);
            return false;
        }

        SafeWrite32(kScriptEffectIsTargetValidEntry, reinterpret_cast<UInt32>(&ScriptEffect_IsTargetValid));

        _MESSAGE("Installed non-actor SEFF creation guard at vtable entry %08X; previous target %08X",
            kScriptEffectIsTargetValidEntry,
            currentTarget);

        return true;
    }

    void LogDeploymentDiagnostics()
    {
        if (GetModuleHandleA("CSI.dll") && !s_loggedCsiDiagnostic)
        {
            s_loggedCsiDiagnostic = true;
            _WARNING("CSI.dll is loaded with OBME.dll. OBME v1.0 and Custom Spell Icons v3.1 patch the same path and are a known hard conflict.");
        }

        if (GetModuleHandleA("OBME_CS.dll") && !s_loggedObmeCsDiagnostic)
        {
            s_loggedObmeCsDiagnostic = true;
            _WARNING("OBME_CS.dll is loaded in the game runtime. OBME_CS.dll is for Construction Set authoring, not gameplay.");
        }
    }

    void VerifyMagicItemVFXHook(HMODULE obme)
    {
        UInt32 currentTarget = GetRelativeJumpTarget(kMagicItemGetVFXEffectHookSite);
        if (!currentTarget)
        {
            if (s_lastMagicItemVFXHookDiagnosticState != kMagicItemVFXHookDiagnostic_NotJump ||
                s_lastMagicItemVFXHookDiagnosticTarget != 0)
            {
                s_lastMagicItemVFXHookDiagnosticState = kMagicItemVFXHookDiagnostic_NotJump;
                s_lastMagicItemVFXHookDiagnosticTarget = 0;
                _WARNING("MagicItem::GetVFXEffect at %08X is not a relative jump; OBME's VFX/icon hook may be missing or overwritten",
                    kMagicItemGetVFXEffectHookSite);
            }
            return;
        }

        if (IsAddressInModule(obme, currentTarget))
        {
            s_lastMagicItemVFXHookDiagnosticState = kMagicItemVFXHookDiagnostic_None;
            s_lastMagicItemVFXHookDiagnosticTarget = 0;
            return;
        }

        if (GetModuleHandleA("CSI.dll"))
        {
            if (s_lastMagicItemVFXHookDiagnosticState != kMagicItemVFXHookDiagnostic_CsiConflict ||
                s_lastMagicItemVFXHookDiagnosticTarget != currentTarget)
            {
                s_lastMagicItemVFXHookDiagnosticState = kMagicItemVFXHookDiagnostic_CsiConflict;
                s_lastMagicItemVFXHookDiagnosticTarget = currentTarget;
                _ERROR("MagicItem::GetVFXEffect hook target %08X is not inside OBME.dll while CSI.dll is loaded; this matches the known OBME/CSI hard conflict",
                    currentTarget);
            }
        }
        else
        {
            if (s_lastMagicItemVFXHookDiagnosticState != kMagicItemVFXHookDiagnostic_UnknownOwner ||
                s_lastMagicItemVFXHookDiagnosticTarget != currentTarget)
            {
                s_lastMagicItemVFXHookDiagnosticState = kMagicItemVFXHookDiagnostic_UnknownOwner;
                s_lastMagicItemVFXHookDiagnosticTarget = currentTarget;
                _WARNING("MagicItem::GetVFXEffect hook target %08X is not inside OBME.dll; another plugin may have overwritten OBME's hook",
                    currentTarget);
            }
        }
    }

    bool InstallRuntimeGuards()
    {
        HMODULE obme = GetModuleHandleA("OBME.dll");
        if (!obme)
        {
            _ERROR("OBME.dll is not loaded. Keep the original OBME.dll installed beside OBMEFix.dll.");
            return false;
        }

        LogDeploymentDiagnostics();
        VerifyMagicItemVFXHook(obme);

        if (!InstallNonActorScriptEffectLoadGuard())
            return false;

        const bool wasInstalled = s_runtimeGuardsInstalled;
        bool creationGuardInstalled = InstallNonActorScriptEffectCreationGuard();
        if (!creationGuardInstalled)
        {
            _WARNING("OBMEFix load-salvage guard is installed, but creation prevention is not active");
        }

        s_runtimeGuardsInstalled = creationGuardInstalled;
        if (s_runtimeGuardsInstalled && !wasInstalled)
            _MESSAGE("OBMEFix runtime guards installed");
        else if (s_runtimeGuardsInstalled)
            _MESSAGE("OBMEFix runtime guards verified");

        return true;
    }

    void MessageHandler(OBSEMessagingInterface::Message* message)
    {
        if (!message)
            return;

        if (message->type == OBSEMessagingInterface::kMessage_PostLoad ||
            message->type == OBSEMessagingInterface::kMessage_PostPostLoad)
        {
            _MESSAGE("OBMEFix received OBSE post-load message; verifying runtime guard installation");
            InstallRuntimeGuards();
        }
    }
}

extern "C"
{
    bool OBSEPlugin_Query(const OBSEInterface* obse, PluginInfo* info)
    {
        info->infoVersion = PluginInfo::kInfoVersion;
        info->name = "OBMEFix";
        info->version = 1;

        if (obse->isEditor)
            return false;

        gLog.Open("OBMEFix.log");
#ifdef _DEBUG
        gLog.SetLogLevel(IDebugLog::kLevel_DebugMessage);
#else
        gLog.SetLogLevel(IDebugLog::kLevel_Message);
#endif

        _MESSAGE("OBMEFix Query");

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

        return true;
    }

    bool OBSEPlugin_Load(const OBSEInterface* obse)
    {
        s_pluginHandle = obse->GetPluginHandle();
        _MESSAGE("OBMEFix Load");

        OBSEMessagingInterface* messaging = reinterpret_cast<OBSEMessagingInterface*>(obse->QueryInterface(kInterface_Messaging));
        if (!messaging)
        {
            _ERROR("OBSE messaging interface is not available; cannot safely defer OBMEFix installation");
            return false;
        }

        if (!messaging->RegisterListener(s_pluginHandle, "OBSE", MessageHandler))
        {
            _ERROR("Failed to register OBMEFix OBSE message listener");
            return false;
        }

        if (GetModuleHandleA("OBME.dll"))
        {
            if (!InstallRuntimeGuards())
                return false;
        }
        else
        {
            _WARNING("OBME.dll is not loaded yet; OBMEFix will retry when OBSE sends PostLoad");
        }

        _MESSAGE("OBMEFix Load Complete");
        return true;
    }
}
