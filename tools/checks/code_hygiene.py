"""Code hygiene before a commit of a LocalSplitScreen mod.

Refuses a commit whose staged source adds what cleanup passes keep finding afterwards:

1. a raw offset outside the facts header: an added line of `src/` that adds a hex literal to an
   identifier (`node + 0x18`) or passes one as a Read*/Write* accessor's offset argument
   (`ReadPointer(map, 0x20)`), in a file that is not the facts header or a code-bytes header;
2. a duplicated line: an added line of code that already exists elsewhere in `src/` or `tests/`
   after the change, compared with its spacing collapsed, its trailing comment dropped and the
   facts header's aliases read as `facts::` (a copy that differs only in alignment is a copy);
3. a duplicated value: an added declaration or assignment whose right-hand side (40 characters or
   more, compared the same way) is already written elsewhere: the same expression under another
   variable's name;
4. a repeated predicate: a parenthesised group with && or || that already exists elsewhere;
5. the same game fact read twice: an added rebase of a game global (`image.Rebase(facts::kX`,
   `Global(image, facts::kX`) whose global another file also rebases, or an added pointer hop
   (`ReadPointer(x, facts::kY)`) through an offset another line also hops through: one reader, not
   copies;
6. a duplicated struct: an added plain-data struct whose fields (types and names) another struct
   already has (an engine type mirrored twice);
7. a dead function: an added function definition whose name is referenced nowhere else in
   `src/` or `tests/` (a helper no one calls).

Each finding names the file and line. Fix the findings, or name each in the commit message with
its reason and commit with LSS_HYGIENE_REVIEWED=1 set.

Three ways in:
  python tools/checks/code_hygiene.py staged [repo]   the staged diff (the git pre-commit hook in
      .githooks runs this): exit 1 with the findings, or 0 with them printed when
      LSS_HYGIENE_REVIEWED is set in the environment.
  python tools/checks/code_hygiene.py census <repo>   every line of the tree, as if all were added,
      by kind: for calibrating the rules, and for a cleanup pass's list.
  A Claude Code PreToolUse hook on Bash|PowerShell (no arguments, the event on stdin as JSON):
      a `git commit` in a mod repository is blocked (exit 2, the list on stderr) unless the command
      carries LSS_HYGIENE_REVIEWED=1, when the list comes back as context. Never
      `permissionDecision` (the desktop app fails a deferred call). Fails open on any unexpected
      error.

A mod repository is one with src/ beside tools/checks/ or a LocalSplitScreen folder name. ASCII only.
"""

import json
import os
import re
import subprocess
import sys

GIT_COMMIT = re.compile(
    r"\bgit\s+(?:(?:-[cC]|--git-dir|--work-tree|--namespace)\s+\S+\s+|--?[A-Za-z][-\w]*(?:=\S+)?\s+)*commit\b",
    re.IGNORECASE,
)
REPO_DIR = re.compile(r"(?:Set-Location|cd|git\s+-C)\s+(?:-Path\s+)?[\"']?([A-Za-z]:[^\s;\"']+)", re.IGNORECASE)
MOD_REPO = "LocalSplitScreen"
# The command sets the escape hatch (`LSS_HYGIENE_REVIEWED=1 git commit`, `$env:LSS_HYGIENE_REVIEWED = 1`), not
# merely names it in its message.
REVIEWED_WORD = re.compile(r"LSS_HYGIENE_REVIEWED\s*=\s*['\"]?(?:1|true|yes)\b", re.IGNORECASE)
FACTS_FILES = ("engine_facts.h", "_code.h", "layout_code.h", "facts/")
RAW_OFFSET = re.compile(r"\b[A-Za-z_]\w*\s*\+\s*0x[0-9A-Fa-f]+\b")
# The same offset passed straight to a game-memory accessor (`ReadPointer(map, 0x20)`, `ReadField<int>(pair, 0x8)`):
# a Read*/Write* call, its template arguments allowed, whose argument after the first is a hex literal.
RAW_ARGUMENT = re.compile(
    r"\b(?:Read|Write)\w*\s*(?:<[^<>;]*(?:<[^<>;]*>[^<>;]*)*>)?\s*\((?:[^;()]|\([^;()]*\))*?,\s*0x[0-9A-Fa-f]+\s*[,)]"
)
FUNCTION = re.compile(
    r"^(?!\s*(?:return|if|for|while|switch|case|else|catch|throw|using|template|static_assert|sizeof|delete|new)\b)"
    r"[A-Za-z_][\w:<>,\*&\s]*?\b([A-Z]\w*)\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:->\s*[\w:<>]+\s*)?$"
)
KEYWORDS = {"EXPECT", "Register", "Log", "LogDebug", "LogTrace", "LogWarning"}
FACTS_ALIAS = re.compile(r"namespace\s+(\w+)\s*=\s*(?:lss::)?facts\s*;")
TRAILING_COMMENT = re.compile(r"\s*//.*$")
REBASE = re.compile(r"\b(?:Rebase\s*\(\s*|Global\s*\(\s*image\s*,\s*)facts::(k\w+)")
HOP = re.compile(r"\bReadPointer\s*\((?:[^;()]|\([^;()]*\))*?,\s*facts::(k\w+)\s*\)")
ASSIGNED = re.compile(r"^[A-Za-z_][\w:<>,\*&\s\.\[\]]*?(?<![=!<>+\-*/%&|^])=(?!=)\s*(.+?);$")
STRUCT = re.compile(r"\bstruct\s+(\w+)\s*\{([^{}]*)\}\s*;")
STRUCT_HEAD = re.compile(r"^\s*struct\s+(\w+)\s*(?:\{.*)?$")
ACCESS = re.compile(r"^\s*(?:public|private|protected)\s*:")


def predicate_groups(text):
    """Every parenthesised group of `text`, at every nesting level, that holds && or || (a predicate): the same one in
    two places is a copy."""
    groups = []
    stack = []
    for i, ch in enumerate(text):
        if ch == "(":
            stack.append(i)
        elif ch == ")" and stack:
            start = stack.pop()
            group = text[start:i + 1]
            if "&&" in group or "||" in group:
                groups.append(group)
    return groups


def normalised(text, aliases):
    """A line as the copy checks compare it: trailing comment dropped, spacing collapsed, the facts header's aliases read
    as `facts::`."""
    s = re.sub(r"\s+", " ", TRAILING_COMMENT.sub("", text.strip()))
    for alias in aliases:
        s = re.sub(r"\b" + re.escape(alias) + r"::k", "facts::k", s)
    return s


def struct_fields(body):
    """A plain-data struct's fields as (type, name) pairs, in order; member functions, constants and access marks
    left out."""
    fields = []
    for decl in re.sub(r"//[^\n]*", "", body).split(";"):
        d = ACCESS.sub("", decl).strip()
        if not d or "(" in d or d.startswith(("static", "using", "friend", "constexpr", "template")):
            continue
        d = re.sub(r"\s*=\s*[^,]+", "", d)
        m = re.match(r"^(?P<type>.+?)\s+(?P<names>[A-Za-z_]\w*(?:\[\w*\])?(?:\s*,\s*[A-Za-z_]\w*(?:\[\w*\])?)*)$", d, re.S)
        if not m:
            continue
        kind = re.sub(r"\s+", " ", m.group("type")).strip()
        for name in m.group("names").split(","):
            fields.append((kind, name.strip()))
    return tuple(fields)


MIN_DUPLICATE_LEN = 48
MIN_EXPRESSION_LEN = 40
MIN_VALUE_LEN = 40
MAX_FINDINGS = 12


def run_git(repo, *args):
    out = subprocess.run(["git", "-C", repo, *args], capture_output=True, text=True, encoding="utf-8", errors="replace")
    return out.stdout if out.returncode == 0 else ""


def is_mod_repo(top):
    """A LocalSplitScreen mod repository: src/ beside tools/checks/, or the folder named so (a sibling mod's clone)."""
    return os.path.isdir(os.path.join(top, "src")) and (
        os.path.isdir(os.path.join(top, "tools", "checks")) or MOD_REPO in top.replace("\\", "/"))


def repo_of(command, cwd):
    """The mod repository the commit runs in: the last directory the command names, else the cwd."""
    candidates = REPO_DIR.findall(command)
    for path in reversed(candidates + [cwd]):
        top = run_git(path, "rev-parse", "--show-toplevel").strip()
        if top:
            return top
    return ""


def staged_additions(repo):
    """The added lines of the staged diff under src/ and tests/, as (file, line number, text)."""
    diff = run_git(repo, "diff", "--cached", "-U0", "--diff-filter=AM", "--no-color", "--", "src", "tests")
    added = []
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
            added.append((path, line, raw[1:]))
            line += 1
        elif not raw.startswith("-"):
            line += 1
    return added


def tree_files(repo):
    """Every tracked and staged file under src/ and tests/ as the working tree has it: {path: text}."""
    files = run_git(repo, "ls-files", "--", "src", "tests").split()
    texts = {}
    for rel in files:
        if not rel.endswith((".cpp", ".h", ".hpp", ".inl")):
            continue
        try:
            with open(os.path.join(repo, rel), encoding="utf-8", errors="replace") as f:
                texts[rel] = f.read().replace("\r\n", "\n")
        except OSError:
            continue
    return texts


def all_lines(texts):
    """Every line of `texts` as (file, line number, text): the census's "added" lines."""
    return [(rel, n, text) for rel, body in texts.items() for n, text in enumerate(body.split("\n"), 1)]


def is_code(text):
    s = text.strip()
    return bool(s) and not s.startswith(("//", "/*", "*", "#")) and 'L"' not in s and '"' not in s


PLAIN_CALL = re.compile(r"[\w:]+(?:<[^<>]*>)?\([\w\s,.:\->&*]*\)")  # one call, plain arguments: a helper being used
ONE_NAME_CAST = re.compile(r"(?:static|reinterpret|const)_cast<[^>]*>\(\s*[\w.\->]+\s*\)")


def plain_value(value):
    """A value that is a helper's call with plain arguments, or a cast of one name: using a helper, not copying one."""
    return PLAIN_CALL.fullmatch(value) is not None or ONE_NAME_CAST.fullmatch(value) is not None


def assigned_value(s):
    """The right-hand side of a declaration or assignment line (normalised) when it reads a game fact, else "". A lookup
    the game-fact rule judges (`FindFunction`, `Rebase`) and a plain value (above) are not values of their own."""
    m = ASSIGNED.match(s)
    value = m.group(1).strip() if m else ""
    if "facts::k" not in value or value.startswith(("platform::FindFunction(", "image.Rebase(")) or plain_value(value):
        return ""
    return value


def findings_of(repo, census=False):
    """The findings as (kind, text) pairs: the staged additions' by default, every line's for a census."""
    texts = tree_files(repo)
    added = all_lines(texts) if census else staged_additions(repo)
    if not added:
        return []
    aliases = {rel: set(FACTS_ALIAS.findall(body)) for rel, body in texts.items()}
    lines = [(rel, n, text, normalised(text, aliases[rel])) for rel, n, text in all_lines(texts)]

    counts, values, rebased, hops = {}, {}, {}, {}
    for rel, n, text, s in lines:
        if not is_code(text):
            continue
        if len(s) >= MIN_DUPLICATE_LEN:
            counts[s] = counts.get(s, 0) + 1
        value = assigned_value(s)
        if len(value) >= MIN_VALUE_LEN:
            values[value] = values.get(value, 0) + 1
        if not any(mark in rel for mark in FACTS_FILES):
            for fact in REBASE.findall(s):
                rebased.setdefault(fact, set()).add(rel)
            for fact in HOP.findall(s):
                hops.setdefault(fact, set()).add((rel, n))
    blob = "\n".join(s for _, _, text, s in lines if is_code(text))
    raw_blob = "\n".join(texts.values())
    structs = {}
    for rel, body in texts.items():
        for m in STRUCT.finditer(body):
            fields = struct_fields(m.group(2))
            if len(fields) >= 2:
                structs.setdefault(fields, []).append((rel, m.group(1)))

    out = []
    seen = set()
    for path, line, text in added:
        if not is_code(text):
            continue
        raw = text.strip()
        s = normalised(text, aliases.get(path, set()))
        in_facts = any(mark in path for mark in FACTS_FILES)
        if not in_facts and path.startswith("src/") and RAW_OFFSET.search(s):
            out.append(("raw offset", f"{path}:{line}: a raw offset added to an identifier ({raw[:80]}); name it in the facts header"))
        elif not in_facts and path.startswith("src/") and RAW_ARGUMENT.search(s):
            out.append(("raw offset", f"{path}:{line}: a raw offset passed to a memory read or write ({raw[:80]}); name it in the facts header"))
        loop_header = s.startswith(("for (", "while (", "for(", "while("))
        # A value that is one call with plain arguments (`ai = FieldAiController(member);`) is the helper being used.
        whole = re.fullmatch(r"[^=(]*=\s*(.+);", s)
        helper_call = whole is not None and plain_value(whole.group(1).strip())
        m = FUNCTION.match(raw)
        definition = bool(m) and not raw.endswith(";")  # a function's own header (every module's Install): not a copy
        signature  = bool(m) or bool(FUNCTION.match(raw.rstrip(";").rstrip()))  # a header's declaration of one either
        if (len(s) >= MIN_DUPLICATE_LEN and counts.get(s, 0) >= 2 and s not in seen and not loop_header and
                not signature and not helper_call and not s.endswith(("{", "}", ";)"))):
            seen.add(s)
            out.append(("line", f"{path}:{line}: this line exists elsewhere in src/ or tests/ ({raw[:80]}); one helper, not copies"))
        value = assigned_value(s)
        if len(value) >= MIN_VALUE_LEN and values.get(value, 0) >= 2 and value not in seen and s not in seen:
            seen.add(value)
            out.append(("value", f"{path}:{line}: the value {value[:80]} is written elsewhere too; one helper, not copies"))
        for group in predicate_groups(s):
            if ";" in group:
                continue  # a loop header, not a predicate
            if len(group) >= MIN_EXPRESSION_LEN and group not in seen and blob.count(group) >= 2:
                seen.add(group)
                out.append(("predicate", f"{path}:{line}: the expression {group[:80]} exists elsewhere in src/ or tests/; one predicate, not copies"))
        if not in_facts:
            for fact in REBASE.findall(s):
                files = rebased.get(fact, set())
                if len(files) >= 2 and ("rebase", fact) not in seen:
                    seen.add(("rebase", fact))
                    others = ", ".join(sorted(f for f in files if f != path)[:3])
                    out.append(("game fact", f"{path}:{line}: the game's {fact} is rebased in {len(files)} files (also {others}); one reader, not copies"))
            for fact in HOP.findall(s):
                sites = hops.get(fact, set())
                spread = len({f for f, _ in sites}) >= 2 or len(sites) >= 3  # two files, or three times in one
                if spread and ("hop", fact) not in seen:
                    seen.add(("hop", fact))
                    others = ", ".join(sorted(f"{f}:{n}" for f, n in sites if (f, n) != (path, line))[:3])
                    out.append(("game fact", f"{path}:{line}: the pointer hop through {fact} is written {len(sites)} times (also {others}); one accessor, not copies"))
        head = STRUCT_HEAD.match(raw)
        if head:
            for fields, owners in structs.items():
                # Another file's struct: two types of one shape in one file are told apart on purpose.
                elsewhere = [(f, n) for f, n in owners if f != path]
                if (path, head.group(1)) in owners and elsewhere and ("struct", fields) not in seen:
                    seen.add(("struct", fields))
                    others = ", ".join(f"{f} {n}" for f, n in elsewhere)
                    out.append(("struct", f"{path}:{line}: the struct {head.group(1)} has the fields of {others}; one type, not copies"))
        if definition:
            name = m.group(1)
            # A reference is a call (`Name(`, `Name<T>(`), the address taken (`&Name`, a redirect's or slot's
            # target) or the name passed or assigned as a value (`WithPage(fields, Name, ...)`, `= Name;`); the
            # definition itself is the one call-shaped match every function has. A constructor is the type's: used
            # wherever the type is.
            calls = len(re.findall(r"\b" + re.escape(name) + r"\s*[(<]", raw_blob))
            taken = len(re.findall(r"&\s*" + re.escape(name) + r"\b", raw_blob))
            passed = len(re.findall(r"[(,=]\s*" + re.escape(name) + r"\s*[,);]", raw_blob))
            constructor = re.search(r"\b(?:struct|class)\s+" + re.escape(name) + r"\b", raw_blob) is not None
            if name not in KEYWORDS and not constructor and calls + taken + passed <= 1:
                out.append(("dead function", f"{path}:{line}: the function {name} is referenced nowhere else in src/ or tests/; delete it or use it"))
        if not census and len(out) >= MAX_FINDINGS:
            out.append(("cut", "(more findings cut)"))
            break
    return out


def census(repo):
    """Every finding of the tree, by kind (for calibrating the rules and for a cleanup pass's list)."""
    findings = findings_of(repo, census=True)
    kinds = {}
    for kind, text in findings:
        kinds.setdefault(kind, []).append(text)
    for kind in sorted(kinds):
        print(f"== {kind}: {len(kinds[kind])}")
        for text in kinds[kind]:
            print("  " + text)
    return 0


def main():
    try:
        event = json.load(sys.stdin)
    except Exception:
        return 0
    try:
        command = str(event.get("tool_input", {}).get("command", "") or "")
        if not command or not GIT_COMMIT.search(command):
            return 0
        repo = repo_of(command, str(event.get("cwd", "") or ""))
        if not repo or not is_mod_repo(repo):
            return 0
        findings = findings_of(repo)
        if not findings:
            return 0
        text = "Code hygiene before this commit (" + repo + "):\n" + "\n".join("- " + f for _, f in findings)
        if REVIEWED_WORD.search(command):
            print(json.dumps({"hookSpecificOutput": {"hookEventName": "PreToolUse",
                                                     "additionalContext": text + "\nReviewed by the command's word; each finding must be named in the commit message with its reason."}}))
            return 0
        sys.stderr.write(text + "\nFix them, or name each in the commit message with its reason and put LSS_HYGIENE_REVIEWED=1 before the git call.\n")
        return 2
    except Exception:
        return 0


def reviewed(name):
    """The escape hatch is set: the environment variable `name` is 1, true or yes."""
    return os.environ.get(name, "").strip().lower() in ("1", "true", "yes")


def staged(repo):
    """The staged diff's findings for a git pre-commit hook: exit 1 with them, or 0 when reviewed. A fault in the check
    itself never blocks a commit: it is reported and the commit goes on."""
    try:
        top = run_git(repo, "rev-parse", "--show-toplevel").strip()
        if not top:
            print("code hygiene: not checked (git could not name the repository)", file=sys.stderr)
            return 0
        findings = findings_of(top)
    except Exception as error:  # noqa: BLE001
        print(f"code hygiene: not checked (the check failed: {error!r})", file=sys.stderr)
        return 0
    if not findings:
        return 0
    text = "Code hygiene before this commit:\n" + "\n".join("- " + f for _, f in findings)
    if reviewed("LSS_HYGIENE_REVIEWED"):
        print(text + "\nReviewed (LSS_HYGIENE_REVIEWED): name each finding in the commit message with its reason.")
        return 0
    sys.stderr.write(text + "\nFix them, or name each in the commit message with its reason and commit with "
                            "LSS_HYGIENE_REVIEWED=1 set.\n")
    return 1


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "census":
        sys.exit(census(sys.argv[2]))
    if len(sys.argv) in (2, 3) and sys.argv[1] == "staged":
        sys.exit(staged(sys.argv[2] if len(sys.argv) == 3 else "."))
    sys.exit(main())
