// Per-user storage root for C:\users\... (AppData, Saved Games, Documents) and the HKCU registry file.
//
//   ~/Library/Application Support/wintomac              plain `build/x.bin game.exe` runs (shared by all games)
//   ~/Library/Application Support/wintomac/<AppName>    WINTOMAC_APPNAME=<AppName>, or when the binary runs from
//                                                       inside <AppName>.app (tools/package_app.py sets both)
// so each packaged game keeps its own saves and registry.
#include <limits.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "win32.h"

static char dir[1100];
static void compute(void) {
    const char *home = getenv("HOME");
    char base[1024]; snprintf(base, sizeof base, "%s/Library/Application Support/wintomac", home && *home ? home : "/tmp");
    char name[256] = "";
    const char *env = getenv("WINTOMAC_APPNAME");
    if (env && *env) snprintf(name, sizeof name, "%s", env);
    else {   // inside a bundle: .../<Name>.app/Contents/MacOS/<binary>
        char exe[PATH_MAX]; uint32_t n = sizeof exe;
        if (!_NSGetExecutablePath(exe, &n)) {
            char *m = strstr(exe, ".app/Contents/MacOS/");
            if (m) { *m = 0; char *s = strrchr(exe, '/'); snprintf(name, sizeof name, "%s", s ? s + 1 : exe); }
        }
    }
    for (char *p = name; *p; p++) if (*p == '/' || *p == ':' || (*p == '.' && p == name)) *p = '_';   // one path component
    mkdir(base, 0755);
    if (*name) { snprintf(dir, sizeof dir, "%s/%s", base, name); mkdir(dir, 0755); }
    else snprintf(dir, sizeof dir, "%s", base);
}
const char *rt_user_data_dir(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, compute);
    return dir;
}
