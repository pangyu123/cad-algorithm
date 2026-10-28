import os, hashlib, collections

BASE = r'D:/workspace/occt-src'
A = os.path.join(BASE, 'OCCT-7.8.1')
B = os.path.join(BASE, 'OCCT-8.0.1')
N1 = os.path.join(BASE, 'normalized', '781')
N2 = os.path.join(BASE, 'normalized', '801')


def link(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if not os.path.exists(dst):
        try:
            os.link(src, dst)
        except OSError:
            with open(src, 'rb') as fi, open(dst, 'wb') as fo:
                fo.write(fi.read())


def is_legacy_toolkit_dir(path):
    # 7.8.1 keeps toolkit build metadata in src/<TK>/ (FILES, PACKAGES, EXTERNLIB)
    names = set(os.listdir(path))
    return 'PACKAGES' in names and 'EXTERNLIB' in names


def build_old():
    root = os.path.join(A, 'src')
    n = 0
    for dp, dn, fs in os.walk(root):
        rel = os.path.relpath(dp, root).replace(os.sep, '/')
        parts = rel.split('/')
        if len(parts) == 1 and is_legacy_toolkit_dir(dp):
            out = os.path.join(N1, '_toolkit', 'legacy__' + parts[0])
        else:
            out = os.path.join(N1, rel)
        for f in fs:
            link(os.path.join(dp, f), os.path.join(out, f))
            n += 1
    return n


def build_new():
    root = os.path.join(B, 'src')
    n = 0
    for dp, dn, fs in os.walk(root):
        rel = os.path.relpath(dp, root).replace(os.sep, '/')
        parts = rel.split('/')
        if len(parts) >= 3:
            # <Module>/<Toolkit>/<Package>/... -> <Package>/...
            out = os.path.join(N2, parts[2], *parts[3:]) if len(parts) > 3 else os.path.join(N2, parts[2])
        elif len(parts) == 2:
            # <Module>/<Toolkit>/<file> -> _toolkit/<Module>__<Toolkit>/<file>
            out = os.path.join(N2, '_toolkit', parts[0] + '__' + parts[1])
        elif len(parts) == 1:
            out = os.path.join(N2, '_module', parts[0])
        else:
            out = N2
        for f in fs:
            link(os.path.join(dp, f), os.path.join(out, f))
            n += 1
    return n


n1 = build_old()
n2 = build_new()
print('normalized 781 files:', n1)
print('normalized 801 files:', n2)

# summary
def collect(root):
    d = {}
    for dp, dn, fs in os.walk(root):
        for f in fs:
            p = os.path.join(dp, f)
            d[os.path.relpath(p, root).replace(os.sep, '/')] = p
    return d

a = collect(N1)
b = collect(N2)
changed, added, removed = [], [], []
for k, p in a.items():
    if k not in b:
        removed.append(k)
    elif os.path.getsize(p) != os.path.getsize(b[k]) or hashlib.md5(open(p, 'rb').read()).digest() != hashlib.md5(open(b[k], 'rb').read()).digest():
        changed.append(k)
for k in b:
    if k not in a:
        added.append(k)

lines = []
lines.append('OCCT 7.8.1 -> 8.0.1 normalized source diff summary')
lines.append('')
lines.append('unchanged files: %d' % (len(a) - len(changed) - len(removed)))
lines.append('changed files:   %d' % len(changed))
lines.append('removed files:   %d' % len(removed))
lines.append('added files:     %d' % len(added))
lines.append('')
pkg = collections.Counter()
for k in changed + removed + added:
    pkg[k.split('/')[0]] += 1
lines.append('Top packages by touched file count:')
for p, c in pkg.most_common(60):
    lines.append('  %-32s %d' % (p, c))
lines.append('')
lines.append('Added packages (present in 8.0.1 only):')
pk_a = set(k.split('/')[0] for k in a)
pk_b = set(k.split('/')[0] for k in b)
lines.append('  ' + ', '.join(sorted(pk_b - pk_a)))
lines.append('')
lines.append('Removed packages (present in 7.8.1 only):')
lines.append('  ' + ', '.join(sorted(pk_a - pk_b)))
open(os.path.join(BASE, 'diff-summary.txt'), 'w', encoding='utf-8').write('\n'.join(lines))
print('\n'.join(lines[:12]))
