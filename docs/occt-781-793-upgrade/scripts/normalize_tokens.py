"""Build token-stream copies of both trees to measure *semantic* code churn.

Each source file becomes one token per line, so line re-wrapping / clang-format
has no effect and `git diff --numstat` counts added/deleted *tokens*.

Normalized away (mechanical migrations):
  comments, whitespace, line breaks
  occ::handle<X> / opencascade::handle<X> / Handle_X  ->  Handle(X)
  Standard_Integer/Real/Boolean/...                   ->  int/double/bool/...
  std::int32_t/uint8_t/size_t/...                     ->  same canonical tokens
  Standard_EXPORT / Standard_HIDDEN                   ->  dropped
"""
import os, re

BASE = r'D:/workspace/occt-src'
N1 = os.path.join(BASE, 'normalized', '781')
N2 = os.path.join(BASE, 'normalized', '801')
T1 = os.path.join(BASE, 'tok2', '781')
T2 = os.path.join(BASE, 'tok2', '801')

EXTS = {'.hxx', '.cxx', '.pxx', '.gxx', '.lxx', '.h', '.hpp', '.cpp', '.inl', '.ixx'}

PRE = [
    (re.compile(r'\b(?:opencascade|occ)::handle\s*<\s*([A-Za-z_]\w*(?:::\w+)*)\s*>'), r'Handle(\1)'),
    (re.compile(r'\bHandle_([A-Za-z_]\w*)\b'), r'Handle(\1)'),
    (re.compile(r'\bStandard_Integer\b'), 'INT'),
    (re.compile(r'\bstd::int32_t\b'), 'INT'),
    (re.compile(r'\bStandard_Real\b'), 'REAL'),
    (re.compile(r'\bStandard_ShortReal\b'), 'FLOAT'),
    (re.compile(r'\bStandard_Boolean\b'), 'BOOL'),
    (re.compile(r'\bStandard_Byte\b'), 'U8'),
    (re.compile(r'\bstd::uint8_t\b'), 'U8'),
    (re.compile(r'\bstd::uint32_t\b'), 'U32'),
    (re.compile(r'\bstd::int64_t\b'), 'I64'),
    (re.compile(r'\bstd::uint64_t\b'), 'U64'),
    (re.compile(r'\bStandard_Size\b'), 'SIZE_T'),
    (re.compile(r'\bstd::size_t\b'), 'SIZE_T'),
    (re.compile(r'\bStandard_Character\b'), 'CHAR'),
    (re.compile(r'\bStandard_True\b'), 'TRUE'),
    (re.compile(r'\bStandard_False\b'), 'FALSE'),
    (re.compile(r'\bStandard_EXPORT\b'), ' '),
    (re.compile(r'\bStandard_HIDDEN\b'), ' '),
]

TOKEN = re.compile(
    r'"(?:\\.|[^"\\])*"'      # string literal
    r"|'(?:\\.|[^'\\])*'"     # char literal
    r'|[A-Za-z_]\w*'          # identifier
    r'|\d[\w.]*'              # number
    r'|::|->|<<|>>|<=|>=|==|!=|&&|\|\|\+\+|--'
    r'|\S'
)

KEYMAP = {
    'int': 'INT', 'signed': 'INT',
    'double': 'REAL',
    'float': 'FLOAT',
    'bool': 'BOOL',
    'size_t': 'SIZE_T',
    'unsigned': 'UNSIGNED',
    'true': 'TRUE', 'false': 'FALSE',
}


def strip_comments(txt):
    """Remove // and /* */ comments while respecting string literals."""
    out = []
    i, n = 0, len(txt)
    while i < n:
        c = txt[i]
        if c == '"' or c == "'":
            q = c
            out.append(c)
            i += 1
            while i < n:
                if txt[i] == '\\':
                    out.append(txt[i:i + 2])
                    i += 2
                    continue
                out.append(txt[i])
                if txt[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c == '/' and i + 1 < n:
            if txt[i + 1] == '/':
                j = txt.find('\n', i)
                i = n if j < 0 else j
                continue
            if txt[i + 1] == '*':
                j = txt.find('*/', i + 2)
                i = n if j < 0 else j + 2
                out.append(' ')
                continue
        out.append(c)
        i += 1
    return ''.join(out)


def norm_text(txt):
    txt = strip_comments(txt)
    for rx, rep in PRE:
        txt = rx.sub(rep, txt)
    toks = []
    for t in TOKEN.findall(txt):
        t = KEYMAP.get(t, t)
        if t in ('Standard_EXPORT', 'Standard_HIDDEN'):
            continue
        toks.append(t)
    return '\n'.join(toks) + ('\n' if toks else '')


def run(root, out_root):
    n = 0
    for dp, dn, fs in os.walk(root):
        for f in fs:
            if os.path.splitext(f)[1].lower() not in EXTS:
                continue
            src = os.path.join(dp, f)
            rel = os.path.relpath(src, root)
            dst = os.path.join(out_root, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            txt = open(src, encoding='utf-8', errors='ignore').read()
            with open(dst, 'w', encoding='utf-8', newline='\n') as fh:
                fh.write(norm_text(txt))
            n += 1
    return n


if __name__ == '__main__':
    print('781:', run(N1, T1))
    print('801:', run(N2, T2))
