#!/usr/bin/env python3
"""Build a single-window Settings APK from the tagged AOSP 17 sources, offline."""

import argparse
from collections import Counter, defaultdict, deque
from dataclasses import replace
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

sys.dont_write_bytecode = True
from build_pm_dex import write_depfile, write_jar
from build_upstream_systemui import (
    ResourceFile, count_report, declarations, digest, file_digest, lines, resource_mentions,
    run, select_values,
)

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent.parent
PROFILE = ROOT / "patches/aosp17/settings"
PREFERENCE_LINK = PROFILE / "PreferenceLink.java"
FRAMEWORK_ADAPTERS = (
    TOOLS / "framework_build/java/android/app/Fragment.java",
    TOOLS / "framework_build/java/org/wasmandroid/ui/PreferenceGridView.java",
    TOOLS / "framework_build/java/org/wasmandroid/ui/PreferenceRuntime.java",
)
SETTINGS = Path("packages/apps/Settings")
BASE = Path("frameworks/base")
SETTINGSLIB = BASE / "packages/SettingsLib"
TAG = "android-17.0.0_r1"
PACKAGE = "com.android.settings"
APK = "Settings-upstream.apk"
REPORT = "upstream_provenance.json"
MANIFEST = SETTINGS / "AndroidManifest.xml"
MIN_SDK = "30"
TARGET_SDK = "36"

AARS = (
    "annotation-experimental-1.4.1", "core-1.15.0", "customview-1.1.0",
    "customview-poolingcontainer-1.0.0", "interpolator-1.0.0", "versionedparcelable-1.1.1",
    "vectordrawable-1.1.0", "vectordrawable-animated-1.1.0", "appcompat-resources-1.7.0",
    "appcompat-1.7.0", "recyclerview-1.4.0", "preference-1.2.1",
)
JARS = (
    "annotation-jvm-1.9.1", "annotations-23.0.0", "collection-jvm-1.4.4",
    "lifecycle-common-jvm-2.8.7", "kotlin-stdlib-2.1.21",
)
RESOURCE_DIRS = (
    SETTINGSLIB / "res",
    *(SETTINGSLIB / name / "res" for name in (
        "LayoutPreference", "SettingsTheme", "UsageProgressBarPreference", "TopIntroPreference",
        "AppPreference", "FooterPreference", "Category", "TwoTargetPreference",
    )),
    SETTINGS / "res", SETTINGS / "res-product",
)
JAVA_RESOURCE = re.compile(r"(?<![\w.])(?:com\.android\.(?:settings|settingslib)\.)?R\.(\w+)\.(\w+)")
LOCALE_VALUES = re.compile(r"values-(?:[a-z]{2,3}(?:-|$)|b\+)")
ANDROID = "http://schemas.android.com/apk/res/android"


def select_manifest(original, output):
    """Project the system-image manifest onto the upstream homepage entry point."""
    ET.register_namespace("android", ANDROID)
    name = f"{{{ANDROID}}}name"
    root = ET.parse(original).getroot()
    app = root.find("application")
    homepage = alias = None
    for component in list(app):
        key = (component.tag, component.get(name))
        if key == ("activity", ".homepage.SettingsHomepageActivity"):
            homepage = component
        else:
            if key == ("activity-alias", "Settings"):
                alias = component
            app.remove(component)
    if homepage is None or alias is None or alias.get(f"{{{ANDROID}}}targetActivity") != homepage.get(name):
        raise ValueError("Upstream Settings launcher declarations are missing")
    # The embedded launcher starts the resolved component directly. Keep the
    # original filter and attach it to the original alias target.
    homepage.extend(alias.findall("intent-filter"))
    for child in list(root):
        if child is not app:
            root.remove(child)
    ET.indent(root, space="    ")
    ET.ElementTree(root).write(output, encoding="utf-8", xml_declaration=True)


def upstream_revisions(upstream):
    revisions = {}
    for project in (SETTINGS, BASE):
        if not (upstream / project / ".git").exists():
            raise ValueError(f"Missing Git metadata for {upstream / project}")
        head, tag = run("git", "-C", upstream / project, "rev-parse", "HEAD",
                        f"refs/tags/{TAG}^{{commit}}", capture_output=True, text=True).stdout.splitlines()
        if head != tag:
            raise ValueError(f"Expected {TAG} for {project}, found {head}")
        revisions[project.as_posix()] = head
    return revisions


def stage_sources(upstream, work, sources, inputs):
    originals = {}

    def copy(relative):
        source = upstream / relative
        target = work / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        originals[relative] = file_digest(target)
        inputs.add(source)

    for relative in sorted({MANIFEST, *sources}):
        copy(relative)
    patches = []
    git_env = {**os.environ, "GIT_CEILING_DIRECTORIES": str(work.parent)}
    for patch in sorted(PROFILE.glob("*.patch")):
        stats = run("git", "apply", "--numstat", "-z", patch, cwd=work, env=git_env,
                    capture_output=True).stdout
        if not stats or not re.match(r"\d{4}-", patch.name):
            raise ValueError(f"Expected a numbered unified patch at {patch}")
        changes = []
        for record in stats.rstrip(b"\0").decode().split("\0"):
            added, removed, name = record.split("\t", 2)
            relative = Path(name)
            if (".." in relative.parts
                    or not any(relative.is_relative_to(root) for root in (SETTINGS, SETTINGSLIB))
                    or not (upstream / relative).is_file()):
                raise ValueError(f"{patch.name} must modify an existing upstream file: {name}")
            if relative not in originals:
                copy(relative)
            changes.append({"path": name, "added": int(added), "removed": int(removed),
                            "input_sha256": file_digest(work / relative)})
        run("git", "apply", "--no-index", patch, cwd=work, env=git_env)
        for change in changes:
            after = work / change["path"]
            change["output_sha256"] = file_digest(after)
            if change["output_sha256"] == change["input_sha256"]:
                raise ValueError(f"{patch.name} did not update {after}")
        patches.append({"name": patch.name, "sha256": file_digest(patch), "files": changes})
        inputs.add(patch)
    return originals, patches


def dependencies(args, stage, inputs):
    libraries = {}
    resources = []
    adapted = {}
    jars = [args.deps_dir / "jars" / (name + ".jar") for name in JARS]
    inputs.update(jars)
    for name in AARS:
        archive = args.deps_dir / "aars" / (name + ".aar")
        inputs.add(archive)
        with zipfile.ZipFile(archive) as aar:
            package = ET.fromstring(aar.read("AndroidManifest.xml")).attrib["package"]
            aar_files = sorted(aar.namelist())
            libraries[package] = aar.read("R.txt").decode().splitlines()
            for entry in aar_files:
                path = PurePosixPath(entry)
                if len(path.parts) == 3 and path.parts[0] == "res" and not entry.endswith("/"):
                    resources.append(ResourceFile(archive.name, path, aar.read(entry), False))
                elif entry == "classes.jar" or (entry.startswith("libs/") and path.suffix == ".jar"):
                    jar = stage / "aars" / name / path.name
                    jar.parent.mkdir(parents=True, exist_ok=True)
                    jar.write_bytes(aar.read(entry))
                    if name == "preference-1.2.1" and entry == "classes.jar":
                        output = jar.with_name("embedded.jar")
                        run(args.java, "--add-exports", "java.base/jdk.internal.org.objectweb.asm=ALL-UNNAMED",
                            PREFERENCE_LINK, jar, output)
                        adapted[archive.name] = {
                            "original_classes_sha256": file_digest(jar),
                            "embedded_classes_sha256": file_digest(output),
                        }
                        jar = output
                    jars.append(jar)
    return jars, libraries, resources, adapted


def java_symbols(generated, symbols, libraries):
    linked = {}
    for line in symbols.read_text().splitlines():
        field_type, kind, name, value = line.split(maxsplit=3)
        linked[kind, name] = f"public static final {field_type} {name} = {value};"
    libraries = {**libraries, "com.android.settingslib": symbols.read_text().splitlines()}
    for package, entries in sorted(libraries.items()):
        groups = defaultdict(list)
        for entry in entries:
            _, kind, name, _ = entry.split(maxsplit=3)
            if declaration := linked.get((kind, name)):
                groups[kind].append(declaration)
        path = generated.joinpath(*package.split("."), "R.java")
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", encoding="utf-8") as out:
            out.write(f"package {package};\npublic final class R {{\n")
            for kind, fields in sorted(groups.items()):
                out.write(f"  public static final class {kind} {{\n")
                for field in fields:
                    out.write(f"    {field}\n")
                out.write("  }\n")
            out.write("}\n")


def resource_key(path):
    return f"{path.parent.name.partition('-')[0]}/{path.name.partition('.')[0]}"


def select_settings_resources(files, records, roots, deferred):
    """Walk resource references, loading non-values XML only when reached."""
    declared = defaultdict(list)
    resource_files = defaultdict(list)
    for record in records:
        for name in record.names:
            declared[name].append(record)
    for index, resource in enumerate(files):
        if resource.path.parent.name.partition("-")[0] != "values":
            resource_files[resource_key(resource.path)].append(index)

    selected = defaultdict(set)
    names = set()
    unresolved = {}
    resolved = set()
    queue = deque((name, name) for name in sorted(roots, key=lambda name: (name.startswith("id/"), name)))
    while queue:
        name, chain = queue.popleft()
        if name in resolved or name.startswith("android:"):
            continue
        resolved.add(name)
        choices = declared.get(name, ())
        indices = resource_files.get(name, ())
        if not choices and not indices:
            unresolved[name] = chain
            continue
        if name.startswith("id/") and name in names:
            continue
        for record in choices:
            if record.child in selected[record.file]:
                continue
            selected[record.file].add(record.child)
            names.update(record.names)
            queue.extend((ref, f"{chain} -> {ref}") for ref in sorted(record.dependencies))
        for index in indices:
            if None in selected[index]:
                continue
            if index in deferred:
                files[index] = replace(files[index], data=deferred[index].read_bytes())
            record, = declarations([files[index]])
            selected[index].add(None)
            names.update(record.names)
            queue.extend((ref, f"{chain} -> {ref}") for ref in sorted(record.dependencies))
    missing = [chain for name, chain in unresolved.items() if name not in names]
    if missing:
        raise ValueError("Unresolved Settings resource references:\n  " + "\n  ".join(missing))
    return selected, frozenset(names)


def link_resources(args, stage, work, originals, sources, libraries, files, inputs):
    localized = []
    deferred = {}
    for directory in RESOURCE_DIRS:
        for path in sorted((args.aosp_root / directory).glob("*/*")):
            if not path.is_file() or path.name.startswith("."):
                continue
            relative = path.relative_to(args.aosp_root)
            source = work / relative if relative in originals else path
            inputs.add(path)
            if LOCALE_VALUES.match(path.parent.name):
                localized.append((directory, relative, source))
            else:
                data = b""
                if path.parent.name.partition("-")[0] == "values":
                    data = source.read_bytes()
                else:
                    deferred[len(files)] = source
                files.append(ResourceFile(directory.as_posix(), PurePosixPath(relative),
                                          data, True))

    records = [replace(record, file=index) for index, file in enumerate(files)
               if file.path.parent.name.partition("-")[0] == "values"
               for record in declarations([file])]
    index = {name for record in records for name in record.names}
    index.update(resource_key(file.path) for file in files if file.path.parent.name.partition("-")[0] != "values")
    roots = set(lines(PROFILE / "resources.txt"))
    manifest = stage / "AndroidManifest.xml"
    select_manifest(work / MANIFEST, manifest)
    manifest_refs, _ = resource_mentions(ET.parse(manifest).getroot())
    roots.update(manifest_refs)
    java_names = {name.replace(".", "_"): name for name in index}
    for source in sources:
        for kind, name in JAVA_RESOURCE.findall((work / source).read_text(encoding="utf-8")):
            key = f"{kind}/{name}"
            if kind == "styleable":
                while key not in index and "_" in key:
                    key = key.rsplit("_", 1)[0]
            if kind != "id" and key in java_names:
                roots.add(java_names[key])
    for package in ("androidx.preference", "androidx.recyclerview"):
        for entry in libraries[package]:
            _, kind, name, _ = entry.split(maxsplit=3)
            if resource := java_names.get(f"{kind}/{name}"):
                roots.add(resource)
    selected, selected_names = select_settings_resources(files, records, roots, deferred)
    ordered = {resource.origin: index for index, resource in enumerate(files)}
    for directory, relative, source in localized:
        data = source.read_bytes()
        resource = ResourceFile(directory.as_posix(), PurePosixPath(relative), data, True, selected_names)
        elements = declarations([resource])
        if elements:
            selected[len(files)] = {element.child for element in elements}
            files.append(resource)
    directories = {}
    report = []
    for index, children in sorted(selected.items()):
        resource = files[index]
        raw = resource.data if None in children else select_values(resource.data, children)
        directory = stage / "resources" / resource.origin
        directories[directory] = None
        output = directory / resource.path.parent.name / resource.path.name
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(raw)
        report.append({
            "origin": resource.origin, "path": str(resource.path), "upstream": resource.upstream,
            "sha256": file_digest(args.aosp_root / resource.path) if resource.upstream else digest(resource.data),
            "compiled_sha256": digest(raw),
            "value_elements": None if None in children else sorted(children),
        })
    compiled = []
    for i, directory in enumerate(sorted(directories, key=lambda path:
                                             ordered.get(path.relative_to(stage / "resources").as_posix(), 0))):
        flat = stage / f"resources-{i}.zip"
        run(args.aapt2, "compile", "--dir", directory, "-o", flat)
        compiled.extend(("-R", flat))
    generated = stage / "generated"
    generated.mkdir()
    symbols = stage / "R.txt"
    resource_apk = stage / "resources.apk"
    run(args.aapt2, "link", "-I", args.android_jar, "--manifest", manifest,
        "--java", generated, "--output-text-symbols", symbols,
        "--min-sdk-version", MIN_SDK, "--target-sdk-version", TARGET_SDK,
        "--product", "default", "--auto-add-overlay", "-o", resource_apk, *compiled)
    java_symbols(generated, symbols, libraries)
    return resource_apk, generated, report


def compile_sources(args, stage, work, sources, generated, jars, inputs):
    java = [work / path for path in sources if path.suffix == ".java"]
    kotlin = [work / path for path in sources if path.suffix == ".kt"]
    java.extend(sorted(generated.rglob("*.java")))
    classes = stage / "classes"
    classes.mkdir()
    classpath = os.pathsep.join(map(str, (classes, *jars, args.android_jar)))
    if kotlin:
        compiler = sorted((args.deps_dir / "kotlinc").glob("*.jar"))
        if not compiler:
            raise ValueError("Offline Kotlin compiler jars are missing")
        inputs.update(compiler)
        run(args.java, "-cp", os.pathsep.join(map(str, compiler)),
            "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler", "-jvm-target", "17",
            "-no-stdlib", "-no-reflect", "-classpath", classpath, "-d", classes, *kotlin, *java)
    run(args.javac, "--release", "17", "-encoding", "UTF-8", "-proc:none",
        "-g:source,lines", "-XDstringConcat=inline", "-sourcepath", "",
        "-classpath", classpath, "-d", classes, *java)

    program = stage / "settings.jar"
    write_jar(program, {path.relative_to(classes).as_posix(): path.read_bytes()
                        for path in sorted(classes.rglob("*.class"))})
    dex_dir = stage / "dex"
    dex_dir.mkdir()
    run(args.d8, "--release", "--min-api", MIN_SDK, "--thread-count", "4", "--lib", args.android_jar,
        "--output", dex_dir, program, *jars)
    dex = sorted(dex_dir.glob("classes*.dex"))
    if not dex or dex[0].name != "classes.dex":
        raise ValueError("D8 emitted no primary DEX file")
    return dex


def sign_apk(args, stage, resources, dex, inputs):
    signing = TOOLS / "launcher3_build/signing"
    key, cert = signing / "offline-test.pk8", signing / "offline-test.x509.pem"
    inputs.update((key, cert))
    with zipfile.ZipFile(resources, "a") as apk:
        for path in dex:
            apk.writestr(zipfile.ZipInfo(path.name), path.read_bytes())
    aligned = stage / "aligned.apk"
    apk = stage / APK
    run(args.zipalign, "-P", "16", "4", resources, aligned)
    run(args.apksigner, "sign", "--key", key, "--cert", cert, "--v1-signing-enabled", "false",
        "--v2-signing-enabled", "true", "--v3-signing-enabled", "false",
        "--v4-signing-enabled", "false", "--out", apk, aligned)
    run(args.zipalign, "-c", "-P", "16", "4", apk)
    run(args.apksigner, "verify", apk)
    return apk


def dex_report(path):
    with path.open("rb") as stream:
        header = stream.read(112)
    if len(header) != 112 or not header.startswith(b"dex\n"):
        raise ValueError(f"Invalid DEX header in {path}")
    return {**count_report(path), "classes": struct.unpack_from("<I", header, 96)[0]}


def build(args, stage):
    inputs = {Path(__file__).resolve(), TOOLS / "build_pm_dex.py", TOOLS / "build_upstream_systemui.py",
              PROFILE / "sources.txt", PROFILE / "resources.txt", PREFERENCE_LINK, *FRAMEWORK_ADAPTERS}
    tools = ("aapt2", "android_jar", "apksigner", "d8", "java", "javac", "zipalign")
    inputs.update(getattr(args, name) for name in tools)
    for name in ("d8", "apksigner"):
        library = getattr(args, name).parent / "lib" / (name + ".jar")
        if library.is_file():
            inputs.add(library)
    revisions = upstream_revisions(args.aosp_root)
    sources = [Path(value) for value in lines(PROFILE / "sources.txt")]
    if not sources or len(set(sources)) != len(sources) or any(
        path.suffix not in (".java", ".kt") or ".." in path.parts
        or not any(path.is_relative_to(root) for root in (SETTINGS, SETTINGSLIB))
        for path in sources
    ):
        raise ValueError("sources.txt must list unique Settings or SettingsLib Java/Kotlin sources")
    work = stage / "upstream"
    originals, patches = stage_sources(args.aosp_root, work, sources, inputs)
    print("Linking resources from the Settings profile and offline AARs", flush=True)
    jars, libraries, candidates, adapted = dependencies(args, stage, inputs)
    resources, generated, resource_inputs = link_resources(
        args, stage, work, originals, sources, libraries, candidates, inputs)
    print("Compiling the upstream Settings sources and producing DEX", flush=True)
    dex = compile_sources(args, stage, work, sources, generated, jars, inputs)
    apk = sign_apk(args, stage, resources, dex, inputs)
    source_report = []
    for path in sorted({MANIFEST, *sources}):
        if file_digest(args.aosp_root / path) != originals[path]:
            raise ValueError(f"Upstream source changed while building: {path}")
        source_report.append({"path": path.as_posix(), "sha256": originals[path],
                              "compiled_sha256": file_digest(work / path)})
    upstream_files = [Path(record["path"]) for record in (*source_report, *resource_inputs)
                      if record.get("upstream", True)]
    counts = {str(root): dict(Counter(
        path.suffix.removeprefix(".") for path in upstream_files if path.is_relative_to(root)
    )) for root in (SETTINGS, SETTINGSLIB)}
    provenance = {
        "tag": TAG, "package": PACKAGE, "upstream_revisions": revisions,
        "upstream_input_counts": counts, "upstream_sources": source_report,
        "resource_inputs": resource_inputs, "patches": patches,
        "linked_manifest_sha256": file_digest(stage / "AndroidManifest.xml"),
        "adapted_dependency_archives": adapted,
        "compatibility_inputs": {str(path.relative_to(ROOT)): file_digest(path)
                                 for path in (PREFERENCE_LINK, *FRAMEWORK_ADAPTERS)},
        "apk": count_report(apk), "dex": {path.name: dex_report(path) for path in dex},
        "toolchain": {name: count_report(getattr(args, name)) for name in tools},
        "dependency_archives": {
            str(path.relative_to(args.deps_dir)): count_report(path)
            for path in sorted(inputs) if path.is_relative_to(args.deps_dir)
        },
    }
    (stage / REPORT).write_text(json.dumps(provenance, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return inputs


def main():
    home_sdk = Path.home() / ("Library/Android/sdk" if sys.platform == "darwin" else "Android/Sdk")
    sdk = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME") or home_sdk
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, default=Path(sdk))
    parser.add_argument("--aosp-root", type=Path, default=ROOT / "third_party/aosp17")
    parser.add_argument("--deps-dir", "--deps", type=Path, default=ROOT / "third_party/aosp17_deps")
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--depfile", type=Path)
    parser.add_argument("--check", action="store_true", help="Rebuild and compare the APK and provenance")
    for tool in ("aapt2", "d8", "zipalign", "apksigner", "android-jar"):
        parser.add_argument("--" + tool, type=Path)
    for tool in ("java", "javac"):
        parser.add_argument("--" + tool, type=Path, default=Path(shutil.which(tool) or f"/usr/bin/{tool}"))
    args = parser.parse_args()
    build_tools = args.sdk / "build-tools/36.0.0"
    for tool in ("aapt2", "d8", "zipalign", "apksigner"):
        if getattr(args, tool) is None:
            setattr(args, tool, build_tools / tool)
    if args.android_jar is None:
        args.android_jar = args.sdk / "platforms/android-36/android.jar"
    for name, path in vars(args).items():
        if isinstance(path, Path):
            setattr(args, name, path.absolute())
    if args.check and args.depfile:
        parser.error("--check and --depfile are mutually exclusive")
    for path in (args.out_dir, args.depfile):
        if path and any(path.resolve().is_relative_to(root.resolve())
                        for root in (args.aosp_root, args.deps_dir)):
            parser.error("Choose output paths outside the read-only upstream inputs")
    os.environ.update(GIT_NO_LAZY_FETCH="1", GIT_OPTIONAL_LOCKS="0", LC_ALL="C", TZ="UTC")
    try:
        if not args.check:
            args.out_dir.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".settings-", dir=args.out_dir) as temp:
            stage = Path(temp)
            inputs = build(args, stage)
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
        print(f"{'Verified' if args.check else 'Built'} {args.out_dir / APK}")
    except subprocess.CalledProcessError as error:
        parser.exit(1, f"{parser.prog}: {Path(error.cmd[0]).name} exited with status {error.returncode}\n")
    except (OSError, ValueError, ET.ParseError, zipfile.BadZipFile) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    main()
