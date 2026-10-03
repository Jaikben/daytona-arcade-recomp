"""Name the game thread's PC samples (build_dreamcast.py game --sample).

The frontend prints "SAMPLE address count" lines, 64-byte buckets of the
program; this sums them by function from game.elf's symbols (sh-elf-nm) and
prints the busiest, with their share of the samples.

    python platform/dreamcast/scripts/pc_profile.py [SERIAL] [--elf ELF] [--top N] [--from FRAME]
"""
import argparse
import bisect
import collections
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NM = Path("C:/DreamSDK/opt/toolchains/dc/sh-elf/bin/sh-elf-nm.exe")


def symbols(elf):
    out = subprocess.run([str(NM), "-C", "-n", str(elf)], capture_output=True, text=True, check=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[1] in "TtWw":
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
    return addrs, names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("serial", nargs="?", default=ROOT / "build-daytona/dreamcast/flycast/serial.txt")
    ap.add_argument("--elf", default=ROOT / "build-daytona/dreamcast/game.elf")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--from", dest="first", type=int, default=0, help="only reports from this frame on")
    args = ap.parse_args()

    addrs, names = symbols(args.elf)
    by_name, total, shown, frame = collections.Counter(), 0, 0, 0
    callers = collections.Counter()
    for line in Path(args.serial).read_text(errors="replace").splitlines():
        m = re.match(r"SAMPLE frame (\d+): (\d+) samples", line)
        if m:
            frame = int(m.group(1))
            if frame >= args.first:
                total += int(m.group(2))
            continue
        m = re.match(r"CALLER ([0-9a-f]{8}) (\d+)", line)
        if m and frame >= args.first:
            i = bisect.bisect_right(addrs, int(m.group(1), 16) + 64) - 1
            callers[names[i] if i >= 0 else "?"] += int(m.group(2))
            continue
        m = re.match(r"SAMPLE ([0-9a-f]{8}) (\d+)", line)
        if m and frame >= args.first:
            addr, count = int(m.group(1), 16), int(m.group(2))
            # The bucket's middle: a bucket can straddle two functions.
            i = bisect.bisect_right(addrs, addr + 32) - 1
            by_name[names[i] if i >= 0 else "?"] += count
            shown += count
    if not total:
        sys.exit("pc_profile: no SAMPLE lines (build with --sample)")
    print(f"{total} samples, {shown} in the printed buckets ({100 * shown / total:.1f}%)")
    for name, count in by_name.most_common(args.top):
        print(f"{100 * count / total:6.2f}%  {count:7d}  {name[:150]}")
    if callers:
        print("memcpy/memset called from:")
        for name, count in callers.most_common(15):
            print(f"{100 * count / total:6.2f}%  {count:7d}  {name[:150]}")


if __name__ == "__main__":
    main()
