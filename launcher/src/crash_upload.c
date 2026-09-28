// crash_upload.c — auto-post client log tails to
//   https://koenflow.com/api/telemetry/crash
// after the overlay child exits. Silent on failure — never blocks the launcher.
//
// Payload: multipart/form-data, plain-text log bytes (no gzip in this build for
// simplicity; server accepts up to 8 MB total). Fields:
//   license_hash  64-hex SHA256(machine_guid + computer_name)  — server dir key
//   version       compile-time string
//   os_build      GetVersionEx CurrentBuild
//   exit_code     overlay child exit code, hex
//   parent_pid    launcher PID
//   child_pid     overlay PID
//   ah_reader     up to 500 KB tail
//   ah_procs      up to 500 KB tail
//   crash_marker  1 byte from %TEMP%\.abi_lc (optional)
//
// Auth: none beyond license_hash. Clients only push — server has no reply the
// launcher needs.

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

#define TELEM_HOST   L"koenflow.com"
#define TELEM_PATH   L"/api/telemetry/crash"
#define TAIL_BYTES   (500 * 1024)
// v1.0.28: head/tail split for reader.log. Init phase (gpu_probe, WARP
// marker, LATCH, SIG-SCAN, canary log) always lives at the very top; a
// pure 500 KB tail throws it away once the loop runs long enough to
// generate half a meg of ENEMY-DECRYPT lines. Keep the first HEAD_BYTES
// (init) and the last TAIL_TAIL_BYTES (crash context).
#define HEAD_BYTES        (100 * 1024)
#define TAIL_TAIL_BYTES   (400 * 1024)
#define BODY_MAX     (8  * 1024 * 1024)

extern void ah_log(const char* fmt, ...);   // provided by ah_launcher.c

// ─── helpers ────────────────────────────────────────────────────────────────

static void get_machine_guid(char out[64]) {
    HKEY hk = NULL;
    out[0] = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Cryptography", 0,
                      KEY_READ | KEY_WOW64_64KEY, &hk) == ERROR_SUCCESS) {
        wchar_t buf[64] = {0};
        DWORD cb = sizeof(buf);
        DWORD type = 0;
        if (RegQueryValueExW(hk, L"MachineGuid", NULL, &type,
                             (LPBYTE)buf, &cb) == ERROR_SUCCESS && type == REG_SZ) {
            WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, 63, NULL, NULL);
        }
        RegCloseKey(hk);
    }
}

static void compute_license_hash(char hex_out[65]) {
    char guid[64] = {0};
    get_machine_guid(guid);
    char host[64] = {0};
    DWORD hn = sizeof(host);
    GetComputerNameA(host, &hn);

    char blob[192];
    int n = _snprintf_s(blob, sizeof(blob), _TRUNCATE, "%s|%s", guid, host);
    if (n <= 0) { strcpy_s(hex_out, 65, "0"); return; }

    BCRYPT_ALG_HANDLE alg = NULL;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) {
        strcpy_s(hex_out, 65, "1"); return;
    }
    BYTE digest[32] = {0};
    NTSTATUS st = BCryptHash(alg, NULL, 0, (PUCHAR)blob, (ULONG)n, digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(alg, 0);
    if (st < 0) { strcpy_s(hex_out, 65, "2"); return; }
    static const char* HEX = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex_out[i*2]   = HEX[(digest[i] >> 4) & 0xF];
        hex_out[i*2+1] = HEX[ digest[i]       & 0xF];
    }
    hex_out[64] = 0;
}

static DWORD get_os_build(void) {
    typedef LONG (WINAPI *pfnRtlGetVersion)(POSVERSIONINFOEXW);
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    if (!nt) return 0;
    pfnRtlGetVersion p = (pfnRtlGetVersion)GetProcAddress(nt, "RtlGetVersion");
    if (!p) return 0;
    OSVERSIONINFOEXW v = {0}; v.dwOSVersionInfoSize = sizeof(v);
    if (p(&v) < 0) return 0;
    return v.dwBuildNumber;
}

// Read tail up to `max` bytes from a file. Returns malloc'd buffer + size,
// or (NULL, 0) if file missing.
static void read_tail(const wchar_t* path, uint8_t** out_buf, DWORD* out_size) {
    *out_buf = NULL; *out_size = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return; }
    DWORD want = (DWORD)((sz.QuadPart > (LONGLONG)TAIL_BYTES) ? TAIL_BYTES : sz.QuadPart);
    if (want == 0) { CloseHandle(h); return; }
    LARGE_INTEGER off; off.QuadPart = sz.QuadPart - (LONGLONG)want;
    SetFilePointerEx(h, off, NULL, FILE_BEGIN);
    uint8_t* buf = (uint8_t*)HeapAlloc(GetProcessHeap(), 0, want);
    if (!buf) { CloseHandle(h); return; }
    DWORD got = 0;
    if (!ReadFile(h, buf, want, &got, NULL) || got == 0) {
        HeapFree(GetProcessHeap(), 0, buf); CloseHandle(h); return;
    }
    CloseHandle(h);
    *out_buf  = buf;
    *out_size = got;
}

// v1.0.28: head + tail split reader. For big logs, we keep the first
// `head_max` bytes (init phase — WARP marker probe, gpu_probe, LATCH,
// SIG-SCAN, canary) AND the last `tail_max` bytes (recent activity before
// exit / crash context). If the file fits in head_max+tail_max we send it
// whole. Otherwise we insert a marker line between the two halves.
//
// Total output <= head_max + SEP_MAX + tail_max, always <= TAIL_BYTES.
static void read_head_tail(const wchar_t* path,
                           DWORD head_max, DWORD tail_max,
                           uint8_t** out_buf, DWORD* out_size) {
    *out_buf = NULL; *out_size = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) { CloseHandle(h); return; }

    if ((ULONGLONG)sz.QuadPart <= (ULONGLONG)(head_max + tail_max)) {
        DWORD want = (DWORD)sz.QuadPart;
        uint8_t* buf = (uint8_t*)HeapAlloc(GetProcessHeap(), 0, want);
        if (!buf) { CloseHandle(h); return; }
        DWORD got = 0;
        if (!ReadFile(h, buf, want, &got, NULL) || got == 0) {
            HeapFree(GetProcessHeap(), 0, buf); CloseHandle(h); return;
        }
        CloseHandle(h);
        *out_buf = buf; *out_size = got;
        return;
    }

    char sep[128];
    LONGLONG cut = sz.QuadPart - (LONGLONG)head_max - (LONGLONG)tail_max;
    int sep_n = _snprintf_s(sep, sizeof(sep), _TRUNCATE,
        "\r\n=== TRUNCATED %lld bytes (head=%lu, tail=%lu, total=%lld) ===\r\n",
        cut, (unsigned long)head_max, (unsigned long)tail_max, sz.QuadPart);
    if (sep_n <= 0) sep_n = 0;

    DWORD total = head_max + (DWORD)sep_n + tail_max;
    uint8_t* buf = (uint8_t*)HeapAlloc(GetProcessHeap(), 0, total);
    if (!buf) { CloseHandle(h); return; }

    LARGE_INTEGER off; off.QuadPart = 0;
    SetFilePointerEx(h, off, NULL, FILE_BEGIN);
    DWORD gh = 0;
    if (!ReadFile(h, buf, head_max, &gh, NULL) || gh == 0) {
        HeapFree(GetProcessHeap(), 0, buf); CloseHandle(h); return;
    }

    if (sep_n > 0) memcpy(buf + gh, sep, (size_t)sep_n);

    off.QuadPart = sz.QuadPart - (LONGLONG)tail_max;
    SetFilePointerEx(h, off, NULL, FILE_BEGIN);
    DWORD gt = 0;
    if (!ReadFile(h, buf + gh + sep_n, tail_max, &gt, NULL)) gt = 0;

    CloseHandle(h);
    *out_buf = buf;
    *out_size = gh + (DWORD)sep_n + gt;
}

// v1.0.24: whole-file reader — for binary artefacts like MiniDump where the
// format doesn't survive tail-truncation. Skips (returns 0/NULL) if the file
// is larger than `cap`, rather than corrupting it. cap=2 MB is enough for
// MiniDumpNormal + thread info + handle data on our overlay.
static void read_whole(const wchar_t* path, DWORD cap,
                       uint8_t** out_buf, DWORD* out_size) {
    *out_buf = NULL; *out_size = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (LONGLONG)cap) {
        CloseHandle(h); return;
    }
    DWORD want = (DWORD)sz.QuadPart;
    uint8_t* buf = (uint8_t*)HeapAlloc(GetProcessHeap(), 0, want);
    if (!buf) { CloseHandle(h); return; }
    DWORD got = 0;
    if (!ReadFile(h, buf, want, &got, NULL) || got != want) {
        HeapFree(GetProcessHeap(), 0, buf); CloseHandle(h); return;
    }
    CloseHandle(h);
    *out_buf  = buf;
    *out_size = got;
}

static void read_crash_marker(uint8_t out[2]) {
    out[0] = 0; out[1] = 0;
    wchar_t p[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, p)) return;
    wcscat_s(p, MAX_PATH, L".abi_lc");
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD got = 0;
    ReadFile(h, out, 1, &got, NULL);
    CloseHandle(h);
    out[1] = (got == 1) ? 1 : 0;
    // v1.0.29: consume the marker after read so the next launcher run
    // doesn't ship the same stale byte and mis-classify graceful exit.
    if (out[1]) {
        DeleteFileW(p);
    }
}

// Minimal multipart writer. Appends one field to a growing buffer.
static int mp_field(uint8_t** body, size_t* len, size_t* cap,
                    const char* boundary,
                    const char* name,
                    const char* filename_or_null,
                    const uint8_t* data, size_t data_len) {
    char header[512];
    int hn;
    if (filename_or_null) {
        hn = _snprintf_s(header, sizeof(header), _TRUNCATE,
            "--%s\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
            "Content-Type: application/octet-stream\r\n\r\n",
            boundary, name, filename_or_null);
    } else {
        hn = _snprintf_s(header, sizeof(header), _TRUNCATE,
            "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n",
            boundary, name);
    }
    if (hn <= 0) return 0;

    size_t need = *len + (size_t)hn + data_len + 2;   // +2 for trailing \r\n
    if (need > BODY_MAX) return 0;
    if (need > *cap) {
        size_t new_cap = *cap ? *cap * 2 : 4096;
        while (new_cap < need) new_cap *= 2;
        // v1.0.14-debug fix: HeapReAlloc requires non-NULL Memory param. On the
        // first call *body is NULL, which made HeapReAlloc return NULL and
        // silently fail EVERY mp_field call. Result: "empty body" and no POST.
        uint8_t* nb = *body
            ? (uint8_t*)HeapReAlloc(GetProcessHeap(), 0, *body, new_cap)
            : (uint8_t*)HeapAlloc  (GetProcessHeap(), 0, new_cap);
        if (!nb) return 0;
        *body = nb; *cap = new_cap;
    }
    memcpy(*body + *len, header, (size_t)hn); *len += (size_t)hn;
    memcpy(*body + *len, data, data_len);     *len += data_len;
    memcpy(*body + *len, "\r\n", 2);          *len += 2;
    return 1;
}

// Public entry — called from ah_launcher.c after WaitForSingleObject(child).
void crash_upload_after_child(DWORD child_pid, DWORD exit_code, const char* version)
{
    // 1. Gather metadata.
    char license_hash[65]; compute_license_hash(license_hash);
    DWORD os_build = get_os_build();
    DWORD parent_pid = GetCurrentProcessId();

    uint8_t crash_marker[2]; read_crash_marker(crash_marker);

    uint8_t* reader_buf = NULL; DWORD reader_len = 0;
    uint8_t* procs_buf  = NULL; DWORD procs_len  = 0;
    // v1.0.28: reader.log gets head+tail split — init phase in head,
    // recent activity in tail. procs.log stays raw tail.
    read_head_tail(L"C:\\Users\\Public\\ah_reader.log",
                   HEAD_BYTES, TAIL_TAIL_BYTES, &reader_buf, &reader_len);
    read_tail(L"C:\\Users\\Public\\ah_procs.log",  &procs_buf,  &procs_len);

    // v1.0.22: launcher-side log too — crucial for marker=0 crashes where
    // the child died before installing its SEH filter. Path is %TEMP%\ah_launcher.log
    // (built dynamically because GetTempPath varies per-user).
    uint8_t* launcher_buf = NULL; DWORD launcher_len = 0;
    uint8_t* crashmeta_buf = NULL; DWORD crashmeta_len = 0;
    uint8_t* dump_buf = NULL; DWORD dump_len = 0;
    {
        wchar_t p[MAX_PATH];
        DWORD n = GetTempPathW(MAX_PATH, p);
        if (n > 0 && n < MAX_PATH - 32) {
            // launcher stub log
            wcscat_s(p, MAX_PATH, L"ah_launcher.log");
            read_tail(p, &launcher_buf, &launcher_len);

            // v1.0.24: VEH crash-meta (exception code, RIP, backtrace).
            // Small text file — read whole, not tail.
            p[n] = 0;
            wcscat_s(p, MAX_PATH, L".abi_crash_meta");
            read_whole(p, 16 * 1024, &crashmeta_buf, &crashmeta_len);
            if (crashmeta_len) DeleteFileW(p);   // reap so next run is fresh

            // v1.0.24: MiniDump — binary format, MUST be read whole. Cap 2 MB.
            p[n] = 0;
            wcscat_s(p, MAX_PATH, L".abi_crash_dump.dmp");
            read_whole(p, 2 * 1024 * 1024, &dump_buf, &dump_len);
            if (dump_len) DeleteFileW(p);
        }
    }

    ah_log("crash_upload: hash=%.16s... build=%u ec=0x%08lX reader=%lu procs=%lu launcher=%lu meta=%lu dump=%lu marker=%u",
           license_hash, os_build, exit_code,
           reader_len, procs_len, launcher_len, crashmeta_len, dump_len, crash_marker[1]);

    // Randomish boundary.
    char boundary[48];
    ULONGLONG t = GetTickCount64();
    _snprintf_s(boundary, sizeof(boundary), _TRUNCATE,
                "----ahUp%016llX%08X", t, (unsigned)parent_pid);

    // 2. Build multipart body.
    uint8_t* body = NULL; size_t body_len = 0, body_cap = 0;
    char meta[64];

    mp_field(&body, &body_len, &body_cap, boundary, "license_hash", NULL,
             (const uint8_t*)license_hash, 64);

    _snprintf_s(meta, sizeof(meta), _TRUNCATE, "%s", version ? version : "unknown");
    mp_field(&body, &body_len, &body_cap, boundary, "version", NULL,
             (const uint8_t*)meta, strlen(meta));

    _snprintf_s(meta, sizeof(meta), _TRUNCATE, "%u", os_build);
    mp_field(&body, &body_len, &body_cap, boundary, "os_build", NULL,
             (const uint8_t*)meta, strlen(meta));

    _snprintf_s(meta, sizeof(meta), _TRUNCATE, "0x%08lX", exit_code);
    mp_field(&body, &body_len, &body_cap, boundary, "exit_code", NULL,
             (const uint8_t*)meta, strlen(meta));

    _snprintf_s(meta, sizeof(meta), _TRUNCATE, "%lu", parent_pid);
    mp_field(&body, &body_len, &body_cap, boundary, "parent_pid", NULL,
             (const uint8_t*)meta, strlen(meta));

    _snprintf_s(meta, sizeof(meta), _TRUNCATE, "%lu", child_pid);
    mp_field(&body, &body_len, &body_cap, boundary, "child_pid", NULL,
             (const uint8_t*)meta, strlen(meta));

    if (reader_buf && reader_len) {
        mp_field(&body, &body_len, &body_cap, boundary, "ah_reader",
                 "ah_reader.log", reader_buf, reader_len);
    }
    if (procs_buf && procs_len) {
        mp_field(&body, &body_len, &body_cap, boundary, "ah_procs",
                 "ah_procs.log", procs_buf, procs_len);
    }
    if (launcher_buf && launcher_len) {
        mp_field(&body, &body_len, &body_cap, boundary, "ah_launcher",
                 "ah_launcher.log", launcher_buf, launcher_len);
    }
    if (crashmeta_buf && crashmeta_len) {
        mp_field(&body, &body_len, &body_cap, boundary, "ah_crashmeta",
                 "crash_meta.txt", crashmeta_buf, crashmeta_len);
    }
    if (dump_buf && dump_len) {
        mp_field(&body, &body_len, &body_cap, boundary, "ah_dump",
                 "crash_dump.dmp", dump_buf, dump_len);
    }
    if (crash_marker[1]) {
        mp_field(&body, &body_len, &body_cap, boundary, "crash_marker",
                 NULL, &crash_marker[0], 1);
    }

    // Final boundary.
    {
        char tail[128];
        int tn = _snprintf_s(tail, sizeof(tail), _TRUNCATE, "--%s--\r\n", boundary);
        if (tn > 0 && body_len + (size_t)tn <= BODY_MAX) {
            if (body_len + (size_t)tn > body_cap) {
                size_t nc = body_cap + (size_t)tn + 64;
                uint8_t* nb = (uint8_t*)HeapReAlloc(GetProcessHeap(), 0, body, nc);
                if (nb) { body = nb; body_cap = nc; }
            }
            if (body) {
                memcpy(body + body_len, tail, (size_t)tn);
                body_len += (size_t)tn;
            }
        }
    }

    if (!body || body_len == 0) {
        ah_log("crash_upload: empty body — skipping POST");
        if (reader_buf)    HeapFree(GetProcessHeap(), 0, reader_buf);
        if (procs_buf)     HeapFree(GetProcessHeap(), 0, procs_buf);
        if (launcher_buf)  HeapFree(GetProcessHeap(), 0, launcher_buf);
        if (crashmeta_buf) HeapFree(GetProcessHeap(), 0, crashmeta_buf);
        if (dump_buf)      HeapFree(GetProcessHeap(), 0, dump_buf);
        if (body)          HeapFree(GetProcessHeap(), 0, body);
        return;
    }

    // 3. WinHTTP POST.
    HINTERNET hSess = WinHttpOpen(L"ahUploader/1.0",
                                  WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) { ah_log("crash_upload: WinHttpOpen fail gle=%lu", GetLastError()); goto cleanup; }

    // 5s timeouts everywhere so we can't block launcher exit.
    WinHttpSetTimeouts(hSess, 5000, 5000, 5000, 5000);

    HINTERNET hConn = WinHttpConnect(hSess, TELEM_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConn) { ah_log("crash_upload: WinHttpConnect fail"); WinHttpCloseHandle(hSess); goto cleanup; }

    HINTERNET hReq = WinHttpOpenRequest(hConn, L"POST", TELEM_PATH, NULL,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE);
    if (!hReq) {
        ah_log("crash_upload: OpenRequest fail");
        WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess); goto cleanup;
    }

    wchar_t ct_hdr[128];
    _snwprintf_s(ct_hdr, 128, _TRUNCATE, L"Content-Type: multipart/form-data; boundary=%S", boundary);

    BOOL ok = WinHttpSendRequest(hReq, ct_hdr, (DWORD)-1L,
                                 body, (DWORD)body_len, (DWORD)body_len, 0);
    if (ok) ok = WinHttpReceiveResponse(hReq, NULL);
    DWORD status = 0, szsz = sizeof(status);
    if (ok) {
        WinHttpQueryHeaders(hReq,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            NULL, &status, &szsz, WINHTTP_NO_HEADER_INDEX);
    }
    ah_log("crash_upload: POST status=%lu (body=%zu)", status, body_len);

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSess);

cleanup:
    if (reader_buf)    HeapFree(GetProcessHeap(), 0, reader_buf);
    if (procs_buf)     HeapFree(GetProcessHeap(), 0, procs_buf);
    if (launcher_buf)  HeapFree(GetProcessHeap(), 0, launcher_buf);
    if (crashmeta_buf) HeapFree(GetProcessHeap(), 0, crashmeta_buf);
    if (dump_buf)      HeapFree(GetProcessHeap(), 0, dump_buf);
    if (body)          HeapFree(GetProcessHeap(), 0, body);
}
