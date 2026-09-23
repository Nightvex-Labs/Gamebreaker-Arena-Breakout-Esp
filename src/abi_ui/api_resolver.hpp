// api_resolver.hpp — hash-based export lookup via PEB.Ldr walk.
//
// Purpose: eliminate GetProcAddress/GetModuleHandleW from IAT + kill all
// API name strings from .rdata. Static YARA scanners hunt for clusters
// like ("NtLoadDriver", "NtUnloadDriver", "RtlAdjustPrivilege") sitting
// adjacent — resolving via 32-bit compile-time hash + runtime PEB walk
// leaves no string trace and removes the kernel32 GetProcAddress import.
//
// Usage:
//   auto NtLoad = api::resolve<NTSTATUS(NTAPI*)(PUNICODE_STRING)>(
//       api::hash("ntdll.dll"), api::hash("NtLoadDriver"));
//   if (NtLoad) NtLoad(&us);
//
// Hash: djb2 32-bit — collision-free for the small set of exports we call.
// Both module basename (lowercase) and export name are hashed narrow ASCII.
// Module comparison uses lowercase since PEB.Ldr BaseDllName is UNICODE
// in mixed case ("ntdll.dll" vs "NTDLL.dll") — we lowercase on walk.

#pragma once
#include <windows.h>
#include <winternl.h>
#include <cstdint>
#include <cstddef>

namespace api {

// ─── compile-time djb2 hash ─────────────────────────────────────────────
constexpr uint32_t hash_narrow(const char* s) {
    uint32_t h = 5381;
    while (*s) {
        char c = *s++;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');  // lowercase
        h = ((h << 5) + h) + (uint8_t)c;
    }
    return h;
}

// Runtime version — used by find_export when iterating export names
// (name comes from loaded module's PE, not our .rdata).
constexpr uint32_t hash_rt(const char* s) { return hash_narrow(s); }

// v0.9.472: consteval FORCES compile-time evaluation. Any callsite that
// passes a string literal — the string is hashed at compile time and the
// literal is dropped from .rdata entirely. Attempting to call this with
// a runtime const char* is a compile error, guaranteeing no plaintext
// leak from careless callers.
template<size_t N>
consteval uint32_t hash(const char (&s)[N]) {
    uint32_t h = 5381;
    for (size_t i = 0; i < N - 1; ++i) {  // N-1 to skip terminator
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        h = ((h << 5) + h) + (uint8_t)c;
    }
    return h;
}

// Wide-to-lower-hash for PEB.Ldr BaseDllName matching.
inline uint32_t hash_wide_basename(const wchar_t* s, size_t wchars) {
    uint32_t h = 5381;
    for (size_t i = 0; i < wchars; ++i) {
        wchar_t c = s[i];
        if (c == 0) break;
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        // We hash the low-byte only — matches ASCII-range DLL names
        // (ntdll.dll, kernel32.dll, etc). No non-ASCII in Windows DLL names.
        h = ((h << 5) + h) + (uint8_t)c;
    }
    return h;
}

// ─── PEB.Ldr walk — find loaded module by basename hash ─────────────────
// Returns HMODULE (== DllBase) or nullptr if not loaded.
inline HMODULE find_module(uint32_t module_hash) {
    // GS:[0x60] on x64 points at PEB.
    PPEB peb = (PPEB)__readgsqword(0x60);
    if (!peb || !peb->Ldr) return nullptr;

    // PEB_LDR_DATA.InMemoryOrderModuleList is what winternl exposes.
    // Each entry sits at offset sizeof(LIST_ENTRY) into LDR_DATA_TABLE_ENTRY
    // (i.e., InLoadOrderLinks is one LIST_ENTRY before InMemoryOrderLinks).
    struct LDR_LIKE {
        LIST_ENTRY  InLoadOrderLinks;
        LIST_ENTRY  InMemoryOrderLinks;
        LIST_ENTRY  InInitializationOrderLinks;
        PVOID       DllBase;
        PVOID       EntryPoint;
        ULONG       SizeOfImage;
        UNICODE_STRING FullDllName;
        UNICODE_STRING BaseDllName;
    };

    LIST_ENTRY* head = &peb->Ldr->InMemoryOrderModuleList;
    LIST_ENTRY* cur  = head->Flink;
    // Guard against corrupted list — bound loop.
    for (int i = 0; i < 512 && cur && cur != head; ++i) {
        LDR_LIKE* e = (LDR_LIKE*)((uint8_t*)cur - sizeof(LIST_ENTRY));
        if (e->BaseDllName.Buffer && e->BaseDllName.Length) {
            uint32_t h = hash_wide_basename(e->BaseDllName.Buffer,
                                            e->BaseDllName.Length / sizeof(wchar_t));
            if (h == module_hash) return (HMODULE)e->DllBase;
        }
        cur = cur->Flink;
    }
    return nullptr;
}

// ─── Export table walk — find export by name hash ───────────────────────
inline void* find_export(HMODULE mod, uint32_t name_hash) {
    if (!mod) return nullptr;
    auto base = (uint8_t*)mod;
    auto dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    auto& exp_dir_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!exp_dir_rva.VirtualAddress || !exp_dir_rva.Size) return nullptr;

    auto ed = (IMAGE_EXPORT_DIRECTORY*)(base + exp_dir_rva.VirtualAddress);
    auto name_rvas = (uint32_t*)(base + ed->AddressOfNames);
    auto ord_table = (uint16_t*)(base + ed->AddressOfNameOrdinals);
    auto func_rvas = (uint32_t*)(base + ed->AddressOfFunctions);

    for (uint32_t i = 0; i < ed->NumberOfNames; ++i) {
        const char* name = (const char*)(base + name_rvas[i]);
        if (hash_rt(name) == name_hash) {
            uint16_t ord = ord_table[i];
            if (ord >= ed->NumberOfFunctions) return nullptr;
            uint32_t rva = func_rvas[ord];
            // Forwarders (RVA within export dir range) not handled — none
            // of the Nt*/Rtl* we resolve are forwarded on x64 Windows.
            return base + rva;
        }
    }
    return nullptr;
}

// ─── Typed wrapper — one-liner for callers ──────────────────────────────
template<typename Fn>
inline Fn resolve(uint32_t module_hash, uint32_t name_hash) {
    HMODULE m = find_module(module_hash);
    return (Fn)find_export(m, name_hash);
}

// Constants for the modules we care about (compile-time hashes).
constexpr uint32_t NTDLL     = hash("ntdll.dll");
constexpr uint32_t KERNEL32  = hash("kernel32.dll");
constexpr uint32_t KERNELBASE= hash("kernelbase.dll");
constexpr uint32_t PSAPI     = hash("psapi.dll");
constexpr uint32_t ADVAPI32  = hash("advapi32.dll");

}  // namespace api

// Convenience macro so callsites read close to old GetProcAddress:
//   API_RESOLVE(NtLoadDriver, NTSTATUS(NTAPI*)(PUNICODE_STRING))
// Expands to:
//   api::resolve<NTSTATUS(NTAPI*)(PUNICODE_STRING)>(api::NTDLL, api::hash("NtLoadDriver"))
// Compile-time hash of literal string — no plaintext name in .rdata.
#define API_NTDLL_RESOLVE(name, sig) \
    api::resolve<sig>(api::NTDLL, api::hash(#name))
#define API_KERNEL32_RESOLVE(name, sig) \
    api::resolve<sig>(api::KERNEL32, api::hash(#name))
#define API_PSAPI_RESOLVE(name, sig) \
    api::resolve<sig>(api::PSAPI, api::hash(#name))
