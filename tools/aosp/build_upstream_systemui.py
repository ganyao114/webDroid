#!/usr/bin/env python3
"""Build the offline AOSP 17 SystemUI phone-bar APK from upstream inputs."""

import argparse
from collections import Counter, defaultdict, deque
from dataclasses import dataclass
import hashlib
from io import BytesIO
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from xml.parsers import expat
import zipfile

sys.dont_write_bytecode = True
from build_pm_dex import write_depfile, write_jar

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parent.parent
INPUTS = REPO / "patches/aosp17/systemui"
SYSTEMUI = Path("frameworks/base/packages/SystemUI")
SETTINGSLIB = Path("frameworks/base/packages/SettingsLib")
FRAMEWORK_RES = Path("frameworks/base/core/res/res")
SYSTEMUI_LIBS = Path("frameworks/libs/systemui")
SOURCE_ROOTS = (SYSTEMUI, SETTINGSLIB, SYSTEMUI_LIBS)
AOSP_TAG = "android-17.0.0_r1"
APK = "SystemUI-upstream.apk"
REPORT = "systemui_provenance.json"
PACKAGE = "com.android.systemui"
MANIFEST = SYSTEMUI / "AndroidManifest-res.xml"
FEATURE_FLAGS = {
    "android.net.platform.flags.powered_off_finding_message_new_product_name": False,
    "com.android.systemui.desktop_sizing": False,
    "com.android.systemui.dream_overlay_updated_ui": False,
}
RESOURCE_DIRS = ("res", "res-keyguard", "res-product")
FRAMEWORK_VALUES = {
    "config.xml": {
        "bool/config_batterymeterDualTone",
        "bool/config_battery_percentage_setting_available",
        "bool/config_defaultBatteryPercentageSetting",
        "integer/config_criticalBatteryWarningLevel",
        "integer/config_navBarInteractionMode",
        "string/config_batterymeterBoltPath",
        "string/config_batterymeterErrorPerimeterPath",
        "string/config_batterymeterFillMask",
        "string/config_batterymeterPerimeterPath",
        "string/config_batterymeterPowersavePath",
    },
    "dimens.xml": {
        "dimen/navigation_bar_height",
        "dimen/navigation_bar_height_landscape",
        "dimen/status_bar_height_default",
    },
}
AAR_NAMES = (
    "core-1.15.0", "appcompat-resources-1.7.0", "appcompat-1.7.0",
    "constraintlayout-2.2.0", "material-1.12.0",
)
APP_NAMESPACES = {
    "http://schemas.android.com/apk/res-auto",
    f"http://schemas.android.com/apk/res/{PACKAGE}",
    f"http://schemas.android.com/apk/res/{PACKAGE}.res",
}
ANDROID_PRIVATE_NAMESPACE = "http://schemas.android.com/apk/prv/res/android"
RESOURCE_REF = re.compile(
    r"(?<!\\)([@?])(\+)?\*?(?:([\w.]+):)?([\w-]+)/([\w.]+)"
)
ATTRIBUTE_REF = re.compile(r"\?(?:\*?([\w.]+):)?([\w.]+)\Z")
JAVA_REF = re.compile(r"(?<![\w.])(?:com\.android\.settingslib\.)?R\.(\w+)\.(\w+)")


def run(*arguments, **kwargs):
    return subprocess.run([str(argument) for argument in arguments], check=True, **kwargs)


def lines(path):
    return [value for line in path.read_text(encoding="utf-8").splitlines()
            if (value := line.partition("#")[0].strip())]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_digest(path):
    sha256 = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            sha256.update(chunk)
    return sha256.hexdigest()


def count_report(path):
    return {"bytes": path.stat().st_size, "sha256": file_digest(path)}


@dataclass(frozen=True)
class ResourceFile:
    origin: str
    path: PurePosixPath
    data: bytes
    upstream: bool
    names: frozenset[str] | None = None


@dataclass(frozen=True)
class Declaration:
    file: int
    child: int | None
    names: frozenset[str]
    dependencies: frozenset[str]


def qualified(package, name):
    if package == "androidprv":
        package = "android"
    return name if package in ("", None, PACKAGE, PACKAGE + ".res") else f"{package}:{name}"


def resource_mentions(element):
    references = set()
    ids = set()
    for node in element.iter():
        for value in (node.text, node.tail, *node.attrib.values()):
            if not value:
                continue
            for sigil, define, package, kind, name in RESOURCE_REF.findall(value):
                key = qualified(package, f"{kind}/{name}")
                if sigil == "@" and define and kind == "id":
                    ids.add(key)
                else:
                    references.add(key)
            theme_attr = ATTRIBUTE_REF.fullmatch(value.strip())
            if theme_attr:
                references.add(qualified(theme_attr[1], f"attr/{theme_attr[2]}"))
        for attribute in node.attrib:
            if attribute.startswith("{"):
                namespace, name = attribute[1:].split("}", 1)
                if namespace in APP_NAMESPACES:
                    references.add(f"attr/{name}")
                elif namespace == ANDROID_PRIVATE_NAMESPACE:
                    references.add(f"android:attr/{name}")
    return references, ids


def value_name(element):
    name = element.get("name")
    if not name:
        return None
    kind = element.tag
    if kind in ("item", "java-symbol", "public"):
        kind = element.get("type")
    elif kind == "declare-styleable":
        kind = "styleable"
    elif kind in ("string-array", "integer-array"):
        kind = "array"
    if kind and kind not in ("eat-comment", "add-resource", "overlayable", "skip"):
        return f"{kind}/{name}"
    return None


def value_dependencies(element, refs, names):
    if element.tag == "style":
        name = element.attrib["name"]
        parent = element.get("parent", name.rpartition(".")[0])
        if parent and not parent.startswith("@"):
            package, colon, entry = parent.replace("*", "").partition(":")
            refs.add(qualified(package if colon else None, f"style/{entry if colon else package}"))
        for item in element.findall("item"):
            name = item.attrib["name"].lstrip("*")
            package, colon, attr = name.partition(":")
            refs.add(qualified(package if colon else None, f"attr/{attr if colon else package}"))
    if element.tag == "declare-styleable":
        for attr in element.findall("attr"):
            package, colon, name = attr.attrib["name"].lstrip("*").partition(":")
            if colon:
                refs.add(qualified(package, f"attr/{name}"))
            elif len(attr) or "format" in attr.attrib:
                names.add(f"attr/{package}")
            else:
                refs.add(f"attr/{package}")


def declarations(files):
    result = []
    for index, origin in enumerate(files):
        kind = origin.path.parent.name.partition("-")[0]
        if kind == "values":
            for child, element in enumerate(ET.fromstring(origin.data)):
                name = value_name(element)
                if (name is None or element.tag in ("java-symbol", "public")
                        or origin.names is not None and name not in origin.names):
                    continue
                refs, names = resource_mentions(element)
                names.add(name)
                value_dependencies(element, refs, names)
                result.append(Declaration(index, child, frozenset(names), frozenset(refs - names)))
        else:
            name = f"{kind}/{origin.path.name.split('.')[0]}"
            refs, names = set(), {name}
            if origin.path.suffix == ".xml":
                refs, ids = resource_mentions(ET.fromstring(origin.data))
                names.update(ids)
            result.append(Declaration(index, None, frozenset(names), frozenset(refs - names)))
    return result


def select_resources(files, records, roots, platform):
    by_name = defaultdict(list)
    for record in records:
        for name in record.names:
            by_name[name].append(record)
    selected = defaultdict(set)
    visited = set()
    missing = []
    queue = deque((root, root) for root in sorted(roots))
    while queue:
        name, chain = queue.popleft()
        if name in visited:
            continue
        visited.add(name)
        if name.startswith("android:"):
            if name.removeprefix("android:") not in platform:
                missing.append(chain)
            continue
        candidates = by_name.get(name, [])
        if not candidates:
            missing.append(chain)
            continue
        # IDs can be defined in many unrelated layout files. One definition is sufficient.
        if name.startswith("id/"):
            if any(record.child in selected.get(record.file, ()) for record in candidates):
                continue
            candidates = sorted(candidates, key=lambda record: (
                not files[record.file].upstream, record.child is None, record.file
            ))[:1]
        for record in candidates:
            if record.child in selected[record.file]:
                continue
            selected[record.file].add(record.child)
            queue.extend((ref, f"{chain} -> {ref}") for ref in sorted(record.dependencies))
    if missing:
        raise ValueError("Unresolved upstream resource references:\n  " + "\n  ".join(missing))
    return selected


def select_values(data, wanted):
    """Copy chosen XML elements verbatim, preserving namespace declarations and encodings."""
    parser = expat.ParserCreate()
    pieces = []
    depth = 0
    root_start = root_end = child_start = 0

    def start(_name, _attrs):
        nonlocal depth, root_start, child_start
        if depth == 0:
            root_start = parser.CurrentByteIndex
        elif depth == 1:
            child_start = parser.CurrentByteIndex
        depth += 1

    def end(name):
        nonlocal depth, root_end
        depth -= 1
        offset = parser.CurrentByteIndex
        if depth == 0:
            root_end = offset
        elif depth == 1:
            if data.startswith(b"</" + name.encode("utf-8"), offset):
                offset = data.index(b">", offset) + 1
            pieces.append(data[child_start:offset])

    parser.StartElementHandler = start
    parser.EndElementHandler = end
    parser.Parse(data, True)
    if len(wanted) == len(pieces):
        return data
    return (data[:data.index(b">", root_start) + 1] + b"\n"
            + b"\n".join(pieces[index] for index in sorted(wanted))
            + b"\n" + data[root_end:])


def patch_sources(upstream, work, copied, inputs):
    patches = []
    git_env = {**os.environ, "GIT_CEILING_DIRECTORIES": str(work.parent.resolve())}
    for patch in sorted(INPUTS.glob("*.patch")):
        stats = run("git", "apply", "--numstat", "-z", patch, cwd=work, env=git_env,
                    capture_output=True).stdout
        if not stats:
            raise ValueError(f"No patch paths in {patch.name}")
        changes = []
        for item in stats.rstrip(b"\x00").split(b"\x00"):
            added, removed, path = item.decode("utf-8").split("\t", 2)
            relative = Path(path)
            if (not any(relative.is_relative_to(root) for root in SOURCE_ROOTS)
                    or ".." in relative.parts or not (upstream / relative).is_file()):
                raise ValueError(f"Patch must modify an existing upstream file: {patch.name}: {path}")
            if relative not in copied:
                destination = work / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(upstream / relative, destination)
                copied.add(relative)
            changes.append({"path": path, "added": int(added), "removed": int(removed),
                            "input_sha256": file_digest(work / relative)})
        run("git", "apply", "--no-index", "-", cwd=work, env=git_env, input=patch.read_bytes())
        for change in changes:
            after = file_digest(work / change["path"])
            if after == change["input_sha256"]:
                raise ValueError(f"Patch did not change {change['path']}: {patch.name}")
            change["output_sha256"] = after
        inputs.add(patch)
        patches.append({"name": patch.name, "sha256": file_digest(patch), "files": changes})
    return patches


def collect_resources(args, work, copied, inputs):
    files = []
    for archive_name in AAR_NAMES:
        archive = args.deps_dir / "aars" / f"{archive_name}.aar"
        inputs.add(archive)
        with zipfile.ZipFile(archive) as aar:
            for name in sorted(aar.namelist()):
                path = PurePosixPath(name)
                if len(path.parts) == 3 and path.parts[0] == "res" and not name.endswith("/"):
                    files.append(ResourceFile(archive.name, path, aar.read(name), False))
    resource_dirs = (SYSTEMUI_LIBS / "dynamiccolors/res", SETTINGSLIB / "res",
                     *(SYSTEMUI / directory for directory in RESOURCE_DIRS))
    for directory in resource_dirs:
        for path in sorted((args.aosp_root / directory).glob("*/*")):
            if not path.is_file() or path.name.startswith("."):
                continue
            relative = path.relative_to(args.aosp_root)
            inputs.add(path)
            source = work / relative if relative in copied else path
            files.append(ResourceFile(str(directory), PurePosixPath(relative), source.read_bytes(), True))
    # Link only the private framework values used by the embedded views, under the
    # APK package so they do not depend on private IDs in the SDK 36 resource table.
    for filename, names in FRAMEWORK_VALUES.items():
        for path in sorted((args.aosp_root / FRAMEWORK_RES).glob(f"values*/{filename}")):
            inputs.add(path)
            relative = path.relative_to(args.aosp_root)
            files.append(ResourceFile(str(FRAMEWORK_RES), PurePosixPath(relative), path.read_bytes(),
                                      True, frozenset(names)))
    return files


def link_resources(args, stage, work, copied, source_names, inputs):
    files = collect_resources(args, work, copied, inputs)
    platform_dump = run(args.aapt2, "dump", "resources", args.android_jar, capture_output=True).stdout
    platform = set(re.findall(rb"^\s*resource 0x[0-9a-f]+ ([\w-]+/[\w.]+)", platform_dump, re.M))
    platform = {name.decode("utf-8") for name in platform}
    roots = set(lines(INPUTS / "resources.txt"))
    records = declarations(files)
    index = {name for record in records for name in record.names}
    for source in source_names:
        for kind, entry in JAVA_REF.findall((work / source).read_text(encoding="utf-8")):
            if kind == "style":
                entry = entry.replace("_", ".")
            elif kind == "styleable":
                entry = entry.split("_", 1)[0]
            name = f"{kind}/{entry}"
            if name in index:
                roots.add(name)
    selection = select_resources(files, records, roots, platform)
    provenance = []
    directories = {}
    for file, children in sorted(selection.items()):
        resource = files[file]
        raw = resource.data if None in children else select_values(resource.data, children)
        directory = stage / "resource-inputs" / resource.origin / "res"
        directories.setdefault(directory, None)
        output = directory / resource.path.parent.name / resource.path.name
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(raw)
        origin = args.aosp_root / resource.path if resource.upstream else None
        provenance.append({
            "path": str(resource.path), "origin": resource.origin,
            "upstream": resource.upstream,
            "sha256": file_digest(origin) if origin else digest(resource.data),
            "compiled_sha256": digest(raw),
            "value_elements": None if None in children else sorted(children),
        })

    flags = ",".join(f"{name}:ro={str(value).lower()}" for name, value in FEATURE_FLAGS.items())
    compiled = []
    for i, directory in enumerate(directories):
        flat = stage / f"resources-{i}.zip"
        run(args.aapt2, "compile", "--feature-flags", flags, "--dir", directory, "-o", flat)
        compiled.extend(("-R", flat))

    generated = stage / "generated"
    generated.mkdir()
    resource_apk = stage / "resources.apk"
    run(args.aapt2, "link", "-I", args.android_jar, "--manifest", work / MANIFEST,
        "--java", generated, "--custom-package", PACKAGE + ".res",
        "--extra-packages", "com.android.settingslib",
        "--min-sdk-version", "26", "--target-sdk-version", "36",
        "--auto-add-overlay", "--feature-flags", flags, "-o", resource_apk, *compiled)
    return resource_apk, generated, provenance


def compile_sources(args, stage, work, source_names, generated, inputs):
    java = [work / path for path in source_names if path.suffix == ".java"]
    kotlin = [work / path for path in source_names if path.suffix == ".kt"]
    java.extend(sorted(generated.rglob("*.java")))
    jars = [args.deps_dir / "jars" / name for name in (
        "kotlin-stdlib-2.1.21.jar", "annotation-jvm-1.9.1.jar",
    )]
    compiler = sorted((args.deps_dir / "kotlinc").glob("*.jar"))
    inputs.update([*compiler, *jars])
    graphics_aar = args.deps_dir / "aars/core-1.15.0.aar"
    graphics = stage / "androidx-core-graphics.jar"
    graphics_classes = ("ColorUtils", "PathParser")
    with zipfile.ZipFile(graphics_aar) as aar:
        with zipfile.ZipFile(BytesIO(aar.read("classes.jar"))) as jar:
            entries = {}
            for cls in graphics_classes:
                prefix = f"androidx/core/graphics/{cls}"
                entries[f"{prefix}.class"] = jar.read(f"{prefix}.class")
                for name in jar.namelist():
                    if name.startswith(f"{prefix}$") and name.endswith(".class"):
                        entries[name] = jar.read(name)
    write_jar(graphics, entries)
    classpath = os.pathsep.join(map(str, (args.android_jar, *jars, graphics)))
    output = stage / "classes"
    output.mkdir()
    run(args.java, "-cp", os.pathsep.join(map(str, compiler)),
        "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler", "-jvm-target", "17",
        "-Xjvm-default=all", "-no-stdlib", "-no-reflect", "-classpath", classpath,
        "-d", output, *kotlin, *java)
    run(args.javac, "--release", "17", "-encoding", "UTF-8", "-proc:none",
        "-g:source,lines", "-XDstringConcat=inline", "-sourcepath", "",
        "-classpath", os.pathsep.join((str(output), classpath)), "-d", output, *java)
    program = stage / "systemui.jar"
    write_jar(program, {path.relative_to(output).as_posix(): path.read_bytes()
                        for path in sorted(output.rglob("*.class"))})
    dex_dir = stage / "dex"
    dex_dir.mkdir()
    run(args.d8, "--release", "--min-api", "26", "--lib", args.android_jar,
        "--output", dex_dir, program, jars[0], graphics)
    dex = sorted(dex_dir.glob("classes*.dex"))
    if not dex or dex[0].name != "classes.dex":
        raise ValueError("D8 did not produce classes.dex")
    return dex


def sign_apk(args, stage, resources, dex, inputs):
    signing = TOOLS / "launcher3_build/signing"
    key = signing / "offline-test.pk8"
    cert = signing / "offline-test.x509.pem"
    inputs.update((key, cert))
    with zipfile.ZipFile(resources, "a") as apk:
        for path in dex:
            entry = zipfile.ZipInfo(path.name, date_time=(1980, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_STORED
            apk.writestr(entry, path.read_bytes())
    aligned = stage / "aligned.apk"
    run(args.zipalign, "-P", "16", "4", resources, aligned)
    output = stage / APK
    run(args.apksigner, "sign", "--key", key, "--cert", cert, "--v1-signing-enabled", "false",
        "--v2-signing-enabled", "true", "--v3-signing-enabled", "false",
        "--v4-signing-enabled", "false", "--out", output, aligned)
    run(args.zipalign, "-c", "-P", "16", "4", output)
    run(args.apksigner, "verify", output)


def build(args, stage):
    upstream = args.aosp_root
    inputs = {Path(__file__).resolve(), TOOLS / "build_pm_dex.py", INPUTS / "sources.txt",
              INPUTS / "resources.txt", args.android_jar, args.aapt2, args.java, args.javac,
              args.d8, args.zipalign, args.apksigner}
    for tool in (args.d8, args.apksigner):
        library = tool.parent / "lib" / (tool.name + ".jar")
        if library.is_file():
            inputs.add(library)
    revisions = {}
    for project in (Path("frameworks/base"), SYSTEMUI_LIBS):
        revision = run("git", "-C", upstream / project, "rev-parse", "HEAD",
                       capture_output=True, text=True).stdout.strip()
        tag = run("git", "-C", upstream / project, "rev-parse", f"{AOSP_TAG}^{{commit}}",
                  capture_output=True, text=True).stdout.strip()
        if revision != tag:
            raise ValueError(f"Expected {AOSP_TAG} at {upstream / project}, found {revision}")
        revisions[str(project)] = revision
    references = [Path(path) for path in lines(INPUTS / "sources.txt")]
    if (len(references) != len(set(references)) or any(
        path.suffix not in (".java", ".kt") or ".." in path.parts
        or not any(path.is_relative_to(root) for root in SOURCE_ROOTS)
        for path in references
    )):
        raise ValueError("sources.txt must list unique upstream Java and Kotlin files")
    copied = {*references, MANIFEST}
    work = stage / "upstream"
    for relative in sorted(copied):
        original = upstream / relative
        output = work / relative
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(original, output)
        inputs.add(original)
    patches = patch_sources(upstream, work, copied, inputs)
    inputs.update(upstream / relative for relative in copied)
    resources, generated, resource_records = link_resources(
        args, stage, work, copied, references, inputs)
    dex = compile_sources(args, stage, work, references, generated, inputs)
    sign_apk(args, stage, resources, dex, inputs)

    source_records = []
    for path in sorted({*references, MANIFEST}):
        source_records.append({"path": str(path), "sha256": file_digest(upstream / path),
                               "compiled_sha256": file_digest(work / path)})
    upstream_files = [Path(record["path"]) for record in (*source_records, *resource_records)
                      if record.get("upstream", True)]
    counts = {
        root: dict(sorted(Counter(path.suffix[1:] for path in upstream_files
                                  if path.is_relative_to(root)).items()))
        for root in (*SOURCE_ROOTS, FRAMEWORK_RES)
    }
    patch_totals = Counter()
    for patch in patches:
        for change in patch["files"]:
            patch_totals.update({"added": change["added"], "removed": change["removed"]})
    tools = ("aapt2", "android_jar", "apksigner", "d8", "java", "javac", "zipalign")
    report = {
        "upstream_tag": AOSP_TAG,
        "upstream_revisions": revisions,
        "package": PACKAGE,
        "profile": "embedded phone bars",
        "manifest": str(MANIFEST),
        "upstream_input_counts": counts[SYSTEMUI],
        "upstream_settingslib_input_counts": counts[SETTINGSLIB],
        "upstream_library_input_counts": counts[SYSTEMUI_LIBS],
        "upstream_framework_resource_counts": counts[FRAMEWORK_RES],
        "upstream_sources": source_records,
        "resource_inputs": resource_records,
        "feature_flags": FEATURE_FLAGS,
        "patches": patches,
        "patch_line_totals": dict(patch_totals),
        "apk": count_report(stage / APK),
        "dex": {path.name: count_report(path) for path in dex},
        "aars": {archive.name: count_report(archive) for archive in sorted(inputs)
                 if archive.suffix == ".aar"},
        "toolchain": {name: count_report(getattr(args, name)) for name in tools},
        "jars": {str(path.relative_to(args.deps_dir)): count_report(path) for path in sorted(inputs)
                 if path.suffix == ".jar" and path.is_relative_to(args.deps_dir)},
    }
    (stage / REPORT).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return inputs, counts


def main():
    sdk_home = Path.home() / ("Library/Android/sdk" if sys.platform == "darwin" else "Android/Sdk")
    sdk = Path(os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME") or sdk_home)
    build_tools = sdk / "build-tools/36.0.0"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aosp-root", type=Path, default=REPO / "third_party/aosp17")
    parser.add_argument("--deps-dir", type=Path, default=REPO / "third_party/aosp17_deps")
    parser.add_argument("--out-dir", type=Path, required=True)
    for tool in ("aapt2", "d8", "zipalign", "apksigner"):
        parser.add_argument("--" + tool, type=Path, default=build_tools / tool)
    parser.add_argument("--java", type=Path, default=Path(shutil.which("java") or "/usr/bin/java"))
    parser.add_argument("--javac", type=Path, default=Path(shutil.which("javac") or "/usr/bin/javac"))
    parser.add_argument("--android-jar", type=Path, default=sdk / "platforms/android-36/android.jar")
    parser.add_argument("--depfile", type=Path)
    parser.add_argument("--check", action="store_true", help="rebuild and compare the signed APK and provenance")
    args = parser.parse_args()
    for name, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, name, value.absolute())
    readonly = (args.aosp_root.resolve(), args.deps_dir.resolve())
    if any(path and any(path.resolve().is_relative_to(root) for root in readonly)
           for path in (args.out_dir, args.depfile)):
        parser.error("Build outputs must be outside the read-only AOSP and dependency inputs")
    if args.check and args.depfile:
        parser.error("--check and --depfile are mutually exclusive")
    os.environ.update(GIT_NO_LAZY_FETCH="1", GIT_OPTIONAL_LOCKS="0")
    try:
        if not args.check:
            args.out_dir.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".systemui-", dir=args.out_dir) as temp:
            stage = Path(temp)
            inputs, counts = build(args, stage)
            for name in (APK, REPORT):
                output = args.out_dir / name
                if args.check:
                    if output.read_bytes() != (stage / name).read_bytes():
                        raise ValueError(f"Reproducibility check failed for {output}")
                else:
                    (stage / name).replace(output)
            if args.depfile:
                args.depfile.parent.mkdir(parents=True, exist_ok=True)
                write_depfile(args.depfile, args.out_dir / APK, inputs)
        verb = "Verified" if args.check else "Built"
        print(f"{verb} {args.out_dir / APK} "
              f"(SystemUI {counts[SYSTEMUI]}, SettingsLib {counts[SETTINGSLIB]})")
    except subprocess.CalledProcessError as error:
        parser.exit(1, f"{parser.prog}: {Path(error.cmd[0]).name} exited with status {error.returncode}\n")
    except (ET.ParseError, OSError, ValueError, zipfile.BadZipFile) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    main()
