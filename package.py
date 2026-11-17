"""Makes the release zip: the mod as a folder to drop into UE4SS's Mods folder.

    python package.py              build, run the host tests, then zip into dist/
    python package.py --no-build   zip what is built (refused when the built DLL or app is not this source's version;
                                   a change to the source without a new version is not seen: build)

The zip holds LocalSplitScreen/ with dlls/main.dll, the setup app, enabled.txt (so UE4SS starts the mod without a line in
mods.txt) and the licence with its exception when the repository has them. The version is src/mod.cpp's and the setup
app's name src/views/display_move.h's, the lines the build reads too. No settings file is packed: the mod writes it.
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


def line_value(path: pathlib.Path, pattern: str) -> str:
    found = re.search(pattern, path.read_text(encoding="utf-8"))
    if not found:
        sys.exit(f"{path.relative_to(ROOT)}: no line matches {pattern}")
    return found.group(1)


def run(*command: str) -> None:
    print(">", " ".join(command))
    if subprocess.run(command, cwd=ROOT).returncode != 0:
        sys.exit(f"failed: {' '.join(command)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--no-build", action="store_true", help="zip what is already built")
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
    for path in (dll, app):
        if not path.exists():
            sys.exit(f"not built: {path.relative_to(ROOT)}")
    # Both carry the version as wide text, the DLL in its code and the app in its file properties: a build of other source
    # is not packed under this one's name. The text is matched with its end, so 1.2.3 does not pass for 1.2.30.
    wide = (version + "\0").encode("utf-16-le")
    for path in (dll, app):
        if wide not in path.read_bytes():
            sys.exit(f"{path.relative_to(ROOT)} is not version {version}: build first")

    files = [(f"{FOLDER}/dlls/main.dll", dll.read_bytes()),
             (f"{FOLDER}/{setup}", app.read_bytes()),
             (f"{FOLDER}/enabled.txt", b"")]
    licence = next((p for name in ("LICENSE", "LICENSE.md", "LICENSE.txt") if (p := ROOT / name).exists()), None)
    if licence is not None:
        files.append((f"{FOLDER}/{licence.name}", licence.read_bytes()))
        if (exception := ROOT / "LICENSE-EXCEPTION").exists():
            files.append((f"{FOLDER}/{exception.name}", exception.read_bytes()))
    else:
        print("note: the repository has no licence file, so the zip carries none")

    out = ROOT / "dist" / f"{FOLDER}-v{version}.zip"
    out.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in files:
            archive.writestr(name, data)
            print(f"  {name:<44} {len(data):>9} bytes  {hashlib.sha256(data).hexdigest()[:8].upper()}")
    print(f"{out.relative_to(ROOT)}: {out.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
