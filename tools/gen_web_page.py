"""Regenerate ESP32_power_supply/web_page.h from the canonical web UI source.

    python tools/gen_web_page.py

Gzips `new design/Power Supply.html` (mtime 0, so the output is deterministic)
into the PROGMEM array the firmware serves at `/`. Replaces the README's
PowerShell snippet, which once wrote a stale header while exiting 0. This
script reads the bytes back and fails loudly if they don't round-trip.
"""
import gzip
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT / "new design" / "Power Supply.html"
OUT = ROOT / "ESP32_power_supply" / "web_page.h"


def main() -> int:
    html = SRC.read_bytes()
    gz = gzip.compress(html, compresslevel=9, mtime=0)
    rows = [", ".join(f"0x{b:02x}" for b in gz[i:i + 16]) for i in range(0, len(gz), 16)]
    text = ("#pragma once\n"
            "const uint8_t index_html_gz[] PROGMEM = {\n  "
            + ",\n  ".join(rows)
            + "\n};\n"
            f"const size_t index_html_gz_len = {len(gz)};\n")
    old = OUT.read_text(encoding="ascii") if OUT.exists() else ""
    OUT.write_text(text, encoding="ascii", newline="\n")

    # Verify: parse the header we just wrote and decompress it back.
    body = OUT.read_text(encoding="ascii")
    hexes = body.split("{", 1)[1].split("}", 1)[0].replace("\n", "").split(",")
    back = gzip.decompress(bytes(int(h, 16) for h in hexes if h.strip()))
    if back != html:
        print("ERROR: web_page.h does not decompress to the source HTML", file=sys.stderr)
        return 1
    old_len = old.rsplit("index_html_gz_len = ", 1)[-1].split(";")[0] if old else "none"
    print(f"web_page.h: {len(html)} B html -> {len(gz)} B gzip (was {old_len}); round-trip OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
