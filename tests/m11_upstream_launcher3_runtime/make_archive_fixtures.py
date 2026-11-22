"""Package the framework DEX for loader-only tests."""

import sys
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile, is_zipfile

source = Path(sys.argv[1])
if is_zipfile(source):
    with ZipFile(source) as framework:
        dex_files = {name: framework.read(name) for name in framework.namelist()}
else:
    dex_files = {"classes.dex": source.read_bytes()}
with ZipFile(sys.argv[2], "w", compression=ZIP_DEFLATED) as archive:
    for name, dex in dex_files.items():
        archive.writestr(name, dex)
    archive.writestr("assets/classes2.dex", b"not a top-level DEX entry")
dex = dex_files["classes.dex"]
with ZipFile(sys.argv[3], "w", compression=ZIP_DEFLATED) as archive:
    archive.writestr("classes.dex", dex)
    archive.writestr("classes2.dex", dex[:-1] + bytes([dex[-1] ^ 1]))
