#pragma once
#include "obse/GameAPI.h"
#include "obse/GameObjects.h"
#include "obse/GameActorValues.h"
#include "CheckedPatch.h"
#include <cstddef>
#include <cstring>
#include <type_traits>

// The SDK owns public engine types. Fields absent from its partial TESSaveLoadGame
// declaration are read only through the verified Oblivion 1.2.0416 byte boundary.
class ChangesMap;
namespace OBMEFixEngine
{
#ifdef OBMEFIX_TESTING
#define OBMEFIX_ENGINE_ADDRESS UInt32
#else
#define OBMEFIX_ENGINE_ADDRESS constexpr UInt32
#endif
    constexpr UInt32 kOblivionVersion = OBLIVION_VERSION;
    OBMEFIX_ENGINE_ADDRESS kLoadActiveEffectCallSite = 0x0068F035;
    constexpr UInt32 kVanillaLoadActiveEffectTarget = 0x0068EDA0;
    OBMEFIX_ENGINE_ADDRESS kCurrentSaveLoadAddr = 0x00B33B00;
    OBMEFIX_ENGINE_ADDRESS kEffectSettingCollectionAddr = 0x00B33508;
    OBMEFIX_ENGINE_ADDRESS kChangesMapFindAddr = 0x00452D10;
    constexpr UInt32 kLookupFormByIDAddr = 0x0046B250;
    constexpr UInt32 kSEFFHandlerCode = 'FFES';
    OBMEFIX_ENGINE_ADDRESS kMagicItemGetVFXEffectHookSite = 0x00419B30;
    constexpr UInt32 kMagicModelHitEffectGetVFXCallSite = 0x0069E4B7;
    OBMEFIX_ENGINE_ADDRESS kAddSpellMessageIconCallSite = 0x00514A98;
    constexpr UInt32 kAddSpellMessageIconReturn = 0x00514AA3;
    constexpr UInt32 kVanillaEffectItemListGetStrongestItem = 0x004152C0;
    constexpr UInt32 kScriptEffectVtbl = 0x00A76AAC;
    OBMEFIX_ENGINE_ADDRESS kScriptEffectIsTargetValidEntry = kScriptEffectVtbl + 0x34;
    OBMEFIX_ENGINE_ADDRESS kVanillaAlwaysTrueIsTargetValid = 0x006FDFF0;
    OBMEFIX_ENGINE_ADDRESS kLoadCallerContext = 0x0068F030;
    OBMEFIX_ENGINE_ADDRESS kLoadCountReload = 0x0068F08F;
#undef OBMEFIX_ENGINE_ADDRESS
    constexpr UInt32 kDamageAVFSlot = 0x2A4;
    constexpr UInt32 kActorVtbl = 0x00A6E074;
    constexpr UInt32 kCharacterVtbl = 0x00A6FC9C;
    constexpr UInt32 kCreatureVtbl = 0x00A710F4;
    constexpr UInt32 kPlayerCharacterVtbl = 0x00A73A0C;
    constexpr UInt32 kVanillaActorDamageAVF = 0x005E2BE0;
    constexpr UInt32 kVanillaCreatureDamageAVF = 0x00625710;
    constexpr UInt32 kVanillaPlayerDamageAVF = 0x0065E530;
    constexpr UInt32 kActorValueFatigue = kActorVal_Fatigue;

    constexpr UInt32 kVFXNonNullContinuation = 0x0069E4BF;
    constexpr UInt32 kVFXNullContinuation = 0x0069E4CA;
    static_assert(sizeof(void*) == 4 && sizeof(UInt32) == 4, "Oblivion x86 ABI");
    static_assert(OBLIVION_VERSION == 0x010201A0, "address table is for 1.2.0416");
    static_assert(offsetof(TESSaveLoadGame, flags) == 0x18, "pinned loader prefix");
    static_assert(offsetof(TESSaveLoadGame, modRefIDTable) == 0x4C, "pinned loader prefix");
    static_assert(kActorVal_Fatigue == 0x0A, "engine-independent clamp contract");

    // Borrowed ChangesMap result: two observed fields; not an owned interface copy.
    struct ChangeRecord { UInt32 flags; UInt8* data; };
    static_assert(sizeof(ChangeRecord) == 8 && offsetof(ChangeRecord, data) == 4, "0x452D10 result");

    class SaveLoadView
    {
    public:
        static constexpr size_t kReadableExtent = 0x84;
        explicit SaveLoadView(TESSaveLoadGame* native) : native_(native) {}
        bool IsReadable() const { return OBMEFixPatch::Readable(native_, kReadableExtent); }
        ChangesMap* Changes() const { return Read<ChangesMap*, 0x00>(); }
        UInt8* Cursor() const { return Read<UInt8*, 0x14>(); }
        NiTLargeArray<UInt32>* Irefs() const { return Read<NiTLargeArray<UInt32>*, 0x74>(); }
        UInt8 Version() const { return Read<UInt8, 0x7C>(); }
        bool UsesIrefs() const { return Read<UInt8, 0x7D>() != 0; }
        const UInt8* FormHeader() const { return Read<const UInt8*, 0x80>(); }
        void SetCursor(UInt8* cursor) const
        {
            std::memcpy(reinterpret_cast<UInt8*>(native_) + 0x14, &cursor, sizeof(cursor));
        }
    private:
        template<class T, size_t Offset> T Read() const
        {
            static_assert(std::is_trivially_copyable_v<T> && Offset + sizeof(T) <= kReadableExtent);
            T value;
            std::memcpy(&value, reinterpret_cast<const UInt8*>(native_) + Offset, sizeof(value));
            return value;
        }
        TESSaveLoadGame* native_; // borrowed; never construct, free, or take ownership
    };

    struct LoadCallContext
    {
        UInt16 totalRecords;
        UInt16 blockLength;
        UInt32 blockStart;
        static LoadCallContext FromEntry(const void* entry)
        {
            // Entry ESP after parent PUSH and CALL 0x68F035; verified from native bytes.
            const auto bytes = static_cast<const UInt8*>(entry);
            LoadCallContext result{};
            std::memcpy(&result.totalRecords, bytes + 0x18, 2);
            std::memcpy(&result.blockLength, bytes + 0x1C, 2);
            std::memcpy(&result.blockStart, bytes + 0x20, 4);
            return result;
        }
    };
    using FindChangeFn = ChangeRecord* (__thiscall *)(ChangesMap*, UInt32);
}
