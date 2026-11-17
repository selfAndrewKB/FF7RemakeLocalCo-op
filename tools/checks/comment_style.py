"""The comment rule of the LocalSplitScreen mods' AGENTS.md ("Code"), enforced.

A comment says what the code does and why and stays true as the code ages. This check refuses comment text that
breaks that, in a LocalSplitScreen mod's C++ (src/ and tests/):

  date        a calendar date (2026-10-27)
  person      a credit to or quote of a person ("the user", "user's")
  version     a mod version (v0.137.1)
  section     a research section number or pass tag (section 7.2, P8.9, S-TGT5, pass SUMMONGAUGE, [DOWNCMD], "pass 9"
              next to a research file name)
  address     an image address (0x14xxxxxxx) outside the facts headers (Rebirth src/facts/ and src/platform/builds/,
              Remake src/engine_facts.h)
  other-mod   the sibling mod's name (Remake in Rebirth's code, Rebirth in Remake's; a repository with
              src/engine_facts.h is Remake's, one with src/facts/ Rebirth's)
  history     a change told against an earlier state (used to, originally, "Changed:", "Taken from", renamed from,
              retired, now also/instead)
  plan        a step number of the plan (step 7 next to "plan", "the close of step", session.md); a plan file's
              name alone is a pointer like a research file's and passes

"The user" and "step N" alone pass: a field action has a user and the game has field steps. The generated
src/platform/builds/build_*.h is skipped (a tool writes it from its notes).

Four ways in:
  `python tools/checks/comment_style.py staged [repo]`: the staged diff's added comment lines (the git pre-commit hook
    in .githooks runs this); exit 1 with the findings, or 0 with them printed when LSS_COMMENT_STYLE_REVIEWED is set
    in the environment, each then named in the commit message with its reason.
  `python tools/checks/comment_style.py check <paths...>`: the same check over whole files or folders, for a census;
    exit 1 on findings.
  A Claude Code PreToolUse hook on Write|Edit|MultiEdit: the new text's comment lines are checked; findings block the
    edit (exit 2, the list on stderr) so the comment is reworded before it lands.
  A Claude Code PreToolUse hook on Bash|PowerShell running `git commit` in a mod repository: as `staged`, the commit
    blocked unless the command carries LSS_COMMENT_STYLE_REVIEWED=1, when the findings come back as context.

A game term or an engine name that happens to match goes in comment_style_allow.txt beside this file (one regular
expression per line; a matching piece of a comment is taken out before the rules run; a line `path piece :: regex`
applies to the files whose path holds that piece only, for one file's own convention). The allow list is the only
escape at edit time, so a false positive is fixed once, visibly, not waved through.

Protocol: the event on stdin as JSON; exit 0 with no output for "no opinion"; exit 2 with the findings on stderr to
block. Never `permissionDecision` (the desktop app fails a deferred call). Fails open on any unexpected error. ASCII only.
"""

import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ALLOW_FILE = os.path.join(HERE, "comment_style_allow.txt")
MOD_REPO = "LocalSplitScreen"
# The command sets the escape hatch (`LSS_COMMENT_STYLE_REVIEWED=1 git commit`, `$env:LSS_COMMENT_STYLE_REVIEWED = 1`),
# not merely names it in its message.
REVIEWED_WORD = re.compile(r"LSS_COMMENT_STYLE_REVIEWED\s*=\s*['\"]?(?:1|true|yes)\b", re.IGNORECASE)
SOURCE_EXT = (".cpp", ".h", ".hpp", ".inl")
FACTS_FILES = ("engine_facts.h", "src/facts/", "src/platform/builds/")
GENERATED = ("src/platform/builds/build_",)  # written by a tool from its notes; not hand-written comments
MAX_FINDINGS = 20

GIT_COMMIT = re.compile(
    r"\bgit\s+(?:(?:-[cC]|--git-dir|--work-tree|--namespace)\s+\S+\s+|--?[A-Za-z][-\w]*(?:=\S+)?\s+)*commit\b",
    re.IGNORECASE,
)
REPO_DIR = re.compile(r"(?:Set-Location|cd|git\s+-C)\s+(?:-Path\s+)?[\"']?([A-Za-z]:[^\s;\"']+)", re.IGNORECASE)

# Each rule: (name, pattern, the words to say). The pattern runs over a comment's text with the allowed pieces taken out.
RULES = [
    ("date", re.compile(r"\b20\d\d-[01]\d-[0-3]\d\b"), "no date in a comment; the history goes in the topic's History"),
    ("person", re.compile(r"\bthe user's (?:call|choice|wish|request|report|ask|answer|decision|rule|words?|eye|play|backlog|go|OK)\b"
                          r"|\(the user\b|\bthe user, 20\d\d|\buser, 20\d\d"
                          r"|\bthe user (?:said|asked|wants|wanted|chose|decided|reported|saw|pivoted|confirmed|picked|read)\b"),
     "no credit to or quote of a person; state the rule or the reason as a fact"),
    ("version", re.compile(r"\bv\d+\.\d+(?:\.\d+)?\b"), "no mod version; the commit message and the plan carry versions"),
    ("section", re.compile(r"\xa7|\bsections? \d|\bP\d+\.\d+\b|\bS-[A-Z0-9]{3,}\b|\bpass(?:es)? [A-Z][A-Z0-9-]{3,}\b"
                           r"|\[(?!CONFIRMED|ASSUMPTION|UNKNOWN)[A-Z][A-Z0-9-]{3,}\]"),
     "no research section number or pass tag; name the research file alone"),
    ("section", re.compile(r"\bpass(?:es)? \d+(?:[-, ]+\d+)*\b(?=.*(?:RE-|research))|(?:RE-|research).*\bpass(?:es)? \d+\b"),
     "no research pass number; name the research file alone"),
    ("address", re.compile(r"\b0x1[0-9A-Fa-f]{8}\b"),
     "no image address in a comment outside the facts headers; name the function or the file's constant"),
    ("history", re.compile(r"\bused to\b|\bformerly\b|\boriginally\b|\bChanged:|\bTaken from\b"
                           r"|\brenamed from\b|\bmoved from\b|\b(?:is|are|was|were) retired\b|\bnow (?:also|instead)\b"
                           r"|\bhas been (?:moved|renamed|replaced|removed|retired)\b|\bearlier (?:version|build|session)\b"
                           r"|\bthe old (?:code|module|design|name|path)\b|\bonce (?:ran|did|had|was)\b"),
     "no history; say what the code does now and what would happen without it"),
    ("plan", re.compile(r"\bsession\.md\b|\bREADME item \d+\b|\bplan\b.*\bstep \d+\b|\bstep \d+\b.*\bplan\b|\bthe close of step\b"),
     "no step number of the plan; a plan file may be named, its steps move"),
]
OTHER_MOD = {"Rebirth": re.compile(r"\bRemake\b"), "Remake": re.compile(r"\bRebirth\b")}


def load_allow():
    """The allowed pieces: (pattern, path piece or None). A line `some/path :: regex` applies only to files whose path
    holds that piece (one file's own convention, such as the facts header's pointers into the research); a bare regex
    applies everywhere."""
    allow = []
    try:
        with open(ALLOW_FILE, encoding="utf-8") as f:
            for raw in f:
                line = raw.split("  #", 1)[0].strip()  # a trailing "  # reason" is not part of the pattern
                if line and not line.startswith("#"):
                    where = None
                    if " :: " in line:
                        where, line = (s.strip() for s in line.split(" :: ", 1))
                        where = where.replace("\\", "/")
                    try:
                        allow.append((re.compile(line), where))
                    except re.error:
                        continue
    except OSError:
        pass
    return allow


def comment_lines(text):
    """Each line's comment text, as (line number, text), with string literals ignored and /* */ blocks followed
    across lines. A line without a comment is absent."""
    out = []
    in_block = False
    for n, line in enumerate(text.splitlines(), 1):
        i = 0
        quote = ""
        pieces = []
        while i < len(line):
            ch = line[i]
            if in_block:
                end = line.find("*/", i)
                if end < 0:
                    pieces.append(line[i:])
                    i = len(line)
                else:
                    pieces.append(line[i:end])
                    in_block = False
                    i = end + 2
                continue
            if quote:
                if ch == "\\":
                    i += 2
                    continue
                if ch == quote:
                    quote = ""
                i += 1
                continue
            if ch in "\"'":
                quote = ch
                i += 1
                continue
            if line.startswith("//", i):
                pieces.append(line[i + 2:])
                break
            if line.startswith("/*", i):
                in_block = True
                i += 2
                continue
            i += 1
        joined = " ".join(p.strip() for p in pieces).strip()
        if joined:
            out.append((n, joined))
    return out


def mod_root(path):
    """The mod repository holding `path`: the nearest folder up with src/ beside tools/checks/, or named
    LocalSplitScreen with a src/ (a sibling mod's clone); "" when none."""
    d = os.path.abspath(path)
    while True:
        if os.path.isdir(os.path.join(d, "src")) and (
                os.path.isdir(os.path.join(d, "tools", "checks")) or os.path.basename(d) == MOD_REPO):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            return ""
        d = parent


def mod_of(path):
    """Which game's mod `path` belongs to, by the facts files its repository has."""
    root = mod_root(path)
    if root and os.path.isfile(os.path.join(root, "src", "engine_facts.h")):
        return "Remake"
    if root and os.path.isdir(os.path.join(root, "src", "facts")):
        return "Rebirth"
    return ""


def is_source(path):
    p = path.replace("\\", "/")
    return p.endswith(SOURCE_EXT) and ("/src/" in p or "/tests/" in p or p.startswith(("src/", "tests/")))


def check_text(path, text, allow, mod=""):
    """The findings for `text` as the comments of `path`: (line, rule, snippet, words)."""
    findings = []
    # The full path: a per-file allow names the file by its place under the project, whatever path the caller gave.
    p = os.path.abspath(path).replace("\\", "/")
    if any(mark in p for mark in GENERATED):
        return findings
    in_facts = any(mark in p for mark in FACTS_FILES)
    other = OTHER_MOD.get(mod or mod_of(p))
    for n, comment in comment_lines(text):
        cleaned = comment
        for a, where in allow:
            if where is None or where in p:
                cleaned = a.sub(" ", cleaned)
        for name, pattern, words in RULES:
            if name == "address" and in_facts:
                continue
            m = pattern.search(cleaned)
            if m:
                findings.append((n, name, comment.strip()[:90], words))
                break
        else:
            if other and other.search(cleaned):
                findings.append((n, "other-mod", comment.strip()[:90], "the other mod is not named; the origin of a piece is in the topic's History"))
    return findings


def check_paths(paths):
    allow = load_allow()
    out = []
    for root in paths:
        files = []
        if os.path.isdir(root):
            for d, _, names in os.walk(root):
                files += [os.path.join(d, x) for x in names if x.endswith(SOURCE_EXT)]
        elif os.path.isfile(root):
            files = [root]
        for f in sorted(files):
            try:
                with open(f, encoding="utf-8", errors="replace") as h:
                    text = h.read()
            except OSError:
                continue
            for n, name, snippet, words in check_text(f, text, allow):
                out.append(f"{f}:{n}: {name}: {snippet}  [{words}]")
    return out


def run_git(repo, *args):
    out = subprocess.run(["git", "-C", repo, *args], capture_output=True, text=True, encoding="utf-8", errors="replace")
    return out.stdout if out.returncode == 0 else ""


def repo_of(command, cwd):
    for path in reversed(REPO_DIR.findall(command) + [cwd]):
        top = run_git(path, "rev-parse", "--show-toplevel").strip()
        if top:
            return top
    return ""


def staged_findings(repo):
    """The findings over the staged diff's added lines: each added line is checked on its own, as a snippet."""
    allow = load_allow()
    mod = mod_of(repo)
    diff = run_git(repo, "diff", "--cached", "-U0", "--diff-filter=AM", "--no-color", "--", "src", "tests")
    out = []
    path = ""
    line = 0
    for raw in diff.splitlines():
        if raw.startswith("+++ "):
            path = raw[4:].strip()
            path = path[2:] if path.startswith("b/") else path
        elif raw.startswith("@@"):
            m = re.search(r"\+(\d+)", raw)
            line = int(m.group(1)) if m else 0
        elif raw.startswith("+") and not raw.startswith("+++"):
            if is_source(path):
                # The full path: a per-file allow names the file by its place under the project, not the repository.
                for _, name, snippet, words in check_text(os.path.join(repo, path), raw[1:], allow, mod):
                    out.append(f"{path}:{line}: {name}: {snippet}  [{words}]")
            line += 1
        elif not raw.startswith("-"):
            line += 1
    return out


def edit_findings(event):
    tool = str(event.get("tool_name", "") or "")
    inp = event.get("tool_input", {}) or {}
    path = str(inp.get("file_path", "") or "")
    if not path or not mod_root(path) or not is_source(path):
        return []
    texts = []
    if tool == "Write":
        texts.append(str(inp.get("content", "") or ""))
    elif tool == "Edit":
        texts.append(str(inp.get("new_string", "") or ""))
    elif tool == "MultiEdit":
        texts += [str(e.get("new_string", "") or "") for e in (inp.get("edits", []) or [])]
    allow = load_allow()
    out = []
    for text in texts:
        for n, name, snippet, words in check_text(path, text, allow):
            out.append(f"{os.path.basename(path)}, new text line {n}: {name}: {snippet}  [{words}]")
    return out


def staged(repo):
    """The staged diff's findings for a git pre-commit hook: exit 1 with them, or 0 when reviewed. A fault in the check
    itself never blocks a commit: it is reported and the commit goes on."""
    try:
        top = run_git(repo, "rev-parse", "--show-toplevel").strip()
        if not top:
            print("comment style: not checked (git could not name the repository)", file=sys.stderr)
            return 0
        findings = staged_findings(top)
    except Exception as error:  # noqa: BLE001
        print(f"comment style: not checked (the check failed: {error!r})", file=sys.stderr)
        return 0
    if not findings:
        return 0
    text = "Comment style before this commit:\n" + "\n".join("- " + f for f in findings[:MAX_FINDINGS])
    if os.environ.get("LSS_COMMENT_STYLE_REVIEWED", "").strip().lower() in ("1", "true", "yes"):
        print(text + "\nReviewed (LSS_COMMENT_STYLE_REVIEWED): name each finding in the commit message with its reason.")
        return 0
    sys.stderr.write(text + "\nReword them, or name each in the commit message with its reason and commit with "
                            "LSS_COMMENT_STYLE_REVIEWED=1 set.\n")
    return 1


def main():
    if len(sys.argv) in (2, 3) and sys.argv[1] == "staged":
        return staged(sys.argv[2] if len(sys.argv) == 3 else ".")
    if len(sys.argv) >= 2 and sys.argv[1] == "check":
        findings = check_paths(sys.argv[2:])
        for f in findings:
            print(f)
        print(f"{len(findings)} comment-style finding(s)")
        return 1 if findings else 0
    try:
        event = json.load(sys.stdin)
    except Exception:
        return 0
    try:
        tool = str(event.get("tool_name", "") or "")
        if tool in ("Write", "Edit", "MultiEdit"):
            findings = edit_findings(event)
            if not findings:
                return 0
            sys.stderr.write("Comment style (AGENTS.md's comment rule) refuses this edit:\n" +
                             "\n".join("- " + f for f in findings[:MAX_FINDINGS]) +
                             "\nReword the comment: what the code does and why, no date, person, version, section, tag, address, "
                             "other mod, history or plan pointer. A game term that matches by accident goes in "
                             "tools/checks/comment_style_allow.txt (one regex per line), named in the report.\n")
            return 2
        if tool in ("Bash", "PowerShell"):
            command = str(event.get("tool_input", {}).get("command", "") or "")
            if not command or not GIT_COMMIT.search(command):
                return 0
            repo = repo_of(command, str(event.get("cwd", "") or ""))
            if not repo or not mod_root(os.path.join(repo, "src")):
                return 0
            findings = staged_findings(repo)
            if not findings:
                return 0
            text = "Comment style before this commit (" + repo + "):\n" + "\n".join("- " + f for f in findings[:MAX_FINDINGS])
            if REVIEWED_WORD.search(command):
                print(json.dumps({"hookSpecificOutput": {"hookEventName": "PreToolUse",
                                                         "additionalContext": text + "\nReviewed by the command's word; each finding must be named in the commit message with its reason."}}))
                return 0
            sys.stderr.write(text + "\nReword them, or name each in the commit message with its reason and put "
                                    "LSS_COMMENT_STYLE_REVIEWED=1 before the git call.\n")
            return 2
        return 0
    except Exception:
        return 0


if __name__ == "__main__":
    sys.exit(main())
