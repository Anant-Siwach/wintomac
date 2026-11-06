// advapi32 / bcrypt: the registry, random numbers, hashing, user name and the security-descriptor surface.
//
// Registry: an in-memory tree seeded with the values games and libraries actually query (Windows version,
// CPU description, shell folders, DirectX version, machine GUID). HKEY_CURRENT_USER is persisted to
// ~/Library/Application Support/wintomac/user.reg (override with WINTOMAC_REGISTRY=<file>, or "none" to keep it
// in memory) so settings games store there (Unity PlayerPrefs, launchers, ...) survive restarts.
// Semantics (sizes, ERROR_MORE_DATA, A/W conversions, RegGetValue type filters, enumeration order) follow the
// Microsoft Learn documentation of each function; Wine's advapi32/kernelbase registry code was used as a
// behavioural reference (no code copied).
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "winstr.h"

#pragma clang diagnostic ignored "-Wdeprecated-declarations"   // CC_MD5 (MD5 is what the guest asked for)

enum { REG_NONE, REG_SZ, REG_EXPAND_SZ, REG_BINARY, REG_DWORD, REG_DWORD_BE, REG_LINK, REG_MULTI_SZ, REG_QWORD = 11 };
enum { OK = 0, E_NOTFOUND = 2, E_ACCESS = 5, E_HANDLE = 6, E_PARAM = 87, E_INSUFF = 122, E_MOREDATA = 234,
       E_NOMORE = 259, E_KEYDELETED = 1018, E_BADTYPE = 1630 };
#define H_REGKEY 23

typedef struct RVal { char *name; u32 type, size; u8 *data; struct RVal *next; } RVal;
typedef struct RKey { char *name; struct RKey *parent, *kids, *next; RVal *vals; int deleted; u64 mtime; } RKey;

static pthread_mutex_t reg_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static RKey *root_hklm, *root_hkcu, *root_hku;
static char reg_file[1024];
static int reg_loading;

static u64 now_ft(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return ((u64)ts.tv_sec + 11644473600ull) * 10000000ull + (u64)ts.tv_nsec / 100; }

static RKey *key_new(RKey *parent, const char *name) {
    RKey *k = calloc(1, sizeof *k);
    k->name = strdup(name); k->parent = parent; k->mtime = now_ft();
    if (parent) {   // children kept sorted case-insensitively, like RegEnumKeyEx reports them
        RKey **pp = &parent->kids;
        while (*pp && strcasecmp((*pp)->name, name) < 0) pp = &(*pp)->next;
        k->next = *pp; *pp = k;
    }
    return k;
}
static RKey *key_child(RKey *k, const char *name, size_t n) {
    for (RKey *c = k->kids; c; c = c->next) if (strlen(c->name) == n && !strncasecmp(c->name, name, n)) return c;
    return NULL;
}
// Walk "a\\b\\c" below k; create missing components when create is set (*created reports whether the leaf was new).
static RKey *key_walk(RKey *k, const char *path, int create, int *created) {
    if (created) *created = 0;
    while (k && path && *path) {
        while (*path == '\\') path++;
        if (!*path) break;
        const char *e = strchr(path, '\\'); size_t n = e ? (size_t)(e - path) : strlen(path);
        RKey *c = key_child(k, path, n);
        if (!c && create) { char nm[512]; snprintf(nm, sizeof nm, "%.*s", (int)n, path); c = key_new(k, nm); if (created) *created = 1; }
        k = c; path += n;
    }
    return k;
}
static RVal *val_find(RKey *k, const char *name) {
    if (!name) name = "";
    for (RVal *v = k->vals; v; v = v->next) if (!strcasecmp(v->name, name)) return v;
    return NULL;
}
static void val_set(RKey *k, const char *name, u32 type, const void *data, u32 size) {
    RVal *v = val_find(k, name);
    if (!v) {   // values enumerate in creation order
        v = calloc(1, sizeof *v); v->name = strdup(name ? name : "");
        RVal **pp = &k->vals; while (*pp) pp = &(*pp)->next; *pp = v;
    }
    free(v->data); v->data = malloc(size ? size : 1); if (size) memcpy(v->data, data, size);
    v->type = type; v->size = size; k->mtime = now_ft();
}
static void set_sz(RKey *k, const char *name, const char *s) {   // stored as UTF-16 like Windows does
    size_t n = wx_u2w(s, 0, 0); u16 *w = malloc(2 * n); wx_u2w(s, (u64)w, n);
    val_set(k, name, REG_SZ, w, (u32)(2 * n)); free(w);
}
static void set_dw(RKey *k, const char *name, u32 d) { val_set(k, name, REG_DWORD, &d, 4); }
static int key_is_under(RKey *k, RKey *root) { for (; k; k = k->parent) if (k == root) return 1; return 0; }

// ---- persistence of HKEY_CURRENT_USER ---------------------------------------------------------------------------
static void hex_out(FILE *f, const u8 *p, size_t n) { for (size_t i = 0; i < n; i++) fprintf(f, "%02x", p[i]); }
static size_t hex_in(const char *s, u8 *out, size_t cap) {
    size_t n = 0;
    while (isxdigit((u8)s[0]) && isxdigit((u8)s[1]) && n < cap) { unsigned b; sscanf(s, "%2x", &b); out[n++] = (u8)b; s += 2; }
    return n;
}
static void save_key(FILE *f, RKey *k, const char *path) {
    fprintf(f, "K "); hex_out(f, (const u8 *)path, strlen(path)); fprintf(f, "\n");
    for (RVal *v = k->vals; v; v = v->next) {
        fprintf(f, "V "); hex_out(f, (const u8 *)v->name, strlen(v->name)); fprintf(f, "- %u ", v->type); hex_out(f, v->data, v->size); fprintf(f, "\n");
    }
    for (RKey *c = k->kids; c; c = c->next) {
        char sub[2048]; snprintf(sub, sizeof sub, "%s%s%s", path, *path ? "\\" : "", c->name);
        save_key(f, c, sub);
    }
}
static void reg_save(void) {
    if (!*reg_file || reg_loading) return;
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", reg_file);
    FILE *f = fopen(tmp, "w"); if (!f) return;
    fprintf(f, "wintomac-registry 1\n");
    save_key(f, root_hkcu, "");
    fclose(f); rename(tmp, reg_file);
}
static void reg_load(void) {
    FILE *f = fopen(reg_file, "r"); if (!f) return;
    reg_loading = 1;
    size_t cap = 1 << 20; char *line = malloc(cap); u8 *buf = malloc(cap / 2);
    RKey *cur = root_hkcu;
    while (fgets(line, (int)cap, f)) {
        if (line[0] == 'K' && line[1] == ' ') {
            size_t n = hex_in(line + 2, buf, cap / 2 - 1); buf[n] = 0;
            cur = key_walk(root_hkcu, (char *)buf, 1, NULL);
        } else if (line[0] == 'V' && line[1] == ' ') {
            char *p = line + 2; size_t n = hex_in(p, buf, 1023); char name[1024]; memcpy(name, buf, n); name[n] = 0;
            p = strchr(p, '-'); if (!p) continue;
            unsigned type = (unsigned)strtoul(p + 1, &p, 10); while (*p == ' ') p++;
            size_t sz = hex_in(p, buf, cap / 2);
            val_set(cur, name, type, buf, (u32)sz);
        }
    }
    free(line); free(buf); fclose(f);
    reg_loading = 0;
}

// ---- seeds -------------------------------------------------------------------------------------------------------
static void reg_seed(void) {
    RKey *k = key_walk(root_hklm, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 1, NULL);
    set_sz(k, "ProductName", "Windows 10 Pro"); set_sz(k, "EditionID", "Professional"); set_sz(k, "InstallationType", "Client");
    set_sz(k, "CurrentVersion", "6.3"); set_sz(k, "CurrentBuild", "19045"); set_sz(k, "CurrentBuildNumber", "19045");
    set_dw(k, "CurrentMajorVersionNumber", 10); set_dw(k, "CurrentMinorVersionNumber", 0); set_dw(k, "UBR", 3803);
    set_sz(k, "DisplayVersion", "22H2"); set_sz(k, "ReleaseId", "2009"); set_sz(k, "SystemRoot", "C:\\windows");
    set_sz(k, "RegisteredOwner", "wintomac"); set_sz(k, "RegisteredOrganization", "");
    k = key_walk(root_hklm, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion", 1, NULL);
    set_sz(k, "ProgramFilesDir", "C:\\Program Files"); set_sz(k, "ProgramFilesDir (x86)", "C:\\Program Files (x86)");
    set_sz(k, "CommonFilesDir", "C:\\Program Files\\Common Files"); set_sz(k, "ProgramW6432Dir", "C:\\Program Files");
    k = key_walk(root_hklm, "SOFTWARE\\Microsoft\\DirectX", 1, NULL);
    set_sz(k, "Version", "4.09.00.0904");
    k = key_walk(root_hklm, "SOFTWARE\\Microsoft\\Cryptography", 1, NULL);
    { uuid_t u; struct timespec w = { 1, 0 }; char g[64] = "a3f1c2d4-5b6e-4f70-8a9b-0c1d2e3f4a5b";
      if (!gethostuuid(u, &w))
          snprintf(g, sizeof g, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0], u[1], u[2], u[3], u[4], u[5],
                   u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
      set_sz(k, "MachineGuid", g); }
    k = key_walk(root_hklm, "SYSTEM\\CurrentControlSet\\Control\\Nls\\Language", 1, NULL);
    set_sz(k, "Default", "0409"); set_sz(k, "InstallLanguage", "0409");
    k = key_walk(root_hklm, "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", 1, NULL);
    set_sz(k, "OS", "Windows_NT"); set_sz(k, "PROCESSOR_ARCHITECTURE", "AMD64");
    k = key_walk(root_hklm, "HARDWARE\\DESCRIPTION\\System\\BIOS", 1, NULL);
    { char model[128] = "Mac"; size_t n = sizeof model; sysctlbyname("hw.model", model, &n, NULL, 0);
      set_sz(k, "SystemManufacturer", "Apple Inc."); set_sz(k, "SystemProductName", model); set_sz(k, "BIOSVendor", "Apple Inc."); }
    int ncpu = 1; size_t sz = sizeof ncpu; sysctlbyname("hw.logicalcpu", &ncpu, &sz, NULL, 0);
    char brand[128] = "Apple Silicon"; sz = sizeof brand; sysctlbyname("machdep.cpu.brand_string", brand, &sz, NULL, 0);
    for (int i = 0; i < ncpu && i < 256; i++) {
        char p[128]; snprintf(p, sizeof p, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\%d", i);
        k = key_walk(root_hklm, p, 1, NULL);
        set_sz(k, "ProcessorNameString", brand); set_sz(k, "VendorIdentifier", "GenuineIntel");
        set_sz(k, "Identifier", "Intel64 Family 6 Model 158 Stepping 10"); set_dw(k, "~MHz", 3200);
    }
    key_walk(root_hklm, "SOFTWARE\\Classes", 1, NULL);
    key_walk(root_hklm, "SYSTEM\\CurrentControlSet\\Hardware Profiles\\Current", 1, NULL);

    k = key_walk(root_hkcu, "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders", 1, NULL);
    RKey *u = key_walk(root_hkcu, "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders", 1, NULL);
    static const char *const sf[][2] = {
        { "AppData", "C:\\users\\wintomac\\AppData\\Roaming" }, { "Local AppData", "C:\\users\\wintomac\\AppData\\Local" },
        { "Personal", "C:\\users\\wintomac\\Documents" }, { "Desktop", "C:\\users\\wintomac\\Desktop" },
        { "{4C5C32FF-BB9D-43B0-B5B4-2D72E54EAAA4}", "C:\\users\\wintomac\\Saved Games" },
    };
    for (unsigned i = 0; i < sizeof sf / sizeof *sf; i++) { set_sz(k, sf[i][0], sf[i][1]); set_sz(u, sf[i][0], sf[i][1]); }
    k = key_walk(root_hkcu, "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 1, NULL);
    set_dw(k, "AppsUseLightTheme", 1); set_dw(k, "SystemUsesLightTheme", 1);
    k = key_walk(root_hkcu, "Control Panel\\International", 1, NULL);
    set_sz(k, "LocaleName", "en-US"); set_sz(k, "sDecimal", "."); set_sz(k, "sThousand", ","); set_sz(k, "sShortDate", "M/d/yyyy");
    set_sz(k, "sLongDate", "dddd, MMMM d, yyyy"); set_sz(k, "sTimeFormat", "h:mm:ss tt"); set_sz(k, "iMeasure", "1");
    k = key_walk(root_hkcu, "Control Panel\\Desktop", 1, NULL);
    set_sz(k, "WheelScrollLines", "3"); set_sz(k, "MenuShowDelay", "400");
    key_walk(root_hku, ".DEFAULT", 1, NULL);
}

static void reg_init(void) {
    static int done;
    if (done) return;
    done = 1;
    root_hklm = key_new(NULL, "HKEY_LOCAL_MACHINE"); root_hkcu = key_new(NULL, "HKEY_CURRENT_USER"); root_hku = key_new(NULL, "HKEY_USERS");
    reg_loading = 1; reg_seed(); reg_loading = 0;
    const char *f = getenv("WINTOMAC_REGISTRY");
    if (f && !strcmp(f, "none")) reg_file[0] = 0;
    else if (f && *f) snprintf(reg_file, sizeof reg_file, "%s", f);
    else {
        char dir[1100]; snprintf(dir, sizeof dir, "%s", rt_user_data_dir());   // per game when packaged (appdata.c)
        mkdir(dir, 0755);
        snprintf(reg_file, sizeof reg_file, "%s/user.reg", dir);
    }
    if (*reg_file) reg_load();
}

// HKEY -> key. Predefined keys are sign-extended 32-bit values (0xFFFFFFFF80000001 for HKCU).
static RKey *key_of(u64 h, u32 *err) {
    reg_init();
    u32 lo = (u32)h, hi = (u32)(h >> 32);
    if ((hi == 0 || hi == 0xFFFFFFFF) && lo >= 0x80000000u && lo <= 0x80000006u) {
        switch (lo) {
        case 0x80000000u: return key_walk(root_hklm, "SOFTWARE\\Classes", 1, NULL);
        case 0x80000001u: return root_hkcu;
        case 0x80000002u: return root_hklm;
        case 0x80000003u: return root_hku;
        case 0x80000005u: return key_walk(root_hklm, "SYSTEM\\CurrentControlSet\\Hardware Profiles\\Current", 1, NULL);
        default: *err = E_HANDLE; return NULL;
        }
    }
    intptr_t p;
    if (!rt_handle_get(h, H_REGKEY, &p)) { *err = E_HANDLE; return NULL; }
    if (((RKey *)p)->deleted) { *err = E_KEYDELETED; return NULL; }
    return (RKey *)p;
}

// Narrow (ANSI = Latin-1 here, like the C locale) <-> UTF-8 for the A entry points.
static void a2u(const char *a, char *out, size_t n) {
    size_t o = 0;
    for (; a && *a && o + 3 < n; a++) { u8 ch = (u8)*a; if (ch < 0x80) out[o++] = (char)ch; else { out[o++] = (char)(0xC0 | ch >> 6); out[o++] = (char)(0x80 | (ch & 63)); } }
    out[o] = 0;
}
static void argstr(u64 p, int wide, char *out, size_t n) { if (!p) { out[0] = 0; return; } if (wide) wx_w2u(p, out, n); else a2u((const char *)p, out, n); }

// ---- open / create / close -----------------------------------------------------------------------------------------
static u32 reg_open(u64 hkey, u64 sub, int wide, u64 phk, int create, u64 disp) {
    u32 err = 0;
    if (!phk) return E_PARAM;
    pthread_mutex_lock(&reg_lock);
    RKey *k = key_of(hkey, &err);
    if (!k) { pthread_mutex_unlock(&reg_lock); ST64(phk, 0); return err; }
    char path[1024]; argstr(sub, wide, path, sizeof path);
    int created = 0;
    RKey *r = key_walk(k, path, create, &created);
    if (!r) { pthread_mutex_unlock(&reg_lock); ST64(phk, 0); return E_NOTFOUND; }
    if (created && key_is_under(r, root_hkcu)) reg_save();
    pthread_mutex_unlock(&reg_lock);
    ST64(phk, rt_handle_new(H_REGKEY, (intptr_t)r));
    if (disp) ST32(disp, created ? 1 : 2);   // REG_CREATED_NEW_KEY / REG_OPENED_EXISTING_KEY
    return OK;
}
SHIM(RegOpenKeyExA) { RET(reg_open(A1, A2, 0, AN(5), 0, 0)); }   // (hkey, sub, options, sam, phk)
SHIM(RegOpenKeyExW) { RET(reg_open(A1, A2, 1, AN(5), 0, 0)); }
SHIM(RegOpenKeyA) { RET(reg_open(A1, A2, 0, A3, 0, 0)); }
SHIM(RegOpenKeyW) { RET(reg_open(A1, A2, 1, A3, 0, 0)); }
SHIM(RegCreateKeyExA) { RET(reg_open(A1, A2, 0, AN(8), 1, AN(9))); }   // (hkey, sub, 0, class, opts, sam, sa, phk, disp)
SHIM(RegCreateKeyExW) { RET(reg_open(A1, A2, 1, AN(8), 1, AN(9))); }
SHIM(RegCreateKeyA) { RET(reg_open(A1, A2, 0, A3, 1, 0)); }
SHIM(RegCreateKeyW) { RET(reg_open(A1, A2, 1, A3, 1, 0)); }
SHIM(RegOpenCurrentUser) { ST64(A2, 0xFFFFFFFF80000001ull); RET(OK); }
SHIM(RegCloseKey) {
    u32 lo = (u32)A1;
    if (lo >= 0x80000000u && lo <= 0x80000006u) RET(OK);
    intptr_t p; if (!rt_handle_get(A1, H_REGKEY, &p)) RET(E_HANDLE);
    rt_handle_close(A1); RET(OK);
}
SHIM(RegFlushKey) { pthread_mutex_lock(&reg_lock); reg_save(); pthread_mutex_unlock(&reg_lock); RET(OK); }
SHIM(RegNotifyChangeKeyValue) { RET(OK); }   // changes are never signalled (nothing else writes our registry)
SHIM(RegDisablePredefinedCache) { RET(OK); }

// ---- values ------------------------------------------------------------------------------------------------------
static int is_str(u32 t) { return t == REG_SZ || t == REG_EXPAND_SZ || t == REG_MULTI_SZ; }
// Value data as the caller sees it: W = stored bytes; A = strings converted to narrow (Latin-1, '?' if unmappable).
static u8 *val_view(RVal *v, int wide, u32 *size) {
    if (wide || !is_str(v->type)) { *size = v->size; return v->data; }
    u32 n = v->size / 2; u8 *a = malloc(n + 1);
    for (u32 i = 0; i < n; i++) { u16 ch = (u16)(v->data[2 * i] | v->data[2 * i + 1] << 8); a[i] = ch < 256 ? (u8)ch : '?'; }
    *size = n; return a;
}
static u32 copy_out(u8 *src, u32 n, u64 data, u64 pcb) {
    if (!pcb) return data ? E_PARAM : OK;
    u32 cap = LD32(pcb); ST32(pcb, n);
    if (!data) return OK;
    if (cap < n) return E_MOREDATA;
    memcpy((void *)data, src, n); return OK;
}
static u32 query_value(u64 hkey, u64 name, int wide, u64 ptype, u64 data, u64 pcb) {
    u32 err = 0;
    pthread_mutex_lock(&reg_lock);
    RKey *k = key_of(hkey, &err);
    if (!k) { pthread_mutex_unlock(&reg_lock); return err; }
    char nm[1024]; argstr(name, wide, nm, sizeof nm);
    RVal *v = val_find(k, nm);
    if (!v) { pthread_mutex_unlock(&reg_lock); return E_NOTFOUND; }
    if (ptype) ST32(ptype, v->type);
    u32 n; u8 *p = val_view(v, wide, &n);
    u32 r = copy_out(p, n, data, pcb);
    if (p != v->data) free(p);
    pthread_mutex_unlock(&reg_lock);
    return r;
}
SHIM(RegQueryValueExA) { RET(query_value(A1, A2, 0, A4, AN(5), AN(6))); }   // (hkey, name, reserved, type*, data, cb*)
SHIM(RegQueryValueExW) { RET(query_value(A1, A2, 1, A4, AN(5), AN(6))); }

static u32 get_value(u64 hkey, u64 sub, u64 name, u32 flags, u64 ptype, u64 data, u64 pcb, int wide) {
    u64 h = hkey; u32 r;
    if (sub && (wide ? LD16(sub) : LD8(sub))) { r = reg_open(hkey, sub, wide, (u64)&h, 0, 0); if (r) return r; }
    u32 type = 0, cap = pcb ? LD32(pcb) : 0;
    r = query_value(h, name, wide, (u64)&type, data, pcb);
    if (h != hkey) rt_handle_close(h);
    if (r && r != E_MOREDATA) return r;
    u32 bit = type == REG_NONE ? 1 : type == REG_SZ ? 2 : type == REG_EXPAND_SZ ? ((flags & 0x10000000) ? 4 : 2) : type == REG_BINARY ? 8 :
              type == REG_DWORD ? 0x10 : type == REG_MULTI_SZ ? 0x20 : type == REG_QWORD ? 0x40 : 0;
    if (!(flags & bit)) { if ((flags & 0x20000000) && data) memset((void *)data, 0, cap); return E_BADTYPE; }
    if (type == REG_EXPAND_SZ && !(flags & 0x10000000)) type = REG_SZ;
    if (ptype) ST32(ptype, type);
    if (r == E_MOREDATA && (flags & 0x20000000) && data) memset((void *)data, 0, cap);
    return r;
}
SHIM(RegGetValueW) { RET(get_value(A1, A2, A3, (u32)A4, AN(5), AN(6), AN(7), 1)); }   // (hkey, sub, value, flags, type*, data, cb*)
SHIM(RegGetValueA) { RET(get_value(A1, A2, A3, (u32)A4, AN(5), AN(6), AN(7), 0)); }

static u32 set_value(u64 hkey, u64 name, int wide, u32 type, u64 data, u32 cb) {
    u32 err = 0;
    pthread_mutex_lock(&reg_lock);
    RKey *k = key_of(hkey, &err);
    if (!k) { pthread_mutex_unlock(&reg_lock); return err; }
    char nm[1024]; argstr(name, wide, nm, sizeof nm);
    if (!wide && is_str(type)) {   // narrow strings are stored as UTF-16
        u16 *w = malloc(2 * (size_t)cb + 2);
        for (u32 i = 0; i < cb; i++) w[i] = LD8(data + i);
        u32 n = cb;
        if (type != REG_MULTI_SZ && (n == 0 || w[n - 1])) w[n++] = 0;   // RegSetValueExA adds the terminator if missing
        val_set(k, nm, type, w, 2 * n); free(w);
    } else val_set(k, nm, type, (void *)data, cb);
    if (key_is_under(k, root_hkcu)) reg_save();
    pthread_mutex_unlock(&reg_lock);
    return OK;
}
SHIM(RegSetValueExA) { RET(set_value(A1, A2, 0, (u32)A4, AN(5), (u32)AN(6))); }   // (hkey, name, reserved, type, data, cb)
SHIM(RegSetValueExW) { RET(set_value(A1, A2, 1, (u32)A4, AN(5), (u32)AN(6))); }
static u32 set_key_value(u64 hkey, u64 sub, u64 name, u32 type, u64 data, u32 cb, int wide) {
    u64 h = hkey; u32 r;
    if (sub && (wide ? LD16(sub) : LD8(sub))) { r = reg_open(hkey, sub, wide, (u64)&h, 1, 0); if (r) return r; }
    r = set_value(h, name, wide, type, data, cb);
    if (h != hkey) rt_handle_close(h);
    return r;
}
SHIM(RegSetKeyValueW) { RET(set_key_value(A1, A2, A3, (u32)A4, AN(5), (u32)AN(6), 1)); }
SHIM(RegSetKeyValueA) { RET(set_key_value(A1, A2, A3, (u32)A4, AN(5), (u32)AN(6), 0)); }

static u32 delete_value(u64 hkey, u64 name, int wide) {
    u32 err = 0;
    pthread_mutex_lock(&reg_lock);
    RKey *k = key_of(hkey, &err);
    if (!k) { pthread_mutex_unlock(&reg_lock); return err; }
    char nm[1024]; argstr(name, wide, nm, sizeof nm);
    for (RVal **pp = &k->vals; *pp; pp = &(*pp)->next)
        if (!strcasecmp((*pp)->name, nm)) {
            RVal *v = *pp; *pp = v->next; free(v->name); free(v->data); free(v);
            if (key_is_under(k, root_hkcu)) reg_save();
            pthread_mutex_unlock(&reg_lock); return OK;
        }
    pthread_mutex_unlock(&reg_lock);
    return E_NOTFOUND;
}
SHIM(RegDeleteValueA) { RET(delete_value(A1, A2, 0)); }
SHIM(RegDeleteValueW) { RET(delete_value(A1, A2, 1)); }

static void key_unlink(RKey *k) {
    k->deleted = 1;
    for (RKey *c = k->kids; c; c = c->next) key_unlink(c);
    if (k->parent) for (RKey **pp = &k->parent->kids; *pp; pp = &(*pp)->next) if (*pp == k) { *pp = k->next; break; }
}
static u32 delete_key(u64 hkey, u64 sub, int wide, int tree) {
    u32 err = 0;
    pthread_mutex_lock(&reg_lock);
    RKey *k = key_of(hkey, &err);
    if (!k) { pthread_mutex_unlock(&reg_lock); return err; }
    char path[1024]; argstr(sub, wide, path, sizeof path);
    RKey *r = key_walk(k, path, 0, NULL);
    u32 res = OK;
    if (!r) res = E_NOTFOUND;
    else if (tree) {   // RegDeleteTree: the subkey with everything below it; with no subkey, only the key's contents
        if (*path) key_unlink(r);
        else { while (r->kids) key_unlink(r->kids); while (r->vals) { RVal *v = r->vals; r->vals = v->next; free(v->name); free(v->data); free(v); } }
    } else if (!r->parent || !r->parent->parent) res = E_ACCESS;   // roots and the hives' first level stay
    else if (r->kids) res = E_ACCESS;    // RegDeleteKey refuses keys that still have subkeys
    else key_unlink(r);
    if (!res && key_is_under(k, root_hkcu)) reg_save();
    pthread_mutex_unlock(&reg_lock);
    return res;
}
SHIM(RegDeleteKeyA) { RET(delete_key(A1, A2, 0, 0)); }
SHIM(RegDeleteKeyW) { RET(delete_key(A1, A2, 1, 0)); }
SHIM(RegDeleteKeyExA) { RET(delete_key(A1, A2, 0, 0)); }
SHIM(RegDeleteKeyExW) { RET(delete_key(A1, A2, 1, 0)); }
SHIM(RegDeleteTreeA) { RET(delete_key(A1, A2, 0, 1)); }
SHIM(RegDeleteTreeW) { RET(delete_key(A1, A2, 1, 1)); }

// ---- enumeration / info --------------------------------------------------------------------------------------------
static u32 put_name(const char *s, u64 out, u64 pcch, int wide) {   // cch in/out excludes the NUL on success
    u32 cap = LD32(pcch);
    size_t need = wide ? wx_u2w(s, 0, 0) : strlen(s) + 1;
    if (!out || cap < need) { return E_MOREDATA; }
    if (wide) wx_u2w(s, out, cap); else { char a[1024]; size_t o = 0;   // UTF-8 -> Latin-1
        for (const u8 *p = (const u8 *)s; *p && o + 1 < sizeof a; ) { u32 cp = *p++; if (cp >= 0xC0 && *p) { cp = (cp & 31) << 6 | (*p++ & 63); } a[o++] = cp < 256 ? (char)cp : '?'; }
        a[o] = 0; memcpy((void *)out, a, o + 1); need = o + 1; }
    ST32(pcch, (u32)need - 1);
    return OK;
}
static u32 enum_key(u64 hkey, u32 idx, u64 name, u64 pcch, u64 ft, int wide) {
    u32 err = 0;
    pthread_mutex_lock(&reg_lock);
    RKey *k = key_of(hkey, &err);
    if (!k) { pthread_mutex_unlock(&reg_lock); return err; }
    RKey *c = k->kids; for (u32 i = 0; c && i < idx; i++) c = c->next;
    u32 r = c ? put_name(c->name, name, pcch, wide) : E_NOMORE;
    if (c && !r && ft) ST64(ft, c->mtime);
    pthread_mutex_unlock(&reg_lock);
    return r;
}
SHIM(RegEnumKeyExA) { RET(enum_key(A1, (u32)A2, A3, A4, AN(8), 0)); }   // (hkey, idx, name, *cch, reserved, class, *cchClass, *ft)
SHIM(RegEnumKeyExW) { RET(enum_key(A1, (u32)A2, A3, A4, AN(8), 1)); }
SHIM(RegEnumKeyA) { u32 cch = (u32)A4; RET(enum_key(A1, (u32)A2, A3, (u64)&cch, 0, 0)); }   // (hkey, idx, name, cch)
SHIM(RegEnumKeyW) { u32 cch = (u32)A4; RET(enum_key(A1, (u32)A2, A3, (u64)&cch, 0, 1)); }

static u32 enum_value(u64 hkey, u32 idx, u64 name, u64 pcch, u64 ptype, u64 data, u64 pcb, int wide) {
    u32 err = 0;
    pthread_mutex_lock(&reg_lock);
    RKey *k = key_of(hkey, &err);
    if (!k) { pthread_mutex_unlock(&reg_lock); return err; }
    RVal *v = k->vals; for (u32 i = 0; v && i < idx; i++) v = v->next;
    if (!v) { pthread_mutex_unlock(&reg_lock); return E_NOMORE; }
    u32 r = put_name(v->name, name, pcch, wide);
    if (!r) {
        if (ptype) ST32(ptype, v->type);
        u32 n; u8 *p = val_view(v, wide, &n);
        r = copy_out(p, n, data, pcb);
        if (p != v->data) free(p);
    }
    pthread_mutex_unlock(&reg_lock);
    return r;
}
SHIM(RegEnumValueA) { RET(enum_value(A1, (u32)A2, A3, A4, AN(6), AN(7), AN(8), 0)); }   // (hkey, idx, name, *cch, reserved, *type, data, *cb)
SHIM(RegEnumValueW) { RET(enum_value(A1, (u32)A2, A3, A4, AN(6), AN(7), AN(8), 1)); }

static u32 query_info(Ctx *c, int wide) {   // (hkey, class, *cchClass, reserved, *subkeys, *maxSubLen, *maxClassLen, *values, *maxNameLen, *maxValueLen, *secdesc, *ft)
    u32 err = 0;
    pthread_mutex_lock(&reg_lock);
    RKey *k = key_of(A1, &err);
    if (!k) { pthread_mutex_unlock(&reg_lock); return err; }
    u32 nk = 0, maxk = 0, nv = 0, maxn = 0, maxv = 0;
    for (RKey *s = k->kids; s; s = s->next) { nk++; u32 l = wide ? (u32)wx_u2w(s->name, 0, 0) - 1 : (u32)strlen(s->name); if (l > maxk) maxk = l; }
    for (RVal *v = k->vals; v; v = v->next) {
        nv++; u32 l = wide ? (u32)wx_u2w(v->name, 0, 0) - 1 : (u32)strlen(v->name); if (l > maxn) maxn = l;
        u32 sz = (!wide && is_str(v->type)) ? v->size / 2 : v->size; if (sz > maxv) maxv = sz;
    }
    if (A2 && A3 && LD32(A3)) { if (wide) ST16(A2, 0); else ST8(A2, 0); }
    if (A3) ST32(A3, 0);
    if (AN(5)) ST32(AN(5), nk); if (AN(6)) ST32(AN(6), maxk); if (AN(7)) ST32(AN(7), 0);
    if (AN(8)) ST32(AN(8), nv); if (AN(9)) ST32(AN(9), maxn); if (AN(10)) ST32(AN(10), maxv);
    if (AN(11)) ST32(AN(11), 0); if (AN(12)) ST64(AN(12), k->mtime);
    pthread_mutex_unlock(&reg_lock);
    return OK;
}
SHIM(RegQueryInfoKeyA) { RET(query_info(c, 0)); }
SHIM(RegQueryInfoKeyW) { RET(query_info(c, 1)); }

// ---- shlwapi registry helpers (SHGetValue & co.) -------------------------------------------------------------------
SHIM(SHGetValueW) { RET(get_value(A1, A2, A3, 0x1000FFFF, A4, AN(5), AN(6), 1)); }   // (hkey, sub, value, *type, data, *cb)
SHIM(SHGetValueA) { RET(get_value(A1, A2, A3, 0x1000FFFF, A4, AN(5), AN(6), 0)); }
SHIM(SHRegGetValueW) { RET(get_value(A1, A2, A3, (u32)A4, AN(5), AN(6), AN(7), 1)); }   // (hkey, sub, value, flags, *type, data, *cb)
SHIM(SHRegGetValueA) { RET(get_value(A1, A2, A3, (u32)A4, AN(5), AN(6), AN(7), 0)); }
SHIM(SHSetValueW) { RET(set_key_value(A1, A2, A3, (u32)A4, AN(5), (u32)AN(6), 1)); }   // (hkey, sub, value, type, data, cb)
SHIM(SHSetValueA) { RET(set_key_value(A1, A2, A3, (u32)A4, AN(5), (u32)AN(6), 0)); }
SHIM(SHDeleteKeyW) { RET(delete_key(A1, A2, 1, 1)); }
SHIM(SHDeleteKeyA) { RET(delete_key(A1, A2, 0, 1)); }
SHIM(SHDeleteValueW) { u64 h = A1; u32 r = 0; if (A2 && LD16(A2)) r = reg_open(A1, A2, 1, (u64)&h, 0, 0); if (r) RET(r); r = delete_value(h, A3, 1); if (h != A1) rt_handle_close(h); RET(r); }

// ---- random numbers ------------------------------------------------------------------------------------------------
SHIM(SystemFunction036) { arc4random_buf((void *)A1, (u32)A2); RET(1); }   // RtlGenRandom(buf, len)
SHIM(CryptAcquireContextA) { if (A1) ST64(A1, 0xC0DE0001); RET(1); }       // (*prov, container, provider, type, flags)
SHIM(CryptAcquireContextW) { if (A1) ST64(A1, 0xC0DE0001); RET(1); }
SHIM(CryptReleaseContext) { RET(1); }
SHIM(CryptGenRandom) { arc4random_buf((void *)A3, (u32)A2); RET(1); }      // (prov, len, buf)
SHIM(CryptEnumProvidersA) { rt_set_last_error(E_NOMORE); RET(0); }
SHIM(CryptEnumProvidersW) { rt_set_last_error(E_NOMORE); RET(0); }

// ---- hashing (CryptoAPI + CNG) on CommonCrypto ---------------------------------------------------------------------
enum { HA_MD5 = 1, HA_SHA1, HA_SHA256, HA_SHA384, HA_SHA512 };
typedef struct {
    int alg, hmac;
    union { CC_MD5_CTX md5; CC_SHA1_CTX sha1; CC_SHA256_CTX sha256; CC_SHA512_CTX sha512; } u;
    CCHmacContext hm;
} Hash;
static u32 hash_len(int a) { return a == HA_MD5 ? 16 : a == HA_SHA1 ? 20 : a == HA_SHA256 ? 32 : a == HA_SHA384 ? 48 : 64; }
static CCHmacAlgorithm hmac_alg(int a) { return a == HA_MD5 ? kCCHmacAlgMD5 : a == HA_SHA1 ? kCCHmacAlgSHA1 : a == HA_SHA256 ? kCCHmacAlgSHA256 : a == HA_SHA384 ? kCCHmacAlgSHA384 : kCCHmacAlgSHA512; }
static void hash_init(Hash *h, int alg, const void *key, size_t klen) {
    memset(h, 0, sizeof *h); h->alg = alg;
    if (key) { h->hmac = 1; CCHmacInit(&h->hm, hmac_alg(alg), key, klen); return; }
    switch (alg) { case HA_MD5: CC_MD5_Init(&h->u.md5); break; case HA_SHA1: CC_SHA1_Init(&h->u.sha1); break;
                   case HA_SHA256: CC_SHA256_Init(&h->u.sha256); break; case HA_SHA384: CC_SHA384_Init(&h->u.sha512); break;
                   default: CC_SHA512_Init(&h->u.sha512); }
}
static void hash_update(Hash *h, const void *p, size_t n) {
    if (h->hmac) { CCHmacUpdate(&h->hm, p, n); return; }
    switch (h->alg) { case HA_MD5: CC_MD5_Update(&h->u.md5, p, (CC_LONG)n); break; case HA_SHA1: CC_SHA1_Update(&h->u.sha1, p, (CC_LONG)n); break;
                      case HA_SHA256: CC_SHA256_Update(&h->u.sha256, p, (CC_LONG)n); break; case HA_SHA384: CC_SHA384_Update(&h->u.sha512, p, (CC_LONG)n); break;
                      default: CC_SHA512_Update(&h->u.sha512, p, (CC_LONG)n); }
}
static void hash_final(Hash *h, u8 *out) {   // on a copy: CryptGetHashParam may be called repeatedly
    Hash t = *h;
    if (t.hmac) { CCHmacFinal(&t.hm, out); return; }
    switch (t.alg) { case HA_MD5: CC_MD5_Final(out, &t.u.md5); break; case HA_SHA1: CC_SHA1_Final(out, &t.u.sha1); break;
                     case HA_SHA256: CC_SHA256_Final(out, &t.u.sha256); break; case HA_SHA384: CC_SHA384_Final(out, &t.u.sha512); break;
                     default: CC_SHA512_Final(out, &t.u.sha512); }
}
static int calg(u32 id) { return id == 0x8003 ? HA_MD5 : id == 0x8004 ? HA_SHA1 : id == 0x800c ? HA_SHA256 : id == 0x800d ? HA_SHA384 : id == 0x800e ? HA_SHA512 : 0; }
SHIM(CryptCreateHash) {  // (prov, algid, key, flags, *hash)
    int a = calg((u32)A2);
    if (!a || A3) { rt_set_last_error(0x80090008); RET(0); }   // NTE_BAD_ALGID
    Hash *h = malloc(sizeof *h); hash_init(h, a, NULL, 0); ST64(AN(5), (u64)h); RET(1);
}
SHIM(CryptHashData) { hash_update((Hash *)A1, (void *)A2, (u32)A3); RET(1); }   // (hash, data, len, flags)
SHIM(CryptGetHashParam) {  // (hash, param, data, *len, flags): HP_ALGID 1, HP_HASHVAL 2, HP_HASHSIZE 4
    Hash *h = (Hash *)A1; u32 p = (u32)A2, n = hash_len(h->alg);
    u8 buf[64]; u32 sz;
    if (p == 2) { hash_final(h, buf); sz = n; }
    else if (p == 4) { memcpy(buf, &n, 4); sz = 4; }
    else if (p == 1) { u32 id = h->alg == HA_MD5 ? 0x8003 : h->alg == HA_SHA1 ? 0x8004 : h->alg == HA_SHA256 ? 0x800c : h->alg == HA_SHA384 ? 0x800d : 0x800e; memcpy(buf, &id, 4); sz = 4; }
    else { rt_set_last_error(0x80090009); RET(0); }   // NTE_BAD_FLAGS... (unsupported parameter)
    if (!A3) { ST32(A4, sz); RET(1); }
    if (LD32(A4) < sz) { ST32(A4, sz); rt_set_last_error(E_MOREDATA); RET(0); }
    memcpy((void *)A3, buf, sz); ST32(A4, sz); RET(1);
}
SHIM(CryptDestroyHash) { free((void *)A1); RET(1); }

typedef struct { int alg, hmac, rng; } BAlg;
#define STATUS_NOT_FOUND 0xC0000225u
#define STATUS_INVALID_PARAMETER 0xC000000Du
#define STATUS_NOT_SUPPORTED 0xC00000BBu
#define STATUS_BUFFER_TOO_SMALL 0xC0000023u
SHIM(BCryptOpenAlgorithmProvider) {  // (*alg, id, impl, flags)
    char id[64]; wx_w2u(A2, id, sizeof id);
    BAlg a = { 0, ((u32)A4 & 8) != 0, 0 };
    if (!strcmp(id, "RNG") || !strcmp(id, "FIPS186DSARNG") || !strcmp(id, "DUALECRNG")) a.rng = 1;
    else if (!strcmp(id, "MD5")) a.alg = HA_MD5; else if (!strcmp(id, "SHA1")) a.alg = HA_SHA1;
    else if (!strcmp(id, "SHA256")) a.alg = HA_SHA256; else if (!strcmp(id, "SHA384")) a.alg = HA_SHA384;
    else if (!strcmp(id, "SHA512")) a.alg = HA_SHA512;
    else { wx_stub_log("BCryptOpenAlgorithmProvider(non-hash algorithm)"); ST64(A1, 0); RET(STATUS_NOT_FOUND); }
    BAlg *p = malloc(sizeof *p); *p = a; ST64(A1, (u64)p); RET(0);
}
SHIM(BCryptCloseAlgorithmProvider) { free((void *)A1); RET(0); }
SHIM(BCryptGenRandom) {  // (alg, buf, len, flags): BCRYPT_USE_SYSTEM_PREFERRED_RNG allows a NULL algorithm
    if (!A1 && !((u32)A4 & 2)) RET(STATUS_INVALID_PARAMETER);
    arc4random_buf((void *)A2, (u32)A3); RET(0);
}
SHIM(BCryptGetProperty) {  // (handle, name, out, cb, *result, flags)
    char nm[64]; wx_w2u(A2, nm, sizeof nm);
    BAlg *a = (BAlg *)A1; u32 v;
    if (!strcmp(nm, "ObjectLength")) v = (u32)sizeof(Hash);
    else if (!strcmp(nm, "HashDigestLength") && a && a->alg) v = hash_len(a->alg);
    else if (!strcmp(nm, "HashBlockLength") && a && a->alg) v = a->alg >= HA_SHA384 ? 128 : 64;
    else RET(STATUS_NOT_SUPPORTED);
    if (AN(5)) ST32(AN(5), 4);
    if (!A3) RET(0);
    if ((u32)A4 < 4) RET(STATUS_BUFFER_TOO_SMALL);
    ST32(A3, v); RET(0);
}
SHIM(BCryptSetProperty) { WX_FAIL(BCryptSetProperty, 50, STATUS_NOT_SUPPORTED); }
// Hash objects live in host memory we allocate (the caller's pbHashObject buffer is accepted but not needed).
SHIM(BCryptCreateHash) {  // (alg, *hash, obj, cbObj, secret, cbSecret, flags)
    BAlg *a = (BAlg *)A1;
    if (!a || !a->alg) RET(STATUS_INVALID_PARAMETER);
    Hash *h = malloc(sizeof *h);
    hash_init(h, a->alg, a->hmac ? (void *)(AN(5) ? AN(5) : (u64)"") : NULL, a->hmac ? (u32)AN(6) : 0);
    ST64(A2, (u64)h); RET(0);
}
SHIM(BCryptHashData) { hash_update((Hash *)A1, (void *)A2, (u32)A3); RET(0); }   // (hash, in, cb, flags)
SHIM(BCryptFinishHash) {  // (hash, out, cb, flags)
    Hash *h = (Hash *)A1; if ((u32)A3 != hash_len(h->alg)) RET(STATUS_INVALID_PARAMETER);
    hash_final(h, (u8 *)A2); RET(0);
}
SHIM(BCryptDuplicateHash) { Hash *h = malloc(sizeof *h); *h = *(Hash *)A1; ST64(A2, (u64)h); RET(0); }   // (hash, *new, obj, cb, flags)
SHIM(BCryptDestroyHash) { free((void *)A1); RET(0); }
SHIM(BCryptGenerateSymmetricKey) { WX_FAIL(BCryptGenerateSymmetricKey, 50, STATUS_NOT_SUPPORTED); }
SHIM(BCryptEncrypt) { WX_FAIL(BCryptEncrypt, 50, STATUS_NOT_SUPPORTED); }
SHIM(BCryptDecrypt) { WX_FAIL(BCryptDecrypt, 50, STATUS_NOT_SUPPORTED); }
SHIM(BCryptDestroyKey) { RET(0); }

// ---- user / token / SIDs ---------------------------------------------------------------------------------------------
static const char *user_name(void) { return "wintomac"; }   // matches the C:\users\wintomac profile folder
SHIM(GetUserNameW) {  // (buf, *cch incl. NUL)
    u32 need = (u32)wx_u2w(user_name(), 0, 0), cap = LD32(A2); ST32(A2, need);
    if (!A1 || cap < need) { rt_set_last_error(E_INSUFF); RET(0); }
    wx_u2w(user_name(), A1, cap); RET(1);
}
SHIM(GetUserNameA) {
    u32 need = (u32)strlen(user_name()) + 1, cap = LD32(A2); ST32(A2, need);
    if (!A1 || cap < need) { rt_set_last_error(E_INSUFF); RET(0); }
    memcpy((void *)A1, user_name(), need); RET(1);
}
// TokenUser 1, TokenElevationType 18, TokenElevation 20: a standard, non-elevated user.
SHIM(GetTokenInformation) {  // (token, class, info, len, *retlen)
    u32 cls = (u32)A2, need;
    static const u8 user_sid[] = { 1, 5, 0, 0, 0, 0, 0, 5, 21, 0, 0, 0, 0x2a, 0, 0, 0, 0x2b, 0, 0, 0, 0x2c, 0, 0, 0, 0xe9, 3, 0, 0 };   // S-1-5-21-42-43-44-1001
    if (cls == 20 || cls == 18) need = 4;
    else if (cls == 1) need = 16 + sizeof user_sid;
    else WX_FAIL(GetTokenInformation(class), E_PARAM, 0);
    if (AN(5)) ST32(AN(5), need);
    if ((u32)A4 < need || !A3) { rt_set_last_error(E_INSUFF); RET(0); }
    if (cls == 20) ST32(A3, 0);
    else if (cls == 18) ST32(A3, 1);   // TokenElevationTypeDefault
    else { ST64(A3, A3 + 16); ST32(A3 + 8, 0); memcpy((void *)(A3 + 16), user_sid, sizeof user_sid); }
    RET(1);
}
SHIM(DuplicateToken) { ST64(A3, A1); RET(1); }
SHIM(CheckTokenMembership) { if (A3) ST32(A3, 0); RET(1); }   // not a member of (e.g.) Administrators
// SID: { u8 revision=1, u8 count, u8 authority[6], u32 sub[count] }
SHIM(AllocateAndInitializeSid) {  // (auth*, count, s0..s7, *sid)
    u8 n = (u8)A2; if (n > 8) { rt_set_last_error(E_PARAM); RET(0); }
    u8 *s = calloc(1, 8 + 4 * 8); s[0] = 1; s[1] = n; memcpy(s + 2, (void *)A1, 6);
    u32 sub[8] = { (u32)A3, (u32)A4, (u32)AN(5), (u32)AN(6), (u32)AN(7), (u32)AN(8), (u32)AN(9), (u32)AN(10) };
    memcpy(s + 8, sub, 4 * n); ST64(AN(11), (u64)s); RET(1);
}
SHIM(FreeSid) { free((void *)A1); RET(0); }
SHIM(GetLengthSid) { RET(8 + 4 * LD8(A1 + 1)); }
SHIM(IsValidSid) { RET(A1 && LD8(A1) == 1 && LD8(A1 + 1) <= 15); }
SHIM(EqualSid) { u32 n = 8 + 4 * LD8(A1 + 1); RET(n == 8 + 4 * LD8(A2 + 1) && !memcmp((void *)A1, (void *)A2, n)); }
SHIM(CopySid) { u32 n = 8 + 4 * LD8(A3 + 1); if ((u32)A1 < n) { rt_set_last_error(E_INSUFF); RET(0); } memcpy((void *)A2, (void *)A3, n); RET(1); }
SHIM(GetSidSubAuthority) { RET(A1 + 8 + 4 * (u32)A2); }
SHIM(GetSidSubAuthorityCount) { RET(A1 + 1); }
SHIM(GetSidIdentifierAuthority) { RET(A1 + 2); }
SHIM(InitializeSid) { ST8(A1, 1); ST8(A1 + 1, (u8)A3); memcpy((void *)(A1 + 2), (void *)A2, 6); RET(1); }
// Security descriptors and ACLs are accepted and ignored (macOS permissions are the user's own anyway).
SHIM(InitializeSecurityDescriptor) { memset((void *)A1, 0, 40); ST8(A1, 1); RET(1); }   // SECURITY_DESCRIPTOR (absolute, 40 bytes)
SHIM(SetSecurityDescriptorDacl) { RET(1); }
SHIM(SetSecurityDescriptorOwner) { RET(1); }
SHIM(SetSecurityDescriptorGroup) { RET(1); }
SHIM(InitializeAcl) { memset((void *)A1, 0, 8); ST8(A1, (u8)A3); ST16(A1 + 2, (u16)A2); RET(1); }   // ACL {rev, 0, size, count, 0}
SHIM(AddAccessAllowedAce) { RET(1); }
SHIM(AddAccessAllowedAceEx) { RET(1); }
SHIM(AddAccessDeniedAceEx) { RET(1); }
SHIM(GetSecurityInfo) { WX_FAIL(GetSecurityInfo, 50, 50); }          // returns the error code (ERROR_NOT_SUPPORTED)
SHIM(GetNamedSecurityInfoW) { WX_FAIL(GetNamedSecurityInfoW, 50, 50); }
SHIM(LookupAccountSidW) { WX_FAIL(LookupAccountSidW, 1332, 0); }      // ERROR_NONE_MAPPED
SHIM(LookupAccountNameW) { WX_FAIL(LookupAccountNameW, 1332, 0); }
SHIM(LookupPrivilegeValueW) { ST32(A3, 20); ST32(A3 + 4, 0); RET(1); } // LUID of SeDebugPrivilege-like; AdjustTokenPrivileges ignores it
SHIM(GetCurrentHwProfileA) {  // HW_PROFILE_INFOA { DWORD dockinfo; char guid[39]; char name[80] }
    memset((void *)A1, 0, 4 + 39 + 80); ST32(A1, 4);   // DOCKINFO_USER_SUPPLIED... "undocked"
    strcpy((char *)A1 + 4, "{8c9a1f2e-0000-4000-8000-77696e746f6d}"); strcpy((char *)A1 + 43, "Profile 1"); RET(1);
}

const Shim shims_advapi32[] = {
#define E(n) { NULL, #n, sh_##n },
    E(RegOpenKeyExA) E(RegOpenKeyExW) E(RegOpenKeyA) E(RegOpenKeyW) E(RegCreateKeyExA) E(RegCreateKeyExW) E(RegCreateKeyA)
    E(RegCreateKeyW) E(RegOpenCurrentUser) E(RegCloseKey) E(RegFlushKey) E(RegNotifyChangeKeyValue) E(RegDisablePredefinedCache)
    E(RegQueryValueExA) E(RegQueryValueExW) E(RegGetValueA) E(RegGetValueW) E(RegSetValueExA) E(RegSetValueExW) E(RegSetKeyValueA)
    E(RegSetKeyValueW) E(RegDeleteValueA) E(RegDeleteValueW) E(RegDeleteKeyA) E(RegDeleteKeyW) E(RegDeleteKeyExA) E(RegDeleteKeyExW)
    E(RegDeleteTreeA) E(RegDeleteTreeW) E(RegEnumKeyExA) E(RegEnumKeyExW) E(RegEnumKeyA) E(RegEnumKeyW) E(RegEnumValueA)
    E(RegEnumValueW) E(RegQueryInfoKeyA) E(RegQueryInfoKeyW)
    E(SystemFunction036) E(CryptAcquireContextA) E(CryptAcquireContextW) E(CryptReleaseContext) E(CryptGenRandom)
    E(CryptEnumProvidersA) E(CryptEnumProvidersW) E(CryptCreateHash) E(CryptHashData) E(CryptGetHashParam) E(CryptDestroyHash)
    E(BCryptOpenAlgorithmProvider) E(BCryptCloseAlgorithmProvider) E(BCryptGenRandom) E(BCryptGetProperty) E(BCryptSetProperty)
    E(BCryptCreateHash) E(BCryptHashData) E(BCryptFinishHash) E(BCryptDuplicateHash) E(BCryptDestroyHash)
    E(BCryptGenerateSymmetricKey) E(BCryptEncrypt) E(BCryptDecrypt) E(BCryptDestroyKey)
    E(GetUserNameW) E(GetUserNameA) E(GetTokenInformation) E(DuplicateToken) E(CheckTokenMembership) E(AllocateAndInitializeSid)
    E(FreeSid) E(GetLengthSid) E(IsValidSid) E(EqualSid) E(CopySid) E(GetSidSubAuthority) E(GetSidSubAuthorityCount)
    E(GetSidIdentifierAuthority) E(InitializeSid) E(InitializeSecurityDescriptor) E(SetSecurityDescriptorDacl)
    E(SetSecurityDescriptorOwner) E(SetSecurityDescriptorGroup) E(InitializeAcl) E(AddAccessAllowedAce) E(AddAccessAllowedAceEx)
    E(AddAccessDeniedAceEx) E(GetSecurityInfo) E(GetNamedSecurityInfoW) E(LookupAccountSidW) E(LookupAccountNameW)
    E(LookupPrivilegeValueW) E(GetCurrentHwProfileA) E(SHGetValueW) E(SHGetValueA) E(SHRegGetValueW) E(SHRegGetValueA) E(SHSetValueW)
    E(SHSetValueA) E(SHDeleteKeyW) E(SHDeleteKeyA) E(SHDeleteValueW)
#undef E
    { 0, 0, 0 }
};
