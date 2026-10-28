"""Net token-multiset difference per package.

Moves/renames/merges inside a package cancel out, so this measures how much code
was really added or removed, independent of formatting and file reorganization.
"""
import os, collections

BASE = r'D:/workspace/occt-src'
T1 = os.path.join(BASE, 'tok2', '781')
T2 = os.path.join(BASE, 'tok2', '801')


def count(root):
    per_pkg = collections.defaultdict(collections.Counter)
    total = collections.Counter()
    nfiles = 0
    for dp, dn, fs in os.walk(root):
        for f in fs:
            p = os.path.join(dp, f)
            rel = os.path.relpath(p, root).replace(os.sep, '/')
            pkg = rel.split('/')[0]
            c = per_pkg[pkg]
            with open(p, encoding='utf-8', errors='ignore') as fh:
                for t in fh:
                    t = t.rstrip('\n')
                    if t:
                        c[t] += 1
                        total[t] += 1
            nfiles += 1
    return per_pkg, total, nfiles


p1, t1, n1 = count(T1)
p2, t2, n2 = count(T2)
print('files: %d -> %d' % (n1, n2))
print('tokens: %s -> %s' % (f'{sum(t1.values()):,}', f'{sum(t2.values()):,}'))

allt = set(t1) | set(t2)
pos = sum(max(0, t2[t] - t1[t]) for t in allt)
neg = sum(max(0, t1[t] - t2[t]) for t in allt)
print('net tokens added   : %s' % f'{pos:,}')
print('net tokens removed : %s' % f'{neg:,}')
print('net token churn    : %s' % f'{pos + neg:,}')
print('net growth         : %s (%.1f%%)' % (
    f'{sum(t2.values()) - sum(t1.values()):,}',
    100.0 * (sum(t2.values()) - sum(t1.values())) / max(1, sum(t1.values()))))

print('\nper-package net churn (top 30):')
rows = []
for pkg in allt and (set(p1) | set(p2)):
    a, b = p1.get(pkg, collections.Counter()), p2.get(pkg, collections.Counter())
    keys = set(a) | set(b)
    p = sum(max(0, b[k] - a[k]) for k in keys)
    n = sum(max(0, a[k] - b[k]) for k in keys)
    rows.append((p + n, pkg, p, n, sum(a.values()), sum(b.values())))
for churn, pkg, p, n, o, nw in sorted(rows, reverse=True)[:30]:
    print('  %-24s churn=%9s (+%8s -%8s)  size %8s -> %8s' % (
        pkg, f'{churn:,}', f'{p:,}', f'{n:,}', f'{o:,}', f'{nw:,}'))
