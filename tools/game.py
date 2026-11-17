"""Drive FF7 Remake and the LocalSplitScreen mod from the host.

    python tools/game.py shell                      an interactive console on the mod (tab completes commands)
    python tools/game.py -c join -c "wait-until p2.state == playing"
                                                    send lines, print every reply, exit 1 on a failure
    python tools/game.py run tools/scripts/smoke.txt
                                                    send a file line by line, stopping at the first failure
    python tools/game.py status                     every readable value (exit 2 if the mod is not listening)
    python tools/game.py launch                     start the game through Steam and wait for the console
    python tools/game.py quit                       players 3 and 2 leave, then the engine's quit; waits for the exit
    python tools/game.py relaunch [--deploy [--label v0.8.0]] [--no-launch]
                                                    quit, optionally install the build (hash-checked, backed up) and
                                                    the setup app and set console = on in the settings file, keep the
                                                    old UE4SS.log, launch, wait for the console
    python tools/game.py load-save                  from the title screen: any button, Continue, Yes; waits on the
                                                    mod's game.state / field.step readings for the load
    python tools/game.py shot [--out file] [--width 1280]
    python tools/game.py keys Enter Enter           keys to the game window (tools/keys.ps1); --settle S waits S
                                                    seconds before the first key (a manual save from the title:
                                                    keys --settle 10 --gap 8000 Enter Down Enter Down Enter)

The mod listens on 127.0.0.1:4418 (src/control/remote.h): one command per line, UTF-8; every reply is one line
starting with ok, failed, rejected, dropped, value, help or note (src/control/runner.h). `help` lists the
commands the running build registered.

The game is found through Steam's library list; FF7R_GAME_DIR (the game's End/Binaries/Win64 folder) overrides it.
Screenshots go to LSS_SHOTS_DIR, else a folder in the temp directory.
"""
import argparse
import ctypes
import hashlib
import os
import pathlib
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import winreg

# ---- the one place that knows where things are ------------------------------------------------------
EXE_NAME  = "ff7remake_.exe"
APP_ID    = 1462040
GAME_SUBDIR = pathlib.Path("steamapps") / "common" / "FINAL FANTASY VII REMAKE" / "End" / "Binaries" / "Win64"


def steam_dir() -> pathlib.Path:
    """Steam's own folder, from the registry key Steam writes (the default install path if it is missing)."""
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as key:
            return pathlib.Path(winreg.QueryValueEx(key, "SteamPath")[0])
    except OSError:
        return pathlib.Path(r"C:\Program Files (x86)\Steam")


def game_dir() -> pathlib.Path:
    """The game's Win64 folder: FF7R_GAME_DIR when set, else the Steam library that holds the game (libraryfolders.vdf)."""
    if os.environ.get("FF7R_GAME_DIR"):
        return pathlib.Path(os.environ["FF7R_GAME_DIR"])
    steam = steam_dir()
    libraries = [steam]
    try:
        vdf = (steam / "steamapps" / "libraryfolders.vdf").read_text(encoding="utf-8", errors="replace")
        libraries += [pathlib.Path(p.replace("\\\\", "\\")) for p in re.findall(r'"path"\s+"([^"]+)"', vdf)]
    except OSError:
        pass
    for library in libraries:
        if (library / GAME_SUBDIR / EXE_NAME).exists():
            return library / GAME_SUBDIR
    return steam / GAME_SUBDIR


GAME_DIR  = game_dir()
STEAM     = steam_dir() / "steam.exe"


def game_found() -> bool:
    """The game's exe is in GAME_DIR; else says how to point the tools at it."""
    if (GAME_DIR / EXE_NAME).exists():
        return True
    print(f"the game was not found ({GAME_DIR / EXE_NAME} does not exist): set FF7R_GAME_DIR to the game's "
          r"End\Binaries\Win64 folder")
    return False


def ps_quote(path: pathlib.Path) -> str:
    """`path` as a PowerShell single-quoted string (a quote inside doubled)."""
    return "'" + str(path).replace("'", "''") + "'"
MOD_DIR   = GAME_DIR / "ue4ss" / "Mods" / "LocalSplitScreen"
SETTINGS_FILE = "LocalSplitScreen.ini"  # src/platform/settings.h kFileName
UE4SS_LOG = GAME_DIR / "ue4ss" / "UE4SS.log"
TOOLS     = pathlib.Path(__file__).resolve().parent
BUILD_DLL = TOOLS.parent / "build" / "Game__Shipping__Win64" / "LocalSplitScreen.dll"
SHOTS_DIR = pathlib.Path(os.environ.get("LSS_SHOTS_DIR") or pathlib.Path(tempfile.gettempdir()) / "LocalSplitScreen-shots")
HOST, PORT = "127.0.0.1", 4418
READY_TIMEOUT_S = 150      # from the Steam launch to the console answering
EXIT_TIMEOUT_S  = 120      # from `quit` to the process being gone
STEAM_RELAUNCH_GAP_S = 10  # a launch right after a quit can silently not start


class Console:
    """One connection to the mod's console."""

    def __init__(self, timeout: float = 120.0):
        self.sock = socket.create_connection((HOST, PORT), timeout=5.0)
        self.sock.settimeout(timeout)
        self.buffer = b""
        self.sent = 0

    def close(self):
        self.sock.close()

    def send(self, line: str) -> int:
        """Sends one line; returns its number as the mod will name it in replies."""
        self.sock.sendall((line.rstrip("\r\n") + "\n").encode("utf-8"))
        self.sent += 1
        return self.sent

    def reply(self) -> str:
        """The next reply line (blocks up to the timeout)."""
        while b"\n" not in self.buffer:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("the mod closed the connection")
            self.buffer += chunk
        line, _, self.buffer = self.buffer.partition(b"\n")
        return line.decode("utf-8", errors="replace").rstrip("\r")

    def command(self, line: str, echo=print) -> bool:
        """Sends a line and prints its replies until the one that completes it. False on failed/rejected."""
        number = self.send(line)
        while True:
            reply = self.reply()
            echo(reply)
            word, _, rest = reply.partition(" ")
            if word in ("ok", "failed", "rejected", "note") and rest.split(" ", 1)[0] == str(number):
                return word in ("ok", "note")
            if word == "dropped" and rest.split(" ", 1)[0] == str(number):
                return False

    def values(self) -> dict:
        number = self.send("status")
        out = {}
        while True:
            reply = self.reply()
            word, _, rest = reply.partition(" ")
            if word == "value":
                key, _, value = rest.partition(" ")
                out[key] = value
            elif word == "ok" and rest.split(" ", 1)[0] == str(number):
                return out

    def names(self) -> list:
        number = self.send("help")
        out = []
        while True:
            reply = self.reply()
            word, _, rest = reply.partition(" ")
            if word == "help":
                out.append(rest.split(" ", 1)[0])
            elif word == "ok" and rest.split(" ", 1)[0] == str(number):
                return out


def connect(timeout: float = 120.0):
    try:
        return Console(timeout)
    except OSError:
        sys.exit(f"the mod is not listening on {HOST}:{PORT} (is the game running with the mod, and is console = on under "
             f"[Development] in {MOD_DIR / SETTINGS_FILE}? relaunch --deploy sets it)")


def game_process():
    """The game's PID, or None (tasklist: no extra packages)."""
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {EXE_NAME}", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0].lower() == EXE_NAME:
            return int(parts[1])
    return None


def wait_for_console(seconds: float) -> bool:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            console = Console(5.0)
            console.values()
            console.close()
            return True
        except OSError:
            time.sleep(1.0)
    return False


# ---- subcommands ---------------------------------------------------------------------------------------
def cmd_shell(args) -> int:
    console = connect(timeout=3600.0)
    names = console.names() + ["get", "status", "help", "wait", "wait-until", "hold", "note", "abort"]
    try:
        import readline  # noqa: F401  (pyreadline3 on Windows, if installed)
        readline.set_completer(lambda text, state: ([n for n in names if n.startswith(text)] + [None])[state])
        readline.parse_and_bind("tab: complete")
    except ImportError:
        pass
    print(f"connected to {HOST}:{PORT}; `help` lists the commands, Ctrl+C aborts the queue, Ctrl+D leaves")
    while True:
        try:
            line = input("lss> ").strip()
        except EOFError:
            print()
            break
        except KeyboardInterrupt:
            print()
            console.command("abort")
            continue
        if not line:
            continue
        if line in ("exit", "quit-shell"):
            break
        try:
            console.command(line)
        except KeyboardInterrupt:
            print()
            console.command("abort")
    console.close()
    return 0


def run_lines(lines, stop_on_failure: bool) -> int:
    console = connect()
    ok = True
    for line in lines:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if not console.command(line):
            ok = False
            if stop_on_failure:
                break
    console.close()
    return 0 if ok else 1


def cmd_c(args) -> int:
    return run_lines(args.command, stop_on_failure=True)


def cmd_run(args) -> int:
    return run_lines(pathlib.Path(args.script).read_text(encoding="utf-8").splitlines(), stop_on_failure=True)


def cmd_status(args) -> int:
    try:
        console = Console(10.0)
    except OSError:
        print("not listening")
        return 2
    for key, value in console.values().items():
        print(f"{key} = {value}")
    console.close()
    return 0


def launch(extra: tuple[str, ...] = ()) -> int:
    """Starts the game through Steam; `extra` are the game's own arguments (e.g. -dx11 for a RenderDoc capture)."""
    if game_process():
        print("the game is already running")
        return 0
    if not game_found():
        return 2
    if not STEAM.exists():
        print(f"Steam was not found at {STEAM}")
        return 2
    keep_log()  # UE4SS overwrites its log at the start: a plain launch must not lose the last session's
    start = time.monotonic()
    subprocess.Popen([str(STEAM), "-applaunch", str(APP_ID), *extra])
    print(f"launched through Steam; waiting up to {READY_TIMEOUT_S} s for the console")
    for attempt in range(2):
        if wait_for_console(READY_TIMEOUT_S if attempt == 0 else 60):
            print(f"console ready after {time.monotonic() - start:.0f} s")
            return 0
        if game_process():
            print("the game runs but the console does not answer; see UE4SS.log")
            return 1
        print("no game process: Steam did not start it; relaunching once")
        subprocess.Popen([str(STEAM), "-applaunch", str(APP_ID), *extra])
    print("the game did not start")
    return 1


def cmd_launch(args) -> int:
    return launch(("-dx11",) if args.dx11 else ())


def quit_game() -> int:
    pid = game_process()
    if not pid:
        print("the game is not running")
        return 0
    try:
        console = Console(60.0)
    except OSError:
        print("the game runs but the console does not answer; not quitting (close it by hand)")
        return 1
    values = console.values()
    if values.get("p3.in") == "on":
        console.command("p3.leave")
        console.command("wait-until p3.in == off 900")
    if values.get("p2.in") == "on":
        console.command("leave")
        console.command("wait-until p2.in == off 900")
    console.command("quit")
    console.close()
    deadline = time.monotonic() + EXIT_TIMEOUT_S
    while game_process() and time.monotonic() < deadline:
        time.sleep(1.0)
    if game_process():
        print(f"the game did not exit within {EXIT_TIMEOUT_S} s")
        return 1
    print("game exited")
    return 0


def cmd_quit(args) -> int:
    return quit_game()


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def build_setup() -> pathlib.Path | None:
    """The built setup app, named by the mod's own line (the build names the exe from the same one); None, with a
    word why, when the line cannot be read. Asked for only by a deploy, so no other command depends on the source tree."""
    header = BUILD_DLL.parents[2] / "src" / "views" / "display_move.h"
    try:
        found = re.search(r'kSetupExe = L"([^"]+)"', header.read_text(encoding="utf-8"))
    except OSError as error:
        print(f"the setup app's name could not be read: {error}")
        return None
    if not found:
        print(f"the setup app's name could not be read: no kSetupExe line in {header}")
        return None
    return BUILD_DLL.with_name(found.group(1))


def deploy(label: str) -> int:
    if game_process():
        print("the game is still running; not deploying")
        return 1
    if not game_found():
        return 1
    installed = MOD_DIR / "dlls" / "main.dll"
    if not BUILD_DLL.exists():
        print(f"no build at {BUILD_DLL}")
        return 1
    if installed.exists():
        backup = installed.with_name(f"main.dll.bak-{label}")
        shutil.copy2(installed, backup)
        print(f"backup {backup.name} ({sha256(backup)[:8]})")
    installed.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(BUILD_DLL, installed)
    setup = build_setup()
    if setup is None or not setup.exists():
        print("the setup app was not installed: the mod then has no guard to put moved monitors back after a crash")
    else:
        target = MOD_DIR / setup.name  # the monitor picker, beside the settings file it writes
        for stale in MOD_DIR.glob(f"{setup.name}.in-use-*"):
            try:
                stale.unlink()  # an earlier deploy's set-aside copy, once the app that ran from it has closed
            except OSError:
                pass
        try:
            shutil.copy2(setup, target)
            print(f"installed {setup.name}")
        except PermissionError:
            # The app is open: Windows will not overwrite a running exe, but it lets one be renamed. The open app goes on
            # running from the set-aside file, and the new one takes the name the mod starts its guard by.
            aside = target.with_name(f"{setup.name}.in-use-{label}")
            try:
                target.rename(aside)
                shutil.copy2(setup, target)
                print(f"installed {setup.name} (the app is open: its old file is set aside as {aside.name})")
            except OSError as error:
                print(f"{setup.name} is running and could not be set aside ({error}): the installed one was NOT replaced")
    # No settings file is copied: the mod writes LocalSplitScreen.ini itself from its options at every start. Only the
    # console's key is set, through the profile API the mod reads with, so the build installed here opens its console.
    settings = MOD_DIR / SETTINGS_FILE
    if ctypes.windll.kernel32.WritePrivateProfileStringW("Development", "console", "on", str(settings)):
        print("console = on in the settings file")
    else:
        print(f"console = on could not be written to {settings}: the tools cannot reach this build")
    built, now = sha256(BUILD_DLL), sha256(installed)
    print(f"installed {now[:8]}")
    if built != now:
        print(f"the installed DLL does not match the build ({built[:8]})")
        return 1
    return 0


def keep_log():
    """UE4SS starts a new log on every launch: the last session's is kept first (the newest ten)."""
    if not UE4SS_LOG.exists():
        return
    kept = UE4SS_LOG.parent / "logs"
    kept.mkdir(exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(UE4SS_LOG.stat().st_mtime))
    target = kept / f"UE4SS-{stamp}.log"
    shutil.move(str(UE4SS_LOG), str(target))
    for old in sorted(kept.glob("UE4SS-2*.log"), reverse=True)[10:]:
        old.unlink()
    print(f"previous log kept as logs/{target.name}")


def cmd_relaunch(args) -> int:
    if quit_game() != 0:
        return 1
    if args.deploy:
        if not args.label:
            print("--deploy needs --label for the backup name")
            return 1
        if deploy(args.label) != 0:
            return 1
    keep_log()
    if args.no_launch:
        print("not launched (--no-launch)")
        return 0
    time.sleep(STEAM_RELAUNCH_GAP_S)
    return launch()


def cmd_load_save(args) -> int:
    """From the title screen: any button -> Continue (the default entry) -> Yes, then the load.

    The title screen is a field of its own (game.state play, field.step 13, its own game.location), so the
    arrival there and the load afterwards are waited on through the mod's readings; only the two title-menu
    fades, which change no reading, keep a fixed gap (--gap, 8 s by default: a press during a fade is dropped
    and the next one lands a screen early).
    """
    console = connect(timeout=300.0)
    try:
        if not console.command("wait-until game.state == play 9000", echo=lambda _: None):
            print("the title screen did not come up")
            return 1
        console.command("wait-until field.step == 13 3000", echo=lambda _: None)
        title = console.values().get("game.location", "?")  # 005-TITLB on this build
        print(f"at the title ({title}); pressing through")
        for press in range(3):
            if press:
                time.sleep(args.gap)  # the title menu, then the "resume?" dialog, fade in; no reading changes
            result = subprocess.run(["powershell", "-NoProfile", "-Command",
                                     f"& {ps_quote(TOOLS / 'keys.ps1')} -Keys @('Enter') -GapMs 0"], capture_output=True, text=True)
            if result.returncode != 0:
                print(result.stderr.strip() or "the key could not be sent")
                return 1
        # The load follows the Yes press within a second: leaving, loading (the location changes), play, step 13.
        ok = (console.command(f"wait-until game.location != {title} 1800", echo=lambda _: None)
              and console.command("wait-until game.state == play 12000", echo=lambda _: None)
              and console.command("wait-until field.step == 13 6000", echo=lambda _: None))
        values = console.values()
        print(f"{'loaded' if ok else 'NOT loaded'}: {values.get('game.location', '?')} (state {values.get('game.state')}, step {values.get('field.step')})")
        return 0 if ok else 1
    finally:
        console.close()


def cmd_shot(args) -> int:
    out = args.out or str(SHOTS_DIR / time.strftime("%Y%m%d-%H%M%S.png"))
    pathlib.Path(out).parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(["powershell", "-NoProfile", "-File", str(TOOLS / "shot.ps1"), "-Out", out, "-Width", str(args.width)],
                            capture_output=True, text=True)
    print(result.stdout.strip() or result.stderr.strip())
    return result.returncode


def cmd_keys(args) -> int:
    # -Command with an array literal: under -File a comma-joined list arrives as one word ("Enter,Enter").
    if args.settle > 0:
        time.sleep(args.settle)  # the title accepts a press a few seconds after its location reads
    keys = ",".join(f"'{k}'" for k in args.keys)
    command = f"& {ps_quote(TOOLS / 'keys.ps1')} -Keys @({keys}) -GapMs {args.gap}"
    result = subprocess.run(["powershell", "-NoProfile", "-Command", command], capture_output=True, text=True)
    print(result.stdout.strip() or result.stderr.strip())
    return result.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-c", "--command", action="append", default=[], help="a console line (repeatable)")
    sub = parser.add_subparsers(dest="verb")
    sub.add_parser("shell")
    sub.add_parser("run").add_argument("script")
    sub.add_parser("status")
    sub.add_parser("launch").add_argument("--dx11", action="store_true",
                                          help="start on D3D11 (RenderDoc captures replay on D3D11 only)")
    sub.add_parser("quit")
    relaunch = sub.add_parser("relaunch")
    relaunch.add_argument("--deploy", action="store_true")
    relaunch.add_argument("--label", default="")
    relaunch.add_argument("--no-launch", action="store_true")
    sub.add_parser("load-save").add_argument("--gap", type=float, default=8.0, help="seconds between the title presses")
    shot = sub.add_parser("shot")
    shot.add_argument("--out", default="")
    shot.add_argument("--width", type=int, default=1280)
    keys = sub.add_parser("keys")
    keys.add_argument("keys", nargs="+")
    keys.add_argument("--gap", type=int, default=600, help="milliseconds between keys")
    keys.add_argument("--settle", type=float, default=0.0, help="seconds to wait before the first key")
    args = parser.parse_args()

    if args.command and args.verb:
        parser.error("give -c lines or a subcommand, not both")
    if args.command:
        return cmd_c(args)
    if not args.verb:
        parser.print_help()
        return 2
    return globals()[f"cmd_{args.verb.replace('-', '_')}"](args)


if __name__ == "__main__":
    sys.exit(main())
