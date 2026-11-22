#!/usr/bin/env python3
"""Build the Android 17 UI framework DEX and its matching javac classpath JAR."""

import argparse
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import zipfile

sys.dont_write_bytecode = True
from build_pm_dex import run, sdk_declarations, write_depfile, write_jar
from build_upstream_framework_dex import apply_patch, stage_java
from framework_build.generate_unicode import generate as generate_unicode

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
SOURCES = HERE / "framework_build"
DEX = "aosp17_framework_core.dex"
JAR = "aosp17_framework_classes.jar"

# Share the existing JNI support classes used with the original resource sources.
WAVE6 = """
android/annotation/AnyRes.java
android/content/om/OverlayableInfo.java
android/content/res/AssetFileDescriptor.java
android/content/res/StringBlock.java
android/content/res/Validator.java
android/ravenwood/annotation/RavenwoodKeepWholeClass.java
android/system/OsConstants.java
com/android/internal/annotations/GuardedBy.java
com/android/internal/pm/pkg/component/AconfigFlags.java
dalvik/annotation/optimization/CriticalNative.java
dalvik/annotation/optimization/FastNative.java
libcore/io/IoUtils.java
""".split()


def build(args, stage):
    manifest = SOURCES / "upstream.txt"
    selections = [entry for line in manifest.read_text().splitlines()
                  if (entry := line.partition("#")[0].strip())]
    upstream_names = [entry for entry in selections if not entry.startswith("@patch ")]
    upstream = [args.aosp_root / path for path in upstream_names]
    patches = [SOURCES / "patches" / entry.removeprefix("@patch ")
               for entry in selections if entry.startswith("@patch ")]
    java_base = sorted((SOURCES / "java_base").rglob("*.java"))
    libcore_root = args.aosp_root / "libcore/ojluni/src/main/java"
    libcore = [path for path in upstream if path.is_relative_to(libcore_root)]
    sources = [*(path for path in upstream if path not in libcore),
               *sorted((SOURCES / "java").rglob("*.java"))]
    sources.extend(REPO / "tests/m6_aosp17_pm_java/jvm" / path for path in WAVE6)
    sources.append(REPO / "tests/m7_aosp17_art_interp/java/com/android/internal/util/XmlUtils.java")
    boot_api = SOURCES / "boot_api"
    sources.extend(sorted(boot_api.rglob("*.java")))
    compile_only_annotations = [
        HERE / f"pm_build/android/annotation/{name}.java" for name in ("SystemApi", "TestApi")
    ]
    generator = REPO / "runtime/aosp17_shims/art_interp/generate_jni.py"
    native_table = args.aosp_root / "frameworks/base/core/jni/android_util_AssetManager.cpp"
    template = SOURCES / "AssetManager.java.in"
    inputs = {Path(__file__).resolve(), HERE / "build_pm_dex.py",
              HERE / "build_upstream_framework_dex.py", manifest, generator, *compile_only_annotations, *patches,
              SOURCES / "generate_unicode.py",
              native_table, template, args.javac, args.d8, args.android_jar, *sources, *java_base, *libcore}
    d8_jar = args.d8.parent / "lib/d8.jar"
    if d8_jar.is_file():
        inputs.add(d8_jar)
    for path in sorted(inputs):
        if not path.is_file():
            raise ValueError(f"Required input does not exist: {path}")

    upstream_java = {}
    patched = set()
    if patches:
        upstream_java = {name: path.read_bytes() for name, path in zip(upstream_names, upstream)}
        for patch in patches:
            patched.update(apply_patch(patch, upstream_java)["files"])
        for name in sorted(patched):
            original = args.aosp_root / name
            if original not in libcore:
                sources.remove(original)
                sources.append(stage_java(stage / "upstream", name, upstream_java[name]))

    asset_manager = stage / "android/content/res/AssetManager.java"
    run(sys.executable, generator, "--source", native_table, "--asset-manager",
        asset_manager, "--template", template)
    classes = stage / "classes"
    classes.mkdir()
    icon_entry = "android/graphics/drawable/Icon.class"
    hidden_api = stage / "hidden-api.jar"
    with zipfile.ZipFile(args.android_jar) as sdk:
        write_jar(hidden_api, {icon_entry: sdk_declarations(
            sdk.read(icon_entry), methods=[("sameAs", "(Landroid/graphics/drawable/Icon;)Z", False)])})
    run(args.javac, "--release", "17", "-encoding", "UTF-8", "-proc:none",
        "-g:source,lines", "-XDstringConcat=inline", "-sourcepath", "",
        "-classpath", os.pathsep.join(map(str, (hidden_api, args.android_jar))), "-d", classes,
        *sorted(sources), asset_manager, *compile_only_annotations)
    base = stage / "java-base"
    staged_sources = []
    for original, root in [(path, SOURCES / "java_base") for path in java_base] + [
            (path, libcore_root) for path in libcore]:
        copy = base / original.relative_to(root)
        copy.parent.mkdir(parents=True, exist_ok=True)
        name = os.path.relpath(original, args.aosp_root)
        if name in patched:
            copy.write_bytes(upstream_java[name])
        else:
            shutil.copyfile(original, copy)
        staged_sources.append(copy)
    unicode_data = base / "java/text/UnicodeData.java"
    generate_unicode(unicode_data)
    staged_sources.append(unicode_data)
    thread_context = base / "org/wasmandroid/ui/ThreadContext.java"
    thread_context.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(boot_api / "org/wasmandroid/ui/ThreadContext.java", thread_context)
    staged_sources.append(thread_context)
    run(args.javac, "--release", "17", "--patch-module", f"java.base={base}",
        "-encoding", "UTF-8", "-proc:none", "-g:source,lines", "-XDstringConcat=inline",
        "-d", classes, *staged_sources)
    entries = {path.relative_to(classes).as_posix(): path.read_bytes()
               for path in classes.rglob("*.class")}
    # The interpreter provides Log and Slog as boot classes.
    del entries["android/util/Slog.class"]
    del entries["org/wasmandroid/ui/ThreadContext.class"]
    del entries["org/wasmandroid/ui/SoftwareRaster.class"]
    del entries["android/annotation/SystemApi.class"]
    del entries["android/annotation/SystemApi$Client.class"]
    del entries["android/annotation/TestApi.class"]
    with zipfile.ZipFile(args.android_jar) as sdk:
        # These SDK classes contain only interface declarations and constants.
        for name in ("org/xmlpull/v1/XmlPullParser", "java/util/Map$Entry",
                     "android/provider/BaseColumns",
                     "android/window/OnBackInvokedCallback", "android/window/OnBackInvokedDispatcher"):
            entries[name + ".class"] = sdk.read(name + ".class")
    write_jar(stage / JAR, entries)
    run(args.d8, "--release", "--min-api", "26", "--lib", args.android_jar,
        "--output", stage, stage / JAR)
    if sorted(path.name for path in stage.glob("classes*.dex")) != ["classes.dex"]:
        raise ValueError("Expected exactly one classes.dex from SDK 36 d8")
    (stage / "classes.dex").rename(stage / DEX)
    return inputs, len(upstream)


def main():
    default_sdk = Path.home() / ("Library/Android/sdk" if sys.platform == "darwin" else "Android/Sdk")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aosp-root", type=Path, default=REPO / "third_party/aosp17")
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--android-sdk", type=Path, default=os.environ.get("ANDROID_SDK_ROOT")
                        or os.environ.get("ANDROID_HOME") or default_sdk)
    parser.add_argument("--javac", type=Path, default=Path("/usr/bin/javac"))
    parser.add_argument("--android-jar", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--depfile", type=Path)
    parser.add_argument("--check", action="store_true", help="rebuild and compare the DEX and JAR")
    args = parser.parse_args()
    args.android_jar = args.android_jar or args.android_sdk / "platforms/android-36/android.jar"
    args.d8 = args.d8 or args.android_sdk / "build-tools/36.0.0/d8"
    args.aosp_root = args.aosp_root.absolute()
    args.out_dir = args.out_dir.absolute()
    if any(path and path.resolve().is_relative_to(args.aosp_root.resolve())
           for path in (args.out_dir, args.depfile)):
        parser.error("Build outputs must be outside the read-only AOSP tree")
    if args.check and args.depfile:
        parser.error("--check and --depfile are mutually exclusive")
    try:
        if not args.check:
            args.out_dir.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".framework-", dir=args.out_dir) as tmp:
            stage = Path(tmp)
            inputs, count = build(args, stage)
            for name in (DEX, JAR):
                output = args.out_dir / name
                if args.check:
                    if output.read_bytes() != (stage / name).read_bytes():
                        raise ValueError(f"Reproducibility check failed for {output}")
                else:
                    (stage / name).replace(output)
            if args.depfile:
                args.depfile.parent.mkdir(parents=True, exist_ok=True)
                write_depfile(args.depfile, args.out_dir / DEX, inputs)
            verb = "Verified" if args.check else "Built"
            print(f"{verb} {args.out_dir / DEX} "
                  f"({(args.out_dir / DEX).stat().st_size} bytes, {count} upstream AOSP Java files)")
    except (OSError, ValueError, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    main()
