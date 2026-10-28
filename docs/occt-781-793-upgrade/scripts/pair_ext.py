"""Pair removed/added files that are the same class with a different extension
(.hxx -> .pxx etc.) and measure token churn for the pair instead of counting
them as a full delete + full add."""
import os, re, collections, difflib

BASE = r'D:/workspace/occt-src'
N1 = os.path.join(BASE, 'normalized', '781')
N2 = os.path.join(BASE, 'normalized', '801')
T1 = os.path.join(BASE, 'tok2', '781')
T2 = os.path.join(BASE, 'tok2', '801')


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


added, removed, modified = {}, {}, []
for line in open(os.path.join(BASE, 'diff-numstat-tok2.txt'), encoding='utf-8', errors='ignore'):
    parts = line.rstrip('\n').split('\t')
    if len(parts) != 3:
        continue
    a, d, p = parts
    old, new = expand(p)
    rec = (0 if a == '-' else int(a), 0 if d == '-' else int(d))
    if old is None and new:
        added[new] = rec
    elif new is None and old:
        removed[old] = rec
    elif old and new:
        modified.append((old, new, rec))


def key(p):
    d, f = os.path.split(p)
    return d, os.path.splitext(f)[0]


radd = collections.defaultdict(list)
for p in added:
    radd[key(p)].append(p)
rrem = collections.defaultdict(list)
for p in removed:
    rrem[key(p)].append(p)


def toks(root, p):
    return open(os.path.join(root, p), encoding='utf-8', errors='ignore').read().split('\n')[:-1]


pairs = []
for k in set(radd) & set(rrem):
    if len(radd[k]) == 1 and len(rrem[k]) == 1:
        pairs.append((rrem[k][0], radd[k][0]))

pair_churn = 0
pair_a = pair_d = 0
paired_old = set()
paired_new = set()
by_mod = collections.Counter()
for o, n in pairs:
    to, tn = toks(T1, o), toks(T2, n)
    sm = difflib.SequenceMatcher(None, to, tn, autojunk=False)
    match = sum(b.size for b in sm.get_matching_blocks())
    a, d = len(tn) - match, len(to) - match
    pair_a += a
    pair_d += d
    pair_churn += a + d
    paired_old.add(o)
    paired_new.add(n)
    by_mod[n.split('/')[0]] += a + d

print('extension-migration pairs:', len(pairs))
print('pair token churn: +%s -%s = %s' % (f'{pair_a:,}', f'{pair_d:,}', f'{pair_churn:,}'))
print('top packages by pair churn:')
for p, v in by_mod.most_common(12):
    print('  %-24s %s' % (p, f'{v:,}'))

rem_add = sum(v[0] for p, v in added.items() if p not in paired_new)
rem_del = sum(v[1] for p, v in removed.items() if p not in paired_old)
print('\nremaining brand-new files:', len(added) - len(paired_new), 'tokens +%s' % f'{rem_add:,}')
print('remaining deleted files:  ', len(removed) - len(paired_old), 'tokens -%s' % f'{rem_del:,}')

print('\nremaining new files by package:')
cnt = collections.Counter()
tok = collections.Counter()
for p, v in added.items():
    if p in paired_new:
        continue
    pkg = p.split('/')[0]
    cnt[pkg] += 1
    tok[pkg] += v[0]
for pkg, v in tok.most_common(15):
    print('  %-24s files=%4d tokens=%9s' % (pkg, cnt[pkg], f'{v:,}'))

print('\nremaining deleted files by package:')
cnt = collections.Counter()
tok = collections.Counter()
for p, v in removed.items():
    if p in paired_old:
        continue
    pkg = p.split('/')[0]
    cnt[pkg] += 1
    tok[pkg] += v[1]
for pkg, v in tok.most_common(15):
    print('  %-24s files=%4d tokens=%9s' % (pkg, cnt[pkg], f'{v:,}'))

mod_churn = sum(a + d for _, _, (a, d) in modified)
print('\nmodified same-path token churn: %s' % f'{mod_churn:,}')
print('TOTAL semantic token churn = %s' % f'{mod_churn + pair_churn + rem_add + rem_del:,}')
print('  modified  %10s' % f'{mod_churn:,}')
print('  ext-pairs %10s' % f'{pair_churn:,}')
print('  new       %10s' % f'{rem_add:,}')
print('  deleted   %10s' % f'{rem_del:,}')
