#!/usr/bin/env python3
"""Compile and sign the tagged AOSP built-in apps using the offline SDK."""

import argparse
import collections
from dataclasses import dataclass
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

from build_upstream_launcher3 import (
    ROOT, TAG, argfile, cached, classpath, extract_aars, library_resources, release_id, run, sha256,
)


@dataclass(frozen=True)
class App:
    package: str
    activity: str
    sources: tuple[str, ...]
    resources: str = "res"
    min_api: int = 26
    target_api: int = 30
    dependencies: tuple[str, ...] = ()
    gradle_dependencies: tuple[tuple[str, str, str, str], ...] = ()


ANDROIDX = (
    "annotation-jvm-1.9.1.jar", "annotation-experimental-1.4.1.aar",
    "collection-jvm-1.4.4.jar", "core-common-2.2.0.jar", "core-runtime-2.2.0.aar",
    "customview-1.1.0.aar", "lifecycle-livedata-core-2.8.7.aar", "loader-1.0.0.aar",
    "viewpager-1.0.0.aar", "versionedparcelable-1.1.1.aar", "listenablefuture-1.0.jar",
    "kotlin-stdlib-2.1.21.jar", "kotlinx-coroutines-core-jvm-1.10.1.jar",
    "kotlinx-coroutines-android-1.10.1.jar",
)

# The Android lifecycle publications live in the same offline cache as Launcher3's.
ANDROIDX_ANDROID = (
    ("androidx.activity", "activity", "1.10.1", "aar"),
    ("androidx.core", "core", "1.16.0", "aar"),
    ("androidx.core", "core-ktx", "1.16.0", "aar"),
    ("androidx.core", "core-viewtree", "1.0.0", "aar"),
    ("androidx.fragment", "fragment", "1.8.8", "aar"),
    ("androidx.lifecycle", "lifecycle-common-jvm", "2.9.2", "jar"),
    ("androidx.lifecycle", "lifecycle-runtime-android", "2.9.2", "aar"),
    ("androidx.lifecycle", "lifecycle-viewmodel-android", "2.9.2", "aar"),
    ("androidx.lifecycle", "lifecycle-viewmodel-savedstate-android", "2.9.2", "aar"),
    ("androidx.savedstate", "savedstate-android", "1.3.1", "aar"),
    ("org.jetbrains.kotlinx", "kotlinx-serialization-core-jvm", "1.7.3", "jar"),
)

LEGACY_SUPPORT = (
    "legacy-support-core-ui-1.0.0.aar", "legacy-support-core-utils-1.0.0.aar",
    "legacy-support-v13-1.0.0.aar", "legacy-support-v4-1.0.0.aar", "media-1.7.0.aar",
    "asynclayoutinflater-1.0.0.aar", "swiperefreshlayout-1.1.0.aar", "print-1.0.0.aar",
    "documentfile-1.0.0.aar", "localbroadcastmanager-1.0.0.aar",
)

APPS = {
    "Browser2": App("org.chromium.webview_shell", "WebViewBrowserActivity", ("src",), target_api=29),
    "Music": App("com.android.music", "MusicBrowserActivity", ("src",), resources="kotlin/res"),
    "Gallery2": App(
        "com.android.gallery3d", "app.GalleryActivity", ("src", "src_pd", "gallerycommon/src"), target_api=28,
        dependencies=ANDROIDX + LEGACY_SUPPORT + (
            "xmpcore-6.1.11.jar", "isoparser-1.1.22.jar", "aspectjrt-1.8.9.jar",
        ),
        gradle_dependencies=ANDROIDX_ANDROID,
    ),
    "DeskClock": App(
        "com.android.deskclock", "DeskClock", ("src", "gen"),
        dependencies=ANDROIDX + LEGACY_SUPPORT + (
            "appcompat-1.7.0.aar", "appcompat-resources-1.7.0.aar", "cardview-1.0.0.aar",
            "constraintlayout-2.2.0.aar", "constraintlayout-core-1.1.0.jar",
            "coordinatorlayout-1.1.0.aar", "cursoradapter-1.0.0.aar",
            "drawerlayout-1.1.1.aar", "dynamicanimation-1.0.0.aar", "emoji2-1.3.0.aar",
            "emoji2-views-helper-1.3.0.aar", "gridlayout-1.0.0.aar", "interpolator-1.0.0.aar",
            "material-1.12.0.aar", "percentlayout-1.0.0.aar", "preference-1.2.1.aar",
            "recyclerview-1.4.0.aar", "startup-runtime-1.1.1.aar", "transition-1.5.0.aar",
            "vectordrawable-1.1.0.aar", "vectordrawable-animated-1.1.0.aar",
            "viewpager2-1.1.0-beta02.aar", "concurrent-futures-1.1.0.jar",
        ),
        gradle_dependencies=ANDROIDX_ANDROID,
    ),
}


def stage_app(source, stage, app):
    files = run("git", "-C", source, "ls-files", "-z", capture_output=True).stdout
    tracked = [Path(os.fsdecode(entry)) for entry in files.split(b"\0") if entry]
    source_roots = [Path(root) for root in app.sources]
    sources = [path for path in tracked if path.suffix in (".java", ".kt") and
               any(path.is_relative_to(root) for root in source_roots)]
    resources = [path for path in tracked if path.is_relative_to(app.resources)]
    assets = [path for path in tracked if path.is_relative_to("assets")]
    originals = [Path("AndroidManifest.xml"), *sources, *resources, *assets]
    for relative in originals:
        target = stage / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / relative, target)
    return sources, resources, assets, originals


def patch_app(stage, originals, patches):
    env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(stage.parent))
    for key in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE"):
        env.pop(key, None)
    applied = []
    tracked = set(originals)
    for patch in patches:
        stats = run("git", "apply", "--numstat", patch, cwd=stage, env=env,
                    capture_output=True, text=True).stdout.splitlines()
        changes = []
        for line in stats:
            added, removed, name = line.split("\t", 2)
            if Path(name) not in tracked:
                raise ValueError(f"{patch.name}: expected a patch to a tagged APK input, got {name}")
            changes.append({"file": name, "added": int(added), "removed": int(removed)})
        if not changes:
            raise ValueError(f"{patch.name}: no source changes")
        run("git", "apply", "--check", patch, cwd=stage, env=env)
        run("git", "apply", patch, cwd=stage, env=env)
        if any(not (stage / relative).is_file() for relative in originals):
            raise ValueError(f"{patch.name}: patch deleted a tagged APK input")
        applied.append({"path": patch.relative_to(ROOT).as_posix(), "sha256": sha256(patch),
                        "changes": changes})
    return applied


def offline_dependencies(args, app):
    resolved, missing = [], []
    for pattern in app.dependencies:
        folder = args.deps / ("aars" if pattern.endswith(".aar") else "jars")
        matches = sorted(folder.glob(pattern))
        if len(matches) != 1:
            missing.append(f"{folder.name}/{pattern} (found {len(matches)})")
        else:
            resolved.append(matches[0])
    if missing:
        raise ValueError("offline dependencies require exactly one archive each: " + ", ".join(missing))
    resolved.extend(cached(args, *coordinates) for coordinates in app.gradle_dependencies)
    return resolved


def link_resources(args, app, tree, work, modules):
    resources = [module / "res" for module in modules if (module / "res").is_dir()]
    resources.append(tree / app.resources)
    overlays = []
    for index, directory in enumerate(resources):
        archive = work / f"resources-{index}.zip"
        run(args.tools / "aapt2", "compile", "--legacy", "--dir", directory, "-o", archive)
        overlays.extend(("-R", archive))
    output = work / "resources.apk"
    symbols = work / "R.txt"
    generated = work / "r"
    assets = ["-A", tree / "assets"] if (tree / "assets").is_dir() else []
    run(args.tools / "aapt2", "link", "-I", args.android_jar,
        "--manifest", tree / "AndroidManifest.xml", "--java", generated,
        "--min-sdk-version", app.min_api, "--target-sdk-version", app.target_api,
        "--output-text-symbols", symbols, "--auto-add-overlay", "--no-resource-removal",
        "-o", output, *assets, *overlays)
    library_resources(modules, symbols, generated)
    return output, sorted(generated.rglob("*.java"))


def compile_dex(args, app, work, sources, jars):
    classes = work / "classes"
    classes.mkdir()
    sdk_extensions = []
    if app.package == "org.chromium.webview_shell":
        sdk_extensions.extend(args.android_jar.parent.glob("optional/android.test.*.jar"))
    if app.package == "com.android.gallery3d":
        sdk_extensions.append(args.android_jar.parent / "optional/org.apache.http.legacy.jar")
    platform = [args.android_jar, *sdk_extensions]
    kotlin = [path for path in sources if path.suffix == ".kt"]
    java = [path for path in sources if path.suffix == ".java"]
    compiler = []
    if kotlin:
        compiler = sorted((args.deps / "kotlinc").glob("*.jar"))
        command = argfile(work / "kotlinc.args", [
            "-jvm-target", "17", "-no-stdlib", "-no-reflect", "-d", classes,
            "-classpath", classpath([*platform, *jars]), *kotlin, *java,
        ])
        run(args.java, "-cp", classpath(compiler), "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler", command)
    if java:
        command = argfile(work / "javac.args", [
            "--release", "17", "-proc:none", "-encoding", "UTF-8", "-d", classes,
            "-classpath", classpath([classes, *platform, *jars]), *java,
        ])
        run(args.javac, command, cwd=work)
    main_class = classes / Path(*(app.package + "." + app.activity).split(".")).with_suffix(".class")
    if not main_class.is_file():
        raise ValueError(f"{app.activity} was not compiled")
    class_jar = work / "app.jar"
    with zipfile.ZipFile(class_jar, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(classes.rglob("*.class")):
            archive.write(path, path.relative_to(classes).as_posix())
    dex = work / "dex"
    dex.mkdir()
    libraries = [value for jar in platform for value in ("--lib", jar)]
    run(args.tools / "d8", "--release", "--min-api", app.min_api,
        *libraries, "--output", dex, class_jar, *jars)
    return sorted(dex.glob("classes*.dex")), [*compiler, *platform]


def sign_apk(args, work, name, resources, dex):
    unsigned = work / "unsigned.apk"
    aligned = work / "aligned.apk"
    apk = work / f"{name}-upstream.apk"
    with zipfile.ZipFile(resources) as original, zipfile.ZipFile(unsigned, "w") as merged:
        for info in original.infolist():
            merged.writestr(info, original.read(info))
        for path in dex:
            merged.writestr(zipfile.ZipInfo(path.name), path.read_bytes())
    signing = ROOT / "tools/aosp/launcher3_build/signing"
    key, cert = signing / "offline-test.pk8", signing / "offline-test.x509.pem"
    run(args.tools / "zipalign", "-f", "4", unsigned, aligned)
    run(args.tools / "apksigner", "sign", "--key", key, "--cert", cert,
        "--v1-signing-enabled", "true", "--v2-signing-enabled", "true",
        "--v3-signing-enabled", "false", "--v4-signing-enabled", "false", "--out", apk, aligned)
    run(args.tools / "apksigner", "verify", apk)
    run(args.tools / "zipalign", "-c", "4", apk)
    return apk, [key, cert]


def build(args, work, name):
    app = APPS[name]
    source = args.aosp_root / "packages/apps" / name
    revision = release_id(source)
    dependencies = offline_dependencies(args, app)
    tree = work / "source"
    sources, resources, assets, originals = stage_app(source, tree, app)
    patches = sorted((ROOT / "patches/aosp17" / name.lower()).glob("*.patch"))
    applied = patch_app(tree, originals, patches)
    aars = [path for path in dependencies if path.suffix == ".aar"]
    modules = extract_aars(aars, work / "aar")
    jars = [path for path in dependencies if path.suffix == ".jar"]
    jars += [path for module in modules for path in sorted(module.rglob("*.jar"))]
    resource_apk, generated = link_resources(args, app, tree, work, modules)
    dex, tool_inputs = compile_dex(args, app, work, [tree / path for path in sources] + generated, jars)
    apk, signing = sign_apk(args, work, name, resource_apk, dex)
    report = {
        "tag": TAG, "source": f"packages/apps/{name}", "commit": revision,
        "package": app.package, "activity": f"{app.package}.{app.activity}",
        "upstream_source_counts": dict(sorted(collections.Counter(path.suffix[1:] for path in sources).items())),
        "upstream_resource_files": len(resources), "upstream_asset_files": len(assets),
        "patches": applied,
        "offline_dependencies": {
            path.relative_to(args.deps).as_posix() if path.is_relative_to(args.deps) else
            "gradle-cache/" + path.relative_to(args.gradle_cache).as_posix(): sha256(path)
            for path in dependencies
        },
        "apk": apk.name, "apk_bytes": apk.stat().st_size, "apk_sha256": sha256(apk),
        "dex_files": len(dex),
        "dex_classes": sum(struct.unpack_from("<I", path.read_bytes(), 96)[0] for path in dex),
        "dex_method_ids": sum(struct.unpack_from("<I", path.read_bytes(), 88)[0] for path in dex),
    }
    inputs = [*(source / relative for relative in originals), *patches, *dependencies, *tool_inputs, *signing]
    return apk, report, inputs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    default_sdk = Path.home() / ("Library/Android/sdk" if sys.platform == "darwin" else "Android/Sdk")
    parser.add_argument("--sdk", type=Path,
                        default=os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME") or default_sdk)
    parser.add_argument("--aosp-root", type=Path, default=ROOT / "third_party/aosp17")
    parser.add_argument("--deps", type=Path, default=ROOT / "third_party/aosp17_deps")
    parser.add_argument("--gradle-cache", type=Path,
                        default=Path.home() / ".gradle/caches/modules-2/files-2.1")
    parser.add_argument("--java", default="java")
    parser.add_argument("--javac", default="javac")
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--app", choices=APPS, action="append", help="build a subset of APKs")
    parser.add_argument("--depfile", type=Path)
    args = parser.parse_args()
    args.aosp_root, args.deps = args.aosp_root.absolute(), args.deps.absolute()
    args.gradle_cache = args.gradle_cache.absolute()
    args.tools = args.sdk.resolve() / "build-tools/36.0.0"
    args.android_jar = args.sdk.resolve() / "platforms/android-36/android.jar"
    args.out_dir = args.out_dir.resolve()
    if any(path and path.resolve().is_relative_to(root.resolve())
           for path in (args.out_dir, args.depfile) for root in (args.aosp_root, args.deps)):
        parser.error("outputs must be outside the read-only AOSP source and dependency trees")
    args.out_dir.mkdir(parents=True, exist_ok=True)
    dependencies = {Path(__file__).resolve(), ROOT / "tools/aosp/build_upstream_launcher3.py",
                    *(args.tools / tool for tool in ("aapt2", "d8", "zipalign", "apksigner"))}
    reports, failures, outputs = {}, {}, []
    selected = dict.fromkeys(args.app or APPS)
    for name in selected:
        try:
            with tempfile.TemporaryDirectory(prefix=f".{name}-", dir=args.out_dir) as stage:
                apk, report, inputs = build(args, Path(stage), name)
                output = args.out_dir / apk.name
                apk.replace(output)
                outputs.append(output)
                reports[name] = report
                dependencies.update(inputs)
                print(f"{output.name}: {report['apk_bytes']} bytes, {report['dex_classes']} DEX classes", flush=True)
        except (OSError, ValueError, subprocess.CalledProcessError) as error:
            failures[name] = str(error)
            print(f"{name}: {error}", file=sys.stderr, flush=True)
    provenance = args.out_dir / "upstream_apps_provenance.json"
    temporary = provenance.with_suffix(".json.tmp")
    temporary.write_text(json.dumps({"tag": TAG, "apps": reports, "failures": failures}, indent=2, sort_keys=True) + "\n",
                         encoding="utf-8")
    temporary.replace(provenance)
    if args.depfile and len(reports) == len(selected):
        escape = lambda path: str(path).replace("$", "$$").replace("#", "\\#").replace(" ", "\\ ")
        args.depfile.write_text(
            " ".join(map(escape, [provenance, *outputs])) + ": " +
            " ".join(map(escape, sorted(dependencies))) + "\n", encoding="utf-8")
    return 0 if len(reports) == len(selected) else 1


if __name__ == "__main__":
    sys.exit(main())
