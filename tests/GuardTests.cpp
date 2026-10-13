#define OBMEFIX_TESTING 1
#include "../RuntimeGuards.cpp"
#include "../main.cpp"
using namespace OBMEFix;
using namespace OBMEFixEngine;
#include <vector>
#include <limits>
#include <cstdio>

static unsigned failures = 0, checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)
template<class T> void Append(std::vector<UInt8>& bytes, T value)
{
    const auto first = reinterpret_cast<const UInt8*>(&value);
    bytes.insert(bytes.end(), first, first + sizeof(value));
}
static std::vector<UInt8> SavedRecord(bool conversion, float duration = 2.0f)
{
    std::vector<UInt8> bytes;
    if (conversion)
    {
        Append<UInt16>(bytes, 32); Append<UInt32>(bytes, 0); Append<SInt8>(bytes, -1);
        OBMEFixCore::Conversion c{}; c.version = 0x0100FF00; c.handlerCode = kSEFFHandlerCode;
        Append(bytes, c);
    }
    Append<UInt16>(bytes, 15); Append<UInt32>(bytes, 0x123); Append<SInt8>(bytes, 0);
    bytes.insert(bytes.end(), 11, 0);
    Append(bytes, duration);
    return bytes;
}
static void ParserCases()
{
    using namespace OBMEFixCore;
    for (bool conversion : { false, true })
    {
        auto bytes = SavedRecord(conversion);
        Record result;
        CHECK(ParseRecord(bytes.data(), bytes.size(), 0x71, 2, result) == ParseStatus::Valid);
        CHECK(result.consumedBytes == bytes.size());
        CHECK(result.consumedRecords == (conversion ? 2u : 1u));
        CHECK(result.hasConversion == conversion && result.duration == 2.0f);
        // Every truncation of these minimal records must be rejected before a read/commit.
        for (size_t cut = 0; cut < bytes.size(); ++cut)
            CHECK(ParseRecord(bytes.data(), cut, 0x71, 2, result) == ParseStatus::Truncated);
        CHECK(ParseRecord(bytes.data(), bytes.size(), 0x29, 2, result) == ParseStatus::UnsupportedVersion);
        CHECK(ParseRecord(bytes.data(), bytes.size(), 0x7D, 2, result) == ParseStatus::Valid);
        CHECK(ParseRecord(bytes.data(), bytes.size(), 0x7E, 2, result) == ParseStatus::UnsupportedVersion);
        CHECK(ParseRecord(bytes.data(), bytes.size(), 0x71, 0, result) == ParseStatus::InvalidCount);
        if (conversion) CHECK(ParseRecord(bytes.data(), bytes.size(), 0x71, 1, result) == ParseStatus::InvalidCount);
    }
    auto bytes = SavedRecord(false);
    Record r;
    bytes[0] = 0xFF; bytes[1] = 0xFF;
    CHECK(ParseRecord(bytes.data(), bytes.size(), 0x71, 1, r) == ParseStatus::Truncated);
    for (float value : {0.0f, -1.0f, 1.0f, 1.01f, std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()})
    {
        bytes = SavedRecord(false, value);
        CHECK(ParseRecord(bytes.data(), bytes.size(), 0x71, 1, r) ==
            (std::isfinite(value) ? ParseStatus::Valid : ParseStatus::InvalidDuration));
        CHECK(LongDuration(value) == (std::isfinite(value) && value > 1.0f));
    }
    bytes = SavedRecord(true); bytes[7] = 0x77;
    CHECK(ParseRecord(bytes.data(), bytes.size(), 0x71, 2, r) == ParseStatus::UnsupportedVersion);
    CHECK(!UsesBlocks(0x1E) && UsesBlocks(0x1F) && UsesBlocks(0x59) && !UsesBlocks(0x5A));
    CHECK(KnownConversion(0) && KnownConversion(0x01000301) && !KnownConversion(0x02000000));
    UInt32 count = 0, messages = 0;
    for (unsigned i = 0; i < 4096; ++i) if (ShouldLogEvent(count)) ++messages;
    CHECK(count == 4096 && messages == 17);
    struct Entry { int* effectItem; Entry* next; };
    int item = 5; Entry tail{&item,nullptr}, head{nullptr,&tail};
    CHECK(ItemAt(&head, 0) == nullptr && ItemAt(&head, 1) == &item);
    CHECK(ItemAt(&head, -1) == nullptr && ItemAt(&head, 2) == nullptr);
    CHECK(std::strcmp(NormalizeIcon("Icons\\ spell.dds"), "spell.dds") == 0);
    CHECK(std::strcmp(NormalizeIcon("  DATA/Textures/Menus/Icons/a.dds"), "a.dds") == 0);
    CHECK(std::strcmp(NormalizeIcon("a/b.dds"), "a/b.dds") == 0);
    CHECK(!NormalizeIcon("Icons\\Icons\\a.dds") && !NormalizeIcon("Icons\\  ") && !NormalizeIcon(nullptr));
    float delta=-30.0f;
    CHECK(ClampDamageAmount(9,25.0f,delta) && delta==-25.0f);
    delta=-30.0f; CHECK(!ClampDamageAmount(10,25.0f,delta) && delta==-30.0f);
    delta=5.0f; CHECK(!ClampDamageAmount(9,0.0f,delta) && delta==5.0f);
    delta=-2.0f; CHECK(ClampDamageAmount(9,0.0f,delta) && delta==0.0f);
    delta=-2.0f; CHECK(!ClampDamageAmount(9,10.0f,delta) && delta==-2.0f);
    delta=-2.0f; CHECK(ClampDamageAmount(9,-5.0f,delta) && delta==5.0f);
}

static BOOL WINAPI FailProtect(LPVOID, SIZE_T, DWORD, PDWORD) { return FALSE; }
static BOOL WINAPI FailFlush(HANDLE, LPCVOID, SIZE_T) { return FALSE; }
static unsigned flushCalls;
static BOOL WINAPI FailFirstFlush(HANDLE process, LPCVOID address, SIZE_T size)
{ return ++flushCalls == 1 ? FALSE : FlushInstructionCache(process,address,size); }
static unsigned protectionCalls;
static BOOL WINAPI FailRestore(LPVOID p, SIZE_T s, DWORD n, PDWORD old)
{
    return ++protectionCalls == 2 ? FALSE : VirtualProtect(p, s, n, old);
}
static void PatchCases()
{
    using namespace OBMEFixPatch;
    auto memory = static_cast<UInt8*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE));
    CHECK(memory != nullptr); if (!memory) return;
    UInt8 old[11]{}, replacement[11]; Transfer(replacement, reinterpret_cast<UInt32>(memory),
        reinterpret_cast<UInt32>(memory)+64, 0xE8, 11);
    DWORD protection = 0; CHECK(VirtualProtect(memory, 4096, PAGE_EXECUTE_READ, &protection));
    CHECK(Write(memory,old,replacement,11,true) == Result::Applied);
    CHECK(Write(memory,old,replacement,11,true) == Result::Unchanged);
    MEMORY_BASIC_INFORMATION info{}; VirtualQuery(memory,&info,sizeof(info)); CHECK(info.Protect == PAGE_EXECUTE_READ);
    // A changed tail must never be silently treated as an owned 11-byte sequence.
    CHECK(VirtualProtect(memory,4096,PAGE_READWRITE,&protection)); memory[10] = 0xCC;
    CHECK(Write(memory,old,replacement,11,true) == Result::UnexpectedBytes);
    std::memcpy(memory,old,11);
    CHECK(VirtualProtect(memory,4096,PAGE_EXECUTE_READ,&protection));
    API fail{}; fail.protect = FailProtect;
    CHECK(Write(memory,old,replacement,11,true,fail) == Result::ProtectionFailed);
    CHECK(std::memcmp(memory,old,11) == 0);
    fail = API{}; fail.flush = FailFirstFlush; flushCalls=0;
    CHECK(Write(memory,old,replacement,11,true,fail) == Result::WriteFailed);
    CHECK(flushCalls==2);
    CHECK(std::memcmp(memory,old,11) == 0);
    fail = API{}; fail.flush = FailFlush;
    CHECK(Write(memory,old,replacement,11,true,fail) == Result::RollbackFailed);
    CHECK(std::memcmp(memory,old,11) == 0);
    fail.protect = FailRestore; protectionCalls=0;
    CHECK(Write(memory,old,replacement,11,true,fail) == Result::RollbackAndRestoreFailed);
    fail = API{}; fail.protect = FailRestore; protectionCalls = 0;
    CHECK(Write(memory,old,replacement,11,true,fail) == Result::RestoreFailed);
    memcpy(memory,old,11); fail=API{}; fail.flush=FailFlush;
    CHECK(!CheckedWrite("rollback regression",reinterpret_cast<UInt32>(memory),old,replacement,11,true,fail));
    CHECK(!PatchHealthy(reinterpret_cast<UInt32>(memory)));
    CHECK(NeedsAttention(Result::RollbackFailed) && NeedsAttention(Result::RollbackAndRestoreFailed));
    CHECK(!NeedsAttention(Result::WriteFailed));
    // Only the isolated fixture resets process-lifetime fault state.
    for(auto& fault:s_protectionFaults) fault=0;
    CHECK(!Readable(reinterpret_cast<void*>(UINTPTR_MAX-2), 8));
    CHECK(VirtualFree(memory,0,MEM_RELEASE));
}

static bool registerResult;
static unsigned registrations;
static PluginHandle FakePluginHandle() { return 1; }
static bool FakeRegister(PluginHandle, const char*, OBSEMessagingInterface::EventCallback)
{ ++registrations; return registerResult; }
static OBSEMessagingInterface fakeMessaging{OBSEMessagingInterface::kVersion, FakeRegister, nullptr};
static void* FakeQuery(UInt32 id) { return id == kInterface_Messaging ? &fakeMessaging : nullptr; }
static void LifetimeCases()
{
    OBSEInterface obse{}; obse.GetPluginHandle = FakePluginHandle; obse.QueryInterface = FakeQuery;
    registerResult=false; CHECK(!OBSEPlugin_Load(&obse));
    registerResult=true; CHECK(OBSEPlugin_Load(&obse));
    CHECK(registrations == 2);
    // No game address is mapped yet. Success proves Load did not attempt installation.
    s_runtimeGuardsInstalled=true;
    CHECK(!InstallRuntimeGuards() && !s_runtimeGuardsInstalled);
}

static unsigned forwarded;
static UInt32 forwardedAV;
static float forwardedAmount;
static void __fastcall FakeDamage(Actor*, void*, UInt32 av, float amount, Actor*)
{ ++forwarded; forwardedAV=av; forwardedAmount=amount; }
static void DamageCases()
{
    UInt32 copiedVtable[200]{};
    alignas(4) UInt8 storage[2048]{};
    *reinterpret_cast<UInt32*>(storage)=reinterpret_cast<UInt32>(copiedVtable);
    auto actor=reinterpret_cast<Actor*>(storage);
    for (auto& spec:s_damageAVFHookSpecs) spec.originalTarget=reinterpret_cast<UInt32>(&FakeDamage);
    DamageAVFEntry<0>(actor,nullptr,8,-2.0f,nullptr);
    DamageAVFEntry<1>(actor,nullptr,8,2.0f,nullptr);
    DamageAVFEntry<2>(actor,nullptr,10,-3.0f,nullptr);
    DamageAVFEntry<3>(actor,nullptr,9,7.0f,nullptr);
    CHECK(forwarded == 4 && forwardedAV == 9 && forwardedAmount == 7.0f);
    for (auto& spec:s_damageAVFHookSpecs) spec.originalTarget=0;
}

static ChangeRecord* testChange;
static ChangeRecord* __fastcall FakeFind(ChangesMap*,void*,UInt32) { return testChange; }
static bool parentIsActor;
static bool __fastcall NotActor(void*,void*) { return parentIsActor; }
static TESObjectREFR* loadParent;
static UInt32 loadedValue, loadedIndex, chainedLoads;
__declspec(naked) void FakePreviousLoader()
{
    __asm { inc chainedLoads
        mov eax,0FEEDh
        ret }
}
__declspec(naked) void InvokeLoadHook()
{
    __asm {
        push ebp
        sub esp,24h
        mov word ptr [esp+10h],2
        mov dword ptr [esp+14h],0
        mov dword ptr [esp+18h],0
        xor ebp,ebp
        push loadParent
        call LoadActiveEffectHook
        add esp,4
        mov loadedValue,eax
        inc ebp
        mov loadedIndex,ebp
        add esp,24h
        pop ebp
        ret
    }
}
static void LoadCases()
{
    auto globals=VirtualAlloc(nullptr,0x10000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    auto code=VirtualAlloc(nullptr,0x10000,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    CHECK(globals && code); if (!globals || !code) return;
    kCurrentSaveLoadAddr=reinterpret_cast<UInt32>(globals);
    kChangesMapFindAddr=reinterpret_cast<UInt32>(code);
    kLoadCallerContext=reinterpret_cast<UInt32>(code)+64;
    kLoadCountReload=reinterpret_cast<UInt32>(code)+96;
    const UInt8 caller[]={0x8B,0x7C,0x24,0x2C,0x57};
    const UInt8 countReload[]={0x0F,0xB7,0x4C,0x24,0x10,0x83,0xC5,0x01,0x3B,0xE9};
    memcpy(reinterpret_cast<void*>(kLoadCallerContext),caller,sizeof(caller));
    memcpy(reinterpret_cast<void*>(kLoadCountReload),countReload,sizeof(countReload));
    UInt8 jump[5]; OBMEFixPatch::Transfer(jump,kChangesMapFindAddr,reinterpret_cast<UInt32>(&FakeFind),0xE9,5);
    memcpy(reinterpret_cast<void*>(kChangesMapFindAddr),jump,5);
    CHECK(FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(kChangesMapFindAddr),5));
    auto payload=SavedRecord(true);
    std::vector<UInt8> buffer(4+payload.size());
    UInt16 length=static_cast<UInt16>(payload.size()); memcpy(buffer.data(),&length,2);
    buffer[2]=0x3E; buffer[3]=0x7D; memcpy(buffer.data()+4,payload.data(),payload.size());
    UInt8 header[12]{}; UInt32 formID=0x123456;
    memcpy(header,&formID,4); header[4]=0x3E; header[9]=0x7D; memcpy(header+10,&length,2);
    UInt32 vtable[200]; for(auto& method:vtable) method=reinterpret_cast<UInt32>(&NotActor);
    alignas(4) UInt8 refr[512]{}; *reinterpret_cast<UInt32*>(refr)=reinterpret_cast<UInt32>(vtable);
    loadParent=reinterpret_cast<TESObjectREFR*>(refr); loadParent->refID=formID;
    ChangeRecord change{0,buffer.data()}; testChange=&change;
    alignas(4) UInt8 loaderBytes[0x84]{};
    auto loader=reinterpret_cast<TESSaveLoadGame*>(loaderBytes);
    const SaveLoadView view(loader);
    auto put=[&]<class T>(size_t offset,T value) { memcpy(loaderBytes+offset,&value,sizeof(value)); };
    put(0x00,reinterpret_cast<ChangesMap*>(&change)); put(0x14,buffer.data()+4); put(0x7C,UInt8(0x7D)); put(0x80,header);
    *reinterpret_cast<TESSaveLoadGame**>(kCurrentSaveLoadAddr)=loader;
    UInt32 end=0;
    CHECK(GetRecordExtent(loader,loadParent,0,0,end));
    CHECK(end==reinterpret_cast<UInt32>(buffer.data()+buffer.size()));
    header[10]++; CHECK(!GetRecordExtent(loader,loadParent,0,0,end)); header[10]--;
    testChange=nullptr; CHECK(!GetRecordExtent(loader,loadParent,0,0,end)); testChange=&change;
    UInt32 stack[9]{}; stack[6]=2; SInt32 index=0;
    CHECK(TryDropCurrentEffect(loadParent,stack,&index));
    CHECK(index==1 && view.Cursor()==buffer.data()+buffer.size());
    put(0x14,buffer.data()+4); index=0;
    *reinterpret_cast<UInt8*>(kLoadCallerContext)=0x90;
    CHECK(!TryDropCurrentEffect(loadParent,stack,&index) && index==0 && view.Cursor()==buffer.data()+4);
    CHECK(!TryDropCurrentEffect(reinterpret_cast<TESObjectREFR*>(1),nullptr,nullptr));
    *reinterpret_cast<UInt8*>(kLoadCallerContext)=0x8B;
    parentIsActor=true;
    CHECK(!TryDropCurrentEffect(loadParent,stack,&index) && index==0 && view.Cursor()==buffer.data()+4);
    parentIsActor=false;
    put(0x14,buffer.data()+4); index=0; stack[6]=1;
    CHECK(!TryDropCurrentEffect(loadParent,stack,&index));
    CHECK(index==0 && view.Cursor()==buffer.data()+4);
    s_obmeLoadActiveEffectHook=reinterpret_cast<UInt32>(&FakePreviousLoader);
    InvokeLoadHook(); CHECK(loadedValue==0 && loadedIndex==2 && chainedLoads==0);
    put(0x14,buffer.data()+4);
    const float duration=1.0f; memcpy(buffer.data()+buffer.size()-4,&duration,4);
    InvokeLoadHook(); CHECK(loadedValue==0xFEED && loadedIndex==1 && chainedLoads==1);
    CHECK(view.Cursor()==buffer.data()+4);
    UInt32 forms[4]={0,0x1000042,0,0};
    alignas(4) UInt8 tableStorage[sizeof(NiTLargeArray<UInt32>)]{};
    auto table=reinterpret_cast<NiTLargeArray<UInt32>*>(tableStorage);
    table->data=forms; table->capacity=4; table->firstFreeEntry=3;
    put(0x74,table); put(0x7D,UInt8(1));
    UInt32 resolved=0;
    CHECK(ResolveSavedFormID(loader,1,resolved) && resolved==forms[1]);
    CHECK(!ResolveSavedFormID(loader,2,resolved));
    CHECK(!ResolveSavedFormID(loader,4,resolved));
    CHECK(ResolveSavedFormID(loader,0xFF000041,resolved) && resolved==0xFF000041);
    CHECK(view.Cursor()==buffer.data()+4);
    buffer[3]=header[9]=0x2A; put(0x7C,UInt8(0x2A));
    put(0x14,buffer.data()+6);
    const UInt32 blockStart=reinterpret_cast<UInt32>(buffer.data()+4);
    CHECK(GetRecordExtent(loader,loadParent,blockStart,length,end));
    CHECK(!GetRecordExtent(loader,loadParent,blockStart,length+1,end));
    s_obmeLoadActiveEffectHook=0;
    CHECK(VirtualFree(code,0,MEM_RELEASE)); CHECK(VirtualFree(globals,0,MEM_RELEASE));
}

static unsigned foreignCalls;
static bool __declspec(noinline) __fastcall ForeignValidator(ActiveEffect* effect, void*, MagicTarget* target)
{ ++foreignCalls; return ScriptEffect_IsTargetValid(effect,nullptr,target); }
static bool __fastcall RejectValidator(ActiveEffect*,void*,MagicTarget*) { return false; }
static void CreationCases()
{
    auto table=VirtualAlloc(nullptr,0x10000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    auto code=VirtualAlloc(nullptr,0x10000,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    CHECK(table && code); if (!table || !code) return;
    kScriptEffectIsTargetValidEntry=reinterpret_cast<UInt32>(table);
    kVanillaAlwaysTrueIsTargetValid=reinterpret_cast<UInt32>(code);
    const UInt8 vanilla[]={0xB0,0x01,0xC2,0x04,0x00};
    memcpy(reinterpret_cast<void*>(kVanillaAlwaysTrueIsTargetValid),vanilla,sizeof(vanilla));
    FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(kVanillaAlwaysTrueIsTargetValid),sizeof(vanilla));
    auto slot=reinterpret_cast<UInt32*>(kScriptEffectIsTargetValidEntry);
    *slot=kVanillaAlwaysTrueIsTargetValid; s_scriptEffectIsTargetValid=0;
    CHECK(InstallNonActorScriptEffectCreationGuard());
    CHECK(InstallNonActorScriptEffectCreationGuard());
    CHECK(reinterpret_cast<UInt32>(&ForeignValidator)!=reinterpret_cast<UInt32>(&ScriptEffect_IsTargetValid));
    *slot=reinterpret_cast<UInt32>(&ForeignValidator);
    CHECK(!InstallNonActorScriptEffectCreationGuard());
    CHECK(*slot==reinterpret_cast<UInt32>(&ForeignValidator) && s_scriptEffectIsTargetValid==kVanillaAlwaysTrueIsTargetValid);
    alignas(4) UInt8 storage[128]{}; auto effect=reinterpret_cast<ActiveEffect*>(storage);
    effect->duration=1.0f; CHECK(ForeignValidator(effect,nullptr,nullptr)); CHECK(foreignCalls==1);
    effect->duration=std::numeric_limits<float>::quiet_NaN(); CHECK(ScriptEffect_IsTargetValid(effect,nullptr,nullptr));
    s_scriptEffectIsTargetValid=reinterpret_cast<UInt32>(&RejectValidator);
    CHECK(!ScriptEffect_IsTargetValid(effect,nullptr,nullptr));
    s_scriptEffectIsTargetValid=0;
    CHECK(VirtualFree(code,0,MEM_RELEASE)); CHECK(VirtualFree(table,0,MEM_RELEASE));
}

static EffectSetting* chosenEffect;
static UInt32 seenRange;
static EffectSetting* __fastcall FakeVFX(MagicItem*, void*, UInt32 range)
{ seenRange=range; return chosenEffect; }
static UInt32 vfxBranch;
static UInt8 hitEffectStorage[64];
static const char modelPath[]="verified.nif";
static const char* __fastcall FakeModelPath(void*,void*) { return modelPath; }
__declspec(naked) void VFXNormalEnd()
{
    __asm {
        // Continuation bytes from 0x69E4BF through the path store, then the test epilogue.
        lea ecx,[eax+18h]
        mov eax,[edx+14h]
        call eax
        mov [esi+2Ch],eax
        mov byte ptr [esi+28h],0
        mov vfxBranch,1
        pop esi
        ret
    }
}
__declspec(naked) void VFXNullEnd()
{
    __asm { mov byte ptr [esi+28h],0
        mov vfxBranch,2
        pop esi
        ret }
}
__declspec(naked) void InvokeVFX()
{
    __asm { push esi
        lea esi,hitEffectStorage
        xor ecx,ecx
        push 3
        jmp MagicModelHitEffectVFXHook }
}
static void VFXCases()
{
    const auto region=VirtualAlloc(nullptr,0x10000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    CHECK(region != nullptr); if (!region) return;
    kEffectSettingCollectionAddr=reinterpret_cast<UInt32>(region);
    auto map=reinterpret_cast<NiTPointerMap<EffectSetting>*>(kEffectSettingCollectionAddr);
    NiTPointerMap<EffectSetting>::Entry* buckets[1]{};
    map->m_numBuckets=1; map->m_buckets=buckets;
    UInt32 modelVtable[6]{}; modelVtable[5]=reinterpret_cast<UInt32>(&FakeModelPath);
    alignas(4) UInt8 effect[256]{};
    *reinterpret_cast<UInt32*>(effect+0x18)=reinterpret_cast<UInt32>(modelVtable);
    s_magicModelHitEffectGetVFXCallTarget=reinterpret_cast<UInt32>(&FakeVFX);
    s_vfxNonNullContinuation=reinterpret_cast<UInt32>(&VFXNormalEnd);
    s_vfxNullContinuation=reinterpret_cast<UInt32>(&VFXNullEnd);
    chosenEffect=reinterpret_cast<EffectSetting*>(effect);
    InvokeVFX(); CHECK(vfxBranch==1 && seenRange==3);
    CHECK(*reinterpret_cast<const char**>(hitEffectStorage+0x2C)==modelPath);
    chosenEffect=nullptr; memset(hitEffectStorage,0,sizeof(hitEffectStorage));
    InvokeVFX(); CHECK(vfxBranch==2 && *reinterpret_cast<UInt32*>(hitEffectStorage+0x2C)==0);
    map->m_numBuckets=0;
    InvokeVFX(); CHECK(vfxBranch==2); // no divide-by-zero when the collection is unavailable
    map->m_numBuckets=1;
    NiTPointerMap<EffectSetting>::Entry entry{nullptr,kSEFFHandlerCode,reinterpret_cast<EffectSetting*>(effect)};
    buckets[0]=&entry;
    InvokeVFX(); CHECK(vfxBranch==1);
    CHECK(VirtualFree(region,0,MEM_RELEASE));
}

static void OwnershipCases()
{
    auto image=static_cast<UInt8*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
    auto sites=static_cast<UInt8*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
    CHECK(image && sites); if (!image || !sites) return;
    auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(image); dos->e_magic=IMAGE_DOS_SIGNATURE; dos->e_lfanew=0x80;
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS*>(image+0x80);
    nt->Signature=IMAGE_NT_SIGNATURE; nt->FileHeader.Machine=IMAGE_FILE_MACHINE_I386;
    nt->OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR32_MAGIC; nt->OptionalHeader.SizeOfImage=4096;
    const UInt32 base=reinterpret_cast<UInt32>(image);
    auto module=reinterpret_cast<HMODULE>(image);
    image[0x300]=0xC3;
    const UInt8 iconPrefix[]={0x83,0xE9,0x24}; memcpy(image+0x200,iconPrefix,3);
    OBMEFixPatch::Transfer(image+0x203,base+0x203,base+0x300,0xE8,5);
    const UInt8 iconRet[]={0xC2,0x08,0}; memcpy(image+0x208,iconRet,3);
    const UInt8 loadPrefix[]={0x55,0x54,0xFF,0x74,0x24,0x0C}; memcpy(image+0x240,loadPrefix,6);
    OBMEFixPatch::Transfer(image+0x246,base+0x246,base+0x300,0xE8,5);
    const UInt8 loadRet[]={0x83,0xC4,0x08,0x5D,0xC3}; memcpy(image+0x24B,loadRet,5);
    CHECK(IsOBMEIconAdapter(module,base+0x200)); CHECK(IsOBMELoadAdapter(module,base+0x240));
    kAddSpellMessageIconCallSite=reinterpret_cast<UInt32>(sites);
    OBMEFixPatch::Transfer(sites,kAddSpellMessageIconCallSite,base+0x200,0xE8,11);
    s_obmeAddSpellMessageIconHook=0;
    CHECK(InstallAddSpellMessageIconGuard(module)); CHECK(InstallAddSpellMessageIconGuard(module));
    OBMEFixPatch::Transfer(sites,kAddSpellMessageIconCallSite,reinterpret_cast<UInt32>(&AddSpellMessageIconHook),0xE9,11);
    CHECK(InstallAddSpellMessageIconGuard(module));
    CHECK(GetRelativeCallTarget(kAddSpellMessageIconCallSite)==reinterpret_cast<UInt32>(&AddSpellMessageIconHook));
    sites[10]=0xCC;
    CHECK(!InstallAddSpellMessageIconGuard(module) && sites[10]==0xCC);
    OBMEFixPatch::Transfer(sites,kAddSpellMessageIconCallSite,reinterpret_cast<UInt32>(&ForeignValidator),0xE9,11);
    UInt8 original[11]; memcpy(original,sites,11);
    CHECK(!InstallAddSpellMessageIconGuard(module) && memcmp(original,sites,11)==0);
    s_obmeAddSpellMessageIconHook=0;
    OBMEFixPatch::Transfer(sites,kAddSpellMessageIconCallSite,reinterpret_cast<UInt32>(&ForeignValidator),0xE8,11);
    memcpy(original,sites,11);
    CHECK(!InstallAddSpellMessageIconGuard(module) && memcmp(original,sites,11)==0);
    kMagicItemGetVFXEffectHookSite=reinterpret_cast<UInt32>(sites+32);
    OBMEFixPatch::Transfer(sites+32,kMagicItemGetVFXEffectHookSite,base+0x300,0xE9,5);
    s_obmeMagicItemVFXHook=0;
    CHECK(VerifyMagicItemVFXHook(module));
    OBMEFixPatch::Transfer(sites+32,kMagicItemGetVFXEffectHookSite,reinterpret_cast<UInt32>(&ForeignValidator),0xE9,5);
    memcpy(original,sites+32,5);
    CHECK(!VerifyMagicItemVFXHook(module) && memcmp(original,sites+32,5)==0);
    kLoadActiveEffectCallSite=reinterpret_cast<UInt32>(sites+64);
    kLoadCallerContext=reinterpret_cast<UInt32>(sites+128);
    kLoadCountReload=reinterpret_cast<UInt32>(sites+160);
    const UInt8 caller[]={0x8B,0x7C,0x24,0x2C,0x57};
    const UInt8 countReload[]={0x0F,0xB7,0x4C,0x24,0x10,0x83,0xC5,0x01,0x3B,0xE9};
    memcpy(sites+128,caller,sizeof(caller)); memcpy(sites+160,countReload,sizeof(countReload));
    OBMEFixPatch::Transfer(sites+64,kLoadActiveEffectCallSite,base+0x240,0xE8,5);
    s_obmeLoadActiveEffectHook=0;
    CHECK(InstallNonActorScriptEffectLoadGuard(module));
    sites[128]=0x90;
    CHECK(!InstallNonActorScriptEffectLoadGuard(module));
    CHECK(GetRelativeCallTarget(kLoadActiveEffectCallSite)==reinterpret_cast<UInt32>(&LoadActiveEffectHook));
    CHECK(VirtualFree(sites,0,MEM_RELEASE)); CHECK(VirtualFree(image,0,MEM_RELEASE));
}

int main()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    ParserCases(); PatchCases(); LifetimeCases(); DamageCases(); LoadCases(); VFXCases(); CreationCases(); OwnershipCases();
    std::printf("OBMEFix targeted checks: %u passed, %u failed\n",checks-failures,failures);
    return failures ? 1 : 0;
}
