"""The README's project map: one line per folder of src/, and the repository's other folders.

    python tools/layout_doc.py            rewrite the map in README.md
    python tools/layout_doc.py --check    exit 1 when the map is out of date (a ctest, so the build says so)

The folders and top-level .h/.cpp files of src/ are read from disk; their descriptions are the tables below. A folder
or file with no description shows a marker, so the check fails until one is written. The repository's other folders
(OTHER) are a list kept here by hand. Only the fenced block under "## Project map" in the README is replaced.
"""
import pathlib
import re
import sys
import textwrap

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / 'src'
README = ROOT / 'README.md'

TOP = {
    'engine_facts.h': 'every game address and offset the mod uses',
    'mod.cpp': 'the entry point',
    'modules': 'every module, in install order',
}
FOLDERS = {
    'platform': 'game-code checks and patches, safe memory access, the log, settings',
    'control': 'the console on 127.0.0.1:4418',
    'players': 'joining and leaving, the party member each player plays, pads, cameras, running one game call as '
               'player 2 or 3',
    'battle': 'command menus, targeting, battle cameras, enemy attention, battle options',
    'world': 'NPCs, fades, field actions, talks, shops, catch-up warps',
    'views': 'screen layouts, each view\'s camera fit, full-screen moments, multiple monitors',
    'hud': 'each player\'s HUD',
    'render': 'each player\'s picture: the split-screen render fixes, shadows, fog, the sun, movies',
    'audio': 'sound heard from every player\'s character',
    'menu': 'the Co-op rows in the main menu and on the title screen, and the Co-op page they open',
}
ORDER = ['platform', 'control', 'players', 'battle', 'world', 'views', 'hud', 'render', 'audio', 'menu']
OTHER = [
    ('setup/', 'the Multi Monitor Setup app'),
    ('tests/', 'host tests, run without the game'),
    ('tools/', 'console client and game control, virtual pad, commit checks, crash and performance tools'),
]
MISSING = '(no description: add one to tools/layout_doc.py)'
WIDTH = 120
NAME_COL = 20


def row(name: str, desc: str, indent: int) -> str:
    head = ' ' * indent + name
    body = textwrap.wrap(desc, width=WIDTH - NAME_COL, break_long_words=False, break_on_hyphens=False) or ['']
    if len(head) >= NAME_COL:
        return '\n'.join([head] + [' ' * NAME_COL + b for b in body])
    return '\n'.join([head.ljust(NAME_COL) + body[0]] + [' ' * NAME_COL + b for b in body[1:]])


def render() -> str:
    tops = sorted({p.stem if p.stem in TOP else p.name for p in SRC.iterdir() if p.suffix in ('.h', '.cpp')})
    folders = {p.name for p in SRC.iterdir() if p.is_dir()}
    lines = ['src/']
    lines += [row(name, TOP.get(name, MISSING), 2) for name in tops]
    lines += [row(f + '/', FOLDERS.get(f, MISSING), 2)
              for f in [f for f in ORDER if f in folders] + sorted(folders - set(ORDER))]
    lines += [row(name, desc, 0) for name, desc in OTHER]
    return '\n'.join(lines)


def main() -> int:
    try:
        raw = README.read_bytes().decode('utf-8')
    except (OSError, UnicodeDecodeError) as error:
        print(f'README could not be read: {error}', file=sys.stderr)
        return 2
    crlf = '\r\n' in raw
    text = raw.replace('\r\n', '\n')
    m = re.search(r'(## Project map\n(?:[^\n]*\n)*?\n?```\n)(.*?)(```)', text, flags=re.S)
    if not m:
        print('README has no "## Project map" fenced block', file=sys.stderr)
        return 2
    new = text[:m.start(2)] + render() + '\n' + text[m.end(2):]
    stale = new != text
    if '--check' in sys.argv:
        print(f'README project map {"OUT OF DATE: run python tools/layout_doc.py" if stale else "current"}')
        return 1 if stale else 0
    README.write_bytes((new.replace('\n', '\r\n') if crlf else new).encode('utf-8'))
    print(f'project map written to {README}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
