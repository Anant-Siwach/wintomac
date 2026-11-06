#!/usr/bin/env python3
"""Generate runtime/gen/steam_ifaces.h: ABI facts for bridging Windows Steamworks calls to the macOS libsteam_api.

    python3 -I tools/gen_steam.py <dir with steamworks_sdk_*/ header sets> runtime/gen/steam_ifaces.h

Input: the per-SDK-version Steamworks interface headers that Proton vendors in its lsteamclient/ directory
(third_party/proton-lsteamclient, fetched by tools/fetch_proton_steam.sh). Those headers are Valve's, under the
Steamworks SDK license (see docs/steam.md); they are read as data and never compiled into wintomac or copied.
The output contains only ABI facts: interface version strings, method names and vtable slots, argument
classes/sizes, struct field offsets for both targets, callback ids and flat-API function names.

Everything is measured by clang itself, for both targets, with stub libc headers (tools/steam_stub_include):
  - vtable slot order: -fdump-vtable-layouts on x86_64-pc-windows-msvc (guest: MSVC reverses overloads) and
    arm64-apple-macos (host, Itanium declaration order);
  - struct layouts: -fdump-record-layouts (Windows packs callback structs to 8, macOS to 4);
  - type classes/sizes and k_iCallback values: constexpr probes compiled to assembly.
"""
import concurrent.futures
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
STUB = os.path.join(HERE, 'steam_stub_include')
TARGETS = {'win': 'x86_64-pc-windows-msvc', 'mac': 'arm64-apple-macos11'}
CLANG = os.environ.get('CLANGXX', 'clang++')
MIN_SDK = (116, '')   # steam_api64.dll appeared around SDK 1.16; older interface sets are 32-bit only

# Functions implemented by hand in runtime/steam.c (never generated as flat forwards).
MANUAL = set('''SteamAPI_Init SteamAPI_InitSafe SteamAPI_InitFlat SteamAPI_Shutdown SteamAPI_IsSteamRunning
SteamAPI_RestartAppIfNecessary SteamAPI_RunCallbacks SteamAPI_RegisterCallback SteamAPI_UnregisterCallback
SteamAPI_RegisterCallResult SteamAPI_UnregisterCallResult SteamAPI_GetHSteamUser SteamAPI_GetHSteamPipe
SteamAPI_ReleaseCurrentThreadMemory SteamAPI_SetMiniDumpComment SteamAPI_WriteMiniDump SteamAPI_SetTryCatchCallbacks
SteamAPI_UseBreakpadCrashHandler SteamAPI_SetBreakpadAppID SteamAPI_GetSteamInstallPath
SteamAPI_ManualDispatch_Init SteamAPI_ManualDispatch_RunFrame SteamAPI_ManualDispatch_GetNextCallback
SteamAPI_ManualDispatch_FreeLastCallback SteamAPI_ManualDispatch_GetAPICallResult'''.split())


def sdk_key(d):
    m = re.match(r'steamworks_sdk_(\d+)([a-z]*)$', d)
    return (int(m.group(1)), m.group(2)) if m else None


def strip_comments(t):
    t = re.sub(r'/\*.*?\*/', ' ', t, flags=re.S)
    return re.sub(r'//[^\n]*', '', t)


def run_clang(target, src, extra, sdk, want_out=False):
    with tempfile.NamedTemporaryFile('w', suffix='.cpp', delete=False) as f:
        f.write(src)
        path = f.name
    try:
        cmd = [CLANG, '-target', TARGETS[target], '-std=c++17', '-ffreestanding', '-nostdlibinc', '-w',
               '-I', STUB, '-I', sdk, '-Wno-everything', '-ferror-limit=0'] + extra + [path]
        p = subprocess.run(cmd, capture_output=True, text=True)
        return p.returncode, p.stdout, p.stderr, path
    finally:
        os.unlink(path)


def run_lines(target, prelude, lines, extra, sdk, filler, suffix='};\n'):
    """Compile prelude + one probe per line; lines that fail to compile are replaced by `filler` and retried."""
    lines = list(lines)
    for _ in range(40):
        src = prelude + ''.join(l + '\n' for l in lines) + suffix
        rc, out, err, path = run_clang(target, src, extra, sdk)
        if rc == 0:
            return out, err, lines
        bad = set()
        first = prelude.count('\n') + 1
        for m in re.finditer(re.escape(os.path.basename(path)) + r':(\d+):\d+: (?:error|note: in instantiation)', err):
            ln = int(m.group(1)) - first
            if 0 <= ln < len(lines):
                bad.add(ln)
        if not bad:
            raise RuntimeError('%s %s: clang failed:\n%s' % (sdk, target, err[:3000]))
        for ln in bad:
            lines[ln] = filler
    raise RuntimeError('%s: too many retries' % sdk)


def includes_for(sdk, files):
    inc = [h for h in ('steam_api.h', 'steam_gameserver.h') if h in files]
    inc += sorted(h for h in files if h.startswith('isteam') and 'ps3' not in h)
    inc += [h for h in ('steamnetworkingfakeip.h',) if h in files]
    return ''.join('#include "%s"\n' % h for h in inc)


def parse_vtables(text, ms):
    hdr = re.compile(r"^%s indices for '(\w+)' \((\d+) entries\)\." % ('VFTable' if ms else 'VTable'))
    out, cur = {}, None
    for line in text.splitlines():
        m = hdr.match(line)
        if m:
            cur = out.setdefault(m.group(1), [])
            if cur:
                cur = None   # printed twice: keep the first
            continue
        if cur is not None:
            m = re.match(r'^\s+(\d+) \| (.*)$', line)
            if m:
                cur.append(m.group(2).replace(' [pure]', '').strip())
            elif line.strip():
                cur = None
    return out


def parse_layouts(text):
    """-fdump-record-layouts -> {record name: (size, align, [(depth, off, type, name)])}"""
    recs = {}
    for blk in text.split('*** Dumping AST Record Layout')[1:]:
        rows, size, align = [], None, None
        for line in blk.splitlines():
            m = re.match(r'^\s*(\d+)(?::\d+-\d+)? \|( +)(.*)$', line)
            if m:
                depth = (len(m.group(2)) - 1) // 2
                rows.append((depth, int(m.group(1)), m.group(3), ':' in line.split('|')[0]))
                continue
            m = re.search(r'\[sizeof=(\d+), (?:dsize=\d+, )?align=(\d+)', line)
            if m:
                size, align = int(m.group(1)), int(m.group(2))
                break
        if not rows or size is None:
            continue
        head = rows[0][2]
        name = re.sub(r'^(struct|class|union) ', '', head)
        fields = []
        for depth, off, txt, bitf in rows[1:]:
            if txt.startswith('('):          # (vftable pointer) etc.
                fields.append((depth, off, None, None, bitf))
                continue
            m = re.match(r'^(.*?)\s*(\w+)$', txt)
            fields.append((depth, off, m.group(1) if m else txt, m.group(2) if m else '', bitf))
        recs.setdefault(name, (size, align, head.split(' ')[0], fields))
    return recs


def split_params(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch in '(<[':
            depth += 1
        elif ch in ')>]':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def parse_sig(sig, cls):
    """'bool ISteamX::Foo(const char *, float *) const' -> (ret, name, [params])"""
    m = re.match(r'^(.*?)\b%s::(\w+)\((.*)\)( const)?$' % re.escape(cls), sig)
    if not m:
        return None
    params = split_params(m.group(3))
    if params == ['void']:
        params = []
    return m.group(1).strip(), m.group(2), params


def norm_type(t):
    t = re.sub(r'\s+', ' ', t.strip())
    t = re.sub(r'\s*([*&])', r' \1', t).replace('* *', '**').replace('* &', '*&')
    return t


KPRELUDE = r'''
template<class T> struct RP { typedef T t; }; template<class T> struct RP<T*> { typedef T t; };
template<class T> struct RP<T&> { typedef T t; }; template<class T> struct RP<T* const> { typedef T t; };
template<class T> struct RC { typedef T t; }; template<class T> struct RC<const T> { typedef T t; };
template<class T> struct RC<volatile T> { typedef T t; };
template<class T, bool C = __is_class(T) || __is_union(T)> struct POLY { enum { v = 0 }; };
template<class T> struct POLY<T, true> { enum { v = __is_polymorphic(T) }; };
template<class T> struct SZ { enum { v = sizeof(T) }; }; template<> struct SZ<void> { enum { v = 0 }; };
template<class T> struct AL { enum { v = alignof(T) }; }; template<> struct AL<void> { enum { v = 0 }; };
template<class T> constexpr unsigned long long K() {
  typedef typename RP<T>::t P; typedef typename RC<P>::t U;
  return __is_void(T) ? 0ull :
    (__is_pointer(T) || __is_reference(T)) ?
      (1ull | (__is_function(U) ? 16 : 0) | ((__is_class(U) || __is_union(U)) ? 32 : 0) | (POLY<U>::v ? 64 : 0) |
       (__is_const(P) ? 128 : 0) | (8ull << 8)) :
    __is_floating_point(T) ? (2ull | ((unsigned long long)SZ<T>::v << 8)) :
    (__is_class(T) || __is_union(T)) ? (3ull | (POLY<T>::v ? 64 : 0) | ((unsigned long long)SZ<T>::v << 8) |
                                        ((unsigned long long)AL<T>::v << 32)) :
    (4ull | (__is_signed(T) ? 8 : 0) | ((unsigned long long)SZ<T>::v << 8));
}
extern "C" const unsigned long long wintomac_probe[] = {
'''


def parse_quads(asm):
    vals = []
    on = False
    for line in asm.splitlines():
        if re.match(r'^_?wintomac_probe:', line.strip()):
            on = True
            continue
        if on:
            m = re.match(r'^\s*\.(quad|xword|8byte)\s+(-?\w+)', line)
            if m:
                vals.append(int(m.group(2), 0) & 0xFFFFFFFFFFFFFFFF)
            elif re.match(r'^\s*\.(zero|space)\s+(\d+)', line):
                n = int(re.match(r'^\s*\.(zero|space)\s+(\d+)', line).group(2)) // 8
                vals += [0] * n
            elif line.strip() and not line.strip().startswith(('.p2align', '.size', '#', ';', '//')) and ':' in line:
                break
    return vals


def process_sdk(root, d):
    sdk = os.path.join(root, d)
    files = {f: open(os.path.join(sdk, f), encoding='latin-1').read() for f in os.listdir(sdk) if f.endswith('.h')}
    files = {f: t for f, t in files.items() if not t.lstrip().startswith('#error')}
    inc = includes_for(sdk, files)
    texts = {f: strip_comments(t) for f, t in files.items()}

    # interface classes and their version strings (#define just after the class, same header)
    classes, versions = set(), {}
    for f, t in texts.items():
        cpos = [(m.start(), m.group(1)) for m in re.finditer(r'\bclass\s+(ISteam\w+)\s*(?::[^{;]*)?\{', t)
                if 'virtual' in t[m.end():m.end() + 4000].split('};')[0]]
        for _, c in cpos:
            classes.add(c)
        for m in re.finditer(r'#define\s+(\w*VERSION\w*)\s+"([A-Za-z_]+[0-9][A-Za-z0-9_]*)"', t):
            prev = [c for p, c in cpos if p < m.start()]
            nxt = [c for p, c in cpos if p > m.start()]
            c = prev[-1] if prev else (nxt[0] if nxt else None)
            if c:
                versions[m.group(2)] = (c, m.group(1))
    cb_structs = set()
    for t in texts.values():
        cb_structs |= set(re.findall(r'struct\s+(\w+)\s*(?::[^{;]*)?\{\s*enum\s*\{\s*k_iCallback\s*=', t))
        cb_structs |= set(re.findall(r'STEAM_CALLBACK_BEGIN\(\s*(\w+)\s*,', t))
        cb_structs |= set(re.findall(r'DEFINE_CALLBACK\(\s*(\w+)\s*,', t))

    # pass A: vtable slot order for both ABIs
    wl = ['struct WP_%s : %s { WP_%s(); }; WP_%s::WP_%s() {}' % (c, c, c, c, c) for c in sorted(classes)]
    vt = {}
    for tg in TARGETS:
        out, err, _ = run_lines(tg, inc, wl, ['-c', '-o', '/dev/null', '-Xclang', '-fdump-vtable-layouts'], sdk, '', '')
        vt[tg] = parse_vtables(out + err, tg == 'win')

    # flat API declarations
    flat = []
    ft = texts.get('steam_api_flat.h', '')
    for m in re.finditer(r'S_API\s+([^;{}()]*?)\s*(?:S_CALLTYPE\s+)?\b(SteamAPI_\w+)\s*\(([^;{}]*?)\)\s*;', ft):
        ret, name, ps = norm_type(m.group(1).replace('S_CALLTYPE', '')), m.group(2), split_params(m.group(3))
        if ps == ['void']:
            ps = []
        types = []
        for p in ps:
            mm = re.match(r'^(.*?[\w*&\s])\s*\b(\w+)$', p)
            types.append(norm_type(mm.group(1) if mm and mm.group(1).strip() not in ('', 'const', 'unsigned')
                                   else p))
        flat.append((name, ret, types))

    # collect type spellings
    methods = {}
    for c in classes:
        if c not in vt['win'] or c not in vt['mac']:
            continue
        ms = []
        for sig in vt['win'][c]:
            if '::~' in sig:   # virtual destructor: 1 MSVC slot, 2 Itanium slots; games never delete interfaces
                ms.append(('~', '~', 0xFFFF, 'void', []))
                continue
            ps = parse_sig(sig, c)
            if not ps:
                raise RuntimeError('%s: cannot parse %s' % (d, sig))
            try:
                host = vt['mac'][c].index(sig)
            except ValueError:
                raise RuntimeError('%s: %s missing from Itanium vtable' % (d, sig))
            ms.append((ps[1], sig.split('::', 1)[1], host, norm_type(ps[0]), [norm_type(p) for p in ps[2]]))
        methods[c] = ms
    types = set()
    for ms in methods.values():
        for m in ms:
            types.add(m[3])
            types.update(m[4])
    for _, r, ps in flat:
        types.add(r)
        types.update(ps)
    types.discard('')
    for c, ms in methods.items():
        for m in ms:
            for t in [m[3]] + m[4]:
                b = re.sub(r'\b(const|struct|class|union|enum)\b', '', t).strip()
                if re.match(r'^\w+(\s*[*&])*$', b) and not re.match(r'^(void|char|int|bool|float|double|unsigned|short|long)\b', b):
                    types.add('%s::%s' % (c, b))
    pointees = set()
    for t in types:
        if '*' in t or '&' in t:
            b = re.sub(r'\b(const|struct|class|union|volatile)\b', '', t).replace('*', '').replace('&', '').strip()
            if re.match(r'^[A-Za-z_][\w:]*$', b) and b not in ('void', 'char', 'int', 'float', 'double', 'bool'):
                pointees.add(b)

    # pass B: struct layouts (callback structs + records passed by pointer)
    recnames = sorted(cb_structs | pointees)
    lay = {}
    for tg in TARGETS:
        out, err, kept = run_lines(tg, inc + 'int wintomac_force[] = {\n', ['(int)sizeof(%s),' % r for r in recnames],
                                   ['-fsyntax-only', '-Xclang', '-fdump-record-layouts'], sdk, '0,')
        lay[tg] = parse_layouts(out + err)

    # which records need conversion; leaf type spellings
    leaf_types = set()
    convs = {}
    for r in recnames:
        if r not in lay['win'] or r not in lay['mac']:
            continue
        w, m = lay['win'][r], lay['mac'][r]
        if w[2] == 'union':
            continue
        fw, fm = w[3], m[3]
        if len(fw) != len(fm) or any((a[0], a[2], a[3]) != (b[0], b[2], b[3]) for a, b in zip(fw, fm)):
            print('warning: %s %s: layouts differ structurally' % (d, r), file=sys.stderr)
            continue
        same = all(a[1] == b[1] for a, b in zip(fw, fm)) and w[0] == m[0]
        if same:
            convs[r] = (w[0], m[0], None)
            continue
        # walk: descend into records whose subtree differs
        leaves = []
        i = 0
        while i < len(fw):
            depth, ow, ty, nm, bf = fw[i]
            om = fm[i][1]
            j = i + 1
            while j < len(fw) and fw[j][0] > depth:
                j += 1
            sub_same = all(fw[k][1] - ow == fm[k][1] - om for k in range(i, j))
            if ty is None:
                i = j
                continue
            if j > i + 1 and not sub_same and not ty.startswith('union'):
                i += 1          # descend
                continue
            leaves.append((ow, om, ty))
            leaf_types.add(ty)
            i = j
        convs[r] = (w[0], m[0], leaves)

    # pass C: type classes/sizes + callback ids
    tlist = sorted(types | leaf_types)
    cbl = sorted(c for c in cb_structs if c in lay['win'])
    probe = {}
    for tg in TARGETS:
        lines = ['K<%s>(),' % t for t in tlist] + ['(unsigned long long)(%s::k_iCallback),' % c for c in cbl]
        out, err, kept = run_lines(tg, inc + KPRELUDE, lines, ['-S', '-o', '-'], sdk, '~0ull,')
        vals = parse_quads(out)
        if len(vals) != len(lines):
            raise RuntimeError('%s %s: probe count %d != %d' % (d, tg, len(vals), len(lines)))
        probe[tg] = vals
    kt = {tg: dict(zip(tlist, probe[tg][:len(tlist)])) for tg in TARGETS}
    cbids = dict(zip(cbl, probe['mac'][len(tlist):]))
    return dict(sdk=d, versions=versions, methods=methods, flat=flat, kt=kt, convs=convs, cbids=cbids,
                classes=classes, pointees=pointees, texts=None)


# ---------------------------------------------------------------------------------------------------------------
K_VOID, K_INT, K_FLT, K_PTR, K_REC, K_PREC, K_POLY, K_FN, K_HOOK, K_IFACE = range(10)
KNAMES = 'K_VOID K_INT K_FLT K_PTR K_REC K_PREC K_POLY K_FN K_HOOK K_IFACE'.split()


def classify(t, kw, km, sdkconv, conv_index, warn):
    """-> (kind, size, flags, conv)  flags: 1 signed / const pointee"""
    if t in ('void', ''):
        return (K_VOID, 0, 0, 0)
    if kw is None or kw == 0xFFFFFFFFFFFFFFFF or km == 0xFFFFFFFFFFFFFFFF:
        warn('unknown type %s' % t)
        return (K_PTR, 8, 0, 0)
    cls, sz = kw & 7, (kw >> 8) & 0xFFFFFF
    if cls == 1:
        if kw & 16:
            return (K_HOOK, 8, 0, 0) if 'WarningMessageHook' in t else (K_FN, 8, 0, 0)
        if kw & 64:
            return (K_POLY, 8, 0, 0)
        if kw & 32:
            b = re.sub(r'\b(const|struct|class|union|volatile)\b', '', t).replace('*', '').replace('&', '').strip()
            cv = sdkconv.get(b)
            if cv and cv[2] is not None and t.count('*') + t.count('&') == 1:
                return (K_PREC, 8, 1 if kw & 128 else 0, conv_index(cv, b))
        return (K_PTR, 8, 0, 0)
    if cls == 2:
        return (K_FLT, sz, 0, 0)
    if cls == 3:
        szm = (km >> 8) & 0xFFFFFF
        if szm != sz:
            warn('by-value record %s has size %d/%d' % (t, sz, szm))
        return (K_REC, szm, sz, 0)     # flags carries the Windows size
    return (K_INT, sz, 1 if kw & 8 else 0, 0)


def main():
    root, outp = sys.argv[1], sys.argv[2]
    dirs = [d for d in os.listdir(root) if sdk_key(d) and sdk_key(d) >= MIN_SDK]
    dirs.sort(key=sdk_key, reverse=True)   # newest first: first definition of a version string wins
    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        futs = {ex.submit(process_sdk, root, d): d for d in dirs}
        for f in concurrent.futures.as_completed(futs):
            d = futs[f]
            try:
                results[d] = f.result()
            except Exception as e:   # an old header set clang can't parse: skip it
                print('warning: skipping %s: %s' % (d, str(e)[:400]), file=sys.stderr)
    warns = set()

    conv_list, conv_ids = [], {}

    def conv_index(cv, name):
        key = (cv[0], cv[1], tuple(cv[2]) if cv[2] is not None else None)
        if key not in conv_ids:
            conv_ids[key] = len(conv_list)
            conv_list.append((name, cv))
        return conv_ids[key]

    ifaces, flat_m, flat_acc, flat_plain, callbacks = {}, {}, {}, {}, {}
    for d in dirs:
        if d not in results:
            continue
        r = results[d]
        kt = r['kt']

        def cl(t, _r=r, cls=None):
            bad = 0xFFFFFFFFFFFFFFFF
            tt = t
            if cls and kt['win'].get(t, bad) == bad:
                q = '%s::%s' % (cls, re.sub(r'\b(const|struct|class|union|enum)\b', '', t).strip())
                if kt['win'].get(q, bad) != bad:
                    tt = q
            return classify(t, kt['win'].get(tt), kt['mac'].get(tt), _r['convs'], conv_index,
                            lambda s: warns.add('%s: %s' % (_r['sdk'], s)))
        for ver, (c, macro) in r['versions'].items():
            if ver in ifaces or c not in r['methods']:
                continue
            ms = []
            for name, key, host, ret, params in r['methods'][c]:
                rk = cl(ret, cls=c)
                ak = [cl(p, cls=c) for p in params]
                if c == 'ISteamClient' and name.startswith('GetISteam') and params and params[-1] == 'const char *':
                    rk = (K_IFACE, 8, len(params) - 1, 0)
                ms.append((name, key, host, rk, ak))
            ifaces[ver] = (c, d, ms)
        for cbn, cid in r['cbids'].items():
            cv = r['convs'].get(cbn)
            if cv is None:
                continue
            idx = conv_index(cv, cbn)
            callbacks.setdefault(cid, [])
            if idx not in callbacks[cid]:
                callbacks[cid].append(idx)
        # flat functions
        clsnames = sorted(r['methods'], key=len, reverse=True)
        for name, ret, ps in r['flat']:
            if name in MANUAL or name in flat_m or name in flat_acc or name in flat_plain:
                continue
            m = re.match(r'^SteamAPI_(Steam\w+?)_v(\d+)$', name) or re.match(r'^SteamAPI_(Steam\w+?_SteamAPI)_v(\d+)$', name)
            if not ps and ret.endswith('*') and m:
                c = ret.replace('*', '').strip()
                vers = [v for v, (cc, _) in r['versions'].items() if cc == c and re.sub(r'\D', '', v).endswith(m.group(2))]
                if vers:
                    kind = 2 if c == 'ISteamNetworkingUtils' else 1 if 'GameServer' in m.group(1) else 0
                    flat_acc[name] = (vers[0], kind)
                    continue
            c = next((c for c in clsnames if name.startswith('SteamAPI_%s_' % c)), None)
            if c:
                rest = name[len('SteamAPI_%s_' % c):]
                cands = [mm for mm in r['methods'][c] if mm[0] == rest]
                if not cands:
                    cands = [mm for mm in r['methods'][c] if rest.startswith(mm[0])]
                    best = max((len(mm[0]) for mm in cands), default=0)
                    cands = [mm for mm in cands if len(mm[0]) == best]
                if len(cands) > 1:
                    def fix(t):
                        t = re.sub(r'\b(class|struct|union|enum) ', '', t)
                        return t.replace('uint64_steamid', 'CSteamID').replace('uint64_gameid', 'CGameID')
                    want = [fix(t) for t in ps[1:]]
                    exact = [mm for mm in cands if [fix(t) for t in mm[4]] == want]
                    if len(exact) == 1:
                        cands = exact
                    else:
                        # overload suffix naming: Int32/Float/Int64/Double/... matched by the differing param
                        sfx = rest[len(cands[0][0]):].lower()
                        hit = [mm for mm in cands if any(sfx and sfx in p.replace(' ', '').replace('*', '').lower()
                                                          for p in mm[4])]
                        if len(hit) == 1:
                            cands = hit
                if len(cands) == 1:
                    flat_m[name] = (c, cands[0][0], cands[0][1])
                    continue
                if cands:
                    warns.add('%s: flat %s: %d candidate methods' % (d, name, len(cands)))
                    continue
                # not a virtual method (inline helper exported by steam_api itself): forward to the host's symbol
            flat_plain[name] = (cl(ret), [cl(p) for p in ps])

    for w in sorted(warns):
        print('warning: ' + w, file=sys.stderr)

    # ---- emit -----------------------------------------------------------------------------------------------
    o = []
    o.append('// Generated by tools/gen_steam.py from the Steamworks interface headers vendored by Proton (lsteamclient).')
    o.append('// ABI facts only (version strings, method names/slots, argument classes, struct offsets). Do not edit.')
    o.append('// SDK header sets used: ' + ' '.join(sorted((d for d in dirs if d in results), key=sdk_key)))
    o.append('#pragma once')
    o.append('')
    sigs, sig_ids = [], {}

    def sig_ref(args):
        key = tuple(args)
        if key not in sig_ids:
            sig_ids[key] = len(sigs)
            sigs.append(key)
        return sig_ids[key]
    iface_rows = []
    meth_rows = []
    for ver in sorted(ifaces):
        c, d, ms = ifaces[ver]
        start = len(meth_rows)
        for name, key, host, rk, ak in ms:
            meth_rows.append((name, key, host, rk, sig_ref(ak), len(ak)))
        iface_rows.append((ver, c, d, start, len(ms)))
    # argument pool
    pool, pool_off = [], {}
    for i, s in enumerate(sigs):
        pool_off[i] = len(pool)
        pool.extend(s)
    flat_plain_rows = []
    for name in sorted(flat_plain):
        rk, ak = flat_plain[name]
        flat_plain_rows.append((name, rk, sig_ref(ak), len(ak)))
    for i in range(len(pool_off), len(sigs)):
        pool_off[i] = len(pool)
        pool.extend(sigs[i])

    def arg(a):
        return '{%s,%d,%d,%d}' % (KNAMES[a[0]], a[1], a[2], a[3])
    o.append('static const SteamArg steam_arg_pool[] = {')
    for i in range(0, len(pool), 8):
        o.append('  ' + ','.join(arg(a) for a in pool[i:i + 8]) + ',')
    o.append('  {0,0,0,0} };')
    o.append('static const SteamMethod steam_methods[] = {')
    for name, key, host, rk, sid, n in meth_rows:
        o.append('  {"%s","%s",%d,%d,%s,%d},' % (name, key, host, n, arg(rk), pool_off[sid]))
    o.append('};')
    o.append('static const SteamIface steam_ifaces[] = {')
    for ver, c, d, start, n in iface_rows:
        o.append('  {"%s","%s","%s",%d,%d},' % (ver, c, d, start, n))
    o.append('};')
    o.append('#define STEAM_NIFACES %d' % len(iface_rows))
    # conversions
    fld = []
    o.append('static const SteamConv steam_convs[] = {')
    for name, (ws, mss, leaves) in conv_list:
        if leaves is None:
            o.append('  {"%s",%d,%d,0,0},' % (name, ws, mss))
            continue
        lt = []
        kt_any = None
        # leaf sizes: measured per SDK; take them from any SDK that has the leaf type (sizes of leaves are stable)
        for ow, om, ty in leaves:
            sw = sm = None
            for rr in results.values():
                if ty in rr['kt']['win']:
                    sw = (rr['kt']['win'][ty] >> 8) & 0xFFFFFF
                    sm = (rr['kt']['mac'][ty] >> 8) & 0xFFFFFF
                    break
            if sw is None:
                warns.add('conv %s: no size for %s' % (name, ty))
                continue
            n = min(sw, sm, ws - ow, mss - om)
            if lt and lt[-1][0] + lt[-1][2] == ow and lt[-1][1] + lt[-1][2] == om:
                lt[-1] = (lt[-1][0], lt[-1][1], lt[-1][2] + n)
            else:
                lt.append((ow, om, n))
        o.append('  {"%s",%d,%d,%d,%d},' % (name, ws, mss, len(lt), len(fld)))
        fld.extend(lt)
    o.append('};')
    o.append('static const SteamField steam_fields[] = {')
    for i in range(0, len(fld), 8):
        o.append('  ' + ','.join('{%d,%d,%d}' % f for f in fld[i:i + 8]) + ',')
    o.append('  {0,0,0} };')
    cbp = []
    o.append('static const SteamCallbackInfo steam_callbacks[] = {')
    for cid in sorted(callbacks):
        o.append('  {%d,%d,%d},' % (cid, len(callbacks[cid]), len(cbp)))
        cbp.extend(callbacks[cid])
    o.append('};')
    o.append('#define STEAM_NCALLBACKS %d' % len(callbacks))
    o.append('static const unsigned short steam_callback_convs[] = {%s0};' % ''.join('%d,' % x for x in cbp))
    # flat
    o.append('static const SteamFlatMethod steam_flat_methods[] = {')
    names_m = sorted(flat_m)
    for n in names_m:
        c, mn, key = flat_m[n]
        o.append('  {"%s","%s","%s","%s"},' % (n, c, mn, key))
    o.append('};')
    o.append('static const SteamFlatPlain steam_flat_plain[] = {')
    for name, rk, sid, n in flat_plain_rows:
        o.append('  {"%s",%s,%d,%d},' % (name, arg(rk), n, pool_off[sid]))
    o.append('};')
    o.append('static const SteamFlatAccessor steam_flat_accessors[] = {')
    for n in sorted(flat_acc):
        o.append('  {"%s","%s",%d},' % (n, flat_acc[n][0], flat_acc[n][1]))
    o.append('};')
    # one shim per flat name (thunks are per function pointer)
    o.append('#define STEAM_FLAT_METHOD_LIST(X) \\')
    o.append(' \\\n'.join('  X(%d, %s)' % (i, n) for i, n in enumerate(names_m)))
    o.append('#define STEAM_FLAT_PLAIN_LIST(X) \\')
    o.append(' \\\n'.join('  X(%d, %s)' % (i, r[0]) for i, r in enumerate(flat_plain_rows)))
    o.append('#define STEAM_FLAT_ACCESSOR_LIST(X) \\')
    o.append(' \\\n'.join('  X(%d, %s)' % (i, n) for i, n in enumerate(sorted(flat_acc))))
    o.append('')
    with open(outp, 'w') as f:
        f.write('\n'.join(o))
    for w in sorted(warns):
        if w.startswith('conv'):
            print('warning: ' + w, file=sys.stderr)
    print('steam: %d SDK header sets, %d interface versions, %d methods, %d callback ids, %d struct layouts, '
          '%d flat methods, %d flat accessors, %d flat plain' % (len(results), len(ifaces), len(meth_rows),
          len(callbacks), len(conv_list), len(flat_m), len(flat_acc), len(flat_plain_rows)), file=sys.stderr)


if __name__ == '__main__':
    main()
