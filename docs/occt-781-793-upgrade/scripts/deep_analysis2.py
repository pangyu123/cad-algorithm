import os, re, collections

BASE = r'D:/workspace/occt-src'
A = os.path.join(BASE, 'OCCT-7.8.1')
B = os.path.join(BASE, 'OCCT-8.0.1')
N1 = os.path.join(BASE, 'normalized', '781')
N2 = os.path.join(BASE, 'normalized', '801')


def expand(path):
    m = re.search(r'\{([^{}]*?) => ([^{}]*?)\}', path)
    if m:
        left = path[:m.start()] + m.group(1) + path[m.end():]
        right = path[:m.start()] + m.group(2) + path[m.end():]
    elif ' => ' in path:
        left, right = path.split(' => ', 1)
    else:
        left = right = path

    def strip(p):
        p = p.strip()
        if p == '/dev/null':
            return None
        return re.sub(r'^(?:normalized|tok2?)/(781|801)/', '', p)

    return strip(left), strip(right)


def parse(fn):
    rows = []
    for line in open(os.path.join(BASE, fn), encoding='utf-8', errors='ignore'):
        parts = line.rstrip('\n').split('\t')
        if len(parts) != 3:
            continue
        a, d, p = parts
        old, new = expand(p)
        rows.append({'old': old, 'new': new,
                     'add': 0 if a == '-' else int(a),
                     'del': 0 if d == '-' else int(d),
                     'bin': a == '-'})
    return rows


def legacy_map():
    src = os.path.join(A, 'src')
    tk2mod = {}
    for line in open(os.path.join(A, 'adm', 'MODULES'), encoding='utf-8', errors='ignore'):
        parts = line.split()
        if len(parts) >= 2:
            for tk in parts[1:]:
                tk2mod[tk] = parts[0]
    pkg2tk = {}
    for tk in tk2mod:
        pf = os.path.join(src, tk, 'PACKAGES')
        if os.path.isfile(pf):
            for line in open(pf, encoding='utf-8', errors='ignore'):
                p = line.strip().split('#')[0].strip()
                if p:
                    pkg2tk[p] = tk
    return {p: (tk2mod.get(tk, '?'), tk) for p, tk in pkg2tk.items()}


def new_map():
    src = os.path.join(B, 'src')
    out = {}
    for mod in os.listdir(src):
        mp = os.path.join(src, mod)
        if not os.path.isdir(mp):
            continue
        for tk in os.listdir(mp):
            tp = os.path.join(mp, tk)
            if not os.path.isdir(tp):
                continue
            for pkg in os.listdir(tp):
                pp = os.path.join(tp, pkg)
                if os.path.isdir(pp):
                    out[pkg] = (mod, tk)
    return out


m1 = legacy_map()
m2 = new_map()
c = set(m1) & set(m2)
print('=== STRUCTURE ===')
print('old packages', len(m1), 'new packages', len(m2), 'common', len(c))
print('same module+toolkit', sum(1 for p in c if m1[p] == m2[p]))
print('\nmoved toolkit only:')
for p in sorted(p for p in c if m1[p][0] == m2[p][0] and m1[p][1] != m2[p][1]):
    print('  %-20s %s -> %s' % (p, m1[p], m2[p]))
print('\nmoved module:')
for p in sorted(p for p in c if m1[p][0] != m2[p][0]):
    print('  %-20s %s -> %s' % (p, m1[p], m2[p]))
print('\nremoved packages (%d):' % len(set(m1) - set(m2)))
for p in sorted(set(m1) - set(m2)):
    print('  %-20s was %s' % (p, m1[p]))
print('\nadded packages (%d):' % len(set(m2) - set(m1)))
for p in sorted(set(m2) - set(m1)):
    print('  %-20s is %s' % (p, m2[p]))

# ---------- extension transitions ----------
print('\n=== EXTENSION TRANSITIONS (same package path) ===')
un1, un2 = {}, {}
for root, d in ((N1, un1), (N2, un2)):
    for dp, dn, fs in os.walk(root):
        for f in fs:
            rel = os.path.relpath(os.path.join(dp, f), root).replace(os.sep, '/')
            d[rel] = f
trans = collections.Counter()
for k in set(un1) & set(un2):
    e1, e2 = os.path.splitext(k)[1], os.path.splitext(k)[1]
for k in set(un1) & set(un2):
    pass
# extension change requires stem match
stems1 = collections.defaultdict(list)
stems2 = collections.defaultdict(list)
for k in un1:
    stem, ext = os.path.splitext(k)
    stems1[stem].append(ext)
for k in un2:
    stem, ext = os.path.splitext(k)
    stems2[stem].append(ext)
for stem in set(stems1) & set(stems2):
    s1, s2 = set(stems1[stem]), set(stems2[stem])
    gone, new = s1 - s2, s2 - s1
    if len(gone) == 1 and len(new) == 1:
        trans[next(iter(gone)) + ' -> ' + next(iter(new))] += 1
for t, n in trans.most_common():
    print('  %-18s %d' % (t, n))

# ---------- churn ----------
def tot(rows):
    return sum(r['add'] for r in rows), sum(r['del'] for r in rows), len(rows)


print('\n=== CHURN ===')
for name, fn in (('raw', 'diff-numstat.txt'),
                 ('ignore-ws', 'diff-numstat-w.txt'),
                 ('token-norm', 'diff-numstat-tok2.txt')):
    rows = parse(fn)
    a, d, n = tot(rows)
    print('%-12s files=%6d +%9s -%9s churn=%10s' % (
        name, n, f'{a:,}', f'{d:,}', f'{a + d:,}'))

# token churn restricted to modified-existing files
tok = parse('diff-numstat-tok2.txt')
mod = [r for r in tok if r['old'] and r['new']]
a, d, n = tot(mod)
print('token-norm, modified existing files only: files=%d +%s -%s churn=%s' % (
    n, f'{a:,}', f'{d:,}', f'{a + d:,}'))
addf = [r for r in tok if r['old'] is None]
delf = [r for r in tok if r['new'] is None]
print('token-norm added files:', len(addf), 'churn', sum(r['add'] for r in addf))
print('token-norm removed files:', len(delf), 'churn', sum(r['del'] for r in delf))
print('token-norm unchanged-content pairs:', sum(1 for r in mod if r['add'] + r['del'] == 0))

# per module token churn
ma, md, mn = collections.Counter(), collections.Counter(), collections.Counter()
for r in mod:
    pkg = r['new'].split('/')[0]
    if pkg in ('GTests', '_toolkit', '_module', 'tests'):
        mod_name = pkg
    else:
        mod_name = m2.get(pkg, ('?', '?'))[0]
    ma[mod_name] += r['add']
    md[mod_name] += r['del']
    mn[mod_name] += 1
print('\n=== TOKEN-NORM CHURN BY MODULE (modified files only) ===')
for k in sorted(ma, key=lambda x: -(ma[x] + md[x])):
    print('  %-22s files=%5d +%9s -%9s' % (
        k, mn[k], f'{ma[k]:,}', f'{md[k]:,}'))

# ---------- class decls across hxx/pxx/lxx ----------
def classes(root):
    names = set()
    rx = re.compile(r'(?:^|\n)\s*(?:template\s*<[^>\n]*>\s*)?(?:class|struct)\s+(?:Standard_EXPORT\s+)?([A-Za-z_]\w*)\s*(?::|$|\n|\{)', re.M)
    for dp, dn, fs in os.walk(root):
        for f in fs:
            if os.path.splitext(f)[1].lower() not in ('.hxx', '.pxx', '.lxx', '.gxx'):
                continue
            txt = open(os.path.join(dp, f), encoding='utf-8', errors='ignore').read()
            for mm in rx.finditer(txt):
                names.add(mm.group(1))
    return names

c1, c2 = classes(N1), classes(N2)
print('\n=== CLASS/STRUCT DEFINITIONS (hxx+pxx+lxx) ===')
print('old', len(c1), 'new', len(c2), 'common', len(c1 & c2), 'added', len(c2 - c1), 'removed', len(c1 - c2))
print('added sample:', sorted(c2 - c1)[:25])
print('removed sample:', sorted(c1 - c2)[:25])
