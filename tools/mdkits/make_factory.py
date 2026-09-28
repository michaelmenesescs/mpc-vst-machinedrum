#!/usr/bin/env python3
"""Dumps the Machinedrum's factory kits from the user's own OS and writes them as one kit pack, FACTORY.syx.

The factory kits are made by the OS itself the first time it initialises an empty flash, so they come from booting
the emulated MD (tools/mdtrace/mdProbe, first-run flash initialisation) and asking it for each of its 64 kits over
sysex (mdProbe's kitdump action, the MD's own kit request $53). Empty slots (name starting $7F) are dropped. The output
is Elektron's data from the user's OS file: per-user build output, never committed or distributed.

    make_factory.py <mdProbe binary> <MD OS .bin (flash image the emulator boots)> <out FACTORY.syx>
"""
import os
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    probe, rom, out = sys.argv[1:]
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([probe, rom, os.path.join(tmp, "flash.bin"), "kitdump:" + tmp], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        kits = []
        for slot in range(64):
            path = os.path.join(tmp, "kit_%02d.syx" % slot)
            if not os.path.exists(path):
                continue
            data = open(path, "rb").read()
            if len(data) >= 0x4d1 and data[0x0a] not in (0x00, 0x7f):   # a named kit, not an empty slot
                kits.append(data)
    if not kits:
        print("no kits dumped", file=sys.stderr)
        return 1
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    open(out, "wb").write(b"".join(kits))
    print("%s: %d factory kits" % (out, len(kits)), file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
