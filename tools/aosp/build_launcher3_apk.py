#!/usr/bin/env python3
"""Build the offline Launcher3 home APK from the AOSP 17 source checkout."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

from launcher3_build.sdk_sources import ATOM_SCHEMAS, write_annotations, write_launcher_atoms


PACKAGE = "com.android.launcher3"
LAUNCHER = Path("packages/apps/Launcher3")
COMPAT = Path(__file__).resolve().with_name("launcher3_build")
OTHER_UPSTREAM_JAVA = (
    Path("frameworks/libs/systemui/animationlib/src/com/android/app/animation/Interpolators.java"),
    Path("frameworks/libs/systemui/animationlib/src/com/android/app/animation/MathUtils.java"),
    Path("frameworks/libs/systemui/iconloaderlib/src/com/android/launcher3/util/ComponentKey.java"),
)
ARTIFACTS = ("Launcher3.apk", "launcher3_manifest_report.json")
ANDROID = "{http://schemas.android.com/apk/res/android}"
REQUIRED_APK_ENTRIES = (
    "AndroidManifest.xml",
    "resources.arsc",
    "res/layout/launcher.xml",
    "res/layout/hotseat.xml",
    "res/layout/workspace_screen.xml",
    "res/layout/all_apps.xml",
    "res/layout/app_icon.xml",
    "res/xml/device_profiles.xml",
    "res/xml/default_workspace_4x4.xml",
    "res/drawable/ic_launcher_home.xml",
    "res/drawable/ic_allapps_search.xml",
    "classes.dex",
)


def run(*command):
    subprocess.run([str(arg) for arg in command], check=True)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def provenance(paths, root):
    return [
        {"path": path.relative_to(root).as_posix(), "sha256": digest(path.read_bytes())}
        for path in sorted(paths)
    ]


def read_source_list(launcher):
    entries = [
        line for raw in (COMPAT / "upstream_java.txt").read_text().splitlines()
        if (line := raw.partition("#")[0].strip())
    ]
    if len(entries) != len(set(entries)):
        raise ValueError("Duplicate path in upstream_java.txt")
    for entry in entries:
        path = Path(entry)
        if path.parts[0] not in ("src", "src_no_quickstep", "shared") or ".." in path.parts:
            raise ValueError(f"Invalid Launcher3 source path in upstream_java.txt: {path}")
        if path.suffix != ".java" or not (launcher / path).is_file():
            raise ValueError(f"Missing upstream Launcher3 Java source: {path}")
    return [launcher / entry for entry in sorted(entries)]


def generate_flags(inputs, output):
    flags = {}
    for path in inputs:
        source = path.read_text(encoding="utf-8")
        package = re.search(r'^package:\s*"([^"]+)"', source, re.M)
        if package is None or package[1] != PACKAGE:
            raise ValueError(f"Unexpected aconfig package in {path}")
        start = r"^[ \t]*flag[ \t]*\{"
        blocks = re.findall(start + r"[ \t]*\n(.*?)^[ \t]*\}", source, re.M | re.S)
        if not blocks or len(blocks) != len(re.findall(start, source, re.M)):
            raise ValueError(f"Could not parse all aconfig flag declarations in {path}")
        for block in blocks:
            name = re.search(r'^\s*name:\s*"([a-z][a-z0-9_]*)"', block, re.M)
            if name is None or name[1] in flags:
                raise ValueError(f"Missing or duplicate aconfig flag name in {path}: {block}")
            # The sparse source tree carries declarations, with no product release value files.
            flags[name[1]] = False

    lines = [
        "// Generated from Launcher3 aconfig declarations for the offline SDK build.",
        f"package {PACKAGE};",
        "",
        "public final class Flags {",
        "    private Flags() {}",
        "",
    ]
    for name, enabled in sorted(flags.items()):
        first, *rest = name.split("_")
        method = first + "".join(word.capitalize() for word in rest)
        lines.append(f'    public static final String FLAG_{name.upper()} = "{PACKAGE}.{name}";')
        lines.append(f"    public static boolean {method}() {{ return {str(enabled).lower()}; }}")
    lines.append("}")
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return len(flags)


def generate_build_config(script, output):
    # Render the upstream Bash template with its default overrides. This also works on
    # hosts whose /bin/bash predates the associative arrays used in the upstream script.
    source = script.read_text(encoding="utf-8")
    package = re.search(r'^overrides\[pkg\]="([^"]+)"$', source, re.M)
    template = re.search(r'^echo "(\npackage .*?\n)"\s*$', source, re.M | re.S)
    if package is None or template is None or package[1] != PACKAGE:
        raise ValueError(f"Unrecognized upstream BuildConfig generator: {script}")
    rendered = template[1].replace("${overrides[pkg]}", PACKAGE)
    rendered = rendered.replace("${overrides[appId]:-" + PACKAGE + "}", PACKAGE)
    rendered = re.sub(r"\$\{overrides\[\w+\]:-(true|false)\}", r"\1", rendered)
    if "${" in rendered:
        raise ValueError(f"Unrecognized BuildConfig substitution in {script}")
    output.write_text(rendered.replace(r'\"', '"') + "\n", encoding="utf-8")


def javac_arguments(path, args, sources):
    # javac's @file format accepts quoted, escaped paths, including spaces.
    command = [
        "--release", "17", "-encoding", "UTF-8", "-proc:none", "-g:source,lines",
        "-XDstringConcat=inline", "-sourcepath", "", "-classpath", args.android_jar,
        "-d", path.parent / "classes", *sources,
    ]
    path.write_text("".join(json.dumps(str(arg), ensure_ascii=False) + "\n" for arg in command),
                    encoding="utf-8")


def zip_entry(name):
    entry = zipfile.ZipInfo(name)
    entry.create_system = 3
    entry.external_attr = 0o100644 << 16
    return entry


def compile_dex(args, stage, min_api, sources):
    classes = stage / "classes"
    classes.mkdir()
    argfile = stage / "javac.args"
    javac_arguments(argfile, args, sources)
    run(args.javac, f"@{argfile}")

    with zipfile.ZipFile(stage / "program.jar", "w") as jar:
        for compiled in sorted(classes.rglob("*.class")):
            name = compiled.relative_to(classes).as_posix()
            if not name.startswith("androidx/annotation/"):
                jar.writestr(zip_entry(name), compiled.read_bytes())
    dex_dir = stage / "dex"
    dex_dir.mkdir()
    run(args.d8, "--release", "--min-api", min_api, "--lib", args.android_jar,
        "--output", dex_dir, stage / "program.jar")
    dex_files = list(dex_dir.glob("classes*.dex"))
    if len(dex_files) != 1 or dex_files[0].name != "classes.dex":
        raise ValueError("Expected exactly one D8 classes.dex")
    return dex_files[0].read_bytes()


def build(args, stage):
    launcher = args.aosp_root / LAUNCHER
    manifest = launcher / "AndroidManifest.xml"
    resource_files = [path for path in (launcher / "res").rglob("*") if path.is_file()]
    java = read_source_list(launcher)
    external = [args.aosp_root / path for path in OTHER_UPSTREAM_JAVA]
    atom_schemas = [launcher / path for path in ATOM_SCHEMAS]
    configs = sorted((launcher / "aconfig").glob("launcher*.aconfig"))
    build_config = launcher / "tools/buildconfig.sh"
    compat_resources = sorted((COMPAT / "res_compat").rglob("*.xml"))
    bridges = sorted((COMPAT / "java").rglob("*.java"))
    debug_key = COMPAT / "signing/offline-test.pk8"
    debug_cert = COMPAT / "signing/offline-test.x509.pem"

    copy = stage / "AndroidManifest.xml"
    original = manifest.read_bytes()
    copy.write_bytes(original.replace(b"${applicationId}", PACKAGE.encode("ascii")))
    root = ET.fromstring(original)
    if root.get("package") != PACKAGE:
        raise ValueError(f"Upstream manifest does not declare {PACKAGE}")
    min_api = root.find("uses-sdk").attrib[ANDROID + "minSdkVersion"]

    upstream_res = stage / "upstream-res.zip"
    compat_res = stage / "compat-res.zip"
    run(args.aapt2, "compile", "--dir", launcher / "res", "-o", upstream_res)
    run(args.aapt2, "compile", "--dir", COMPAT / "res_compat", "-o", compat_res)

    generated = stage / "generated"
    generated.mkdir()
    resource_apk = stage / "resources.apk"
    run(args.aapt2, "link", "-I", args.android_jar, "--manifest", copy, "--java", generated,
        "--keep-raw-values", "--no-resource-removal", "--no-resource-deduping", "--auto-add-overlay",
        "-o", resource_apk, upstream_res, "-R", compat_res)

    package = generated.joinpath(*PACKAGE.split("."))
    flags = package / "Flags.java"
    config = package / "BuildConfig.java"
    flag_count = generate_flags(configs, flags)
    generate_build_config(build_config, config)
    write_annotations(generated)
    write_launcher_atoms(launcher, generated)
    dex = compile_dex(args, stage, min_api,
                      [*java, *external, *bridges, *sorted(generated.rglob("*.java"))])
    with zipfile.ZipFile(resource_apk, "a") as apk:
        apk.writestr(zip_entry("classes.dex"), dex)
        missing = set(REQUIRED_APK_ENTRIES).difference(apk.namelist())
        if missing:
            raise ValueError("Missing from Launcher3.apk: " + ", ".join(sorted(missing)))

    aligned = stage / "aligned.apk"
    apk_path = stage / ARTIFACTS[0]
    run(args.zipalign, "-P", "16", "4", resource_apk, aligned)
    run(args.apksigner, "sign", "--key", debug_key, "--cert", debug_cert,
        "--v1-signing-enabled", "false", "--v2-signing-enabled", "true",
        "--v3-signing-enabled", "false", "--v4-signing-enabled", "false",
        "--out", apk_path, aligned)
    run(args.zipalign, "-c", "-P", "16", "4", apk_path)
    run(args.apksigner, "verify", apk_path)

    report = {
        "package": PACKAGE,
        "upstream_tag": "android-17.0.0_r1",
        "upstream_launcher_java": provenance(java, launcher),
        "upstream_launcher_xml": provenance(
            [manifest, *(path for path in resource_files if path.suffix == ".xml")], launcher),
        "upstream_launcher_other_resources": provenance(
            [path for path in resource_files if path.suffix != ".xml"], launcher),
        "upstream_launcher_aconfig": provenance(configs, launcher),
        "upstream_build_config_generator": provenance([build_config], launcher),
        "upstream_launcher_logging_schemas": provenance(atom_schemas, launcher),
        "other_upstream_java": provenance(external, args.aosp_root),
        "manifest_substitutions": {"${applicationId}": PACKAGE},
        "local_java": provenance(bridges, COMPAT),
        "local_resource_overlays": provenance(compat_resources, COMPAT),
        "generated_flags": flag_count,
        "apk": {"bytes": apk_path.stat().st_size, "sha256": digest(apk_path.read_bytes())},
        "dex": {"bytes": len(dex), "sha256": digest(dex)},
    }
    (stage / ARTIFACTS[1]).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                                     encoding="utf-8")
    sdk_jars = [args.d8.parent / "lib/d8.jar", args.apksigner.parent / "lib/apksigner.jar"]
    return [
        Path(__file__).resolve(), COMPAT / "upstream_java.txt", COMPAT / "sdk_sources.py",
        manifest, build_config, *atom_schemas, *resource_files, *java, *external, *configs,
        *compat_resources, *bridges,
        debug_key, debug_cert, args.aapt2, args.javac, args.d8, args.zipalign, args.apksigner,
        args.android_jar, *(path for path in sdk_jars if path.is_file()),
    ]


def write_depfile(path, target, inputs):
    def escape(item):
        return (str(item).replace("\\", "\\\\").replace("$", "$$")
                .replace(" ", "\\ ").replace("#", "\\#").replace(":", "\\:"))

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(escape(target) + ": " + " ".join(map(escape, sorted(set(inputs)))) + "\n",
                    encoding="utf-8")


def main():
    sdk = Path(os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
               or Path.home() / ("Library/Android/sdk" if sys.platform == "darwin" else "Android/Sdk"))
    tools = sdk / "build-tools/36.0.0"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aosp-root", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--aapt2", type=Path, default=tools / "aapt2")
    parser.add_argument("--javac", type=Path, default=shutil.which("javac"))
    parser.add_argument("--d8", type=Path, default=tools / "d8")
    parser.add_argument("--zipalign", type=Path, default=tools / "zipalign")
    parser.add_argument("--apksigner", type=Path, default=tools / "apksigner")
    parser.add_argument("--android-jar", type=Path, default=sdk / "platforms/android-36/android.jar")
    parser.add_argument("--depfile", type=Path)
    parser.add_argument("--check", action="store_true", help="Rebuild and compare the APK and report")
    args = parser.parse_args()
    args.aosp_root = args.aosp_root.resolve()
    args.out_dir = args.out_dir.resolve()
    if args.out_dir.is_relative_to(args.aosp_root) or (
            args.depfile and args.depfile.resolve().is_relative_to(args.aosp_root)):
        parser.error("Outputs must be outside the AOSP source tree")
    if args.check and args.depfile:
        parser.error("--check does not write a depfile")

    for arg in ("aapt2", "javac", "d8", "zipalign", "apksigner", "android_jar"):
        path = getattr(args, arg)
        tool = (Path(resolved) if path is not None and (resolved := shutil.which(str(path)))
                else path)
        if tool is None or not tool.is_file():
            parser.error(f"{arg.replace('_', '-')} was not found at {tool}")
        setattr(args, arg, tool.absolute())

    try:
        if args.check:
            for name in ARTIFACTS:
                if not (args.out_dir / name).is_file():
                    raise ValueError(f"Missing {args.out_dir / name}")
        else:
            args.out_dir.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".launcher3-", dir=args.out_dir) as tmp:
            stage = Path(tmp)
            inputs = build(args, stage)
            if args.check:
                for name in ARTIFACTS:
                    existing = args.out_dir / name
                    if not existing.is_file() or existing.read_bytes() != (stage / name).read_bytes():
                        raise ValueError(f"Rebuild differs from {existing}")
                print(f"Byte-for-byte reproduction of {args.out_dir / ARTIFACTS[0]} and report")
            else:
                for name in ARTIFACTS:
                    (stage / name).replace(args.out_dir / name)
                if args.depfile:
                    write_depfile(args.depfile, args.out_dir / ARTIFACTS[0], inputs)
                print(f"Built {args.out_dir / ARTIFACTS[0]}")
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Launcher3 build failed: {error}\n")


if __name__ == "__main__":
    main()
