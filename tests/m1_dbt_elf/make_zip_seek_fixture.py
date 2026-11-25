import random
import sys
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile


archive, reference = map(Path, sys.argv[1:])
payload = random.Random(15).randbytes(12 * 1024 * 1024 + 213)
reference.write_bytes(payload)
with ZipFile(archive, "w", compression=ZIP_DEFLATED, compresslevel=1) as zip_file:
    zip_file.writestr("lib/arm64-v8a/libseek.so", payload)
    zip_file.writestr("stored.bin", payload[:8192], compress_type=ZIP_STORED)
