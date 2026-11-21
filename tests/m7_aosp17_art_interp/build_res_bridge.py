#!/usr/bin/env python3
"""Compile the resource Java sources and their host-free boot dependencies."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    for option in ("out", "javac", "d8", "android-jar", "pm-api", "pm-classes"):
        cli.add_argument(f"--{option}", required=True, type=Path)
    cli.add_argument("sources", nargs="+", type=Path)
    args = cli.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="res-bridge-", dir=args.out.parent) as path:
        stage = Path(path)
        classes = stage / "classes"
        classes.mkdir()
        classpath = os.pathsep.join(map(str, (args.pm_classes, args.pm_api, args.android_jar)))
        subprocess.run([args.javac, "--release", "17", "-encoding", "UTF-8", "-proc:none",
                        "-sourcepath", "", "-classpath", classpath, "-d", classes,
                        *args.sources], check=True)

        program = stage / "bridge.jar"
        with zipfile.ZipFile(program, "w") as jar, zipfile.ZipFile(args.android_jar) as sdk:
            for cls in sorted(classes.rglob("*.class")):
                jar.writestr(zipfile.ZipInfo(cls.relative_to(classes).as_posix()), cls.read_bytes())
            # These SDK interfaces contain declarations and constants only.
            for interface in ("org/xmlpull/v1/XmlPullParser", "android/os/Parcelable",
                              "android/os/Parcelable$Creator", "android/os/Parcelable$ClassLoaderCreator"):
                name = f"{interface}.class"
                jar.writestr(zipfile.ZipInfo(name), sdk.read(name))
        subprocess.run([args.d8, "--release", "--min-api", "36", "--lib", args.android_jar,
                        "--classpath", args.pm_api, "--classpath", args.pm_classes,
                        "--output", stage, program], check=True)
        if sorted(p.name for p in stage.glob("classes*.dex")) != ["classes.dex"]:
            raise RuntimeError("Expected one resource bridge DEX")
        (stage / "classes.dex").replace(args.out)


if __name__ == "__main__":
    main()
