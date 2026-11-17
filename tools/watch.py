"""Record every change of the mod's readings while the user plays (read-only).

    python tools/watch.py --out watch.log [--fast 0.25] [--slow 1.0]

Polls the running mod's console (tools/game.py's Console) and appends one timestamped line per change to the
output file: `party.ai` per row, player 2's state and member, the party funnels heard, the game state, the field
step and the location. Reconnects while the game is away. Stop it with Ctrl+C or by killing the process.
"""
import argparse
import datetime
import pathlib
import re
import socket
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from game import Console  # noqa: E402

FAST = ["party.ai", "p2.state", "p2.member", "party-events", "operator-battle-mode"]
SLOW = ["game.state", "game.paused", "game.menu-kind", "field.step", "field.running", "game.location",
        "command-menu.leader", "enemy-leader", "targeting.lock", "pads", "lead.gate"]  # pad.events is a counter: every poll
ROW = re.compile(r"\[row (\d+) (.*?)\](?= \[row |\s*$)")  # a row holds its own [layer kinds] bracket
PAD = re.compile(r"packet (\d+) changed ([\d.]+) s ago")


def shape(key: str, value: str) -> str:
    """The part of a reading worth a line: `pads` counts and ages change every poll, so a pad is
    'live' (reported within 3 s) or 'silent', and only the connected set and that word are compared."""
    if key == "pads":
        return PAD.sub(lambda m: "live" if float(m.group(2)) < 3.0 else "silent", value)
    return value


def read(con: Console, key: str) -> str:
    number = con.send(f"get {key}")
    value = ""
    while True:
        reply = con.reply()
        word, _, rest = reply.partition(" ")
        if word == "value":
            _, _, value = rest.partition(" ")
        elif word in ("ok", "failed", "rejected", "dropped", "note") and rest.split(" ", 1)[0] == str(number):
            return value


def split_party(value: str) -> dict:
    """party.ai -> {'leader': ..., 'row 0': ..., ...} so a change names the row it is in."""
    out = {"leader": value.split(";", 1)[0].strip()}
    for m in ROW.finditer(value):
        out[f"row {m.group(1)}"] = m.group(2)
    return out


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--out", required=True)
    p.add_argument("--fast", type=float, default=0.25, help="seconds between polls of the party/state readings")
    p.add_argument("--slow", type=float, default=1.0, help="seconds between polls of the field/game readings")
    a = p.parse_args()
    out = open(a.out, "a", encoding="utf-8")

    def line(text: str):
        stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        out.write(f"[{stamp}] {text}\n")
        out.flush()

    last: dict = {}
    con = None
    next_slow = 0.0
    line("watch started")
    while True:
        try:
            if con is None:
                con = Console(timeout=10.0)
                line("connected")
                last.clear()
            now = time.monotonic()
            keys = list(FAST)
            if now >= next_slow:
                keys += SLOW
                next_slow = now + a.slow
            for key in keys:
                value = read(con, key)
                if key == "party.ai":
                    for sub, text in split_party(value).items():
                        name = f"party.ai {sub}"
                        if last.get(name) != text:
                            line(f"{name}: {text}")
                            last[name] = text
                else:
                    value = shape(key, value)
                    if last.get(key) != value:
                        line(f"{key}: {value}")
                        last[key] = value
            time.sleep(a.fast)
        except KeyboardInterrupt:
            line("watch stopped")
            return 0
        except (OSError, ConnectionError) as e:
            if con is not None:
                line(f"disconnected: {e}")
                try:
                    con.close()
                except OSError:
                    pass
                con = None
            time.sleep(2.0)


if __name__ == "__main__":
    sys.exit(main())
