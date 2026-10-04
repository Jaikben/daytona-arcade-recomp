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
- dispatch, and any body that jumps away, start the count again (left = 1);
- the instruction count is kept in a local (`n`, one register add instead
  of a 64-bit add in memory) and added to ls.count where anything else can
  see it: before any instruction that is not register-only (its body may
  call the runtime, which reads the count), at recheck and at dispatch (so
  before every return);
- a load or store at a fixed, aligned work-RAM address (0x00500000-0x005fffff,
  plain RAM nothing watches) goes straight to it (gen::wram_*), and such an
  instruction counts as register-only. The frame-wait byte (0x00500000, read
  through the bus for the frame-wait skip) keeps its call.

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
PURE_CALLS = re.compile(r"\bgen::(cc_[su]|wram_[rw](8|16|32))\(")

# Fixed work-RAM accesses: (pattern, alignment, replacement).
WRAM = [
    (re.compile(r"c\.i960_read_dword_unaligned\(0x005([0-9a-f]{5})u\)"), 4, "gen::wram_r32(c, 0x{}u)"),
    (re.compile(r"c\.i960_read_word_unaligned\(0x005([0-9a-f]{5})u\)"), 2, "gen::wram_r16(c, 0x{}u)"),
    (re.compile(r"c\.bus->read_byte\(0x005([0-9a-f]{5})u\)"), 1, "gen::wram_r8(c, 0x{}u)"),
    (re.compile(r"c\.i960_write_dword_unaligned\(0x005([0-9a-f]{5})u, "), 4, "gen::wram_w32(c, 0x{}u, "),
    (re.compile(r"c\.i960_write_word_unaligned\(0x005([0-9a-f]{5})u, "), 2, "gen::wram_w16(c, 0x{}u, "),
    (re.compile(r"c\.bus->write_byte\(0x005([0-9a-f]{5})u, "), 1, "gen::wram_w8(c, 0x{}u, "),
]


def direct_wram(line, stats):
    for pattern, align, repl in WRAM:
        def sub(m):
            offset = int(m.group(1), 16)
            if offset % align or (repl.startswith("gen::wram_r8") and offset == 0):
                return m.group(0)  # unaligned, or the frame-wait byte: the bus as before
            stats["wram"] += 1
            return repl.format(m.group(1))
        line = pattern.sub(sub, line)
    return line


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
    stats = {"pure": 0, "other": 0, "wram": 0}
    # Prologue: declare `left` after AC, reset it at dispatch.
    while i < len(lines) and not LABEL.match(lines[i]):
        line = lines[i]
        out.append(line)
        if line == "    uint32_t &AC = c.m_AC;":
            out.append("    uint32_t left = 1; // instructions until the next lockstep event (fast_gen.py)")
            out.append("    uint32_t n = 0;    // instructions run and not yet added to ls.count (fast_gen.py)")
        elif line == "dispatch:":
            out.append("    ls.count += n;")
            out.append("    n = 0;")
            out.append("    left = 1;")
        elif line == "    if (ls.finished()) return;":
            out += ["    goto resume;",
                    "recheck: // left ran out at the instruction at c.m_IP",
                    "    ls.count += n;",
                    "    n = 0;",
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
        body, tail = [direct_wram(b, stats) for b in unit[3:k]], unit[k:]
        if any("ls.count" in t for t in body + tail[1:]):
            raise SystemExit(f"fast_gen: ls.count used inside the instruction at {line!r}")
        tail = ["    ++n;"] + tail[1:]
        out.append(unit[0])
        if pure(body):
            stats["pure"] += 1
            out.append(f"    if (!--left) {{ c.m_IP = {ip.group(1)}; goto recheck; }}")
            out += body
        else:
            stats["other"] += 1
            out.append(unit[1])
            out.append("    if (!--left) goto recheck;")
            out.append("    ls.count += n; // the runtime may read the count")
            out.append("    n = 0;")
            if jumps(body):
                out.append("    left = 1;")
                out += body
            else:
                out.append("    { const uint32_t epoch = ls.epoch;")
                out += body
                out.append("    if (ls.epoch != epoch) left = 1; }")
        out += tail
    return "\n".join(out), stats


TGP_RUN = "void run(Tgp &t, uint64_t budget) {\n"


def rewrite_tgp(source):
    """The TGP's generated code (tgp_gen.cpp): its instruction count kept in
    a local of run() instead of Tgp::count, stored back at every return.
    Nothing run() calls reads the count (the board's FIFOs, memory and bank
    hooks do not), so the count everything else sees is the same; the
    increment is a register add instead of a 64-bit add through memory."""
    start = source.index(TGP_RUN) + len(TGP_RUN)
    end = source.index("\n}\n\n} // namespace rt::tgpgen")
    body = source[start:end]
    if "t.hook" in body or "count = " in body:
        raise SystemExit("fast_gen: unexpected tgp_gen.cpp")
    body = re.sub(r"\bt\.count\b", "count", body.replace("return;", "{ t.count = count; return; }"))
    # (The stores back just made by the line above, spelled out again.)
    body = body.replace("{ count = count; return; }", "{ t.count = count; return; }")
    if re.search(r"\bt\.count\b", body.replace("{ t.count = count; return; }", "")):
        raise SystemExit("fast_gen: tgp_gen.cpp uses the count elsewhere")
    head = ("#ifdef M2TGP_WITH_HOOK\n"
            "#error \"fast_gen.py's TGP code keeps the count in a local: a hook would not see it\"\n"
            "#endif\n")
    return (source[:start].replace('#include "runtime/tgp.h"\n', '#include "runtime/tgp.h"\n' + head, 1) +
            "    uint64_t count = t.count; // (fast_gen.py) Tgp::count, stored back at every return\n" + body +
            source[end:])


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    dst.mkdir(parents=True, exist_ok=True)
    total = {"pure": 0, "other": 0, "wram": 0}
    for f in sorted(src.glob("*.cpp")):
        text = f.read_text()
        if f.name.startswith("chunk_"):
            text, stats = rewrite(text)
            for k in total:
                total[k] += stats[k]
        elif f.name == "tgp_gen.cpp":
            text = rewrite_tgp(text)
        out = dst / f.name
        if not out.is_file() or out.read_text() != text:
            out.write_text(text, newline="\n")
    n = total["pure"] + total["other"]
    if not n:
        print(f"fast_gen: {src.name}: no chunks (tgp_gen.cpp's count kept in a local)")
        return
    print(f"fast_gen: {n} instructions, {total['pure']} register-only ({100 * total['pure'] / max(n, 1):.0f}%), "
          f"{total['wram']} work-RAM accesses direct")


if __name__ == "__main__":
    main()
