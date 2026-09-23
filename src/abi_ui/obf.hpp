// obf.hpp — compile-time XOR string obfuscation.
// Any string wrapped in OBF("literal") is stored XOR-encoded in the binary
// (.rdata contains scrambled bytes, no literal ASCII form). Decoded on first
// use into a per-callsite static buffer.
//
// Use ONLY for strings that would otherwise expose the tool's identity — file
// names ("abi_drv.sys"), target process names ("UAGame.exe"), driver device
// paths, diagnostic messages containing "bridge"/"BYOVD"/"ACE"/etc.
//
// Do NOT wrap heavily-used hot-path strings — the per-callsite static decode
// buffer adds a data-cache pressure and a first-touch mutex; only worth it for
// signal-carrying literals, not every diag print.

#pragma once
#include <cstddef>
#include <cstdint>

namespace obf {

constexpr uint8_t KEY_A = 0x5A;
constexpr uint8_t KEY_B = 0x37;

template<std::size_t N>
struct EncStr {
    char data[N];
    constexpr EncStr(const char (&s)[N]) : data{} {
        for (std::size_t i = 0; i < N; i++) {
            data[i] = static_cast<char>(
                static_cast<uint8_t>(s[i]) ^ (KEY_A ^ static_cast<uint8_t>(i & 0xff)) ^ KEY_B);
        }
    }
};

template<std::size_t N>
inline const char* decode_into(char (&dst)[N], const char* enc) {
    for (std::size_t i = 0; i < N; i++) {
        dst[i] = static_cast<char>(
            static_cast<uint8_t>(enc[i]) ^ (KEY_A ^ static_cast<uint8_t>(i & 0xff)) ^ KEY_B);
    }
    return dst;
}

template<std::size_t N>
struct EncWStr {
    wchar_t data[N];
    constexpr EncWStr(const wchar_t (&s)[N]) : data{} {
        for (std::size_t i = 0; i < N; i++) {
            data[i] = static_cast<wchar_t>(
                (static_cast<uint16_t>(s[i]) ^ (0x5A37 ^ static_cast<uint16_t>(i))));
        }
    }
};

template<std::size_t N>
inline const wchar_t* decode_wide_into(wchar_t (&dst)[N], const wchar_t* enc) {
    for (std::size_t i = 0; i < N; i++) {
        dst[i] = static_cast<wchar_t>(
            (static_cast<uint16_t>(enc[i]) ^ (0x5A37 ^ static_cast<uint16_t>(i))));
    }
    return dst;
}

} // namespace obf

// OBF("literal") — narrow C-string. Value is a `const char*` to a
// per-callsite static buffer that only holds plaintext after first decode.
#define OBF(s) ([]() -> const char* {                                          \
    static constexpr auto _enc = ::obf::EncStr(s);                             \
    static char _buf[sizeof(_enc.data)];                                       \
    static bool _done = false;                                                 \
    if (!_done) { ::obf::decode_into(_buf, _enc.data); _done = true; }         \
    return _buf;                                                               \
}())

// OBFW(L"literal") — wide C-string.
#define OBFW(s) ([]() -> const wchar_t* {                                      \
    static constexpr auto _enc = ::obf::EncWStr(s);                            \
    static wchar_t _buf[sizeof(_enc.data)/sizeof(wchar_t)];                    \
    static bool _done = false;                                                 \
    if (!_done) { ::obf::decode_wide_into(_buf, _enc.data); _done = true; }    \
    return _buf;                                                               \
}())
