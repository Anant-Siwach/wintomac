#!/usr/bin/env python3
"""Package a recompiled Windows game as a self-contained macOS .app (see docs/packaging.md).

    tools/package_app.py <game_dir> <exe> [dlls...] --name "Game" [--args "..."] [--icon path] [--link-data]
                         [--bundle-id id] [--version 1.0] [--out dist] [--recompile] [--microphone] [--no-sign]

  1. recompile <game_dir>/<exe> (+ dlls) into build/<slug>.gen, unless build/<slug>.gen/report.json exists
     (--recompile forces it);
  2. link it against the current runtime (`make link`) into build/<slug>.bin;
  3. build dist/<Name>.app:
       Contents/MacOS/<Name>            launcher script (CFBundleExecutable): cds into the game folder, sets the
                                        Vulkan ICD + WINTOMAC_APPNAME, execs the binary with ./<exe> [args] "$@"
       Contents/MacOS/<slug>.bin        the recompiled game
       Contents/Frameworks/*.dylib      SDL3, FAudio, Vulkan loader, MoltenVK, vkd3d, ... with @rpath install names
       Contents/Resources/game/         the game's files (copied; --link-data references <game_dir> in place)
       Contents/Resources/vulkan/icd.d/MoltenVK_icd.json
       Contents/Resources/<Name>.icns   from the exe's RT_GROUP_ICON (or --icon, or the generic app icon)
       Contents/Info.plist
  4. ad-hoc sign everything inside-out (codesign -s -), with the com.apple.security.cs.allow-jit entitlement.

Input files are only parsed (PE resources) and copied, never executed. Run with `python3 -I`.
"""
import argparse
import os
import plistlib
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYSTEM_PREFIXES = ("/usr/lib/", "/System/")
GENERIC_ICON = "/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/GenericApplicationIcon.icns"


def log(msg):
    print(f"[package] {msg}", flush=True)


def run(cmd, **kw):
    r = subprocess.run(cmd, **kw)
    if r.returncode:
        sys.exit(f"[package] command failed ({r.returncode}): {' '.join(shlex.quote(c) for c in cmd)}")
    return r


def out(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def brew_prefix():
    try:
        return out(["brew", "--prefix"]).strip()
    except (OSError, subprocess.CalledProcessError):
        return "/opt/homebrew"


# ---------------------------------------------------------------------------------------------------- icon
def pe_icon(path):
    """Largest icon of the first RT_GROUP_ICON in a PE file, as (bytes, is_png), or None."""
    data = open(path, "rb").read()
    if data[:2] != b"MZ":
        return None
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        return None
    nsec, optsize = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    dd = opt + (112 if magic == 0x20B else 96)
    rsrc_rva, rsrc_size = struct.unpack_from("<II", data, dd + 2 * 8)
    if not rsrc_rva:
        return None
    secs = []
    for i in range(nsec):
        s = pe + 24 + optsize + 40 * i
        vsize, va, rawsize, raw = struct.unpack_from("<IIII", data, s + 8)
        secs.append((va, max(vsize, rawsize), raw))

    def off(rva):
        for va, size, raw in secs:
            if va <= rva < va + size:
                return raw + rva - va
        raise ValueError("rva outside sections")

    base = off(rsrc_rva)

    def entries(dir_off):
        named, ids = struct.unpack_from("<HH", data, base + dir_off + 12)
        for k in range(named + ids):
            name, target = struct.unpack_from("<II", data, base + dir_off + 16 + 8 * k)
            yield (None if name & 0x80000000 else name), target

    def leaf(target):   # descend first entries until a data entry
        while target & 0x80000000:
            target = next(entries(target & 0x7FFFFFFF))[1]
        rva, size = struct.unpack_from("<II", data, base + target)
        return data[off(rva):off(rva) + size]

    types = dict(entries(0))
    if 14 not in types or 3 not in types:
        return None
    group = leaf(next(entries(types[14] & 0x7FFFFFFF))[1])
    icons = {i: t for i, t in entries(types[3] & 0x7FFFFFFF) if i is not None}
    count = struct.unpack_from("<H", group, 4)[0]
    best = None
    for k in range(count):
        w, h, _, _, _, bpp, size, rid = struct.unpack_from("<BBBBHHIH", group, 6 + 14 * k)
        w, h = w or 256, h or 256
        if rid in icons and (best is None or (w * h, bpp) > best[0]):
            best = ((w * h, bpp), rid, w, h, bpp)
    if not best:
        return None
    _, rid, w, h, bpp = best
    img = leaf(icons[rid])
    if img[:8] == b"\x89PNG\r\n\x1a\n":
        return img, True
    # DIB: wrap as a one-image .ico so ImageIO (sips) can read it
    ico = struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHII", w % 256, h % 256, 0, 0, 1, bpp, len(img), 22) + img
    return ico, False


def make_icns(src_img, is_png, dest, tmp):
    src = os.path.join(tmp, "icon." + ("png" if is_png else "ico"))
    open(src, "wb").write(src_img)
    png = os.path.join(tmp, "icon.png")
    if subprocess.run(["sips", "-s", "format", "png", src, "--out", png], capture_output=True).returncode:
        return False
    iconset = os.path.join(tmp, "app.iconset")
    os.makedirs(iconset, exist_ok=True)
    for size in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            px = size * scale
            name = f"icon_{size}x{size}{'@2x' if scale == 2 else ''}.png"
            if subprocess.run(["sips", "-z", str(px), str(px), png, "--out", os.path.join(iconset, name)], capture_output=True).returncode:
                return False
    return subprocess.run(["iconutil", "-c", "icns", iconset, "-o", dest], capture_output=True).returncode == 0


def install_icon(args, exe_path, dest, tmp):
    if args.icon:
        ext = os.path.splitext(args.icon)[1].lower()
        if ext == ".icns":
            shutil.copy(args.icon, dest)
            return "from --icon"
        if make_icns(open(args.icon, "rb").read(), ext == ".png", dest, tmp):
            return "from --icon"
        log(f"warning: could not convert {args.icon}; trying the exe's icon")
    try:
        ic = pe_icon(exe_path)
    except (ValueError, struct.error, StopIteration):
        ic = None
    if ic and make_icns(ic[0], ic[1], dest, tmp):
        return "extracted from the exe (RT_GROUP_ICON)"
    if os.path.exists(GENERIC_ICON):
        shutil.copy(GENERIC_ICON, dest)
        return "generic application icon (the exe has none)"
    return None


# ---------------------------------------------------------------------------------------------- dylibs
def load_commands(path):
    """(dependencies, rpaths) of a Mach-O file."""
    deps = [l.strip().split(" (compatibility")[0] for l in out(["otool", "-L", path]).splitlines()[1:] if l.strip()]
    rpaths, lines = [], out(["otool", "-l", path]).splitlines()
    for i, l in enumerate(lines):
        if l.strip() == "cmd LC_RPATH":
            for m in lines[i + 1:i + 4]:
                if m.strip().startswith("path "):
                    rpaths.append(m.strip()[5:].split(" (offset")[0])
    return deps, rpaths


def install_id(path):
    o = out(["otool", "-D", path]).splitlines()
    return o[1].strip() if len(o) > 1 else None


def resolve(ref, loader, rpaths, exe_dir):
    if ref.startswith("@rpath/"):
        for rp in rpaths:
            rp = rp.replace("@loader_path", os.path.dirname(loader)).replace("@executable_path", exe_dir)
            cand = os.path.join(rp, ref[7:])
            if os.path.exists(cand):
                return cand
        cand = os.path.join(brew_prefix(), "lib", ref[7:])
        return cand if os.path.exists(cand) else None
    if ref.startswith("@loader_path/"):
        return os.path.join(os.path.dirname(loader), ref[13:])
    if ref.startswith("@executable_path/"):
        return os.path.join(exe_dir, ref[17:])
    return ref if os.path.exists(ref) else None


def bundle_dylibs(binary, frameworks, extra):
    """Copy every non-system dylib the binary (and `extra` libraries) needs into frameworks; rewrite references."""
    exe_dir = os.path.dirname(binary)
    copied = {}          # basename -> source path
    queue = [(binary, None)] + [(e, None) for e in extra]
    fixups = {}          # Mach-O file in the bundle -> list of (old ref, new ref)
    while queue:
        src, dest = queue.pop()
        target = dest or src
        deps, rpaths = load_commands(src)
        own = install_id(src)
        fixups.setdefault(target, [])
        for ref in deps:
            if ref == own or ref.startswith(SYSTEM_PREFIXES):
                continue
            path = resolve(ref, src, rpaths, exe_dir)
            if not path:
                sys.exit(f"[package] cannot find {ref} (needed by {src})")
            name = os.path.basename(ref)
            fixups[target].append((ref, f"@rpath/{name}"))
            if name not in copied:
                copied[name] = os.path.realpath(path)
                queue.append((copied[name], os.path.join(frameworks, name)))
    for e in extra:
        name = os.path.basename(e)
        if name not in copied:
            copied[name] = os.path.realpath(e)
        fixups[os.path.join(frameworks, name)] = fixups.pop(e, [])
    os.makedirs(frameworks, exist_ok=True)
    for name, src in copied.items():
        d = os.path.join(frameworks, name)
        shutil.copy2(src, d)
        os.chmod(d, 0o755)
        run(["install_name_tool", "-id", f"@rpath/{name}", d], capture_output=True)
    for f, changes in fixups.items():
        if f == binary or f.startswith(frameworks):
            for old, new in changes:
                run(["install_name_tool", "-change", old, new, f], capture_output=True)
            _, rpaths = load_commands(f)
            for rp in rpaths:   # drop build-machine rpaths (Homebrew, the vkd3d build)
                run(["install_name_tool", "-delete_rpath", rp, f], capture_output=True)
            run(["install_name_tool", "-add_rpath", "@executable_path/../Frameworks" if f == binary else "@loader_path", f],
                capture_output=True)
    return sorted(copied)


def min_macos(files):
    best = (0,)
    for f in files:
        lines = out(["otool", "-l", f]).splitlines()
        for i, l in enumerate(lines):
            if l.strip() in ("cmd LC_BUILD_VERSION", "cmd LC_VERSION_MIN_MACOSX"):
                for m in lines[i + 1:i + 5]:
                    m = m.split()
                    if m and m[0] in ("minos", "version"):
                        best = max(best, tuple(int(x) for x in m[1].split(".")))
    return ".".join(str(x) for x in best) if best != (0,) else "11.0"


# ------------------------------------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game_dir")
    ap.add_argument("exe", help="the game's .exe, relative to game_dir")
    ap.add_argument("dlls", nargs="*", help="DLLs recompiled with the game (relative to game_dir or paths)")
    ap.add_argument("--name", required=True, help="app name (bundle name, Dock name, save-data folder)")
    ap.add_argument("--args", default="", help="extra command-line arguments for the game")
    ap.add_argument("--icon", help=".icns, .png or .ico to use instead of the exe's icon")
    ap.add_argument("--link-data", action="store_true", help="don't copy game data: the app runs the game from game_dir in place")
    ap.add_argument("--bundle-id", help="CFBundleIdentifier (default com.wintomac.<slug>)")
    ap.add_argument("--version", default="1.0")
    ap.add_argument("--out", default=os.path.join(ROOT, "dist"))
    ap.add_argument("--recompile", action="store_true", help="recompile even if build/<slug>.gen exists")
    ap.add_argument("--recomp-flags", default="", help="extra flags for `python -m recomp` (e.g. \"--no-callopt\"); implies --recompile")
    ap.add_argument("--microphone", action="store_true", help="add NSMicrophoneUsageDescription (only if the game needs a real mic)")
    ap.add_argument("--exclude", action="append", default=["*.pdb"], help="glob of game files not to copy (default: *.pdb)")
    ap.add_argument("--no-sign", action="store_true")
    a = ap.parse_args()

    game_dir = os.path.realpath(a.game_dir)
    exe_path = os.path.join(game_dir, a.exe)
    if not os.path.isfile(exe_path):
        sys.exit(f"[package] no such exe: {exe_path}")
    name = a.name
    if "/" in name or not name.strip():
        sys.exit("[package] --name must be a plain file name")
    slug = re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_") or "game"
    gen = os.path.join(ROOT, "build", f"{slug}.gen")
    binpath = os.path.join(ROOT, "build", f"{slug}.bin")

    # 1. recompile (a generated tree older than the recompiler or recomp.h no longer links)
    report = os.path.join(gen, "report.json")
    import glob
    stale = os.path.exists(report) and any(os.path.getmtime(f) > os.path.getmtime(report)
                                           for f in glob.glob(os.path.join(ROOT, "recomp", "*.py")) + [os.path.join(ROOT, "runtime", "recomp.h")])
    if stale:
        log(f"{os.path.relpath(gen, ROOT)} is older than the recompiler; recompiling")
    if a.recompile or a.recomp_flags or stale or not os.path.exists(report):
        dlls = [d if os.path.isabs(d) else os.path.join(game_dir, d) for d in a.dlls]
        log(f"recompiling {a.exe} + {len(dlls)} DLLs -> {os.path.relpath(gen, ROOT)}")
        run([os.path.join(ROOT, ".venv/bin/python"), "-m", "recomp", exe_path, *dlls, "-o", gen, *shlex.split(a.recomp_flags)], cwd=ROOT)
    else:
        log(f"reusing {os.path.relpath(gen, ROOT)} (--recompile to redo)")
    # 2. link against the current runtime
    log(f"linking {os.path.relpath(binpath, ROOT)}")
    run(["make", "-s", "link", f"GEN={gen}", f"OUT={binpath}"], cwd=ROOT)

    # 3. bundle
    app = os.path.join(os.path.realpath(a.out), f"{name}.app")
    contents = os.path.join(app, "Contents")
    macos, res, fw = (os.path.join(contents, d) for d in ("MacOS", "Resources", "Frameworks"))
    if os.path.exists(app):
        shutil.rmtree(app)
    for d in (macos, res, fw):
        os.makedirs(d)
    bundled_bin = os.path.join(macos, f"{slug}.bin")
    shutil.copy2(binpath, bundled_bin)

    # dylibs (+ MoltenVK, which the Vulkan loader opens through its ICD manifest)
    mvk = os.path.join(brew_prefix(), "lib", "libMoltenVK.dylib")
    extra = [mvk] if os.path.exists(mvk) else []
    libs = bundle_dylibs(bundled_bin, fw, extra)
    log(f"bundled {len(libs)} dylibs: {', '.join(libs)}")
    if extra:
        icd = os.path.join(res, "vulkan", "icd.d")
        os.makedirs(icd)
        with open(os.path.join(icd, "MoltenVK_icd.json"), "w") as f:
            f.write('{\n    "file_format_version": "1.0.0",\n    "ICD": {\n'
                    '        "library_path": "../../../Frameworks/libMoltenVK.dylib",\n'
                    '        "api_version": "1.4.0",\n        "is_portability_driver": true\n    }\n}\n')
    else:
        log("warning: MoltenVK not found; Vulkan/D3D games will not run on a Mac without it")

    # game data
    if a.link_data:
        game_ref = game_dir
        log(f"game data referenced in place: {game_dir}")
    else:
        import fnmatch
        dest = os.path.join(res, "game")

        def ignore(d, names):
            return [n for n in names if any(fnmatch.fnmatch(n, g) for g in a.exclude) or n in (".DS_Store",)]
        shutil.copytree(game_dir, dest, symlinks=False, ignore=ignore)
        game_ref = None
        log(f"copied game data ({sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(dest) for f in fs) >> 20} MB)")

    # launcher
    exe_q = shlex.quote("./" + a.exe.replace(os.sep, "/"))
    game_args = " ".join(shlex.quote(x) for x in shlex.split(a.args))
    cd = f'cd {shlex.quote(game_ref)}' if game_ref else 'cd "$res/game"'
    launcher = f"""#!/bin/sh
# {name}: Windows game recompiled to native arm64 by wintomac (tools/package_app.py).
here=$(cd "$(dirname "$0")" && pwd)
res="$here/../Resources"
{cd} || {{ echo "{name}: game data folder missing" >&2; exit 1; }}
export WINTOMAC_APPNAME="${{WINTOMAC_APPNAME:-{name}}}"   # saves + registry: ~/Library/Application Support/wintomac/<name>
if [ -f "$res/vulkan/icd.d/MoltenVK_icd.json" ]; then
  export VK_DRIVER_FILES="${{VK_DRIVER_FILES:-$res/vulkan/icd.d/MoltenVK_icd.json}}"
  export VK_ICD_FILENAMES="${{VK_ICD_FILENAMES:-$VK_DRIVER_FILES}}"
fi
for a in "$@"; do shift; case "$a" in -psn_*) ;; *) set -- "$@" "$a" ;; esac; done   # Finder's process serial number

# --- startup diagnostics: ~/Library/Logs/wintomac/<Name>.log (timestamped; set WINTOMAC_LOG=0 to disable) ---
app={shlex.quote(name)}
bin="$here/{slug}.bin"
logdir="$HOME/Library/Logs/wintomac"; log="$logdir/$app.log"
cachedir="$HOME/Library/Caches/wintomac/$app"
mkdir -p "$logdir" "$cachedir" 2>/dev/null
# First run on this Mac: MoltenVK/Metal compiles every shader pipeline before the first frame (vkQuake: ~40 s cold,
# ~4 s warm). The game shows nothing meanwhile, so tell the user; the Metal cache makes later launches fast.
if [ ! -f "$cachedir/first-run-done" ]; then
  echo "$app: first launch, compiling graphics shaders, this can take up to a minute; later launches are fast." >&2
  [ -z "$WINTOMAC_HEADLESS" ] && ( osascript -e 'on run argv' -e 'display notification "First launch compiles graphics shaders (up to a minute). Later launches are fast." with title (item 1 of argv)' -e 'end run' "$app" >/dev/null 2>&1 & ) 2>/dev/null
  : > "$cachedir/first-run-done"
fi
if [ "${{WINTOMAC_LOG:-1}}" = 0 ]; then exec "$bin" {exe_q} {game_args} "$@"; fi
if [ "$(wc -c < "$log" 2>/dev/null || echo 0)" -gt 2000000 ]; then mv -f "$log" "$log.old"; fi
printf '\n===== %s launch pid=%s args=%s =====\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$$" "$*" >> "$log"
if [ -t 1 ] || [ "${{WINTOMAC_TRACE_STARTUP:-0}}" = 1 ]; then
  # Per-line timestamps (seconds since launch) in the log; echoed to the terminal when there is one.
  fifo=$(mktemp -u "${{TMPDIR:-/tmp}}/wintomac.XXXXXX"); mkfifo "$fifo"
  tty=0; [ -t 1 ] && tty=1
  perl -MTime::HiRes=time -e '$|=1; $t0=time; $tty=shift; open L,">>",shift; select((select(L),$|=1)[0]);
    while(<STDIN>){{ $s=sprintf("[%7.2fs] %s",time-$t0,$_); print L $s; print STDOUT $_ if $tty }}' "$tty" "$log" < "$fifo" &
  "$bin" {exe_q} {game_args} "$@" > "$fifo" 2>&1 &
  child=$!
  trap 'kill -TERM $child 2>/dev/null' TERM INT HUP
  wait $child; rc=$?
  trap - TERM INT HUP; wait $child 2>/dev/null; rm -f "$fifo"
  printf '[exit] %s code=%s\n' "$(date '+%H:%M:%S')" "$rc" >> "$log"
  exit $rc
fi
# Finder launch: keep exec (the app process is the game); stdout/stderr go straight to the log.
exec "$bin" {exe_q} {game_args} "$@" >> "$log" 2>&1
"""
    exe_name = re.sub(r"[^A-Za-z0-9._-]+", "", name) or slug
    lpath = os.path.join(macos, exe_name)
    with open(lpath, "w") as f:
        f.write(launcher)
    os.chmod(lpath, 0o755)

    # icon
    with tempfile.TemporaryDirectory() as tmp:
        icon_src = install_icon(a, exe_path, os.path.join(res, f"{exe_name}.icns"), tmp)
    log(f"icon: {icon_src or 'none'}")

    # Info.plist
    plist = {
        "CFBundleName": name[:15], "CFBundleDisplayName": name, "CFBundleExecutable": exe_name,
        "CFBundleIdentifier": a.bundle_id or f"com.wintomac.{re.sub(r'[^A-Za-z0-9-]+', '-', slug)}",
        "CFBundlePackageType": "APPL", "CFBundleInfoDictionaryVersion": "6.0", "CFBundleShortVersionString": a.version,
        "CFBundleVersion": a.version, "CFBundleDevelopmentRegion": "en",
        "LSMinimumSystemVersion": min_macos([bundled_bin] + [os.path.join(fw, l) for l in libs]),
        "NSHighResolutionCapable": True, "LSApplicationCategoryType": "public.app-category.games",
        "NSSupportsAutomaticGraphicsSwitching": True,
    }
    if icon_src:
        plist["CFBundleIconFile"] = f"{exe_name}.icns"
    if a.microphone:
        plist["NSMicrophoneUsageDescription"] = f"{name} uses the microphone for voice input."
    with open(os.path.join(contents, "Info.plist"), "wb") as f:
        plistlib.dump(plist, f)
    with open(os.path.join(contents, "PkgInfo"), "w") as f:
        f.write("APPL????")

    # 4. ad-hoc signature, inside-out; allow-jit for the runtime's JIT
    if not a.no_sign:
        ent = os.path.join(ROOT, "build", "wintomac.entitlements")
        with open(ent, "wb") as f:
            plistlib.dump({"com.apple.security.cs.allow-jit": True}, f)
        for l in libs:
            run(["codesign", "-f", "-s", "-", os.path.join(fw, l)], capture_output=True)
        run(["codesign", "-f", "-s", "-", "--entitlements", ent, bundled_bin], capture_output=True)
        run(["codesign", "-f", "-s", "-", "--entitlements", ent, app], capture_output=True)
        run(["codesign", "-v", "--strict", app])
        log("signed (ad-hoc) and verified")
    log(f"done: {app}")
    print(f"\nRun it:   open {shlex.quote(app)}\nor:       {shlex.quote(lpath)}   (console output)")


if __name__ == "__main__":
    main()
