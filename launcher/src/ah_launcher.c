// arenahack launcher stub — KFPL wrapper (ship variant).
//
// Ship shape: two files inside dist/
//   dist/App.exe          — this binary
//   dist/ah_bundle.kfpl   — encrypted ah_overlay.exe (AES-256-GCM, AHKF magic)
//   dist/assets/          — operator.png (character sprite)
//
// Key resolution:
//   1. env AH_KFPL_KEY_B64  — base64(32 bytes) — backend can inject on Play
//   2. env AH_KFPL_KEY_HEX  — hex 64 chars
//   3. baked KFPL_KEY[32]   — replaced by bake.py at each release build
//
// bundle.kfpl path is resolved relative to App.exe's own dir so the site's
// ZIP works when extracted anywhere.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <shlwapi.h>
#include "../deps/vmprotect/inc/VMProtectSDK.h"

#pragma comment(lib, "Bcrypt.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "VMProtectSDK64.lib")

// Diagnostic: file log to %TEMP%\ah_launcher.log. Silent otherwise.
// Popup errors with -DAH_LAUNCHER_VERBOSE for local diagnostic.
#ifdef AH_LAUNCHER_VERBOSE
#  define AH_MB(text) MessageBoxW(NULL, (text), L"arenahack", MB_ICONERROR)
#  define AH_MB_F(buf, fmt, ...) do { swprintf((buf), sizeof(buf)/sizeof((buf)[0]), (fmt), __VA_ARGS__); MessageBoxW(NULL, (buf), L"arenahack", MB_ICONERROR); } while (0)
#else
#  define AH_MB(text) ((void)0)
#  define AH_MB_F(buf, fmt, ...) ((void)(buf))
#endif

void ah_log(const char* fmt, ...)
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wcscat_s(tmp, MAX_PATH, L"ah_launcher.log");
    FILE* f = _wfopen(tmp, L"a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    vfprintf(f, fmt, ap);
    fprintf(f, "\n");
    va_end(ap);
    fclose(f);
}

// Baked fallback key — overwritten by launcher/bake.py before each release.
static const uint8_t KFPL_KEY[32] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};

static const uint8_t KFPL_MAGIC[4] = { 'A','H','K','F' };   // arenahack blob magic — distinct from KoenFlow's "KFPL" so KoenFlow doesn't try to decrypt bundle.kfpl
#define NONCE_LEN 12
#define TAG_LEN   16
#define KFPL_HEADER_LEN (4 + 4 + NONCE_LEN + 8)

// ─── base64 decode ─────────────────────────────────────────────────────────
static int b64_decode(const char* in, uint8_t* out, int out_cap)
{
    static int8_t T[256]; static int init = 0;
    if (!init) {
        for (int i = 0; i < 256; i++) T[i] = -1;
        const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) T[(uint8_t)A[i]] = (int8_t)i;
        init = 1;
    }
    uint32_t buf = 0; int bits = 0, out_n = 0;
    for (const char* p = in; *p; p++) {
        if (*p == '=' || *p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') continue;
        int8_t v = T[(uint8_t)*p];
        if (v < 0) return -1;
        buf = (buf << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out_n >= out_cap) return -1;
            out[out_n++] = (uint8_t)((buf >> bits) & 0xFF);
        }
    }
    return out_n;
}

static int hex_decode(const wchar_t* in, uint8_t* out, int out_cap)
{
    int n = 0;
    for (const wchar_t* p = in; p[0] && p[1]; p += 2) {
        if (n >= out_cap) return -1;
        int hi = (p[0] >= L'0' && p[0] <= L'9') ? p[0]-L'0' :
                 (p[0] >= L'a' && p[0] <= L'f') ? p[0]-L'a'+10 :
                 (p[0] >= L'A' && p[0] <= L'F') ? p[0]-L'A'+10 : -1;
        int lo = (p[1] >= L'0' && p[1] <= L'9') ? p[1]-L'0' :
                 (p[1] >= L'a' && p[1] <= L'f') ? p[1]-L'a'+10 :
                 (p[1] >= L'A' && p[1] <= L'F') ? p[1]-L'A'+10 : -1;
        if (hi < 0 || lo < 0) return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

// ─── AES-256-GCM via CNG ───────────────────────────────────────────────────
static NTSTATUS aes_gcm_decrypt(const uint8_t* key32,
                                const uint8_t* nonce, DWORD nonce_len,
                                const uint8_t* ct, DWORD ct_len,
                                const uint8_t* tag, DWORD tag_len,
                                const uint8_t* aad, DWORD aad_len,
                                uint8_t* pt_out, DWORD* pt_len_out)
{
    VMProtectBeginUltra("aes_gcm_decrypt");
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_KEY_HANDLE hKey = NULL;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, NULL, 0);
    if (st < 0) { VMProtectEnd(); return st; }
    st = BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                           (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                           sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    if (st < 0) goto out;
    st = BCryptGenerateSymmetricKey(hAlg, &hKey, NULL, 0, (PUCHAR)key32, 32, 0);
    if (st < 0) goto out;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = (PUCHAR)nonce; info.cbNonce = nonce_len;
    info.pbAuthData = (PUCHAR)aad; info.cbAuthData = aad_len;
    info.pbTag = (PUCHAR)tag; info.cbTag = tag_len;

    ULONG produced = 0;
    st = BCryptDecrypt(hKey, (PUCHAR)ct, ct_len, &info,
                       NULL, 0, pt_out, ct_len, &produced, 0);
    if (st >= 0 && pt_len_out) *pt_len_out = produced;
out:
    if (hKey) BCryptDestroyKey(hKey);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    VMProtectEnd();
    return st;
}

// Context-file key cache. Populated by extract_key_from_launch_context()
// early in wmain BEFORE cage move, so it survives respawn without needing
// the file to be re-read after path relocation.
static uint8_t g_context_key[32];
static int     g_context_key_valid = 0;

// Parse argv for --koenflow-launch-context (or the -keonflow- typo variant)
// and pull "kfplKey":"<b64>" out of the JSON context. This is the only path
// that survives Verb=runas — the C# side (BackendProductLaunchService.cs
// WriteLaunchContextFile) drops the per-release key into that JSON precisely
// because Windows ShellExecute strips process env vars on elevation.
//
// Plain-JSON only. When hasLoaderKey=true the C# side wraps the JSON with
// DPAPI+"KFPC" magic — arenahack doesn't ship a loaderKey so we never hit
// that branch. If we ever do, add CryptUnprotectData here.
static void extract_key_from_launch_context(int argc, wchar_t** argv)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (_wcsicmp(argv[i], L"--koenflow-launch-context") != 0 &&
            _wcsicmp(argv[i], L"--keonflow-launch-context") != 0) continue;
        const wchar_t* path = argv[i + 1];

        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 0x10000) {
            CloseHandle(h); return;
        }
        char* buf = (char*)VirtualAlloc(NULL, (SIZE_T)sz.QuadPart + 1,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!buf) { CloseHandle(h); return; }
        DWORD got = 0;
        BOOL rd = ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL);
        CloseHandle(h);
        if (!rd || got == 0) { VirtualFree(buf, 0, MEM_RELEASE); return; }
        buf[got] = 0;

        // KFPC = DPAPI-wrapped variant (loaderKey products). Not supported
        // here; arenahack always takes the plain-JSON path.
        if (got >= 4 && memcmp(buf, "KFPC", 4) == 0) {
            ah_log("context: KFPC-wrapped context — DPAPI unwrap not supported (loaderKey product?)");
            VirtualFree(buf, 0, MEM_RELEASE); return;
        }

        // Locate "kfplKey" : "<value>"  — minimal string walk, no full JSON parse.
        const char* needle = "\"kfplKey\"";
        char* p = strstr(buf, needle);
        if (!p) { ah_log("context: kfplKey field not present"); VirtualFree(buf, 0, MEM_RELEASE); return; }
        p += 9;   // past "kfplKey"
        while (*p == ' ' || *p == '\t') p++;
        if (*p != ':') { VirtualFree(buf, 0, MEM_RELEASE); return; }
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 'n' && memcmp(p, "null", 4) == 0) {
            ah_log("context: kfplKey=null (backend returned no per-release key)");
            VirtualFree(buf, 0, MEM_RELEASE); return;
        }
        if (*p != '"') { VirtualFree(buf, 0, MEM_RELEASE); return; }
        p++;

        char b64buf[80];
        int b64_len = 0;
        while (*p && *p != '"' && b64_len < (int)sizeof(b64buf) - 1) b64buf[b64_len++] = *p++;
        b64buf[b64_len] = 0;

        uint8_t out[32];
        if (b64_decode(b64buf, out, 32) == 32) {
            memcpy(g_context_key, out, 32);
            g_context_key_valid = 1;
            ah_log("context: kfplKey extracted from JSON (b64 %d chars)", b64_len);
        } else {
            ah_log("context: kfplKey b64 decode failed (%d chars)", b64_len);
        }
        SecureZeroMemory(buf, got);
        VirtualFree(buf, 0, MEM_RELEASE);
        return;
    }
}

// Env-var conventions: try multiple names so admin backend can inject key
// under whatever prefix it uses. All-zero baked KFPL_KEY[32] in release
// builds forces env to be present — no in-binary decrypt possible.
static void resolve_key(uint8_t key32[32])
{
    VMProtectBeginUltra("resolve_key");
    uint8_t out[32]; int ok = 0;

    // Priority 0: context-file key. This is the ShellExecute-runas-safe path
    // — env vars are stripped by UAC, but arg-passed --koenflow-launch-context
    // survives, and the C# WriteLaunchContextFile now writes kfplKey there.
    if (g_context_key_valid) {
        memcpy(out, g_context_key, 32);
        ok = 1;
    }

    static const char* B64_NAMES[] = {
        "AH_KFPL_KEY_B64",         // arenahack native
        "KFPL_KEY_B64",            // KoenFlow generic
        "DH_KFPL_KEY_B64",         // DeltaHack-style
        "KOENFLOW_KFPL_KEY_B64",   // KoenFlow prefixed
    };
    for (int i = 0; !ok && i < 4; i++) {
        char env_b64[128];
        DWORD n = GetEnvironmentVariableA(B64_NAMES[i], env_b64, sizeof(env_b64));
        if (n > 0 && n < sizeof(env_b64)) {
            env_b64[n] = 0;
            if (b64_decode(env_b64, out, 32) == 32) ok = 1;
        }
    }
    if (!ok) {
        static const wchar_t* HEX_NAMES[] = {
            L"AH_KFPL_KEY_HEX",
            L"KFPL_KEY_HEX",
            L"DH_KFPL_KEY_HEX",
            L"KOENFLOW_KFPL_KEY_HEX",
        };
        for (int i = 0; !ok && i < 4; i++) {
            wchar_t env_hex[128];
            DWORD nh = GetEnvironmentVariableW(HEX_NAMES[i], env_hex, 128);
            if (nh > 0 && nh < 128) {
                env_hex[nh] = 0;
                if (hex_decode(env_hex, out, 32) == 32) ok = 1;
            }
        }
    }
    if (!ok) memcpy(out, KFPL_KEY, 32);
    memcpy(key32, out, 32);
    VMProtectEnd();
}

// Trailer format appended past the PE end by scripts/make_release_zip.py:
//   [ bundle bytes (N) ][ u64 LE bundle_size ][ 4-byte magic "AHBE" ]
// Trailer total = 12 bytes.
// Windows loader ignores bytes past the last PE section, so the file
// still runs as a plain MZ .exe — no sidecar bundle.kfpl needed on disk.
#define AHBE_MAGIC      "AHBE"
#define AHBE_TRAILER    12       // 8 bytes size + 4 bytes magic

// Reads the KFPL bundle blob appended to the tail of our own on-disk PE.
// Returns VirtualAlloc'd buffer (caller frees with VirtualFree) or NULL.
static uint8_t* read_bundle_from_self(DWORD* out_size)
{
    if (out_size) *out_size = 0;

    wchar_t self_path[MAX_PATH];
    DWORD np = GetModuleFileNameW(NULL, self_path, MAX_PATH);
    if (!np || np >= MAX_PATH) return NULL;

    HANDLE h = CreateFileW(self_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= AHBE_TRAILER || sz.QuadPart > 0x40000000LL) {
        CloseHandle(h); return NULL;
    }

    // Read trailer.
    LARGE_INTEGER off; off.QuadPart = sz.QuadPart - AHBE_TRAILER;
    if (!SetFilePointerEx(h, off, NULL, FILE_BEGIN)) { CloseHandle(h); return NULL; }
    uint8_t trailer[AHBE_TRAILER];
    DWORD rd = 0;
    if (!ReadFile(h, trailer, AHBE_TRAILER, &rd, NULL) || rd != AHBE_TRAILER) {
        CloseHandle(h); return NULL;
    }
    if (memcmp(trailer + 8, AHBE_MAGIC, 4) != 0) {
        // No embed found — legacy sidecar mode fallback.
        CloseHandle(h); return NULL;
    }
    uint64_t bundle_size = 0;
    memcpy(&bundle_size, trailer, 8);
    if (bundle_size == 0 || bundle_size > 0x20000000ULL ||
        bundle_size + AHBE_TRAILER > (uint64_t)sz.QuadPart) {
        CloseHandle(h); return NULL;
    }

    // Seek to bundle start and read it out.
    off.QuadPart = sz.QuadPart - AHBE_TRAILER - (LONGLONG)bundle_size;
    if (!SetFilePointerEx(h, off, NULL, FILE_BEGIN)) { CloseHandle(h); return NULL; }

    uint8_t* buf = (uint8_t*)VirtualAlloc(NULL, (SIZE_T)bundle_size,
                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) { CloseHandle(h); return NULL; }

    DWORD remaining = (DWORD)bundle_size, got = 0;
    uint8_t* cur = buf;
    while (remaining) {
        DWORD chunk = 0;
        if (!ReadFile(h, cur, remaining, &chunk, NULL) || chunk == 0) {
            VirtualFree(buf, 0, MEM_RELEASE); CloseHandle(h); return NULL;
        }
        cur += chunk; got += chunk; remaining -= chunk;
    }
    CloseHandle(h);
    if (out_size) *out_size = got;
    return buf;
}

// Legacy sidecar path — retained for dev workflow where bundle.kfpl still
// lives next to build\WinRuntimeHost.exe before append.
static int bundle_path(wchar_t* out, size_t out_cch)
{
    DWORD n = GetModuleFileNameW(NULL, out, (DWORD)out_cch);
    if (!n || n >= out_cch) return 0;
    PathRemoveFileSpecW(out);
    if (!PathAppendW(out, L"bundle.kfpl")) return 0;
    return 1;
}

static void make_random_name(wchar_t* out, size_t out_len)
{
    uint8_t buf[6];
    BCryptGenRandom(NULL, buf, sizeof(buf), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    swprintf(out, out_len, L"%02X%02X%02X%02X%02X%02X.exe",
             buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
}

static uint8_t* read_file_all(const wchar_t* path, DWORD* out_size)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 0x40000000LL) {
        CloseHandle(h); return NULL;
    }
    DWORD size = (DWORD)sz.QuadPart;
    uint8_t* buf = (uint8_t*)VirtualAlloc(NULL, size,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) { CloseHandle(h); return NULL; }
    DWORD off = 0;
    while (off < size) {
        DWORD got = 0;
        if (!ReadFile(h, buf + off, size - off, &got, NULL) || got == 0) {
            VirtualFree(buf, 0, MEM_RELEASE); CloseHandle(h); return NULL;
        }
        off += got;
    }
    CloseHandle(h);
    *out_size = size;
    return buf;
}

// ─── Runtime cage + janitor (ported from DeltaHack dh_launcher.c) ─────────
// KoenFlow launcher extracts our zip to a FIXED path:
//   %LOCALAPPDATA%\KoenFlowLauncher\products\arena-breakout-esp\current\
// That path is a stable forensic breadcrumb — any post-play scan sees our
// product name in the folder tree, our WinRuntimeHost.exe, our bundle.kfpl.
// Cage moves everything to a RANDOM GUID folder under a system-looking dir
// (Microsoft\Windows\SystemCache\{GUID}\), then a batch janitor rmdir's
// BOTH the cage + the KoenFlow product cache once all WinRuntimeHost.exe
// processes exit. Nothing persists past a clean run.

static void make_cage_guid(wchar_t* out, size_t out_len)
{
    uint8_t b[16];
    BCryptGenRandom(NULL, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    swprintf(out, out_len,
        L"{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7],
        b[8],b[9], b[10],b[11],b[12],b[13],b[14],b[15]);
}

static int ensure_dir_one(const wchar_t* path)
{
    if (CreateDirectoryW(path, NULL)) return 1;
    return GetLastError() == ERROR_ALREADY_EXISTS ? 1 : 0;
}

static int ensure_dir_tree(const wchar_t* full)
{
    wchar_t buf[MAX_PATH];
    wcscpy_s(buf, MAX_PATH, full);
    for (wchar_t* p = buf; *p; p++) {
        if (*p == L'\\' && p > buf + 3) {
            *p = 0;
            ensure_dir_one(buf);
            *p = L'\\';
        }
    }
    return ensure_dir_one(buf);
}

static void copy_subdir(const wchar_t* src_dir, const wchar_t* dst_dir, const wchar_t* sub)
{
    wchar_t src[MAX_PATH], dst[MAX_PATH];
    swprintf(src, MAX_PATH, L"%s\\%s", src_dir, sub);
    swprintf(dst, MAX_PATH, L"%s\\%s", dst_dir, sub);
    ensure_dir_one(dst);
    wchar_t search[MAX_PATH];
    swprintf(search, MAX_PATH, L"%s\\*", src);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wchar_t s2[MAX_PATH], d2[MAX_PATH];
        swprintf(s2, MAX_PATH, L"%s\\%s", src, fd.cFileName);
        swprintf(d2, MAX_PATH, L"%s\\%s", dst, fd.cFileName);
        CopyFileW(s2, d2, FALSE);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int move_to_cage_and_respawn(const wchar_t* self_path,
                                    const wchar_t* self_dir,
                                    int argc, wchar_t** argv)
{
    wchar_t local[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return 0;

    wchar_t guid[64]; make_cage_guid(guid, 64);
    wchar_t cage[MAX_PATH];
    swprintf(cage, MAX_PATH,
             L"%s\\Microsoft\\Windows\\SystemCache\\%s", local, guid);
    if (!ensure_dir_tree(cage)) return 0;

    // Basename identical — janitor's tasklist filter matches WinRuntimeHost.exe.
    wchar_t dst_exe[MAX_PATH];
    swprintf(dst_exe, MAX_PATH, L"%s\\WinRuntimeHost.exe", cage);
    if (!CopyFileW(self_path, dst_exe, FALSE)) return 0;

    wchar_t src_b[MAX_PATH], dst_b[MAX_PATH];
    swprintf(src_b, MAX_PATH, L"%s\\bundle.kfpl", self_dir);
    swprintf(dst_b, MAX_PATH, L"%s\\bundle.kfpl", cage);
    if (!CopyFileW(src_b, dst_b, FALSE)) return 0;

    // VMProtectSDK64.dll must ship alongside the wrapped launcher.
    wchar_t src_v[MAX_PATH], dst_v[MAX_PATH];
    swprintf(src_v, MAX_PATH, L"%s\\VMProtectSDK64.dll", self_dir);
    swprintf(dst_v, MAX_PATH, L"%s\\VMProtectSDK64.dll", cage);
    CopyFileW(src_v, dst_v, FALSE);

    // Copy db/ (kdu payloads) + assets/ (operator.png).
    copy_subdir(self_dir, cage, L"db");
    copy_subdir(self_dir, cage, L"assets");

    // Preserve args verbatim so KoenFlow platform args survive respawn.
    wchar_t cmd[8192];
    swprintf(cmd, 8192, L"\"%s\"", dst_exe);
    for (int i = 1; i < argc; i++) {
        wcscat_s(cmd, 8192, L" \"");
        wcscat_s(cmd, 8192, argv[i]);
        wcscat_s(cmd, 8192, L"\"");
    }

    SetEnvironmentVariableW(L"DH_CAGE", L"1");
    SetEnvironmentVariableW(L"DH_INSTALL_DIR", cage);
    SetEnvironmentVariableW(L"DH_LAUNCHER_CACHE", self_dir);

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessW(dst_exe, cmd, NULL, NULL, FALSE,
                             CREATE_NO_WINDOW | DETACHED_PROCESS,
                             NULL, cage, &si, &pi);
    if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    return ok ? 1 : 0;
}

static void spawn_janitor(const wchar_t* cage, const wchar_t* launcher_cache)
{
    wchar_t tmp_dir[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp_dir);
    uint8_t rn[4];
    BCryptGenRandom(NULL, rn, sizeof(rn), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    wchar_t bat[MAX_PATH];
    swprintf(bat, MAX_PATH, L"%s%02X%02X%02X%02X.bat",
             tmp_dir, rn[0], rn[1], rn[2], rn[3]);

    HANDLE h = CreateFileW(bat, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    char lc_line[MAX_PATH * 2] = "";
    if (launcher_cache && launcher_cache[0]) {
        _snprintf(lc_line, sizeof(lc_line),
                  "rmdir /s /q \"%ls\" 2>nul\r\n", launcher_cache);
    }

    char content[4096];
    int n = _snprintf(content, sizeof(content),
        "@echo off\r\n"
        "timeout /t 10 /nobreak >nul 2>&1\r\n"
        ":loop\r\n"
        "tasklist /fi \"imagename eq WinRuntimeHost.exe\" 2>nul | find /i \"WinRuntimeHost.exe\" >nul\r\n"
        "if not errorlevel 1 (\r\n"
        "  timeout /t 3 /nobreak >nul 2>&1\r\n"
        "  goto loop\r\n"
        ")\r\n"
        "timeout /t 5 /nobreak >nul 2>&1\r\n"
        "rmdir /s /q \"%ls\" 2>nul\r\n"
        "%s"
        "del \"%%~f0\" 2>nul\r\n",
        cage, lc_line);
    if (n <= 0) { CloseHandle(h); DeleteFileW(bat); return; }
    DWORD w = 0; WriteFile(h, content, (DWORD)n, &w, NULL); CloseHandle(h);

    wchar_t cmd[MAX_PATH + 32];
    swprintf(cmd, MAX_PATH + 32, L"cmd.exe /c \"%s\"", bat);
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                       NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
}

int wmain(int argc, wchar_t** argv)
{
    ah_log("--- launcher start argc=%d", argc);

    // Extract per-release KFPL key from the C#-written launch context BEFORE
    // any resolve_key call — Verb=runas strips env vars, this arg-passed
    // context file is how per-release keys reach the elevated stub.
    extract_key_from_launch_context(argc, argv);

    wchar_t self_path[MAX_PATH];
    GetModuleFileNameW(NULL, self_path, MAX_PATH);
    wchar_t self_dir_early[MAX_PATH];
    wcscpy_s(self_dir_early, MAX_PATH, self_path);
    PathRemoveFileSpecW(self_dir_early);
    ah_log("self_dir=%ls", self_dir_early);

    // ── First hop: move to cage + respawn ──────────────────────────────
    // Skip if we're already in the cage (DH_CAGE=1 set by move_to_cage).
    wchar_t cage_flag[8];
    DWORD in_cage = GetEnvironmentVariableW(L"DH_CAGE", cage_flag, 8);
    if (in_cage == 0) {
        if (move_to_cage_and_respawn(self_path, self_dir_early, argc, argv)) {
            ah_log("caged + respawned; original exiting");
            return 0;   // caged instance takes over
        }
        // Cage move failed — fall through, run in-place, no janitor.
        ah_log("cage move failed, running in-place");
    } else {
        ah_log("running inside cage");
    }

    // Provider chain finds db/*.bin under DH_INSTALL_DIR (set by cage-move
    // if we caged, or set here fallback).
    SetEnvironmentVariableW(L"DH_INSTALL_DIR", self_dir_early);

    // Bundle acquisition. Primary path: trailer-embedded blob past our own
    // PE end (single-file distribution — user only ever sees one .exe). If
    // no AHBE trailer is present (dev / local build), fall back to sidecar
    // bundle.kfpl next to us.
    DWORD blob_size = 0;
    uint8_t* blob = read_bundle_from_self(&blob_size);
    if (blob) {
        ah_log("bundle: self-trailer size=%lu", blob_size);
    } else {
        wchar_t bpath[MAX_PATH];
        if (!bundle_path(bpath, MAX_PATH)) { ah_log("bundle_path fail"); AH_MB(L"bundle path"); return 2; }
        ah_log("bundle: sidecar path=%ls", bpath);
        blob = read_file_all(bpath, &blob_size);
        if (!blob) { ah_log("bundle read fail"); AH_MB(L"bundle read"); return 2; }
        ah_log("bundle: sidecar size=%lu", blob_size);
    }
    if (blob_size < KFPL_HEADER_LEN + TAG_LEN) {
        ah_log("bundle truncated"); VirtualFree(blob, 0, MEM_RELEASE); AH_MB(L"bundle truncated"); return 2;
    }

    // Parse KFPL header.
    if (memcmp(blob, KFPL_MAGIC, 4) != 0) {
        ah_log("magic mismatch: %02X %02X %02X %02X", blob[0],blob[1],blob[2],blob[3]);
        VirtualFree(blob, 0, MEM_RELEASE); AH_MB(L"magic"); return 2;
    }
    uint32_t version; memcpy(&version, blob + 4, 4);
    if (version != 1) {
        VirtualFree(blob, 0, MEM_RELEASE); AH_MB(L"version"); return 2;
    }
    const uint8_t* nonce = blob + 8;
    uint64_t ct_len; memcpy(&ct_len, blob + 20, 8);
    if (KFPL_HEADER_LEN + ct_len + TAG_LEN != blob_size) {
        VirtualFree(blob, 0, MEM_RELEASE); AH_MB(L"size"); return 2;
    }
    const uint8_t* ct  = blob + KFPL_HEADER_LEN;
    const uint8_t* tag = ct + ct_len;

    // Decrypt.
    uint8_t key32[32]; resolve_key(key32);
    ah_log("key32 first bytes: %02X %02X %02X %02X ...",
           key32[0], key32[1], key32[2], key32[3]);
    uint8_t* pt = (uint8_t*)VirtualAlloc(NULL, (SIZE_T)ct_len,
                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pt) { VirtualFree(blob, 0, MEM_RELEASE); AH_MB(L"alloc"); return 99; }

    DWORD pt_len = 0;
    NTSTATUS st = aes_gcm_decrypt(key32, nonce, NONCE_LEN,
                                  ct, (DWORD)ct_len, tag, TAG_LEN,
                                  KFPL_MAGIC, sizeof(KFPL_MAGIC),
                                  pt, &pt_len);
    SecureZeroMemory(key32, sizeof(key32));
    VirtualFree(blob, 0, MEM_RELEASE);
    if (st < 0) {
        ah_log("aes_gcm_decrypt fail NTSTATUS=0x%08lX", (unsigned long)st);
        SecureZeroMemory(pt, (SIZE_T)ct_len); VirtualFree(pt, 0, MEM_RELEASE);
        wchar_t msg[64]; (void)msg;
        AH_MB_F(msg, L"decrypt %08lX", (unsigned long)st);
        return 2;
    }
    ah_log("decrypt OK pt_len=%lu", pt_len);

    // Materialize temp exe.
    wchar_t tmp_dir[MAX_PATH]; GetTempPathW(MAX_PATH, tmp_dir);
    wchar_t rand_name[64]; make_random_name(rand_name, 64);
    wchar_t tmp_path[MAX_PATH];
    swprintf(tmp_path, MAX_PATH, L"%s%s", tmp_dir, rand_name);

    ah_log("tmp path=%ls", tmp_path);
    HANDLE hFile = CreateFileW(tmp_path, GENERIC_WRITE, FILE_SHARE_READ,
                               NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        ah_log("tmp create fail gle=%lu", GetLastError());
        SecureZeroMemory(pt, (SIZE_T)ct_len); VirtualFree(pt, 0, MEM_RELEASE);
        AH_MB(L"tmp create"); return 99;
    }
    DWORD written = 0;
    WriteFile(hFile, pt, pt_len, &written, NULL);
    CloseHandle(hFile);
    ah_log("tmp write %lu of %lu", written, pt_len);
    SecureZeroMemory(pt, (SIZE_T)ct_len); VirtualFree(pt, 0, MEM_RELEASE);
    if (written != pt_len) { DeleteFileW(tmp_path); AH_MB(L"tmp write"); return 99; }

    // Spawn payload. Working dir = self dir so assets/operator.png resolves.
    wchar_t self_dir[MAX_PATH];
    GetModuleFileNameW(NULL, self_dir, MAX_PATH);
    PathRemoveFileSpecW(self_dir);
    ah_log("cwd for child=%ls", self_dir);

    wchar_t cmdline[8192];
    swprintf(cmdline, 8192, L"\"%s\"", tmp_path);
    for (int i = 1; i < argc; i++) {
        wcscat_s(cmdline, 8192, L" \"");
        wcscat_s(cmdline, 8192, argv[i]);
        wcscat_s(cmdline, 8192, L"\"");
    }
    ah_log("cmdline=%ls", cmdline);

    // DETACHED_PROCESS + CREATE_NO_WINDOW — matches DeltaHack ship setup.
    // Overlay is CONSOLE-subsystem but doesn't use stdio; CRT init handles
    // detached mode fine.
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessW(tmp_path, cmdline, NULL, NULL, FALSE,
                             DETACHED_PROCESS | CREATE_NO_WINDOW,
                             NULL, self_dir, &si, &pi);
    if (!ok) {
        DWORD e = GetLastError(); ah_log("CreateProcess fail gle=%lu", e);
        DeleteFileW(tmp_path);
        wchar_t msg[128]; (void)msg;
        AH_MB_F(msg, L"CreateProcess %lu", e);
        return 99;
    }
    ah_log("child spawned pid=%lu", pi.dwProcessId);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0; GetExitCodeProcess(pi.hProcess, &exit_code);
    ah_log("child exit_code=%lu (0x%08lX)", exit_code, exit_code);

    // v1.0.14-debug: auto-upload log tails to koenflow.com telemetry so we can
    // see what killed the overlay without asking clients to hand over files.
    // Silent, best-effort, 5s timeouts — never blocks launcher exit.
    extern void crash_upload_after_child(DWORD child_pid, DWORD exit_code,
                                         const char* version);
    crash_upload_after_child(pi.dwProcessId, exit_code, "1.0.14-debug");

    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    DeleteFileW(tmp_path);

    // Janitor — wipes cage dir + KoenFlow launcher-cache dir once all
    // WinRuntimeHost.exe processes exit. Only when we're actually in the
    // cage (DH_LAUNCHER_CACHE set); in-place run has nothing to wipe.
    if (in_cage != 0) {
        wchar_t lc[MAX_PATH];
        DWORD lc_n = GetEnvironmentVariableW(L"DH_LAUNCHER_CACHE", lc, MAX_PATH);
        spawn_janitor(self_dir_early, lc_n > 0 ? lc : NULL);
        ah_log("janitor spawned; cage=%ls launcher_cache=%ls",
               self_dir_early, lc_n > 0 ? lc : L"(none)");
    }
    return (int)exit_code;
}
