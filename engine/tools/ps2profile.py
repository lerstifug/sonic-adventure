#!/usr/bin/env python3
"""Name a PS2 run's samples by the functions they fell in.

ps2profile.py DIR [COUNT [RUN]] reads RUN/profile.bin, DIR/game's by default, which a run with the
settings' sample 1 writes (engine/ps2/profile.c), and prints the COUNT
functions, 40 by default, that took the most samples, with each one's share.

The program's own code is named by DIR/main.elf's symbols. The console's
RAM holds the loader's programs, which overlap there: a sample there is
named by the programs DIR/game/log.txt says the run loaded (a run with
trace 1 names each), by their links in DIR/link/, the last loaded first.

PS2DEV, from the environment, names the toolchain, ~/ps2dev by default."""
import bisect
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

NM = Path(os.environ.get('PS2DEV', Path.home() / 'ps2dev')) / 'ee' / 'bin' / 'mips64r5900el-ps2-elf-nm'
MAGIC = 0x46524250


def symbols(elf):
    """an ELF's code symbols, (address, name) sorted by address"""
    done = subprocess.run([str(NM), '-n', '-C', '--defined-only', str(elf)], capture_output=True, text=True)
    rows = []
    for line in done.stdout.splitlines():
        fields = line.split(maxsplit=2)
        if len(fields) == 3 and fields[1] in 'tTwW':
            rows.append((int(fields[0], 16), fields[2]))
    return rows


def namer(tables):
    """a function from an address to the name of the symbol at or below it,
    by the first table that holds one within 64 KB"""
    def name(address):
        for rows in tables:
            at = bisect.bisect_right(rows, (address, '￿')) - 1
            if at >= 0 and address - rows[at][0] < 0x10000:
                return rows[at][1]
        return f'?{address:08x}'
    return name


def main():
    directory = Path(sys.argv[1])
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    # the run's folder: DIR's game, or the tree's where the build lies
    # outside it (engine/mk/ps2.mk's PS2_OUT)
    run = Path(sys.argv[3]) if len(sys.argv) > 3 else directory / 'game'
    data = (run / 'profile.bin').read_bytes()
    magic, shift = struct.unpack_from('<2I', data, 0)
    if magic != MAGIC:
        sys.exit('ps2profile: profile.bin is no profile')
    spans = [struct.unpack_from('<2I', data, 8 + 8 * i) for i in range(2)]
    at = 24
    counts = []
    for first, buckets in spans:
        counts.append((first, struct.unpack_from(f'<{buckets}I', data, at)))
        at += 4 * buckets

    loaded = []
    log = run / 'log.txt'
    if log.exists():
        for match in re.finditer(r'program programs/(\w+)\.BIN', log.read_text(errors='replace')):
            name = match[1].lower()
            if name not in loaded:
                loaded.append(name)
    program_tables = [symbols(elf) for name in reversed(loaded)
                      if (elf := directory / 'link' / f'{name}.elf').exists()]
    names = [namer([symbols(directory / 'main.elf')]), namer(program_tables)]

    totals = {}
    total = 0
    for span, (first, buckets) in enumerate(counts):
        for i, n in enumerate(buckets):
            if n:
                name = names[span](first + (i << shift))
                totals[name] = totals.get(name, 0) + n
                total += n
    print(f'{total} samples, programs named by: {", ".join(reversed(loaded)) or "none"}')
    for name, n in sorted(totals.items(), key=lambda row: -row[1])[:count]:
        print(f'{100 * n / total:6.2f}% {n:7d}  {name}')


if __name__ == '__main__':
    main()
