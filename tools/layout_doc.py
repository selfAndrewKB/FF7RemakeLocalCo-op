"""The mod README's Layout, generated from each module's first comment.

Run from anywhere:  python tools/layout_doc.py           (writes the README's Layout)
                    python tools/layout_doc.py --check   (exit 1 when the README is out of date)

Every module under src/ (a header, or a .cpp with no header) is one row: its name and the first sentence of the
first comment block in its header (the paragraph of `//` lines after `#pragma once`, before any code). A module
with no such comment shows "(no header comment)" so the gap is visible. The folders carry a one-line description
kept in FOLDERS below (the only hand-kept text). tests/ is one row with its count. The block between the README's
"## Layout" fence lines is replaced; nothing else in the README is touched. Run it at every milestone.
"""
import pathlib, re, sys, textwrap

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / 'src'
README = ROOT / 'README.md'

FOLDERS = {
    'platform': 'the image, verified functions and vtable slots, code and call redirects, guards, the log, the settings file',
    'control': 'the console: the command registry, the language, the runner, the socket (127.0.0.1:4418)',
    'hud': 'the HUD per column: the band, the images, the passes, the markers, the minimap, the arrows',
    'players': 'the seats: the session, the party member and its funnels, the scopes, the pads, each seat\'s camera and trigger',
    'render': 'the picture per view: the ghost fixes, one family per player, shadows, fog, sun, the movie frame, the haze watches',
    'world': 'the world around every player: fades, animation, NPCs, hiding, the field targets, walk-up moves, talks, shops',
    'views': 'the views: the span over a row of monitors, the columns, the mirror player, the scene camera, the split scale',
    'battle': 'battle per seat: the command menu, targeting, the cursor, the lock and sequence cameras, the enemies\' attention, the options',
    'audio': 'sound for every player: a listener per further player, each sound measured from the nearest',
    'menu': "the mod's own menu: a Co-op row in the game's main menu opening a page of the game's Options screen",
}
ORDER = ['platform', 'control', 'hud', 'players', 'render', 'world', 'views', 'battle', 'audio', 'menu']
WIDTH = 120
NAME_COL = 28  # "    name" padded to this column, then the description


def first_comment(path: pathlib.Path) -> str:
    """The first sentence of the first `//` paragraph before any code line."""
    lines = path.read_text(encoding='utf-8').split('\n')
    para = []
    for line in lines[:60]:
        s = line.strip()
        if para:
            if s.startswith('//'):
                body = s[2:].strip()
                if body == '' or body.startswith('- '):
                    break  # the paragraph ends; a list is not the sentence
                para.append(body)
                continue
            break
        if s.startswith('//'):
            body = s[2:].strip()
            if body:
                para.append(body)
            continue
        if s == '' or s.startswith('#pragma') or s.startswith('#include') or s.startswith('#define'):
            continue
        break  # code before any comment
    if not para:
        return '(no header comment)'
    text = ' '.join(para)
    sentence = text
    for m in re.finditer(r'\.\s', text):
        if re.search(r'(e\.g|i\.e|vs|cf)$', text[:m.start()]):
            continue
        sentence = text[:m.start() + 1]
        break
    if sentence.endswith('.') and not sentence.endswith('..'):
        sentence = sentence[:-1]
    if len(sentence) > 160:
        # The references in parentheses (research files, sibling-mod counterparts, quoted words) belong to the header.
        shorter = re.sub(r'\s*\((?:[^()]|\([^()]*\))*(?:RE-|research/|plan/|Rebirth|the user|tests/|§)(?:[^()]|\([^()]*\))*\)', '', sentence)
        if shorter.strip():
            sentence = shorter.strip().rstrip(',;:')
    if len(sentence) > 230:
        cut = sentence[:230].rsplit(' ', 1)[0]
        sentence = cut + ' …'
    return sentence


def modules():
    """{folder: [(name, description)]} and the top-level rows, headers preferred over their .cpp."""
    by_folder = {}
    top = []
    seen = set()
    for path in sorted(SRC.rglob('*')):
        if path.suffix not in ('.h', '.cpp'):
            continue
        rel = path.relative_to(SRC)
        key = rel.with_suffix('')
        if key in seen:
            continue
        header = path.with_suffix('.h')
        src = header if header.exists() else path
        seen.add(key)
        name = key.name + ('.h' if not path.with_suffix('.cpp').exists() and src.suffix == '.h' else '')
        row = (name, first_comment(src))
        if len(rel.parts) == 1:
            top.append((rel.name, first_comment(src)))
        else:
            by_folder.setdefault(rel.parts[0], []).append(row)
    return top, by_folder


def wrap(name: str, desc: str, indent: int) -> str:
    head = ' ' * indent + name
    pad = max(NAME_COL - len(head), 1)
    first = head + ' ' * pad
    body = textwrap.wrap(desc, width=WIDTH - NAME_COL, break_long_words=False, break_on_hyphens=False) or ['']
    out = [first + body[0]]
    out += [' ' * NAME_COL + b for b in body[1:]]
    return '\n'.join(out)


def render() -> str:
    top, by_folder = modules()
    lines = ['src/']
    for name, desc in top:
        lines.append(wrap(name, desc, 2))
    folders = [f for f in ORDER if f in by_folder] + sorted(f for f in by_folder if f not in ORDER)
    for folder in folders:
        lines.append(wrap(folder + '/', FOLDERS.get(folder, '(no folder description: add it to tools/layout_doc.py)'), 2))
        for name, desc in by_folder[folder]:
            lines.append(wrap(name, desc, 4))
    tests = sorted((ROOT / 'tests').glob('*_test.cpp'))
    lines.append(wrap('tests/', f'{len(tests)} host-side test programs (ctest), one per pure header or code stub; run without the game', 0))
    count = len(top) + sum(len(v) for v in by_folder.values())
    return '\n'.join(lines), count


def main() -> int:
    block, count = render()
    raw = README.read_bytes().decode('utf-8')
    crlf = '\r\n' in raw
    text = raw.replace('\r\n', '\n')
    m = re.search(r'(## Layout\n\n```\n)(.*?)(```)', text, flags=re.S)
    if not m:
        print('README has no "## Layout" fenced block', file=sys.stderr)
        return 2
    new = text[:m.start(2)] + block + '\n' + text[m.end(2):]
    stale = new != text
    if '--check' in sys.argv:
        print(f'{count} modules; README Layout {"OUT OF DATE" if stale else "current"}')
        return 1 if stale else 0
    README.write_bytes((new.replace('\n', '\r\n') if crlf else new).encode('utf-8'))
    print(f'{count} modules written to {README}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
