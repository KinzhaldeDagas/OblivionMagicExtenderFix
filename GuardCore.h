#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>

// Engine-independent policy and parsing. No cursor or caller state is changed here.
namespace OBMEFixCore
{
    // Oblivion 1.2.0416 initializes +0x71/+0x7C to 0x7D at 0x0045B648.
    constexpr std::uint8_t kNewestRecordVersion = 0x7D;
    struct Conversion
    {
        std::uint32_t version, handlerCode, mgefCode;
        std::int32_t relativeIndex;
        std::uint32_t reserved[4];
    };
    static_assert(sizeof(Conversion) == 32, "OBME conversion layout");

    enum class ParseStatus { Valid, UnsupportedVersion, Truncated, InvalidCount, InvalidDuration };
    struct Record
    {
        Conversion conversion{};
        std::uint32_t encodedFormID = 0;
        std::int8_t effectIndex = -1;
        std::uint16_t payloadSize = 0;
        float duration = 0;
        std::size_t consumedBytes = 0;
        unsigned consumedRecords = 1;
        bool hasConversion = false;
    };

    inline bool UsesBlocks(std::uint8_t version) { return version >= 0x1F && version < 0x5A; }
    inline bool LongDuration(float duration) { return std::isfinite(duration) && duration > 1.0f; }
    inline bool ClampDamageAmount(std::uint32_t actorValue, float current, float& amount)
    {
        if (amount < 0.0f && actorValue != 0x0A && current + amount < 0.0f)
        {
            amount = -current;
            return true;
        }
        return false;
    }
    inline bool KnownConversion(std::uint32_t version)
    {
        return version == 0 || version == 0x01000301 ||
            (version >= 0x01000300 && version <= 0x0100FF00 && (version & 0xFF) == 0);
    }

    inline ParseStatus ParseRecord(const std::uint8_t* data, std::size_t available,
        std::uint8_t version, unsigned remainingRecords, Record& result)
    {
        result = Record{};
        if (version < 0x2A || version > kNewestRecordVersion) return ParseStatus::UnsupportedVersion;
        if (!remainingRecords) return ParseStatus::InvalidCount;
        std::size_t at = 0;
        auto read = [&](void* destination, std::size_t size) {
            if (at > available || size > available - at || !data) return false;
            std::memcpy(destination, data + at, size);
            at += size;
            return true;
        };
        auto header = [&]() {
            return read(&result.payloadSize, 2) && read(&result.encodedFormID, 4) &&
                read(&result.effectIndex, 1);
        };
        if (!header()) return ParseStatus::Truncated;
        // Test the raw marker, before any form-ID remapping can turn an ID into zero.
        if (result.payloadSize == sizeof(Conversion) && !result.encodedFormID && result.effectIndex == -1)
        {
            if (remainingRecords < 2) return ParseStatus::InvalidCount;
            if (!read(&result.conversion, sizeof(Conversion))) return ParseStatus::Truncated;
            if (!KnownConversion(result.conversion.version)) return ParseStatus::UnsupportedVersion;
            result.hasConversion = true;
            result.consumedRecords = 2;
            if (!header()) return ParseStatus::Truncated;
        }
        if (result.payloadSize < 15 || result.payloadSize > available - at) return ParseStatus::Truncated;
        std::memcpy(&result.duration, data + at + 11, sizeof(float));
        result.consumedBytes = at + result.payloadSize;
        return std::isfinite(result.duration) ? ParseStatus::Valid : ParseStatus::InvalidDuration;
    }

    inline char Lower(char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
    inline bool Prefix(const char* path, const char* prefix)
    {
        if (!path) return false;
        while (*prefix)
        {
            char a = *path++, b = *prefix++;
            if (a == '/') a = '\\';
            if (b == '/') b = '\\';
            if (Lower(a) != Lower(b)) return false;
        }
        return true;
    }
    inline const char* NormalizeIcon(const char* path)
    {
        if (!path) return nullptr;
        while (*path == ' ' || *path == '\t') ++path;
        // Strip one recognized root. A repeated root is ambiguous and falls back.
        if (Prefix(path, "Data\\Textures\\Menus\\Icons\\")) path += 26;
        else if (Prefix(path, "Textures\\Menus\\Icons\\")) path += 21;
        else if (Prefix(path, "Menus\\Icons\\")) path += 12;
        else if (Prefix(path, "Icons\\")) path += 6;
        while (*path == ' ' || *path == '\t') ++path;
        if (!*path || Prefix(path, "Icons\\") || Prefix(path, "Menus\\Icons\\") ||
            Prefix(path, "Textures\\Menus\\Icons\\") || Prefix(path, "Data\\Textures\\Menus\\Icons\\")) return nullptr;
        return path;
    }

    template<class Entry>
    auto ItemAt(Entry* entry, std::int8_t index) -> decltype(entry->effectItem)
    {
        if (index < 0) return nullptr;
        for (unsigned n = 0; entry; entry = entry->next, ++n)
            if (n == static_cast<unsigned>(index)) return entry->effectItem;
        return nullptr;
    }

    // One initial event, then one summary per 256 events. Counts always advance.
    template<class Counter>
    inline bool ShouldLogEvent(Counter& count)
    {
        static_assert(sizeof(Counter) == 4, "32-bit event counter");
        ++count;
        return count == 1 || (count & 255) == 0;
    }
}
