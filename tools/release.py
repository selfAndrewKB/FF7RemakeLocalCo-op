"""Makes a GitHub release of a version tag: the release zip and the setup app on its own; the Nexus Mods zip is made
beside them in dist/, for upload by hand.

    python tools/release.py v1.0.0 --notes "Initial release."   build, test and package, then create the release
    python tools/release.py v1.0.0 --notes "..." --dry-run       everything but the release and the GitHub tag check

Refused unless the zips are the tag's: the tag names the version src/mod.cpp carries (v1.0.0 for "1.0.0", v1.1.0
for "1.1.0-name"); the working tree has no tracked changes and src/ and setup/ no untracked files; nothing
the zips are built from differs between the tag and HEAD (src/, setup/, CMakeLists.txt, the licences, package.py), so
HEAD may be a later commit of tooling or documentation; the RE-UE4SS checkout has no changes and is the one the build
uses; and, for a real release, the tag on GitHub is the same commit. It runs package.py (build, host tests, the release
zip) and package.py --no-build --nexus (without the setup app), then gh release create with the release zip and
MultiMonitorSetup.exe attached, so a page elsewhere can link the app at .../releases/latest/download/MultiMonitorSetup.exe.
The description is the --notes text as given. Needs the GitHub CLI (gh), signed in.
"""
import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
import package  # noqa: E402  (its version reader and number, zip names, build configuration, UE4SS release and runner)

BUILT_FROM = ["src", "setup", "CMakeLists.txt", "LICENSE", "LICENSE-EXCEPTION", "package.py"]


def git(*args: str, check: bool = True) -> str:
    done = subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True)
    if check and done.returncode != 0:
        sys.exit(f"git {' '.join(args)}: {done.stderr.strip()}")
    return done.stdout.strip()


def tag_commit(tag: str) -> str:
    """The commit a tag names locally (a tag only, never a branch of the same name)."""
    return git("rev-parse", "--verify", f"refs/tags/{tag}^{{commit}}")


def remote_tag_commit(tag: str) -> str:
    """The commit the tag names on GitHub, or empty: the peeled line of an annotated tag, else the tag's own."""
    lines = dict(reversed(line.split("\t")) for line in git("ls-remote", "origin", f"refs/tags/{tag}*").splitlines())
    return lines.get(f"refs/tags/{tag}^{{}}") or lines.get(f"refs/tags/{tag}", "")


def checked_sources(tag: str) -> None:
    if git("status", "--porcelain", "--untracked-files=no"):
        sys.exit("the working tree has tracked changes: commit or stash them first")
    if git("status", "--porcelain", "--untracked-files=all", "--", "src", "setup"):
        sys.exit("src/ or setup/ has untracked files the build may pick up: remove or commit them first")
    if subprocess.run(["git", "-C", str(ROOT), "diff", "--quiet", tag, "HEAD", "--", *BUILT_FROM]).returncode != 0:
        sys.exit(f"what the zips are built from differs between {tag} and HEAD: check out {tag} to release it")
    if subprocess.run(["git", "-C", str(package.UE4SS), "status", "--porcelain"], capture_output=True, text=True).stdout.strip():
        sys.exit(f"{package.UE4SS.relative_to(ROOT)} has local changes: the build would not be UE4SS {package.UE4SS_RELEASE}'s")
    cache = ROOT / "build" / "CMakeCache.txt"
    used = re.search(r"^UE4SS_ROOT:PATH=(.*)$", cache.read_text(encoding="utf-8", errors="replace"), re.M) if cache.exists() else None
    if used is None or pathlib.Path(used.group(1)).resolve() != package.UE4SS.resolve():
        sys.exit(f"build/ is not configured against {package.UE4SS.relative_to(ROOT)} (UE4SS_ROOT "
                 f"{used.group(1) if used else 'unset'}): see README.md, Building")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("tag", help="the version tag, e.g. v1.0.0")
    parser.add_argument("--notes", required=True, help="the release's description, as the maintainer writes it")
    parser.add_argument("--dry-run", action="store_true", help="build and package, but create no release")
    args = parser.parse_args()
    if not args.notes.strip():
        sys.exit("--notes is empty: a release needs the maintainer's description")

    version = package.line_value(ROOT / "src" / "mod.cpp", r'kVersion = L"([^"]+)"')
    if args.tag != "v" + package.number(version):
        sys.exit(f"{args.tag} is not the version src/mod.cpp carries ({version})")
    local = tag_commit(args.tag)
    checked_sources(args.tag)
    if not args.dry_run:
        remote = remote_tag_commit(args.tag)
        if remote != local:
            sys.exit(f"{args.tag} on GitHub is {remote[:8] or 'missing'}, here {local[:8]}: git push origin {args.tag}")

    package.run(sys.executable, "package.py")
    package.run(sys.executable, "package.py", "--no-build", "--nexus")
    zip_path = package.zip_path(version, nexus=False)
    nexus = package.zip_path(version, nexus=True)
    setup = package.line_value(ROOT / "src" / "views" / "display_move.h", r'kSetupExe = L"([^"]+)"')
    app = ROOT / "build" / package.CONFIG / setup
    for path in (zip_path, nexus, app):
        if not path.exists():
            sys.exit(f"missing: {path.relative_to(ROOT)}")

    notes = args.notes.strip() + "\n"
    print(f"release {args.tag}: {zip_path.name}, {app.name}; Nexus zip for upload by hand: {nexus.relative_to(ROOT)}")
    if args.dry_run:
        print("--- notes ---\n" + notes + "--- (dry run: no release made)")
        return 0
    with tempfile.NamedTemporaryFile("w", suffix=".md", delete=False, encoding="utf-8") as file:
        file.write(notes)
        notes_path = file.name
    try:
        package.run("gh", "release", "create", args.tag, str(zip_path), str(app), "--title", args.tag, "--notes-file",
                    notes_path, "--verify-tag")
    finally:
        pathlib.Path(notes_path).unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
