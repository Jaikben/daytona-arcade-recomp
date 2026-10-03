"""The Dreamcast's pass over the desktop's generated i960 code (as the Vita
makes its own): the same instructions, fewer lockstep checks.

The desktop's code calls Lockstep::boundary() before every instruction (and
stores the IP for it). Here each chunk counts down `left`, the instructions
it may run before the next lockstep event, worked out at the last full check
(rt::Lockstep::check, M2_DC_SPEED):

- an instruction whose body only touches registers (no call into the runtime,
  no memory) costs a decrement; it checks in full only when `left` runs out,
  which is exactly when boundary() would have acted;
- any other instruction keeps its IP store (the runtime reads it: the
  frame-wait skip, calls) and, if the lockstep's epoch moved during its body
  (an interrupt line, a callback, the count jumping), makes the next
  instruction check in full;
- dispatch, and any body that jumps away, start the count again (left = 1).

When `left` runs out the instruction stores its IP and jumps to the chunk's
one `recheck:` (Lockstep::check, then back in through the chunk's own
dispatch switch, `left` one more for the instruction's decrement): the slow
path is not repeated at every instruction, so the code stays the size of the
desktop's.

    python platform/dreamcast/scripts/fast_gen.py GEN_SET_DIR OUT_DIR

Files other than chunk_*.cpp are copied unchanged. A file is rewritten only
when its output changes (make rebuilds only those). Fails on anything it
does not recognise rather than guess.
"""
import re
import sys
from pathlib import Path

LABEL = re.compile(r"^L_([0-9a-f]{8}): ")
IP = re.compile(r"^    c\.m_IP = (0x[0-9a-f]{8}u);$")
CHECK = "    if (ls.boundary()) goto dispatch;"
COUNT = "    ++ls.count;"
PURE_CALLS = re.compile(r"\bgen::cc_[su]\(")


def pure(body):
    text = "\n".join(body)
    text = PURE_CALLS.sub("(", text)
    return not any(w in text for w in ("c.", "ls.", "rt::", "gen::", "throw", "goto", "return"))


def jumps(body):
    text = "\n".join(body)
    return "goto" in text or "return" in text


def rewrite(source):
    lines = source.split("\n")
    out = []
    i = 0
    stats = {"pure": 0, "other": 0}
    # Prologue: declare `left` after AC, reset it at dispatch.
    while i < len(lines) and not LABEL.match(lines[i]):
        line = lines[i]
        out.append(line)
        if line == "    uint32_t &AC = c.m_AC;":
            out.append("    uint32_t left = 1; // instructions until the next lockstep event (fast_gen.py)")
        elif line == "dispatch:":
            out.append("    left = 1;")
        elif line == "    if (ls.finished()) return;":
            out += ["    goto resume;",
                    "recheck: // left ran out at the instruction at c.m_IP",
                    "    left = ls.check(c.m_IP);",
                    "    if (!left) goto dispatch;",
                    "    ++left; // the instruction decrements it again",
                    "resume:"]
        i += 1
    if not any("uint32_t left = 1;" in l for l in out) or "resume:" not in out:
        raise SystemExit("fast_gen: unexpected chunk prologue")
    while i < len(lines):
        line = lines[i]
        m = LABEL.match(line)
        if not m:
            out.append(line)  # the function's end
            i += 1
            continue
        # One instruction: label, IP store, boundary, body, ++count, tail.
        j = i + 1
        while j < len(lines) and not LABEL.match(lines[j]) and lines[j] != "}":
            j += 1
        unit = lines[i:j]
        i = j
        ip = IP.match(unit[1]) if len(unit) > 2 else None
        if not ip or unit[2] != CHECK or COUNT not in unit:
            raise SystemExit(f"fast_gen: unexpected instruction at {line!r}")
        k = unit.index(COUNT)
        body, tail = unit[3:k], unit[k:]
        out.append(unit[0])
        if pure(body):
            stats["pure"] += 1
            out.append(f"    if (!--left) {{ c.m_IP = {ip.group(1)}; goto recheck; }}")
            out += body
        else:
            stats["other"] += 1
            out.append(unit[1])
            out.append("    if (!--left) goto recheck;")
            if jumps(body):
                out.append("    left = 1;")
                out += body
            else:
                out.append("    { const uint32_t epoch = ls.epoch;")
                out += body
                out.append("    if (ls.epoch != epoch) left = 1; }")
        out += tail
    return "\n".join(out), stats


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    dst.mkdir(parents=True, exist_ok=True)
    total = {"pure": 0, "other": 0}
    for f in sorted(src.glob("*.cpp")):
        text = f.read_text()
        if f.name.startswith("chunk_"):
            text, stats = rewrite(text)
            for k in total:
                total[k] += stats[k]
        out = dst / f.name
        if not out.is_file() or out.read_text() != text:
            out.write_text(text, newline="\n")
    n = total["pure"] + total["other"]
    print(f"fast_gen: {n} instructions, {total['pure']} register-only ({100 * total['pure'] / max(n, 1):.0f}%)")


if __name__ == "__main__":
    main()
