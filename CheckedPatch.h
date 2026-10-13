#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstring>

namespace OBMEFixPatch
{
    inline bool Readable(const void* pointer, std::size_t size)
    {
        auto at = reinterpret_cast<std::uintptr_t>(pointer);
        if (!at || size > UINTPTR_MAX - at) return false;
        const auto end = at + size;
        while (at < end)
        {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQuery(reinterpret_cast<void*>(at), &region, sizeof(region)) ||
                region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
                !(region.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
            const auto base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
            if (region.RegionSize > UINTPTR_MAX - base) return false;
            const auto next = base + region.RegionSize;
            if (next <= at) return false;
            at = next < end ? next : end;
        }
        return true;
    }
    inline bool Executable(std::uintptr_t address)
    {
        MEMORY_BASIC_INFORMATION region{};
        return VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)) &&
            region.State == MEM_COMMIT && !(region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
    }
    enum class Result { Applied, Unchanged, UnexpectedBytes, ProtectionFailed, WriteFailed, RestoreFailed,
        RollbackFailed, RollbackAndRestoreFailed };
    struct API
    {
        decltype(&VirtualProtect) protect = &VirtualProtect;
        decltype(&FlushInstructionCache) flush = &FlushInstructionCache;
    };
    inline Result Write(void* destination, const void* expected, const void* replacement,
        std::size_t size, bool code, const API& api = API{})
    {
        if (!size || !Readable(destination, size)) return Result::UnexpectedBytes;
        if (std::memcmp(destination, replacement, size) == 0) return Result::Unchanged;
        if (std::memcmp(destination, expected, size) != 0) return Result::UnexpectedBytes;
        // Refuse a transaction spanning regions with different original protections.
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(destination, &region, sizeof(region))) return Result::ProtectionFailed;
        const auto at = reinterpret_cast<std::uintptr_t>(destination);
        const auto end = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (size > end - at) return Result::ProtectionFailed;
        DWORD oldProtect = 0;
        if (!api.protect(destination, size, code ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &oldProtect)) return Result::ProtectionFailed;
        Result result = Result::UnexpectedBytes;
        // Recheck ownership after changing protections; never roll back over someone else's patch.
        if (std::memcmp(destination, expected, size) == 0)
        {
            std::memcpy(destination, replacement, size);
            const bool flushed = !code || api.flush(GetCurrentProcess(), destination, size);
            result = flushed && std::memcmp(destination, replacement, size) == 0 ? Result::Applied : Result::WriteFailed;
            if (result == Result::WriteFailed && std::memcmp(destination, replacement, size) == 0)
            {
                std::memcpy(destination, expected, size);
                const bool rollbackFlushed = !code || api.flush(GetCurrentProcess(), destination, size);
                if (!rollbackFlushed || std::memcmp(destination, expected, size) != 0)
                    result = Result::RollbackFailed;
            }
        }
        DWORD ignored = 0;
        if (!api.protect(destination, size, oldProtect, &ignored))
            return result == Result::RollbackFailed ? Result::RollbackAndRestoreFailed : Result::RestoreFailed;
        return result;
    }
    inline bool Success(Result r) { return r == Result::Applied || r == Result::Unchanged; }
    inline bool NeedsAttention(Result r)
    {
        return r == Result::RestoreFailed || r == Result::RollbackFailed || r == Result::RollbackAndRestoreFailed;
    }
    inline const char* Describe(Result r)
    {
        switch (r)
        {
        case Result::Applied: return "applied";
        case Result::Unchanged: return "already installed";
        case Result::UnexpectedBytes: return "unexpected bytes; no authorized write";
        case Result::ProtectionFailed: return "could not change page protection";
        case Result::WriteFailed: return "write/cache verification failed";
        case Result::RestoreFailed: return "page protection restoration failed";
        case Result::RollbackFailed: return "rollback cache/readback failed; instruction state unverified";
        case Result::RollbackAndRestoreFailed: return "rollback and page protection restoration failed";
        }
        return "unknown patch outcome";
    }
    inline void Transfer(std::uint8_t* bytes, std::uintptr_t site, std::uintptr_t target,
        std::uint8_t opcode, std::size_t size)
    {
        std::memset(bytes, 0x90, size);
        bytes[0] = opcode;
        const auto displacement = static_cast<std::uint32_t>(target - site - 5);
        std::memcpy(bytes + 1, &displacement, sizeof(displacement));
    }
}
