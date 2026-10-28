"""Build a package-level normalized view of OCCT 7.9.3 (same flat layout as 7.8.1)."""
import os

BASE = r'D:/workspace/occt-src'
A = os.path.join(BASE, 'OCCT-7.9.3')
N = os.path.join(BASE, 'normalized', '793')


def link(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if not os.path.exists(dst):
        try:
            os.link(src, dst)
        except OSError:
            with open(src, 'rb') as fi, open(dst, 'wb') as fo:
                fo.write(fi.read())


def is_legacy_toolkit_dir(path):
    names = set(os.listdir(path))
    return 'PACKAGES' in names and 'EXTERNLIB' in names


root = os.path.join(A, 'src')
n = 0
for dp, dn, fs in os.walk(root):
    rel = os.path.relpath(dp, root).replace(os.sep, '/')
    parts = rel.split('/')
    if len(parts) == 1 and is_legacy_toolkit_dir(dp):
        out = os.path.join(N, '_toolkit', 'legacy__' + parts[0])
    else:
        out = os.path.join(N, rel)
    for f in fs:
        link(os.path.join(dp, f), os.path.join(out, f))
        n += 1
print('normalized 793 files:', n)
