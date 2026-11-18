"""Makes the release zip, to unpack into the game's End\\Binaries\\Win64 folder over a UE4SS install.

    python package.py              build, run the host tests, then zip into dist/
    python package.py --no-build   zip what is built (refused when the built DLL or app is not this source's version;
                                   a change to the source without a new version is not seen: build)
    python package.py --nexus      the same without the setup app (Nexus Mods holds uploads with programs in them for a
                                   manual check; the app is then fetched from the repository's releases)

The zip holds ue4ss/Mods/LocalSplitScreen/ with dlls/main.dll, the setup app, enabled.txt (so UE4SS starts the mod
without a line in mods.txt), the licence with its exception, and UE4SS's licence for the files below; and beside it, in
ue4ss/, UE4SS's custom game config for this game, as RE-UE4SS ships it (its layouts, its signatures and its
UE4SS-settings.ini), with one change: HookLoadMap = 0 (with 1, UE4SS's map-load hook lands on the engine's exit path in
this game and every exit crashes). The config comes from the RE-UE4SS checkout the build uses (deps/RE-UE4SS, which must
be the release the mod is built for). The version is src/mod.cpp's and the setup app's name src/views/display_move.h's,
the lines the build reads too; the zip is named by the version's number alone (LocalSplitScreen-v1.1.0.zip for
"1.1.0-name"). No settings file of the mod is packed: the mod writes it.
"""
import argparse
import hashlib
import pathlib
import re
import subprocess
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent
CONFIG = "Game__Shipping__Win64"
FOLDER = "LocalSplitScreen"
UE4SS = ROOT / "deps" / "RE-UE4SS"
UE4SS_RELEASE = "v4.0.0"  # the UE4SS the mod is built against (README.md, Requirements)
GAME_CONFIG = UE4SS / "assets" / "CustomGameConfigs" / "Final Fantasy 7 Remake"


def line_value(path: pathlib.Path, pattern: str) -> str:
    found = re.search(pattern, path.read_text(encoding="utf-8"))
    if not found:
        sys.exit(f"{path.relative_to(ROOT)}: no line matches {pattern}")
    return found.group(1)


def number(version: str) -> str:
    """The version's number, its name dropped ("1.1.0" for "1.1.0-name"): what the zips and the release tag carry."""
    return version.split("-")[0]


def zip_path(version: str, nexus: bool) -> pathlib.Path:
    return ROOT / "dist" / f"{FOLDER}-v{number(version)}{'-nexus' if nexus else ''}.zip"


def run(*command: str) -> None:
    print(">", " ".join(command))
    if subprocess.run(command, cwd=ROOT).returncode != 0:
        sys.exit(f"failed: {' '.join(command)}")


def game_config() -> list[tuple[str, bytes]]:
    """UE4SS's custom game config for this game, as files under ue4ss/, its settings with HookLoadMap set to 0."""
    tag = subprocess.run(["git", "-C", str(UE4SS), "describe", "--tags", "--exact-match"], capture_output=True, text=True)
    if tag.returncode != 0 or tag.stdout.strip() != UE4SS_RELEASE:
        sys.exit(f"{UE4SS.relative_to(ROOT)} is not RE-UE4SS {UE4SS_RELEASE} ({tag.stdout.strip() or tag.stderr.strip()}): "
                 "see README.md, Building")
    if not GAME_CONFIG.is_dir():
        sys.exit(f"no game config at {GAME_CONFIG.relative_to(ROOT)}")
    files = []
    for path in sorted(p for p in GAME_CONFIG.rglob("*") if p.is_file()):
        data = path.read_bytes()
        if path.name == "UE4SS-settings.ini":
            data, count = re.subn(rb"(?m)^HookLoadMap\s*=\s*1[ \t]*(?=\r?$)", b"HookLoadMap = 0", data)
            if count != 1:
                sys.exit(f"{path.relative_to(ROOT)}: expected one 'HookLoadMap = 1' line, found {count}")
        files.append((f"ue4ss/{path.relative_to(GAME_CONFIG).as_posix()}", data))
    return files


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--no-build", action="store_true", help="zip what is already built")
    parser.add_argument("--nexus", action="store_true", help="leave the setup app out")
    args = parser.parse_args()

    version = line_value(ROOT / "src" / "mod.cpp", r'kVersion = L"([^"]+)"')
    setup = line_value(ROOT / "src" / "views" / "display_move.h", r'kSetupExe = L"([^"]+)"')
    if not args.no_build:
        if not (ROOT / "build" / "CMakeCache.txt").exists():
            sys.exit("build/ is not configured: see README.md, Building")
        run("cmake", "--build", "build", "--config", CONFIG)
        run("ctest", "--test-dir", "build", "-C", CONFIG, "--output-on-failure")

    built = ROOT / "build" / CONFIG
    dll = built / "LocalSplitScreen.dll"
    app = built / setup
    packed = [dll] if args.nexus else [dll, app]
    for path in packed:
        if not path.exists():
            sys.exit(f"not built: {path.relative_to(ROOT)}")
    # Both carry the version as wide text, the DLL in its code and the app in its file properties: a build of other source
    # is not packed under this one's name. The text is matched with its end, so 1.2.3 does not pass for 1.2.30.
    wide = (version + "\0").encode("utf-16-le")
    for path in packed:
        if wide not in path.read_bytes():
            sys.exit(f"{path.relative_to(ROOT)} is not version {version}: build first")

    mod = f"ue4ss/Mods/{FOLDER}"
    files = [(f"{mod}/dlls/main.dll", dll.read_bytes())]
    if not args.nexus:
        files.append((f"{mod}/{setup}", app.read_bytes()))
    files.append((f"{mod}/enabled.txt", b""))
    for name in ("LICENSE", "LICENSE-EXCEPTION"):
        if not (ROOT / name).exists():
            sys.exit(f"no {name} in the repository")
        files.append((f"{mod}/{name}", (ROOT / name).read_bytes()))
    files.append((f"{mod}/LICENSE-UE4SS", (UE4SS / "LICENSE").read_bytes()))
    files += game_config()

    out = zip_path(version, args.nexus)
    out.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in files:
            archive.writestr(name, data)
            print(f"  {name:<58} {len(data):>9} bytes  {hashlib.sha256(data).hexdigest()[:8].upper()}")
    print(f"{out.relative_to(ROOT)}: {out.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
