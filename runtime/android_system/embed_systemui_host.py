"""Embed the launcher window adapter DEX in the native and Wasm runtimes."""

from pathlib import Path
import sys

dex = Path(sys.argv[1]).read_bytes()
rows = [
    "    " + ", ".join(f"0x{byte:02x}" for byte in dex[offset:offset + 16]) + ",\n"
    for offset in range(0, len(dex), 16)
]
Path(sys.argv[2]).write_text(
    "constexpr uint8_t kLauncherSystemUiDex[] = {\n" + "".join(rows) + "};\n",
    encoding="utf-8",
)
