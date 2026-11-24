#!/usr/bin/env python3
"""Link the PS2 slice: the program, and each program the loader reads.

ps2link.py takes psplink.py's words and links as it does, with ps2dev's
binutils for the EE in the PSP's place: psplink.py partial OUT PROGRAM
OBJECT... and psplink.py link DIR PLACED PROGRAM... -- WORD... say what each
does. The PS2 has no memory at the console's addresses either, and its
console_ram holds the console's RAM as the PSP's does (engine/psp/psp.h).

The PSP loads its program where the system chooses and moves it; the EE
loads an ELF at the addresses its link gave it, from 0x00100000
(ps2sdk's linkfile). So the program's base is 0 here: the program's link
is its final place, and a program of the loader's links where the memory
contract places it in console_ram, at an address the program's own link
fixes, as psplink.py's second pass binds it.

The environment's PS2DEV names the toolchain, ~/ps2dev by default."""
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import psplink  # noqa: E402

BIN = Path(os.environ.get('PS2DEV', Path.home() / 'ps2dev')) / 'ee' / 'bin'
PREFIX = 'mips64r5900el-ps2-elf-'


def tool(name):
    return BIN / f'{PREFIX}{name}'


def fail(words):
    sys.exit(f'ps2link: {words}')


run_tool = psplink.run


def run(*words, check=True):
    """psplink.py's run. its own assembly of console_ram goes through the
    compiler's driver, which gives the assembler the EE's float and call
    model as it gives them for the program's objects: the assembler's own
    defaults are another's"""
    if words and Path(str(words[0])).name == f'{PREFIX}as':
        words = (tool('gcc'), '-c', '-x', 'assembler', *words[1:])
    return run_tool(*words, check=check)


psplink.BIN = BIN
psplink.BASE = 0
psplink.tool = tool
psplink.fail = fail
psplink.run = run

if __name__ == '__main__':
    psplink.main()
