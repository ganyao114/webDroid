#!/usr/bin/env python3
"""Build the source-selected Android 17 framework and service DEX files offline."""

import argparse
from collections import defaultdict
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tempfile
import zipfile

sys.dont_write_bytecode = True
from build_pm_dex import dex_profile, run, sdk_declarations, write_depfile, write_jar


REPO = Path(__file__).resolve().parents[2]
PROFILE = REPO / "patches/aosp17/frameworks_base"
JAVA_BASE = Path("libcore/ojluni/src/main/java")
FRAMEWORK_DEX = "aosp17_upstream_framework.dex"
FRAMEWORK_JAR = "aosp17_upstream_framework_classes.jar"
SERVICES_DEX = "aosp17_upstream_services.dex"
SERVICES_JAR = "aosp17_upstream_services_classes.jar"
FRAMEWORK_RES = "framework-res-upstream.apk"
PROVENANCE = "framework_provenance.json"
ARTIFACTS = (FRAMEWORK_DEX, FRAMEWORK_JAR, SERVICES_DEX, FRAMEWORK_RES, PROVENANCE)
HUNK = re.compile(rb"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@(?: .*)?\n")
RESOURCE_FLAG = re.compile(rb'\bandroid:featureFlag\s*=\s*"!?([\w.]+)"')
SYMBOL_FLAG = re.compile(rb"@android\.annotation\.FlaggedApi\(([\w.]+)\.Flags\.(FLAG_[A-Z0-9_]+)\)")
METHODS_TO_AUDIT = (
    "android.app.Activity",
    "android.content.Intent",
    "android.content.res.Resources",
    "android.graphics.Bitmap",
    "android.graphics.Canvas",
    "android.graphics.Paint",
    "android.graphics.Path",
    "android.os.Looper",
    "android.view.LayoutInflater",
    "android.view.MotionEvent",
    "android.view.View",
    "android.view.ViewGroup",
    "android.widget.FrameLayout",
    "android.widget.LinearLayout",
    "android.widget.TextView",
    "com.android.server.IntentResolver",
)

# SDK 36 exposes the public classes but omits these platform members. The
# projections are only javac inputs and are built from the SDK classfiles.
HIDDEN_METHODS = {
    "android/content/res/AssetManager": ("frameworks/base/core/java/android/content/res/AssetManager.java", (
        ("xmlBlockGone", "(I)V", False),
    )),
    "android/os/Parcel": ("frameworks/base/core/java/android/os/Parcel.java", (
        ("adoptClassCookies", "(Landroid/os/Parcel;)V", False),
        ("compareData", "(Landroid/os/Parcel;)I", False),
        ("destroy", "()V", False),
        ("hasBinders", "()Z", False),
        ("hasFileDescriptors", "(Ljava/lang/Object;)Z", True),
        ("hasReadWriteHelper", "()Z", False),
        ("pushAllowFds", "(Z)Z", False),
        ("readString8", "()Ljava/lang/String;", False),
        ("readCharSequence", "()Ljava/lang/CharSequence;", False),
        ("readArrayMap", "(Landroid/util/ArrayMap;Ljava/lang/ClassLoader;)V", False),
        ("restoreAllowFds", "(Z)V", False),
        ("writeArrayMap", "(Landroid/util/ArrayMap;)V", False),
        ("writeArrayMapInternal", "(Landroid/util/ArrayMap;)V", False),
        ("writeCharSequence", "(Ljava/lang/CharSequence;)V", False),
        ("writeString8", "(Ljava/lang/String;)V", False),
    )),
    "android/os/ParcelFileDescriptor": ("frameworks/base/core/java/android/os/ParcelFileDescriptor.java", (
        ("seekTo", "(J)J", False),
    )),
    "android/graphics/ColorSpace": ("frameworks/base/graphics/java/android/graphics/ColorSpace.java", (
        ("get", "(I)Landroid/graphics/ColorSpace;", True),
        ("getNativeInstance", "()J", False),
    )),
    "android/graphics/Bitmap": ("frameworks/base/graphics/java/android/graphics/Bitmap.java", (
        ("checkRecycled", "(Ljava/lang/String;)V", False),
        ("createScaledAshmemBitmap", "(Landroid/graphics/Bitmap;IIZ)Landroid/graphics/Bitmap;", True),
        ("getDefaultDensity", "()I", True),
        ("getNinePatchInsets", "()Landroid/graphics/NinePatch$InsetStruct;", False),
        ("getNativeInstance", "()J", False),
        ("getOpticalInsets", "(Landroid/graphics/Rect;)V", False),
        ("scaleFromDensity", "(III)I", True),
    )),
    "android/graphics/Mesh": ("frameworks/base/graphics/java/android/graphics/Mesh.java", (
        ("getNativeWrapperInstance", "()J", False),
    )),
    "android/graphics/ColorFilter": ("frameworks/base/graphics/java/android/graphics/ColorFilter.java", (
        ("getNativeInstance", "()J", False),
    )),
    "android/graphics/Typeface": ("frameworks/base/graphics/java/android/graphics/Typeface.java", (
        ("createFromTypefaceWithVariation",
         "(Landroid/graphics/Typeface;Ljava/util/List;)Landroid/graphics/Typeface;", True),
        ("getNativeInstance", "()J", False),
        ("isSupportedAxes", "(I)Z", False),
    )),
    "android/graphics/fonts/Font": ("frameworks/base/graphics/java/android/graphics/fonts/Font.java", (
        ("getNativePtr", "()J", False),
    )),
    "android/graphics/fonts/FontStyle": ("frameworks/base/graphics/java/android/graphics/fonts/FontStyle.java", (
        ("getMatchScore", "(Landroid/graphics/fonts/FontStyle;)I", False),
    )),
    "android/graphics/fonts/FontFamily": ("frameworks/base/graphics/java/android/graphics/fonts/FontFamily.java", (
        ("getNativePtr", "()J", False),
    )),
    "android/graphics/fonts/Font$Builder": ("frameworks/base/graphics/java/android/graphics/fonts/Font.java", (
        ("createBuffer", "(Landroid/content/res/AssetManager;Ljava/lang/String;ZI)Ljava/nio/ByteBuffer;", True),
    )),
    "android/graphics/fonts/SystemFonts": ("frameworks/base/graphics/java/android/graphics/fonts/SystemFonts.java", (
        ("getSystemPreinstalledFontConfig", "()Landroid/text/FontConfig;", True),
        ("getSystemPreinstalledFontConfigFromLegacyXml", "()Landroid/text/FontConfig;", True),
        ("buildSystemFallback", "(Landroid/text/FontConfig;)Ljava/util/Map;", True),
        ("buildSystemTypefaces", "(Landroid/text/FontConfig;Ljava/util/Map;)Ljava/util/Map;", True),
    )),
    "android/graphics/ImageDecoder": ("frameworks/base/graphics/java/android/graphics/ImageDecoder.java", (
        ("createSource", "(Landroid/content/res/Resources;Ljava/io/InputStream;I)Landroid/graphics/ImageDecoder$Source;", True),
    )),
    "android/provider/FontsContract": ("sdk/android/provider/FontsContract.java", (
        ("getFontSync", "(Landroid/provider/FontRequest;)Landroid/graphics/Typeface;", True),
    )),
    "android/animation/AnimatorInflater": ("frameworks/base/core/java/android/animation/AnimatorInflater.java", (
        ("loadAnimator", "(Landroid/content/res/Resources;Landroid/content/res/Resources$Theme;IF)Landroid/animation/Animator;", True),
    )),
    "android/animation/PropertyValuesHolder": ("frameworks/base/core/java/android/animation/PropertyValuesHolder.java", (
        ("getValueType", "()Ljava/lang/Class;", False),
    )),
    "android/animation/AnimatorSet": ("frameworks/base/core/java/android/animation/AnimatorSet.java", (
        ("canReverse", "()Z", False),
    )),
    "android/graphics/text/MeasuredText": ("frameworks/base/graphics/java/android/graphics/text/MeasuredText.java", (
        ("getChars", "()[C", False),
        ("getNativePtr", "()J", False),
    )),
    "android/content/Intent": ("frameworks/base/core/java/android/content/Intent.java", (
        ("maybeMarkAsMissingCreatorToken", "(Ljava/lang/Object;)V", True),
    )),
    "android/text/TextUtils": ("frameworks/base/core/java/android/text/TextUtils.java", (
        ("nullIfEmpty", "(Ljava/lang/String;)Ljava/lang/String;", True),
    )),
    "android/view/View": ("frameworks/base/core/java/android/view/View.java", (
        ("getAccessibilityViewId", "()I", False),
        ("getAccessibilityWindowId", "()I", False),
        ("includeForAccessibility", "()Z", False),
    )),
    "android/view/ViewGroup": ("frameworks/base/core/java/android/view/ViewGroup.java", (
        ("generateLayoutParams", "(Landroid/content/Context;Landroid/util/AttributeSet;)Landroid/view/ViewGroup$LayoutParams;", False),
    )),
    "android/view/MotionEvent": ("frameworks/base/core/java/android/view/MotionEvent.java", (
        ("scale", "(F)V", False),
    )),
    "android/view/WindowManager$LayoutParams": ("frameworks/base/core/java/android/view/WindowManager.java", (
        ("scale", "(F)V", False),
    )),
    "android/util/Xml": ("sdk/android/util/Xml.java", (
        ("newFastPullParser", "()Lcom/android/modules/utils/TypedXmlPullParser;", True),
        ("newFastSerializer", "()Lcom/android/modules/utils/TypedXmlSerializer;", True),
    )),
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def profile_sources(aosp_root, inputs, manifest=PROFILE / "sources.txt"):
    groups = defaultdict(list)
    directories = defaultdict(list)
    exclusions = defaultdict(set)
    group = None
    for number, line in enumerate(manifest.read_text(encoding="utf-8").splitlines(), 1):
        line = line.partition("#")[0].strip()
        if not line:
            continue
        if line in ("[framework]", "[java-base]", "[services]", "[annotations]", "[sdk-support]",
                    "[aconfig]", "[aidl]", "[patches]"):
            group = line[1:-1]
            continue
        parts = line.split(maxsplit=1)
        directive, name = parts if line.startswith("@") and len(parts) == 2 else ("", line)
        path = PurePosixPath(name)
        if group is None or not path.parts or path.is_absolute() or ".." in path.parts or path.as_posix() != name:
            raise ValueError(f"{manifest}:{number}: invalid source path {name!r}")
        if not directive and not name.startswith("@"):
            groups[group].append(name)
        elif group in ("framework", "services") and directive == "@dir":
            directories[group].append(path)
        elif group in ("framework", "services") and directive == "@exclude":
            if name in exclusions[group]:
                raise ValueError(f"{manifest}:{number}: repeated source exclusion {name}")
            exclusions[group].add(name)
        else:
            raise ValueError(f"{manifest}:{number}: unsupported source directive {line!r}")
    seen = set()
    for group in sorted(groups.keys() | directories.keys() | exclusions.keys()):
        expanded = []
        for directory in directories[group]:
            root = aosp_root / directory
            java_files = sorted(root.rglob("*.java"))
            if not java_files:
                raise ValueError(f"{manifest}: no Java files under {directory}")
            inputs.add(root)
            inputs.update(path for path in root.rglob("*") if path.is_dir())
            expanded.extend(path.relative_to(aosp_root).as_posix() for path in java_files)
        missing = exclusions[group].difference(expanded)
        if missing:
            raise ValueError(f"{manifest}: exclusions outside selected directories: {sorted(missing)}")
        groups[group].extend(name for name in expanded if name not in exclusions[group])
        for name in groups[group]:
            if name in seen:
                raise ValueError(f"{manifest}: duplicate source {name}")
            seen.add(name)
    for name in ("framework", "services"):
        if not groups[name]:
            raise ValueError(f"{manifest}: empty {name} group")
    return groups


def apply_patch(path, sources):
    """Apply exact text hunks at their declared line numbers, in memory."""
    data = path.read_bytes()
    lines = data.splitlines(keepends=True)
    position = 0
    changes = {}
    if not lines:
        raise ValueError(f"{path}: empty patch")
    while position < len(lines):
        old = lines[position]
        new = lines[position + 1] if position + 1 < len(lines) else b""
        if not old.startswith(b"--- a/") or not new.startswith(b"+++ b/"):
            raise ValueError(f"{path}:{position + 1}: expected unified file headers")
        name = old[6:].removesuffix(b"\n").decode("utf-8")
        if name not in sources or new != f"+++ b/{name}\n".encode() or name in changes:
            raise ValueError(f"{path}:{position + 1}: invalid or repeated Java target {name!r}")
        original = sources[name].splitlines(keepends=True)
        result = []
        consumed = inserted = removed = hunks = 0
        position += 2
        while position < len(lines) and lines[position].startswith(b"@@"):
            number = position + 1
            match = HUNK.fullmatch(lines[position])
            if not match:
                raise ValueError(f"{path}:{number}: malformed hunk header")
            old_line, old_size, new_line, new_size = (
                int(value) if value is not None else 1 for value in match.groups()
            )
            old_start = old_line - (old_size != 0)
            new_start = new_line - (new_size != 0)
            if old_start < consumed or old_start + old_size > len(original):
                raise ValueError(f"{path}:{number}: hunk outside {name}")
            result.extend(original[consumed:old_start])
            if new_start != len(result):
                raise ValueError(f"{path}:{number}: incorrect new line number in {name}")
            consumed = old_start
            old_remaining, new_remaining = old_size, new_size
            position += 1
            while old_remaining or new_remaining:
                line = lines[position] if position < len(lines) else b""
                prefix, content = line[:1], line[1:]
                if prefix in (b" ", b"-") and old_remaining and original[consumed] == content:
                    consumed += 1
                    old_remaining -= 1
                elif prefix != b"+":
                    raise ValueError(f"{path}:{position + 1}: exact hunk match failed in {name}")
                if prefix in (b" ", b"+"):
                    if not new_remaining or not content.endswith(b"\n"):
                        raise ValueError(f"{path}:{position + 1}: invalid new hunk line in {name}")
                    result.append(content)
                    new_remaining -= 1
                inserted += prefix == b"+"
                removed += prefix == b"-"
                position += 1
            hunks += 1
        if not hunks or not (inserted or removed):
            raise ValueError(f"{path}: no edits for {name}")
        sources[name] = b"".join(result + original[consumed:])
        changes[name] = {"added": inserted, "removed": removed}
    return {"sha256": digest(data), "files": changes,
            "added": sum(x["added"] for x in changes.values()),
            "removed": sum(x["removed"] for x in changes.values())}


def stage_java(destination, relative, data):
    file = destination / relative
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_bytes(data)
    return file


def build_resources(args, stage, inputs):
    """Follow core/res/Android.bp for the offline framework resource package."""
    root = args.aosp_root / "frameworks/base/core/res"
    manifest = root / "AndroidManifest.xml"
    remote_manifest = root / "RemoteThemeColorsAndroidManifest.xml"
    remote = root / "remote_color_resources_res"
    folders = (root / "res", root / "assets", remote / "values")
    resource_paths = {manifest, remote_manifest, remote / "symbols.xml"}
    for folder in folders:
        inputs.add(folder)
        for path in folder.rglob("*"):
            if path.is_dir():
                inputs.add(path)
            elif path.is_file() and not path.name.startswith("."):
                resource_paths.add(path)
    inputs.update(resource_paths)

    flags = set()
    records = []
    for path in sorted(resource_paths):
        data = path.read_bytes()
        if path.suffix == ".xml" and not path.is_relative_to(root / "assets"):
            flags.update(name.decode("ascii") for name in RESOURCE_FLAG.findall(data))
        records.append({"path": path.relative_to(args.aosp_root).as_posix(), "kind": "verbatim",
                        "sha256": digest(data), "added": 0, "removed": 0})
    # Resource qualifiers require read-only flags. Match the disabled aconfig
    # defaults used by this offline Java profile.
    flag_values = ",".join(f"{name}:ro=false" for name in sorted(flags))
    flag_args = ["--feature-flags", flag_values] if flags else []

    remote_compiled = stage / "remote-color.zip"
    remote_apk = stage / "remote-color.apk"
    run(args.aapt2, "compile", "--dir", remote, "-o", remote_compiled)
    run(args.aapt2, "link", "--manifest", remote_manifest, "-o", remote_apk, remote_compiled)

    generated_res = stage / "generated-res"
    with zipfile.ZipFile(remote_apk) as apk:
        stage_java(generated_res, "raw/remote_views_color_resources.arsc", apk.read("resources.arsc"))
    stage_java(generated_res, "values/symbols.xml", (remote / "symbols.xml").read_bytes())
    remote_extra = stage / "remote-extra.zip"
    run(args.aapt2, "compile", "--dir", generated_res, "-o", remote_extra)

    compiled = stage / "framework-res.zip"
    # Preserve ordinary PNG bytes. AAPT2 still compiles NinePatch metadata.
    run(args.aapt2, "compile", "--no-crunch", *flag_args, "--dir", root / "res", "-o", compiled)
    classes = stage / "resource-symbols"
    classes.mkdir()
    run(args.aapt2, "link", "-x", "--manifest", manifest, "-A", root / "assets", *flag_args,
        "--min-sdk-version", "26", "--target-sdk-version", "36",
        "--private-symbols", "com.android.internal", "--no-auto-version", "--auto-add-overlay",
        "--enable-sparse-encoding", "--java", classes, "-o", stage / FRAMEWORK_RES,
        compiled, remote_extra)
    return sorted(classes.rglob("*.java")), records, sorted(flags)


def generate_flags(inputs, destination):
    """Emit the aconfig API with the offline profile's flags disabled."""
    flags = defaultdict(dict)
    for name, data in inputs.items():
        source = data.decode("utf-8")
        package = re.search(r'^package:\s*"([\w.]+)"', source, re.MULTILINE)
        names = re.findall(r'^[ \t]+name:\s*"([a-z][a-z0-9_]*)"', source, re.MULTILINE)
        if not package or not names:
            raise ValueError(f"{name}: expected an aconfig package and flag declarations")
        for flag in names:
            if flag in flags[package[1]]:
                raise ValueError(f"{name}: duplicate flag {package[1]}.{flag}")
            flags[package[1]][flag] = name
    for package, declarations in sorted(flags.items()):
        code = [f"package {package};", "public final class Flags {", "    private Flags() {}"]
        for flag in sorted(declarations):
            accessor = re.sub(r"_([a-z0-9])", lambda m: m[1].upper(), flag)
            code += [f'    public static final String FLAG_{flag.upper()} = "{package}.{flag}";',
                     f"    public static boolean {accessor}() {{ return false; }}"]
        code += ["}", ""]
        relative = f"{package.replace('.', '/')}/Flags.java"
        file = stage_java(destination, relative, "\n".join(code).encode())
        yield file, {"path": relative, "generator": "aconfig",
                     "inputs": sorted(set(declarations.values())), "sha256": digest(file.read_bytes())}


def resource_flag_declarations(symbols, destination, program_sources):
    """Supply javac with the constants referenced by SDK 36 aapt2 annotations."""
    fields = defaultdict(set)
    for symbol in symbols:
        for package, field in SYMBOL_FLAG.findall(symbol.read_bytes()):
            fields[package.decode("ascii")].add(field.decode("ascii"))
    for package, names in sorted(fields.items()):
        relative = f"{package.replace('.', '/')}/Flags.java"
        if relative in program_sources:
            continue  # The Java profile already generates this package's aconfig API.
        code = [f"package {package};", "public final class Flags {", "    private Flags() {}"]
        for field in sorted(names):
            name = field.removeprefix("FLAG_").lower()
            code.append(f'    public static final String {field} = "{package}.{name}";')
        code += ["}", ""]
        file = stage_java(destination, relative, "\n".join(code).encode())
        yield file, {"path": relative, "generator": "aapt2_annotation_constants",
                     "kind": "compile_only", "sha256": digest(file.read_bytes())}


def javac(args, files, classes, classpath, *options):
    classes.mkdir()
    if files:
        sources = classes.with_suffix(".sources")
        sources.write_text("".join(json.dumps(str(file), ensure_ascii=False) + "\n"
                                   for file in sorted(files)), encoding="utf-8")
        run(args.javac, "--release", "17", *options, "-encoding", "UTF-8", "-proc:none", "-implicit:none",
            "-g:source,lines", "-XDstringConcat=inline", "-sourcepath", "",
            "-classpath", os.pathsep.join(map(str, classpath)), "-d", classes, "@" + str(sources))


def jar_from_classes(path, *directories):
    entries = {}
    for classes in directories:
        for file in classes.rglob("*.class"):
            name = file.relative_to(classes).as_posix()
            if name in entries:
                raise ValueError(f"{classes}: duplicate class {name}")
            entries[name] = file.read_bytes()
    if not entries:
        raise ValueError(f"{path}: javac produced no classes")
    write_jar(path, entries)


def compile_only_api(args, stage, inputs):
    entries = {}
    records = []
    with zipfile.ZipFile(args.android_jar) as sdk:
        for owner, (name, methods) in HIDDEN_METHODS.items():
            path = (args.sdk_sources / name.removeprefix("sdk/") if name.startswith("sdk/")
                    else args.aosp_root / name)
            data = path.read_bytes()
            inputs.add(path)
            for method, _, _ in methods:
                if not re.search(rb"\b" + method.encode() + rb"\s*\(", data):
                    raise ValueError(f"{path}: missing platform declaration for {method}")
            entry = owner + ".class"
            entries[entry] = sdk_declarations(sdk.read(entry), methods=methods)
            records.append({"path": name, "kind": "compile_api", "sha256": digest(data)})
    output = stage / "compile-only-api.jar"
    write_jar(output, entries)
    return output, records


def compile_only_platform_sources(args, stage, inputs):
    """Project SDK-hidden platform declarations used by framework source."""
    declarations = {
        "dalvik/system/VMRuntime.java": (
            "libcore/libart/src/main/java/dalvik/system/VMRuntime.java",
            ("public static native VMRuntime getRuntime();",
             "public native void registerNativeAllocation(int bytes);",
             "public native void registerNativeFree(int bytes);"),
        ),
        "libcore/util/NativeAllocationRegistry.java": (
            "libcore/luni/src/main/java/libcore/util/NativeAllocationRegistry.java",
            ("public static native NativeAllocationRegistry createMalloced(ClassLoader loader, long freeFunction);",
             "public static native NativeAllocationRegistry createMalloced("
             "ClassLoader loader, long freeFunction, long size);",
             "public native Runnable registerNativeAllocation(Object object, long address);"),
        ),
        "android/os/SystemProperties.java": (
            "frameworks/base/core/java/android/os/SystemProperties.java",
            ("public static native String get(String key, String defaultValue);",),
        ),
        "android/view/InsetsState.java": (
            "frameworks/base/core/java/android/view/InsetsState.java",
            ("public native void scale(float factor);",),
        ),
        "android/view/InsetsSourceControl.java": (
            "frameworks/base/core/java/android/view/InsetsSourceControl.java",
            ("public native android.graphics.Insets getInsetsHint();",
             "public native void setInsetsHint(int left, int top, int right, int bottom);"),
        ),
    }
    for name, (upstream, members) in declarations.items():
        source = args.aosp_root / upstream
        data = source.read_bytes()
        inputs.add(source)
        for member in members:
            method = member.partition("(")[0].split()[-1]
            if not re.search(rb"\b" + method.encode() + rb"\s*\(", data):
                raise ValueError(f"{source}: missing platform declaration for {method}")
        path = PurePosixPath(name)
        package = ".".join(path.parent.parts)
        code = f"package {package};\npublic class {path.stem} {{\n"
        code += "".join(f"    {member}\n" for member in members)
        yield stage_java(stage, name, (code + "}\n").encode())


def dex_from_jar(args, stage, jar, output, classpath, *, compressed_archive=False):
    out = stage / output
    out.mkdir()
    libraries = [arg for path in classpath for arg in ("--classpath", path)]
    run(args.d8, "--release", "--min-api", "26", "--lib", args.android_jar,
        *libraries, "--output", out, jar)
    count = len(list(out.glob("*.dex")))
    names = ["classes.dex", *(f"classes{i}.dex" for i in range(2, count + 1))]
    if not count or any(not (out / name).is_file() for name in names):
        raise ValueError(f"{output}: expected consecutive classes*.dex from D8")
    artifact = stage / (output + ".dex")
    if count == 1 and not compressed_archive:
        (out / names[0]).replace(artifact)
    else:
        # ART recognizes ZIP archives by magic, including single-shard archives.
        compression = zipfile.ZIP_DEFLATED if compressed_archive else zipfile.ZIP_STORED
        write_jar(artifact, {name: (out / name).read_bytes() for name in names}, compression=compression)
    return names


def dex_entries(path):
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            for name in archive.namelist():
                yield archive.read(name)
    else:
        yield path.read_bytes()


def defined_methods(stage):
    counts = {}
    for artifact in (FRAMEWORK_DEX, SERVICES_DEX):
        for dex in dex_entries(stage / artifact):
            for line in dex_profile(dex, set(), set()).splitlines():
                if line.startswith("L"):
                    descriptor, methods, _ = line.split()
                    counts[descriptor[1:-1].replace("/", ".")] = int(methods)
    return {name: counts.get(name) for name in METHODS_TO_AUDIT}


def build(args, stage):
    manifest = PROFILE / "sources.txt"
    inputs = {Path(__file__).resolve(), Path(__file__).with_name("build_pm_dex.py").resolve(),
              manifest, PROFILE, args.javac, args.android_jar, args.d8, args.aapt2}
    groups = profile_sources(args.aosp_root, inputs, manifest)
    toolchain = {"android_jar": args.android_jar, "javac": args.javac, "d8": args.d8,
                 "aapt2": args.aapt2}
    d8_jar = args.d8.parent / "lib/d8.jar"
    if d8_jar.is_file():
        inputs.add(d8_jar)
        toolchain["d8_jar"] = d8_jar

    java = {}
    for group in ("framework", "java-base", "services"):
        for name in groups[group]:
            path = args.aosp_root / name
            if (not name.startswith(("frameworks/base/", "libcore/")) or path.suffix != ".java"):
                raise ValueError(f"{manifest}: expected an upstream Java file in [{group}]: {name}")
            inputs.add(path)
            java[name] = path.read_bytes()
    originals = {name: digest(data) for name, data in java.items()}
    patches = {}
    for name in groups["patches"]:
        path = PROFILE / name
        if path.suffix != ".patch" or path.parent not in (PROFILE, PROFILE / "patches"):
            raise ValueError(f"{manifest}: expected a patch in the framework profile: {name}")
        inputs.add(path)
        patches[path.name] = apply_patch(path, java)
    records = []
    staged = defaultdict(list)
    java_base = stage / "java-base"
    for group in ("framework", "java-base", "services"):
        for name in sorted(groups[group]):
            data = java[name]
            if group == "java-base":
                staged[group].append(stage_java(java_base, Path(name).relative_to(JAVA_BASE), data))
            else:
                staged[group].append(stage_java(stage / "upstream", name, data))
            applied = {patch: stat["files"][name] for patch, stat in patches.items()
                       if name in stat["files"]}
            target = "framework" if group == "java-base" else group
            records.append({"path": name, "target": target, "kind": "patched" if applied else "verbatim",
                            "sha256": originals[name], "compiled_sha256": digest(data),
                            "patches": applied, "added": sum(p["added"] for p in applied.values()),
                            "removed": sum(p["removed"] for p in applied.values())})

    aconfig = {}
    for name in sorted(groups["aconfig"]):
        path = args.aosp_root / name
        if not name.startswith(("frameworks/base/", "libcore/")) or path.suffix != ".aconfig":
            raise ValueError(f"{manifest}: expected an upstream aconfig file: {name}")
        inputs.add(path)
        aconfig[name] = path.read_bytes()
    symbols, resources, resource_flags = build_resources(args, stage, inputs)
    staged["framework"].extend(symbols)
    generated = [{"path": file.relative_to(stage / "resource-symbols").as_posix(),
                  "generator": "aapt2", "sha256": digest(file.read_bytes())}
                 for file in symbols]
    program_flags = set()
    for file, record in generate_flags(aconfig, stage / "generated"):
        staged["framework"].append(file)
        generated.append(record)
        program_flags.add(record["path"])

    if groups["aidl"]:
        framework_aidl = args.android_jar.with_name("framework.aidl")
        toolchain.update(aidl=args.aidl, framework_aidl=framework_aidl)
        inputs.update((args.aidl, framework_aidl))
        aidl_out = stage / "aidl"
        aidl_out.mkdir()
        aidl_inputs = stage / "aidl-inputs"
        aidl_files = {}
        for name in sorted(groups["aidl"]):
            path = args.aosp_root / name
            if not name.startswith("frameworks/base/") or path.suffix != ".aidl":
                raise ValueError(f"{manifest}: expected an upstream AIDL file: {name}")
            inputs.add(path)
            data = path.read_bytes()
            package = re.search(rb"^package\s+([\w.]+)\s*;", data, re.MULTILINE)
            if not package:
                raise ValueError(f"{path}: missing AIDL package")
            relative = f"{package[1].decode().replace('.', '/')}/{path.stem}.java"
            file = aidl_out / relative
            source = stage_java(aidl_inputs, Path(relative).with_suffix(".aidl"), data)
            aidl_files[source] = (file, name, data)
        run(args.aidl, "--lang=java", "--min_sdk_version=26", "--omit_invocation",
            f"--out={aidl_out}", f"-I{aidl_inputs}", f"-p{framework_aidl}", *aidl_files)
        for file, name, data in aidl_files.values():
            staged["framework"].append(file)
            generated.append({"path": file.relative_to(aidl_out).as_posix(), "generator": "aidl",
                              "inputs": sorted(groups["aidl"]), "sha256": digest(file.read_bytes())})
            records.append({"path": name, "kind": "aidl", "sha256": digest(data)})

    sdk_sources = []
    for name in sorted(groups["annotations"]):
        path = args.sdk_sources / name
        if not name.startswith(("android/annotation/", "android/compat/annotation/",
                                "android/ravenwood/annotation/", "com/android/internal/annotations/",
                                "dalvik/annotation/optimization/")) or path.suffix != ".java":
            raise ValueError(f"{manifest}: expected a compile-only SDK annotation: {name}")
        inputs.add(path)
        sdk_sources.append({"path": name, "sha256": digest(path.read_bytes())})
    annotations = stage / "annotations"
    annotation_files = [args.sdk_sources / item["path"] for item in sdk_sources]
    for file, record in resource_flag_declarations(symbols, stage / "resource-annotations", program_flags):
        annotation_files.append(file)
        generated.append(record)
    annotation_files.extend(compile_only_platform_sources(args, stage / "platform-api", inputs))
    thread_context = Path(__file__).with_name("framework_build") / "boot_api/org/wasmandroid/ui/ThreadContext.java"
    annotation_files.append(thread_context)
    inputs.add(thread_context)
    annotation_files.append(stage_java(stage / "platform-api",
        "com/google/errorprone/annotations/CompileTimeConstant.java",
        b"package com.google.errorprone.annotations;\npublic @interface CompileTimeConstant {}\n"))
    javac(args, annotation_files, annotations, [args.android_jar])
    framework = stage / "framework-classes"
    core_library = stage / "java-base-classes"
    services = stage / "services-classes"
    api, api_records = compile_only_api(args, stage, inputs)
    records.extend(api_records)
    sdk_classpath = [annotations, api, args.android_jar]
    # XML Pull supplies the interface and event constants. The parser's
    # implementation comes from the upstream XmlBlock.Parser class.
    parser_entry = "org/xmlpull/v1/XmlPullParser.java"
    parser_source = args.sdk_sources / parser_entry
    inputs.add(parser_source)
    staged["framework"].append(parser_source)
    sdk_runtime_sources = []
    for name in sorted(groups["sdk-support"]):
        path = args.sdk_sources / name
        if path.suffix != ".java":
            raise ValueError(f"{manifest}: expected an SDK support source: {name}")
        inputs.add(path)
        staged["framework"].append(path)
        sdk_runtime_sources.append({"path": name, "sha256": digest(path.read_bytes())})
    javac(args, staged["framework"], framework, sdk_classpath)
    javac(args, staged["java-base"], core_library, sdk_classpath,
          "--patch-module", f"java.base={java_base}")
    javac(args, staged["services"], services, [framework, *sdk_classpath])
    framework_jar = stage / FRAMEWORK_JAR
    services_jar = stage / SERVICES_JAR
    jar_from_classes(framework_jar, framework, core_library)
    jar_from_classes(services_jar, services)
    dex_files = {
        FRAMEWORK_DEX: dex_from_jar(args, stage, framework_jar, Path(FRAMEWORK_DEX).stem, [],
                                    compressed_archive=True),
        SERVICES_DEX: dex_from_jar(args, stage, services_jar, Path(SERVICES_DEX).stem, [framework_jar]),
    }

    records.extend({"path": name, "kind": "aconfig", "sha256": digest(data)}
                   for name, data in aconfig.items())
    stats = {kind: sum(item["kind"] == kind for item in records)
             for kind in ("verbatim", "patched", "aidl", "aconfig", "compile_api")}
    stats.update(added=sum(p["added"] for p in patches.values()),
                 removed=sum(p["removed"] for p in patches.values()),
                 framework_base_java=sum(name.startswith("frameworks/base/")
                                         for name in groups["framework"]))
    report = {"format_version": 1, "upstream_tag": "android-17.0.0_r1",
              "upstream_files": sorted(records, key=lambda item: item["path"]),
              "upstream_resources": resources, "disabled_resource_flags": resource_flags,
              "defined_methods": defined_methods(stage), "dex_files": dex_files,
              "patches": patches, "stats": stats, "generated": generated,
              "sdk_compile_only_annotations": sdk_sources,
              "sdk_runtime_interfaces": [{"path": parser_entry, "sha256": digest(parser_source.read_bytes())}],
              "sdk_runtime_sources": sdk_runtime_sources,
              "toolchain_sha256": {name: digest(path.read_bytes()) for name, path in toolchain.items()},
              "artifacts": {name: digest((stage / name).read_bytes())
                            for name in outputs(args) if name != PROVENANCE}}
    (stage / PROVENANCE).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return inputs, stats


def outputs(args):
    if args.services_classes_jar:
        return (*ARTIFACTS[:-1], SERVICES_JAR, PROVENANCE)
    return ARTIFACTS


def main():
    home_sdk = Path.home() / ("Library/Android/sdk" if sys.platform == "darwin" else "Android/Sdk")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aosp-root", type=Path, default=REPO / "third_party/aosp17")
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--android-sdk", type=Path,
                        default=os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME") or home_sdk)
    parser.add_argument("--javac", type=Path, default=Path("/usr/bin/javac"))
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--aidl", type=Path)
    parser.add_argument("--aapt2", type=Path)
    parser.add_argument("--android-jar", type=Path)
    parser.add_argument("--sdk-sources", type=Path)
    parser.add_argument("--services-classes-jar", action="store_true",
                        help="also emit the service javac classpath for downstream tests")
    parser.add_argument("--depfile", type=Path)
    parser.add_argument("--check", action="store_true", help="rebuild and compare every requested output")
    args = parser.parse_args()
    build_tools = args.android_sdk / "build-tools/36.0.0"
    args.d8 = args.d8 or build_tools / "d8"
    args.aidl = args.aidl or build_tools / "aidl"
    args.aapt2 = args.aapt2 or build_tools / "aapt2"
    args.android_jar = args.android_jar or args.android_sdk / "platforms/android-36/android.jar"
    args.sdk_sources = args.sdk_sources or args.android_sdk / "sources/android-36"
    args.aosp_root = args.aosp_root.absolute()
    args.out_dir = args.out_dir.resolve()
    if any(path and path.resolve().is_relative_to(args.aosp_root.resolve())
           for path in (args.out_dir, args.depfile)):
        parser.error("build outputs must be outside the read-only AOSP tree")
    if args.check and args.depfile:
        parser.error("--check and --depfile are mutually exclusive")
    try:
        if not args.check:
            args.out_dir.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".upstream-framework-", dir=args.out_dir) as work:
            stage = Path(work)
            inputs, stats = build(args, stage)
            for name in outputs(args):
                output = args.out_dir / name
                if args.check:
                    if (stage / name).read_bytes() != output.read_bytes():
                        raise ValueError(f"reproducibility check failed for {output}")
                else:
                    (stage / name).replace(output)
            if args.depfile:
                args.depfile.parent.mkdir(parents=True, exist_ok=True)
                write_depfile(args.depfile, args.out_dir / FRAMEWORK_DEX, inputs)
        verb = "Verified" if args.check else "Built"
        print(f"{verb} framework and services DEX in {args.out_dir} "
              f"({stats['framework_base_java']} frameworks/base Java files in the primary DEX, "
              f"{stats['verbatim']} verbatim, {stats['patched']} patched, "
              f"+{stats['added']}/-{stats['removed']} lines)")
    except (OSError, ValueError, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    main()
