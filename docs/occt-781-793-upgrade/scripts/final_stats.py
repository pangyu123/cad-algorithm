import os, re, collections

BASE = r'D:/workspace/occt-src'


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


rows = []
for line in open(os.path.join(BASE, 'diff-numstat-tok2.txt'), encoding='utf-8', errors='ignore'):
    p = line.rstrip('\n').split('\t')
    if len(p) != 3:
        continue
    a, d, path = p
    o, n = expand(path)
    rows.append((o, n, 0 if a == '-' else int(a), 0 if d == '-' else int(d)))

added = collections.Counter()
removed = collections.Counter()
mod_churn = collections.Counter()
mod_files = collections.Counter()
mod_zero = 0
buckets = collections.Counter()
bucket_tok = collections.Counter()
for o, n, a, d in rows:
    if o and n:
        pkg = n.split('/')[0]
        mod_churn[pkg] += a + d
        mod_files[pkg] += 1
        if a + d == 0:
            mod_zero += 1
        ch = a + d
        if ch == 0:
            b = '0 (format/rename only)'
        elif ch < 20:
            b = '1-19 tokens'
        elif ch < 100:
            b = '20-99'
        elif ch < 500:
            b = '100-499'
        elif ch < 2000:
            b = '500-1999'
        else:
            b = '2000+'
        buckets[b] += 1
        bucket_tok[b] += ch
    elif o is None:
        added[n.split('/')[0]] += a
    elif n is None:
        removed[o.split('/')[0]] += d

print('=== token churn buckets (modified same-path files) ===')
for b in ('0 (format/rename only)', '1-19 tokens', '20-99', '100-499', '500-1999', '2000+'):
    print('  %-24s files=%5d tokens=%10s' % (b, buckets[b], f'{bucket_tok[b]:,}'))
print('  zero-token-change files:', mod_zero)

pkgs = set(added) | set(removed)
print('\n=== packages with both added and removed files (moves/reorg candidates) ===')
reorg = set()
for pkg in sorted(pkgs):
    a, r = added[pkg], removed[pkg]
    if a and r and min(a, r) / max(a, r) > 0.8:
        reorg.add(pkg)
        print('  %-22s +%8s -%8s' % (pkg, f'{a:,}', f'{r:,}'))

TEST_PKGS = {'GTests', 'QABugs', 'QANCollection'}
print('\n=== new-code totals ===')
A = sum(added.values())
R = sum(removed.values())
print('all new files tokens   : %s' % f'{A:,}')
print('all deleted file tokens: %s' % f'{R:,}')
print('new tokens excl. GTests/_toolkit/QABugs: %s' % f'{A - sum(added[p] for p in TEST_PKGS | {"_toolkit"}):,}')
print('deleted tokens excl. tests/Express/QANCollection: %s' % f'{R - sum(removed[p] for p in TEST_PKGS | {"Express", "ExpToCasExe"}):,}')
print('new tokens excl. tests + reorg packages: %s' % f'{A - sum(added[p] for p in TEST_PKGS | {"_toolkit"} | reorg):,}')
print('deleted tokens excl. tests + reorg packages: %s' % f'{R - sum(removed[p] for p in TEST_PKGS | {"Express", "ExpToCasExe"} | reorg):,}')
print('\nreorg packages:', ', '.join(sorted(reorg)))

# line-level ignoring whitespace by module including all file types
print('\n=== line churn -w by package (top 25, all files incl. add/delete) ===')
ladd = collections.Counter()
ldel = collections.Counter()
for line in open(os.path.join(BASE, 'diff-numstat-w.txt'), encoding='utf-8', errors='ignore'):
    p = line.rstrip('\n').split('\t')
    if len(p) != 3:
        continue
    a, d, path = p
    o, n = expand(path)
    pkg = (n or o).split('/')[0]
    ladd[pkg] += 0 if a == '-' else int(a)
    ldel[pkg] += 0 if d == '-' else int(d)
for pkg, _ in sorted(((p, ladd[p] + ldel[p]) for p in ladd), key=lambda kv: -kv[1])[:25]:
    print('  %-24s +%9s -%9s' % (pkg, f'{ladd[pkg]:,}', f'{ldel[pkg]:,}'))
