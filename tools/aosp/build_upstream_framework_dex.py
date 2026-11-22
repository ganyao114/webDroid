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
from build_pm_dex import run, sdk_declarations, write_depfile, write_jar


REPO = Path(__file__).resolve().parents[2]
PROFILE = REPO / "patches/aosp17/frameworks_base"
JAVA_BASE = Path("libcore/ojluni/src/main/java")
FRAMEWORK_DEX = "aosp17_upstream_framework.dex"
FRAMEWORK_JAR = "aosp17_upstream_framework_classes.jar"
SERVICES_DEX = "aosp17_upstream_services.dex"
SERVICES_JAR = "aosp17_upstream_services_classes.jar"
PROVENANCE = "framework_provenance.json"
ARTIFACTS = (FRAMEWORK_DEX, FRAMEWORK_JAR, SERVICES_DEX, PROVENANCE)
HUNK = re.compile(rb"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@(?: .*)?\n")

# SDK 36 exposes the public classes but omits these platform members. The
# projections are only javac inputs and are built from the SDK classfiles.
HIDDEN_METHODS = {
    "frameworks/base/core/java/android/os/Parcel.java": ("android/os/Parcel", (
        ("adoptClassCookies", "(Landroid/os/Parcel;)V", False),
        ("compareData", "(Landroid/os/Parcel;)I", False),
        ("destroy", "()V", False),
        ("hasBinders", "()Z", False),
        ("hasFileDescriptors", "(Ljava/lang/Object;)Z", True),
        ("hasReadWriteHelper", "()Z", False),
        ("pushAllowFds", "(Z)Z", False),
        ("readString8", "()Ljava/lang/String;", False),
        ("restoreAllowFds", "(Z)V", False),
        ("writeArrayMapInternal", "(Landroid/util/ArrayMap;)V", False),
        ("writeString8", "(Ljava/lang/String;)V", False),
    )),
    "frameworks/base/graphics/java/android/graphics/ColorSpace.java": ("android/graphics/ColorSpace", (
        ("get", "(I)Landroid/graphics/ColorSpace;", True),
        ("getNativeInstance", "()J", False),
    )),
    "frameworks/base/graphics/java/android/graphics/Bitmap.java": ("android/graphics/Bitmap", (
        ("checkRecycled", "(Ljava/lang/String;)V", False),
        ("getDefaultDensity", "()I", True),
        ("getNativeInstance", "()J", False),
    )),
    "frameworks/base/graphics/java/android/graphics/Mesh.java": ("android/graphics/Mesh", (
        ("getNativeWrapperInstance", "()J", False),
    )),
    "frameworks/base/graphics/java/android/graphics/ColorFilter.java": ("android/graphics/ColorFilter", (
        ("getNativeInstance", "()J", False),
    )),
    "frameworks/base/graphics/java/android/graphics/Typeface.java": ("android/graphics/Typeface", (
        ("createFromTypefaceWithVariation",
         "(Landroid/graphics/Typeface;Ljava/util/List;)Landroid/graphics/Typeface;", True),
        ("getNativeInstance", "()J", False),
        ("isSupportedAxes", "(I)Z", False),
    )),
    "frameworks/base/graphics/java/android/graphics/fonts/Font.java": ("android/graphics/fonts/Font", (
        ("getNativePtr", "()J", False),
    )),
    "frameworks/base/graphics/java/android/graphics/text/MeasuredText.java": ("android/graphics/text/MeasuredText", (
        ("getChars", "()[C", False),
        ("getNativePtr", "()J", False),
    )),
    "frameworks/base/core/java/android/content/Intent.java": ("android/content/Intent", (
        ("maybeMarkAsMissingCreatorToken", "(Ljava/lang/Object;)V", True),
    )),
    "frameworks/base/core/java/android/text/TextUtils.java": ("android/text/TextUtils", (
        ("nullIfEmpty", "(Ljava/lang/String;)Ljava/lang/String;", True),
    )),
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def profile_sources():
    groups = defaultdict(list)
    manifest = PROFILE / "sources.txt"
    group = None
    for number, line in enumerate(manifest.read_text(encoding="utf-8").splitlines(), 1):
        line = line.partition("#")[0].strip()
        if not line:
            continue
        if line in ("[framework]", "[java-base]", "[services]", "[annotations]",
                    "[aconfig]", "[aidl]", "[patches]"):
            group = line[1:-1]
            continue
        path = PurePosixPath(line)
        if group is None or path.is_absolute() or ".." in path.parts or path.as_posix() != line:
            raise ValueError(f"{manifest}:{number}: invalid source path {line!r}")
        if any(line in paths for paths in groups.values()):
            raise ValueError(f"{manifest}:{number}: duplicate source {line}")
        groups[group].append(line)
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


def javac(args, files, classes, classpath, *options):
    classes.mkdir()
    if files:
        run(args.javac, "--release", "17", *options, "-encoding", "UTF-8", "-proc:none", "-implicit:none",
            "-g:source,lines", "-XDstringConcat=inline", "-sourcepath", "",
            "-classpath", os.pathsep.join(map(str, classpath)), "-d", classes, *sorted(files))


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
        for name, (owner, methods) in HIDDEN_METHODS.items():
            path = args.aosp_root / name
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


def dex_from_jar(args, stage, jar, output, classpath):
    out = stage / output
    out.mkdir()
    libraries = [arg for path in classpath for arg in ("--classpath", path)]
    run(args.d8, "--release", "--min-api", "26", "--lib", args.android_jar,
        *libraries, "--output", out, jar)
    if sorted(file.name for file in out.glob("*.dex")) != ["classes.dex"]:
        raise ValueError(f"{output}: expected one classes.dex from D8")
    (out / "classes.dex").replace(stage / (output + ".dex"))


def build(args, stage):
    manifest = PROFILE / "sources.txt"
    groups = profile_sources()
    inputs = {Path(__file__).resolve(), Path(__file__).with_name("build_pm_dex.py").resolve(),
              manifest, PROFILE, args.javac, args.android_jar, args.d8}
    toolchain = {"android_jar": args.android_jar, "javac": args.javac, "d8": args.d8}
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
        if Path(name).name != name or not name.endswith(".patch"):
            raise ValueError(f"{manifest}: expected a patch filename: {name}")
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
                            "patches": applied})

    aconfig = {}
    for name in sorted(groups["aconfig"]):
        path = args.aosp_root / name
        if not name.startswith(("frameworks/base/", "libcore/")) or path.suffix != ".aconfig":
            raise ValueError(f"{manifest}: expected an upstream aconfig file: {name}")
        inputs.add(path)
        aconfig[name] = path.read_bytes()
    generated = []
    for file, record in generate_flags(aconfig, stage / "generated"):
        staged["framework"].append(file)
        generated.append(record)

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
    javac(args, [args.sdk_sources / item["path"] for item in sdk_sources],
          annotations, [args.android_jar])
    framework = stage / "framework-classes"
    core_library = stage / "java-base-classes"
    services = stage / "services-classes"
    api, api_records = compile_only_api(args, stage, inputs)
    records.extend(api_records)
    sdk_classpath = [annotations, api, args.android_jar]
    javac(args, staged["framework"], framework, sdk_classpath)
    javac(args, staged["java-base"], core_library, sdk_classpath,
          "--patch-module", f"java.base={java_base}")
    javac(args, staged["services"], services, [framework, *sdk_classpath])
    framework_jar = stage / FRAMEWORK_JAR
    services_jar = stage / SERVICES_JAR
    jar_from_classes(framework_jar, framework, core_library)
    jar_from_classes(services_jar, services)
    dex_from_jar(args, stage, framework_jar, Path(FRAMEWORK_DEX).stem, [])
    dex_from_jar(args, stage, services_jar, Path(SERVICES_DEX).stem, [framework_jar])

    records.extend({"path": name, "kind": "aconfig", "sha256": digest(data)}
                   for name, data in aconfig.items())
    stats = {kind: sum(item["kind"] == kind for item in records)
             for kind in ("verbatim", "patched", "aidl", "aconfig", "compile_api")}
    stats.update(added=sum(p["added"] for p in patches.values()),
                 removed=sum(p["removed"] for p in patches.values()))
    report = {"format_version": 1, "upstream_tag": "android-17.0.0_r1",
              "upstream_files": sorted(records, key=lambda item: item["path"]),
              "patches": patches, "stats": stats, "generated": generated,
              "sdk_compile_only_annotations": sdk_sources,
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
              f"({stats['verbatim']} verbatim, {stats['patched']} patched, "
              f"+{stats['added']}/-{stats['removed']} lines)")
    except (OSError, ValueError, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    main()
