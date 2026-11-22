"""Package the framework DEX for loader-only tests."""

import sys
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile

dex = Path(sys.argv[1]).read_bytes()
with ZipFile(sys.argv[2], "w", compression=ZIP_DEFLATED) as archive:
    archive.writestr("classes.dex", dex)
    archive.writestr("assets/classes2.dex", b"not a top-level DEX entry")
with ZipFile(sys.argv[3], "w", compression=ZIP_DEFLATED) as archive:
    archive.writestr("classes.dex", dex)
    archive.writestr("classes2.dex", dex[:-1] + bytes([dex[-1] ^ 1]))
