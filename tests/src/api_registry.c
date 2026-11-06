// Registry (advapi32): create/open/query/enumerate/delete under HKCU, A/W conversions, buffer-size rules,
// RegGetValue type filters, and the Windows version values games read from HKLM.
// Expected output (tests/expected/api_registry.out) is what Windows 10/11 prints (Microsoft Learn semantics).
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define T "Software\\wintomac_test"
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    HKEY k, s; DWORD disp = 0, type, cb; LONG r; char buf[64]; WCHAR wbuf[64];
    RegDeleteTreeA(HKEY_CURRENT_USER, T);
    r = RegOpenKeyExA(HKEY_CURRENT_USER, T, 0, KEY_READ, &k); printf("open missing: %ld\n", r);
    r = RegCreateKeyExA(HKEY_CURRENT_USER, T "\\Sub", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &s, &disp); printf("create nested: %ld disp=%lu\n", r, disp);
    RegCloseKey(s);
    r = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\wintomac_test", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, &disp); printf("create existing: %ld disp=%lu\n", r, disp);

    DWORD dw = 0x12345678; ULONGLONG q = 0x1122334455667788ull; BYTE bin[3] = { 1, 2, 3 };
    RegSetValueExA(k, "dw", 0, REG_DWORD, (BYTE *)&dw, 4);
    RegSetValueExA(k, "str", 0, REG_SZ, (const BYTE *)"hello", 6);
    RegSetValueExW(k, L"wstr", 0, REG_SZ, (const BYTE *)L"wide", 10);
    RegSetValueExA(k, "bin", 0, REG_BINARY, bin, 3);
    RegSetValueExA(k, "q", 0, REG_QWORD, (BYTE *)&q, 8);
    RegSetValueExA(k, "multi", 0, REG_MULTI_SZ, (const BYTE *)"a\0bc\0", 6);
    RegSetValueExA(k, "", 0, REG_SZ, (const BYTE *)"default", 8);

    cb = 0; r = RegQueryValueExA(k, "str", NULL, &type, NULL, &cb); printf("size A: r=%ld type=%lu cb=%lu\n", r, type, cb);
    cb = 0; r = RegQueryValueExW(k, L"str", NULL, &type, NULL, &cb); printf("size W: r=%ld type=%lu cb=%lu\n", r, type, cb);
    cb = 3; r = RegQueryValueExA(k, "str", NULL, NULL, (BYTE *)buf, &cb); printf("small buffer: r=%ld cb=%lu\n", r, cb);
    cb = sizeof buf; r = RegQueryValueExA(k, "wstr", NULL, &type, (BYTE *)buf, &cb); printf("W value via A: r=%ld '%s' cb=%lu\n", r, buf, cb);
    cb = sizeof wbuf; r = RegQueryValueExW(k, L"STR", NULL, &type, (BYTE *)wbuf, &cb); printf("A value via W (case-insensitive name): r=%ld len=%u cb=%lu\n", r, (unsigned)wcslen(wbuf), cb);
    cb = sizeof buf; r = RegQueryValueExA(k, NULL, NULL, &type, (BYTE *)buf, &cb); printf("default value: r=%ld '%s'\n", r, buf);
    cb = sizeof buf; r = RegQueryValueExA(k, "nope", NULL, &type, (BYTE *)buf, &cb); printf("missing value: %ld\n", r);
    cb = 8; q = 0; r = RegQueryValueExA(k, "q", NULL, &type, (BYTE *)&q, &cb); printf("qword: r=%ld type=%lu %llx\n", r, type, q);
    cb = sizeof buf; memset(buf, 0, sizeof buf); r = RegQueryValueExA(k, "multi", NULL, &type, (BYTE *)buf, &cb);
    printf("multi: r=%ld type=%lu cb=%lu '%s' '%s'\n", r, type, cb, buf, buf + 2);

    dw = 0; cb = 4; r = RegGetValueA(HKEY_CURRENT_USER, T, "dw", RRF_RT_REG_DWORD, &type, &dw, &cb); printf("RegGetValue dword: r=%ld %lx\n", r, dw);
    cb = 4; r = RegGetValueA(HKEY_CURRENT_USER, T, "dw", RRF_RT_REG_SZ, &type, buf, &cb); printf("RegGetValue wrong type: %ld\n", r);
    cb = sizeof wbuf; r = RegGetValueW(HKEY_CURRENT_USER, L"Software\\wintomac_test", L"wstr", RRF_RT_REG_SZ, NULL, wbuf, &cb); printf("RegGetValueW sz: r=%ld cb=%lu\n", r, cb);
    cb = sizeof buf; r = RegGetValueA(HKEY_CURRENT_USER, T "\\nokey", "x", RRF_RT_ANY, NULL, buf, &cb); printf("RegGetValue missing key: %ld\n", r);

    for (DWORD i = 0; ; i++) {
        char name[64]; DWORD nl = sizeof name; cb = sizeof buf;
        r = RegEnumValueA(k, i, name, &nl, NULL, &type, (BYTE *)buf, &cb);
        if (r) { printf("enum value end: %ld\n", r); break; }
        printf("value %lu: '%s' len=%lu type=%lu cb=%lu\n", i, name, nl, type, cb);
    }
    for (DWORD i = 0; ; i++) {
        char name[64]; DWORD nl = sizeof name;
        r = RegEnumKeyExA(k, i, name, &nl, NULL, NULL, NULL, NULL);
        if (r) { printf("enum key end: %ld\n", r); break; }
        printf("key %lu: '%s' len=%lu\n", i, name, nl);
    }
    DWORD nsub = 0, maxsub = 0, nval = 0, maxname = 0;
    r = RegQueryInfoKeyA(k, NULL, NULL, NULL, &nsub, &maxsub, NULL, &nval, &maxname, NULL, NULL, NULL);
    printf("info: r=%ld subkeys=%lu maxsub=%lu values=%lu maxname=%lu\n", r, nsub, maxsub, nval, maxname);

    r = RegDeleteValueA(k, "dw"); printf("delete value: %ld\n", r);
    r = RegDeleteValueA(k, "dw"); printf("delete value again: %ld\n", r);
    r = RegDeleteKeyA(HKEY_CURRENT_USER, T); printf("delete key with subkeys: %ld\n", r);
    r = RegDeleteKeyA(k, "Sub"); printf("delete subkey: %ld\n", r);
    RegCloseKey(k);
    r = RegDeleteTreeA(HKEY_CURRENT_USER, T); printf("delete tree: %ld\n", r);
    r = RegOpenKeyExA(HKEY_CURRENT_USER, T, 0, KEY_READ, &k); printf("open deleted: %ld\n", r);

    dw = 0; cb = 4; r = RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentMajorVersionNumber", RRF_RT_REG_DWORD, NULL, &dw, &cb);
    printf("Windows major version: r=%ld %lu\n", r, dw);
    r = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &k);
    cb = sizeof buf; LONG r2 = RegQueryValueExA(k, "ProcessorNameString", NULL, &type, (BYTE *)buf, &cb);
    printf("cpu key: %ld name: %ld type=%lu nonempty=%d\n", r, r2, type, buf[0] != 0);
    RegCloseKey(k);
    return 0;
}
