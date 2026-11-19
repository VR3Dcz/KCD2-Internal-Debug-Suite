#pragma once

#include <Windows.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

/**
 * @brief Memory patching, inline hook installation, and IDA-style pattern scanning utilities.
 */
namespace MyMemDetail {

    struct PatternByte {
        uint8_t value;
        bool wildcard;
    };

    inline std::vector<PatternByte> ParseIdaPattern(const char* pattern) {
        std::vector<PatternByte> parsed;
        if (!pattern)
            return parsed;

        const char* cursor = pattern;
        while (*cursor) {
            while (*cursor == ' ' || *cursor == '\t')
                ++cursor;
            if (*cursor == '\0')
                break;

            if (*cursor == '?') {
                parsed.push_back({0, true});
                while (*cursor == '?')
                    ++cursor;
                continue;
            }

            char* endPtr = nullptr;
            unsigned long byteVal = std::strtoul(cursor, &endPtr, 16);
            if (endPtr == cursor || byteVal > 0xFF) {
                parsed.clear();
                return parsed;
            }
            parsed.push_back({static_cast<uint8_t>(byteVal), false});
            cursor = endPtr;
        }
        return parsed;
    }

    inline bool IsReadableProtection(DWORD protect) {
        if (protect & (PAGE_GUARD | PAGE_NOACCESS))
            return false;
        const DWORD baseProtect = protect & 0xFF;
        return baseProtect == PAGE_READONLY ||
               baseProtect == PAGE_READWRITE ||
               baseProtect == PAGE_WRITECOPY ||
               baseProtect == PAGE_EXECUTE_READ ||
               baseProtect == PAGE_EXECUTE_READWRITE ||
               baseProtect == PAGE_EXECUTE_WRITECOPY;
    }

} // namespace MyMemDetail

/**
 * @brief Scans a loaded module in the current process for an IDA-style byte pattern.
 * @param moduleName Module name passed to GetModuleHandleA (e.g., "WHGame.dll").
 * @param pattern Space-separated hex bytes with '?' or '??' wildcards.
 * @return Virtual address of the first match, or 0 if not found.
 */
inline DWORD64 FindPatternIDA(const char* moduleName, const char* pattern) {
    if (!moduleName || !pattern)
        return 0;

    HMODULE moduleHandle = GetModuleHandleA(moduleName);
    if (!moduleHandle)
        return 0;

    const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleHandle);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;

    const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const uint8_t*>(moduleHandle) + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
        return 0;

    const std::vector<MyMemDetail::PatternByte> sig = MyMemDetail::ParseIdaPattern(pattern);
    if (sig.empty())
        return 0;

    const uintptr_t moduleStart = reinterpret_cast<uintptr_t>(moduleHandle);
    const uintptr_t moduleEnd = moduleStart + ntHeaders->OptionalHeader.SizeOfImage;
    const size_t sigLen = sig.size();

    uintptr_t regionAddr = moduleStart;
    MEMORY_BASIC_INFORMATION mbi{};
    while (regionAddr < moduleEnd && VirtualQuery(reinterpret_cast<LPCVOID>(regionAddr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        const uintptr_t scanEnd = (regionEnd < moduleEnd) ? regionEnd : moduleEnd;

        if (mbi.State == MEM_COMMIT && MyMemDetail::IsReadableProtection(mbi.Protect) && scanEnd > regionAddr && (scanEnd - regionAddr) >= sigLen) {
            const uint8_t* data = reinterpret_cast<const uint8_t*>(regionAddr);
            const size_t limit = (scanEnd - regionAddr) - sigLen;
            for (size_t i = 0; i <= limit; ++i) {
                bool matched = true;
                for (size_t j = 0; j < sigLen; ++j) {
                    if (!sig[j].wildcard && data[i + j] != sig[j].value) {
                        matched = false;
                        break;
                    }
                }
                if (matched)
                    return static_cast<DWORD64>(regionAddr + i);
            }
        }

        if (regionEnd <= regionAddr)
            break;
        regionAddr = regionEnd;
    }

    return 0;
}

/**
 * @brief Overwrites bytes at a target address, temporarily adjusting page protection.
 * @param address Target virtual address.
 * @param bytes Source byte buffer.
 * @param size Number of bytes to write.
 * @return true on success, false on invalid arguments or VirtualProtect failure.
 */
inline bool writeBytes(DWORD64 address, const BYTE* bytes, size_t size) {
    if (address == 0 || !bytes || size == 0)
        return false;

    DWORD oldProtect = 0;
    LPVOID target = reinterpret_cast<LPVOID>(address);
    if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    std::memcpy(target, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), target, size);
    VirtualProtect(target, size, oldProtect, &oldProtect);
    return true;
}

/**
 * @brief Installs an x64 detour preserving RAX on the stack for naked hook prologues.
 *
 * Emits: push rax (1B) + mov rax, ourFunct (10B) + jmp rax (2B) + NOP padding.
 * Hook targets must begin with `pop rax` to restore the original RAX register.
 *
 * @param toHook Address to patch.
 * @param ourFunct Destination hook handler.
 * @param len Total stolen instruction length (must be >= 13).
 * @return true on success, false if parameters are invalid or memory protection fails.
 */
inline bool Hook(void* toHook, void* ourFunct, int len) {
    if (!toHook || !ourFunct || len < 13)
        return false;

    DWORD oldProtect = 0;
    if (!VirtualProtect(toHook, static_cast<SIZE_T>(len), PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    uint8_t* dst = static_cast<uint8_t*>(toHook);
    const uint64_t targetAddr = reinterpret_cast<uint64_t>(ourFunct);

    dst[0] = 0x50; // push rax
    dst[1] = 0x48; // mov rax, imm64
    dst[2] = 0xB8;
    std::memcpy(dst + 3, &targetAddr, sizeof(targetAddr));
    dst[11] = 0xFF; // jmp rax
    dst[12] = 0xE0;

    for (int i = 13; i < len; ++i)
        dst[i] = 0x90; // nop

    FlushInstructionCache(GetCurrentProcess(), toHook, static_cast<SIZE_T>(len));
    VirtualProtect(toHook, static_cast<SIZE_T>(len), oldProtect, &oldProtect);
    return true;
}
