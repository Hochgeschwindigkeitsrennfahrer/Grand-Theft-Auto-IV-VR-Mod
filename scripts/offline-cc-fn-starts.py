#!/usr/bin/env python3
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
SKEW = 0xC00


def main() -> None:
    d = EXE.read_bytes()
    print("ViewConst 0x32470:", d[0x32470 - SKEW : 0x32470 - SKEW + 16].hex(" "))
    print("CC-padded fn candidates 0x31400..0x32A00:")
    for r in range(0x31400, 0x32A00):
        o = r - SKEW
        if r > 0x31400 and d[o - 1] == 0xCC and d[o] != 0xCC:
            b = d[o : o + 12]
            print(f"  0x{r:X}: {b.hex(' ')}")


if __name__ == "__main__":
    main()
