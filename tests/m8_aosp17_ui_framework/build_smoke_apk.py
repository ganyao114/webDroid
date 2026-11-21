#!/usr/bin/env python3
"""Compile resources and app code, then pack them into a zipaligned smoke APK."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("out", "aapt2", "javac", "d8", "zipalign", "android-jar", "framework-jar"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent / "fixture"
    args.out.parent.mkdir(parents=True, exist_ok=True)

    def run(*cmd):
        subprocess.run(cmd, check=True, env={**os.environ, "LC_ALL": "C", "TZ": "UTC"})

    with tempfile.TemporaryDirectory(prefix="apk-", dir=args.out.parent) as directory:
        stage = Path(directory)
        compiled = stage / "compiled.zip"
        unaligned = stage / "unaligned.apk"
        apk = stage / "aligned.apk"
        generated = stage / "generated"
        classes = stage / "classes"
        generated.mkdir()
        classes.mkdir()

        run(args.aapt2, "compile", "--dir", fixture / "res", "-o", compiled)
        run(args.aapt2, "link", "-I", args.android_jar, "--manifest", fixture / "AndroidManifest.xml",
            "--java", generated, "-o", unaligned, compiled)
        java = sorted(generated.rglob("*.java")) + sorted((fixture / "java").rglob("*.java"))
        run(args.javac, "--release", "17", "-encoding", "UTF-8", "-proc:none", "-g:source,lines",
            "-XDstringConcat=inline", "-classpath", os.pathsep.join(map(str, (args.framework_jar,
                                                                            args.android_jar))),
            "-d", classes, *java)
        jar = stage / "app.jar"
        with zipfile.ZipFile(jar, "w") as output:
            for file in sorted(classes.rglob("*.class")):
                output.writestr(zipfile.ZipInfo(file.relative_to(classes).as_posix()), file.read_bytes())
        run(args.d8, "--release", "--min-api", "26", "--lib", args.android_jar, "--classpath",
            args.framework_jar, "--output", stage, jar)
        with zipfile.ZipFile(unaligned, "a") as output:
            output.writestr(zipfile.ZipInfo("classes.dex"), (stage / "classes.dex").read_bytes())
        run(args.zipalign, "-f", "4", unaligned, apk)
        run(args.zipalign, "-c", "4", apk)
        shutil.move(apk, args.out)


if __name__ == "__main__":
    main()
