#!/usr/bin/env python3
"""Build the AOSP Launcher3 (no Quickstep) APK from offline inputs."""

import argparse
import collections
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


ROOT = Path(__file__).resolve().parents[2]
PATCH_DIR = ROOT / "patches/aosp17/launcher3"
TAG = "android-17.0.0_r1"
PACKAGE = "com.android.launcher3"
LAUNCHER = Path("packages/apps/Launcher3")
SYSTEMUI = Path("frameworks/libs/systemui")
PLUGIN = Path("frameworks/base/packages/SystemUI")
LAUNCHER_SOURCES = (
    "src", "src_no_quickstep", "src_plugins", "shared/src", "dagger/src",
    "modules/concurrent/src", "modules/widgetpicker/src", "modules/appfunctions/src",
)
SYSTEMUI_MODULES = (
    "iconloaderlib", "viewcapturelib", "animationlib", "contextualeducationlib",
    "mechanics", "msdllib", "usertypelib", "cuebarlib", "dynamiccolors",
)
PLUGIN_SOURCES = ("plugin_core/src", "plugin_core/annotations/src", "log/core/src")
# Framework-internal tracing implementation and its generated Perfetto dependency
# are unavailable to the public SDK. ViewCaptureFactory uses upstream NoOpViewCapture.
PLATFORM_TRACING = {
    SYSTEMUI / "viewcapturelib/src/com/android/app/viewcapture" / name
    for name in ("PerfettoViewCapture.kt", "ViewCaptureDataSource.java")
}
CORE_CLASSES = (
    "Launcher", "Workspace", "CellLayout", "Hotseat", "BubbleTextView",
    "DeviceProfile", "InvariantDeviceProfile", "dragndrop.DragLayer",
    "LauncherRootView", "allapps.ActivityAllAppsContainerView", "PagedView",
    "pageindicators.PageIndicatorDots", "LauncherModel", "LauncherAppState", "folder.Folder",
)
ANDROID = "{http://schemas.android.com/apk/res/android}"
TOOLS = "{http://schemas.android.com/tools}"

# Android publications of the multiplatform libraries. The deps directory also
# contains their non-Android variants, which must not enter the APK.
CACHED_AARS = {
    "androidx.activity": ("1.10.1", "activity", "activity-compose", "activity-ktx"),
    "androidx.core": ("1.16.0", "core", "core-ktx"),
    "androidx.compose.animation": ("1.8.3", "animation-android", "animation-core-android"),
    "androidx.compose.foundation": ("1.8.3", "foundation-android", "foundation-layout-android"),
    "androidx.compose.material": ("1.8.3", "material-android", "material-ripple-android"),
    "androidx.compose.material3": ("1.3.2", "material3-android"),
    "androidx.compose.runtime": ("1.8.3", "runtime-android", "runtime-saveable-android"),
    "androidx.compose.ui": (
        "1.8.3", "ui-android", "ui-geometry-android", "ui-graphics-android", "ui-text-android",
        "ui-tooling-android", "ui-tooling-data-android", "ui-tooling-preview-android",
        "ui-unit-android", "ui-util-android",
    ),
    "androidx.lifecycle": (
        "2.9.2", "lifecycle-runtime-android", "lifecycle-runtime-compose-android",
        "lifecycle-viewmodel-android", "lifecycle-viewmodel-compose-android",
        "lifecycle-viewmodel-savedstate-android",
    ),
    "androidx.navigation": (
        "2.9.3", "navigation-common-android", "navigation-compose-android", "navigation-runtime-android",
    ),
    "androidx.savedstate": ("1.3.1", "savedstate-android", "savedstate-compose-android"),
}
SUPERSEDED_AARS = {
    "activity-1.8.1", "activity-ktx-1.8.1", "core-1.0.0", "core-1.15.0", "core-ktx-1.15.0",
    "lifecycle-runtime-2.8.7", "lifecycle-runtime-ktx-release", "lifecycle-viewmodel-2.8.7",
    "lifecycle-viewmodel-ktx-2.8.7", "lifecycle-viewmodel-savedstate-2.8.7",
    "savedstate-1.2.1", "savedstate-ktx-1.2.1",
}


def run(*command, **kwargs):
    return subprocess.run([str(arg) for arg in command], check=True, **kwargs)


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def classpath(paths):
    return os.pathsep.join(map(str, paths))


def argfile(path, arguments):
    with path.open("w", encoding="utf-8") as stream:
        for argument in arguments:
            escaped = str(argument).replace("\\", "\\\\").replace('"', '\\"')
            stream.write(f'"{escaped}"\n')
    return f"@{path}"


def protoc_environment(protoc):
    env = os.environ.copy()
    if sys.platform != "darwin":
        return env
    executable = protoc.resolve()
    cellar = next((parent for parent in executable.parents if parent.name == "Cellar"), None)
    if cellar is None:
        return env
    # An updated Homebrew opt symlink can point away from protoc's installed ABI.
    linked = run("otool", "-L", executable, capture_output=True, text=True).stdout
    libraries = [Path(line.strip().split(" (", 1)[0]) for line in linked.splitlines()[1:]]
    missing = [path for path in libraries if path.name.startswith("libabsl_") and not path.exists()]
    if missing:
        for directory in sorted((cellar / "abseil").glob("*/lib")):
            if all((directory / path.name).is_file() for path in missing):
                inherited = env.get("DYLD_LIBRARY_PATH")
                env["DYLD_LIBRARY_PATH"] = classpath([directory, inherited] if inherited else [directory])
                break
    return env


def release_id(path):
    env = dict(os.environ, GIT_OPTIONAL_LOCKS="0")
    commit = run("git", "-C", path, "rev-parse", "HEAD", capture_output=True, text=True, env=env)
    tag = run("git", "-C", path, "rev-parse", f"refs/tags/{TAG}^{{commit}}",
              capture_output=True, text=True, env=env)
    if commit.stdout != tag.stdout:
        raise ValueError(f"{path} is not checked out at {TAG}")
    run("git", "-C", path, "diff", "--quiet", TAG, "--", env=env)
    return commit.stdout.strip()


def stage_sources(aosp, copy):
    roots = [LAUNCHER / path for path in LAUNCHER_SOURCES]
    roots += [SYSTEMUI / module / "src" for module in SYSTEMUI_MODULES if module != "dynamiccolors"]
    roots += [PLUGIN / path for path in PLUGIN_SOURCES]
    roots += [LAUNCHER / "res", LAUNCHER / "modules/widgetpicker/res"]
    roots += [SYSTEMUI / module / "res" for module in SYSTEMUI_MODULES
              if (aosp / SYSTEMUI / module / "res").is_dir()]
    files = [LAUNCHER / name for name in ("AndroidManifest.xml", "AndroidManifest-common.xml")]
    for root in roots:
        if not (aosp / root).is_dir():
            raise ValueError(f"Missing upstream input directory {root}")
        files.extend(path.relative_to(aosp) for path in (aosp / root).rglob("*") if path.is_file())
    originals = {}
    for relative in sorted(set(files)):
        source = aosp / relative
        target = copy / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        originals[relative] = sha256(target)
    return originals


def apply_patches(copy, originals):
    report = []
    # An in-worktree build must apply relative to the staged tree. Git otherwise
    # discovers the parent worktree and silently skips patches outside its prefix.
    env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(copy.parent.resolve()))
    for name in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE"):
        env.pop(name, None)
    for path in sorted(PATCH_DIR.glob("*.patch")):
        patch = path.read_text(encoding="utf-8").splitlines()
        if not patch or any(line.startswith(("new file mode", "deleted file mode")) for line in patch):
            raise ValueError(f"Expected a modification to copied upstream files in {path}")
        for line in patch:
            if line.startswith(("--- a/", "+++ b/")):
                relative = Path(line[6:].split("\t", 1)[0])
                if relative not in originals:
                    raise ValueError(f"{path} patches an uncopied path: {relative}")
        stats = run("git", "apply", "--numstat", path, cwd=copy,
                    capture_output=True, text=True, env=env).stdout
        if not stats.strip():
            raise ValueError(f"No copied files matched {path}")
        added = removed = 0
        for line in stats.splitlines():
            plus, minus, edited = line.split("\t")
            if Path(edited) not in originals:
                raise ValueError(f"{path} patches an uncopied path: {edited}")
            added += int(plus)
            removed += int(minus)
        run("git", "apply", "--check", path, cwd=copy, env=env)
        run("git", "apply", path, cwd=copy, env=env)
        report.append({"path": path.name, "added": added, "removed": removed})
    return report


def cached(args, group, artifact, version, extension):
    directory = args.gradle_cache / group / artifact / version
    matches = sorted(directory.glob(f"*/*.{extension}"))
    matches = [path for path in matches if not path.name.endswith(("-sources.jar", "-javadoc.jar"))]
    if len(matches) != 1:
        raise ValueError(f"Expected one cached {extension} for {group}:{artifact}:{version} in {directory}")
    return matches[0]


def dependencies(args):
    jars = sorted(path for path in (args.deps / "jars").glob("*.jar")
                  if not path.name.endswith("-sources.jar")
                  and not re.search(r"(-desktop-|-jvmstubs-|^listenablefuture-|^lifecycle-common-)", path.name))
    jars += [args.deps / "dagger_ap" / name for name in
             ("guava-33.0.0-jre.jar", "failureaccess-1.0.2.jar", "jsr305-3.0.2.jar")]
    jars += [cached(args, group, name, version, "jar") for group, name, version in (
        ("org.jetbrains.kotlin", "kotlin-parcelize-runtime", "2.1.21"),
        ("androidx.lifecycle", "lifecycle-common-jvm", "2.9.2"),
        ("org.jetbrains.kotlinx", "kotlinx-serialization-core-jvm", "1.7.3"),
    )]
    aars = sorted(path for path in (args.deps / "aars").glob("*.aar") if path.stem not in SUPERSEDED_AARS)
    for group, (version, *artifacts) in CACHED_AARS.items():
        aars.extend(cached(args, group, artifact, version, "aar") for artifact in artifacts)
    aars += [cached(args, group, name, version, "aar") for group, name, version in (
        ("androidx.compose.material", "material-icons-core-android", "1.7.8"),
        ("androidx.compose.material", "material-icons-extended-android", "1.7.8"),
        ("androidx.core", "core-viewtree", "1.0.0"),
        ("androidx.graphics", "graphics-path", "1.0.1"),
        ("androidx.window.extensions.core", "core", "1.0.0"),
    )]
    plugins = [cached(args, "org.jetbrains.kotlin", name, "2.1.21", "jar") for name in
               ("kotlin-compose-compiler-plugin-embeddable", "kotlin-parcelize-compiler")]
    return jars, aars, plugins


def extract_aars(aars, directory):
    modules = []
    for index, aar in enumerate(aars):
        target = directory / f"{index:02}-{aar.stem}"
        with zipfile.ZipFile(aar) as archive:
            for name in archive.namelist():
                if name in {"AndroidManifest.xml", "R.txt", "classes.jar"} or name.startswith(
                    ("res/", "jni/", "assets/", "libs/")
                ):
                    archive.extract(name, target)
        modules.append(target)
    return modules


def flag_sources(args, generated):
    paths = sorted((args.aosp_root / LAUNCHER / "aconfig").glob("*.aconfig"))
    paths += sorted((args.aosp_root / SYSTEMUI / "aconfig").glob("*.aconfig"))
    paths += [args.aosp_root / "frameworks/base/core/java" / name
              for name in ("android/content/pm/multiuser.aconfig", "android/os/flags.aconfig")]
    flags = collections.defaultdict(dict)
    for path in paths:
        source = path.read_text(encoding="utf-8")
        package = re.search(r'^package:\s*"([^"]+)"', source, re.M)[1]
        for block in re.split(r"(?m)^[ \t]*flag[ \t]*\{", source)[1:]:
            name = re.search(r'^\s*name:\s*"([a-z0-9_]+)"', block, re.M)[1]
            if name in flags[package]:
                raise ValueError(f"Duplicate flag {package}.{name}")
            # The sparse checkout has declarations with no product release value overlays.
            flags[package][name] = False
    values = {}
    for package, declarations in sorted(flags.items()):
        lines = [f"package {package};", "", "public final class Flags {", "    private Flags() {}"]
        for name, enabled in sorted(declarations.items()):
            key = f"{package}.{name}"
            values[key] = str(enabled).lower()
            first, *words = name.split("_")
            method = first + "".join(word.capitalize() for word in words)
            lines.append(f'    public static final String FLAG_{name.upper()} = "{key}";')
            lines.append(f"    public static boolean {method}() {{ return {values[key]}; }}")
        lines.append("}")
        output = generated / Path(*package.split(".")) / "Flags.java"
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return values, paths


def generate_sources(args, generated):
    flags, inputs = flag_sources(args, generated)
    launcher = args.aosp_root / LAUNCHER
    config = launcher / "tools/buildconfig.sh"
    # The upstream script uses associative arrays, which macOS's Bash 3 lacks.
    shell = "zsh" if sys.platform == "darwin" else "bash"
    with (generated / "com/android/launcher3/BuildConfig.java").open("wb") as stream:
        run(shell, config, stdout=stream)
    protos = sorted((launcher / "protos").glob("*.proto"))
    protos += sorted((launcher / "protos_overrides").glob("*.proto"))
    run(args.protoc, "-I", launcher / "protos", "-I", launcher / "protos_overrides",
        f"--java_out=lite:{generated}", *protos, env=protoc_environment(args.protoc))
    return flags, [*inputs, config, *protos]


def merge_manifest(base, overlay):
    for name, value in overlay.attrib.items():
        if not name.startswith(TOOLS):
            base.set(name, value)
    for child in overlay:
        if child.tag == "uses-sdk":
            continue
        name = child.get(ANDROID + "name")
        candidates = [node for node in base.findall(child.tag)
                      if child.tag in {"application", "queries"} or node.get(ANDROID + "name") == name]
        if len(candidates) == 1 and (name or child.tag in {"application", "queries"}):
            merge_manifest(candidates[0], child)
        else:
            base.append(child)


def make_manifest(copy, modules, output):
    ET.register_namespace("android", ANDROID[1:-1])
    manifest = ET.Element("manifest")
    launcher = copy / LAUNCHER
    for path in [*(module / "AndroidManifest.xml" for module in modules),
                 launcher / "AndroidManifest-common.xml", launcher / "AndroidManifest.xml"]:
        overlay = ET.parse(path).getroot()
        merge_manifest(manifest, overlay)
    manifest.set("package", PACKAGE)
    # Mirror Soong's source-manifest SDK version override for this toolchain.
    sdk = ET.Element("uses-sdk", {ANDROID + "minSdkVersion": "31", ANDROID + "targetSdkVersion": "36"})
    manifest.insert(0, sdk)
    for node in manifest.iter():
        for key, value in list(node.attrib.items()):
            if key.startswith(TOOLS):
                del node.attrib[key]
            else:
                node.set(key, value.replace("$" + "{applicationId}", PACKAGE)
                         .replace("$" + "{packageName}", PACKAGE))
    ET.indent(manifest)
    ET.ElementTree(manifest).write(output, encoding="utf-8", xml_declaration=True)


def read_symbols(path):
    symbols = {}
    if path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            kind, category, name, value = line.split(None, 3)
            symbols[(category, name)] = (kind, value)
    return symbols


def library_resources(modules, symbol_file, generated):
    linked = read_symbols(symbol_file)
    packages = collections.defaultdict(dict)
    for module in modules:
        package = ET.parse(module / "AndroidManifest.xml").getroot().get("package")
        packages[package].update(read_symbols(module / "R.txt"))
    for package, symbols in sorted(packages.items()):
        if not symbols or package == PACKAGE:
            continue
        members = collections.defaultdict(list)
        for (category, name), (kind, _) in sorted(symbols.items()):
            if (category, name) not in linked:
                # Older AARs also list transitive symbols from superseded libraries.
                continue
            actual_kind, value = linked[category, name]
            if kind != actual_kind:
                raise ValueError(f"Resource type mismatch for {package}.R.{category}.{name}")
            members[category].append(f"        public static final {kind} {name} = {value};")
        output = generated / Path(*package.split(".")) / "R.java"
        output.parent.mkdir(parents=True, exist_ok=True)
        lines = [f"package {package};", "", "public final class R {"]
        for category, fields in sorted(members.items()):
            lines.extend([f"    public static final class {category} {{", *fields, "    }"])
        output.write_text("\n".join([*lines, "}"]) + "\n", encoding="utf-8")


def link_resources(args, stage, copy, modules, generated, flags):
    manifest = stage / "AndroidManifest.xml"
    make_manifest(copy, modules, manifest)
    resources = [module / "res" for module in modules]
    resources += [copy / SYSTEMUI / module / "res" for module in SYSTEMUI_MODULES]
    resources += [copy / LAUNCHER / path for path in ("modules/widgetpicker/res", "res")]
    overlays = []
    for directory in resources:
        if directory.is_dir():
            archive = stage / f"res-{len(overlays):03}.zip"
            run(args.build_tools / "aapt2", "compile", "--dir", directory, "-o", archive)
            overlays.append(archive)
    package = stage / "resources.apk"
    symbols = stage / "R.txt"
    resource_packages = [
        "com.android.app.animation", "com.android.launcher3.icons",
        "com.android.launcher3.widgetpicker", "com.android.systemui.dynamiccolors",
    ]
    run(args.build_tools / "aapt2", "link", "-I", args.android_jar, "--manifest", manifest,
        "--java", generated, "--output-text-symbols", symbols, "--auto-add-overlay",
        "--no-resource-removal",
        "--extra-packages", ":".join(resource_packages),
        "--feature-flags", ",".join(f"{key}={value}" for key, value in sorted(flags.items())),
        "-o", package, *(item for archive in overlays for item in ("-R", archive)))
    library_resources(modules, symbols, generated)
    return package


def compile_sources(args, stage, sources, jars, plugins, external):
    kotlin = [path for path in sources if path.suffix == ".kt"]
    java = [path for path in sources if path.suffix == ".java"]
    compiler = sorted((args.deps / "kotlinc").glob("*.jar"))
    processors = sorted((args.deps / "dagger_ap").glob("*.jar"))
    classes = stage / "classes"
    dagger = stage / "dagger-generated"
    classes.mkdir()
    stub_classes = stage / "stub-classes"
    stub_classes.mkdir()
    if external:
        run(args.javac, "--release", "17", "-classpath", classpath([args.android_jar, *jars]),
            "-d", stub_classes, *external)
    compile_jars = [args.android_jar, stub_classes, *jars]
    compiler_args = argfile(stage / "kotlinc.args", [
        "-no-stdlib", "-no-reflect", "-jvm-target", "17", "-Xjvm-default=all",
        "-classpath", classpath(compile_jars),
        *(f"-Xplugin={path}" for path in plugins), "-d", classes, *kotlin, *java,
    ])
    run(args.java, "-Xmx4g", "-cp", classpath(compiler),
        "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler", compiler_args)
    # javac accepts class names as well as source files for annotation processing.
    # Pass the Kotlin roots so Dagger sees their real constructors and modules.
    kotlin_types = [
        ".".join(path.relative_to(classes).with_suffix("").parts)
        for path in sorted(classes.rglob("*.class")) if "$" not in path.stem
    ]
    java_args = argfile(stage / "javac.args", [
        "--release", "17", "-Xmaxerrs", "500", "-encoding", "UTF-8",
        "-classpath", classpath([classes, *compile_jars]),
        "-processorpath", classpath(processors), "-processor", "dagger.internal.codegen.ComponentProcessor",
        "-s", dagger, "-d", classes, *java, *kotlin_types,
    ])
    run(args.javac, java_args)
    for generated_class in (
        "dagger/DaggerLauncherAppComponent",
        "preview/DaggerPreviewContext_PreviewAppComponent",
    ):
        if not (classes / "com/android/launcher3" / f"{generated_class}.class").is_file():
            raise ValueError(f"Dagger did not generate and compile {generated_class}")
    return classes, sorted(dagger.rglob("*.java")), [*compiler, *processors]


def package_apk(args, stage, resources, classes, jars, aar_modules):
    dex = stage / "dex"
    dex.mkdir()
    class_jar = stage / "launcher.jar"
    with zipfile.ZipFile(class_jar, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(classes.rglob("*.class")):
            archive.write(path, path.relative_to(classes).as_posix())
    run(args.build_tools / "d8", "--min-api", "30", "--lib", args.android_jar,
        "--output", dex, class_jar, *jars)
    resource_apk = stage / "unsigned.apk"
    dex_files = sorted(dex.glob("classes*.dex"))
    if not dex_files:
        raise ValueError("D8 emitted no DEX files")
    with zipfile.ZipFile(resources) as compiled, zipfile.ZipFile(resource_apk, "w") as archive:
        for info in compiled.infolist():
            archive.writestr(info, compiled.read(info))
        for path in dex_files:
            archive.writestr(zipfile.ZipInfo(path.name), path.read_bytes())
        for module in aar_modules:
            for directory in ("assets", "jni"):
                for path in sorted((module / directory).rglob("*")):
                    if path.is_file():
                        suffix = path.relative_to(module / directory).as_posix()
                        name = f'{"lib" if directory == "jni" else "assets"}/{suffix}'
                        if name in archive.namelist():
                            raise ValueError(f"Duplicate APK entry {name} from {module}")
                        archive.writestr(zipfile.ZipInfo(name), path.read_bytes())
    aligned = stage / "aligned.apk"
    apk = stage / "Launcher3-upstream.apk"
    signing = ROOT / "tools/aosp/launcher3_build/signing"
    key = signing / "offline-test.pk8"
    cert = signing / "offline-test.x509.pem"
    run(args.build_tools / "zipalign", "-f", "-P", "16", "4", resource_apk, aligned)
    run(args.build_tools / "apksigner", "sign", "--key", key, "--cert", cert,
        "--v1-signing-enabled", "false", "--v2-signing-enabled", "true",
        "--v3-signing-enabled", "false", "--v4-signing-enabled", "false", "--out", apk, aligned)
    run(args.build_tools / "zipalign", "-c", "-P", "16", "4", apk)
    run(args.build_tools / "apksigner", "verify", apk)
    return apk, dex_files, [key, cert]


def source_report(args, copy, originals, sources, classes):
    selected = set(sources)
    records = {}
    counts = {suffix: {"verbatim": 0, "patched": 0} for suffix in ("java", "kt")}
    for file, before in originals.items():
        if sha256(args.aosp_root / file) != before:
            raise ValueError(f"Upstream input changed during the build: {file}")
        if copy / file not in selected or not file.is_relative_to(LAUNCHER):
            continue
        after = sha256(copy / file)
        records[file.relative_to(LAUNCHER).as_posix()] = {
            "upstream_sha256": before, "compiled_source_sha256": after,
        }
        counts[file.suffix[1:]]["verbatim" if before == after else "patched"] += 1
    core = {}
    for name in CORE_CLASSES:
        path = PACKAGE.replace(".", "/") + "/" + name.replace(".", "/")
        matches = [f"src/{path}.{suffix}" for suffix in ("java", "kt") if f"src/{path}.{suffix}" in records]
        bytecode = classes / f"{path}.class"
        if len(matches) != 1 or not bytecode.is_file():
            raise ValueError(f"Missing upstream source or compiled class for {name}")
        core[f"{PACKAGE}.{name}"] = {"source": matches[0], "class_sha256": sha256(bytecode)}
    return counts, records, core


def write_depfile(path, target, inputs):
    def escape(item):
        return str(item).replace("$", "$$").replace("#", r"\#").replace(" ", r"\ ")

    path.write_text(f"{escape(target)}: " + " ".join(map(escape, sorted(set(inputs)))) + "\n",
                    encoding="utf-8")


def build(args, stage):
    print("Staging tagged AOSP sources and applying patches", flush=True)
    releases = {path.as_posix(): release_id(args.aosp_root / path)
                for path in (LAUNCHER, SYSTEMUI, Path("frameworks/base"))}
    copy = stage / "aosp"
    originals = stage_sources(args.aosp_root, copy)
    patches = apply_patches(copy, originals)
    generated = stage / "generated"
    generated.mkdir()
    flags, tools_inputs = generate_sources(args, generated)

    print("Extracting Android AARs and linking resources", flush=True)
    jars, aars, plugins = dependencies(args)
    modules = extract_aars(aars, stage / "aars")
    dependencies_jars = jars + [path for module in modules for path in sorted(module.rglob("*.jar"))]
    resources = link_resources(args, stage, copy, modules, generated, {
        **flags, "android.security.app_lock_apis": "false",
    })

    print("Compiling upstream Kotlin and Java with Dagger", flush=True)
    sources = sorted(copy / path for path in originals
                     if path.suffix in {".java", ".kt"} and path not in PLATFORM_TRACING)
    external = sorted((PATCH_DIR / "external_stubs").rglob("*.java"))
    classes, dagger, compiler_inputs = compile_sources(args, stage, [
        *sources, *sorted(generated.rglob("*.java")),
    ], dependencies_jars, plugins, external)
    counts, records, core = source_report(args, copy, originals, sources, classes)

    print("Dexing, zipaligning and signing", flush=True)
    apk, dex_files, signing_inputs = package_apk(args, stage, resources, classes, dependencies_jars, modules)
    report = {
        "tag": TAG, "releases": releases, "launcher_source_counts": counts,
        "launcher_sources": records, "core_classes": core, "patches": patches,
        "patch_lines": {key: sum(item[key] for item in patches) for key in ("added", "removed")},
        "external_stubs": [path.relative_to(PATCH_DIR).as_posix() for path in external],
        "excluded_platform_sources": sorted(path.as_posix() for path in PLATFORM_TRACING),
        "dagger_generated_sources": len(dagger),
        "dependency_archives": [{"name": path.name, "sha256": sha256(path)}
                                for path in [*jars, *aars, *plugins]],
        "dex": [{"name": path.name, "bytes": path.stat().st_size, "sha256": sha256(path)}
                for path in dex_files],
        "apk": {"bytes": apk.stat().st_size, "sha256": sha256(apk)},
    }
    provenance = stage / "upstream_provenance.json"
    provenance.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(apk, args.out_dir / apk.name)
    os.replace(provenance, args.out_dir / provenance.name)
    if args.depfile:
        write_depfile(args.depfile, args.out_dir / apk.name, [
            Path(__file__).resolve(), *PATCH_DIR.glob("*.patch"), *external,
            *(args.aosp_root / path for path in originals), *tools_inputs, *jars, *aars, *plugins,
            *compiler_inputs, *signing_inputs, args.android_jar, args.protoc, args.java, args.javac,
            *(args.build_tools / name for name in ("aapt2", "d8", "zipalign", "apksigner")),
        ])
    print(f"Built {args.out_dir / apk.name}", flush=True)


def main():
    sdk = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
    sdk_default = Path.home() / ("Library/Android/sdk" if sys.platform == "darwin" else "Android/Sdk")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aosp-root", type=Path, default=ROOT / "third_party/aosp17")
    parser.add_argument("--deps", type=Path, default=ROOT / "third_party/aosp17_deps")
    parser.add_argument("--gradle-cache", type=Path, default=Path.home() / ".gradle/caches/modules-2/files-2.1")
    parser.add_argument("--sdk", type=Path, default=Path(sdk) if sdk else sdk_default)
    parser.add_argument("--protoc", type=Path, default=shutil.which("protoc"))
    parser.add_argument("--java", type=Path, default=shutil.which("java"))
    parser.add_argument("--javac", type=Path, default=shutil.which("javac"))
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--depfile", type=Path)
    args = parser.parse_args()
    args.build_tools = args.sdk / "build-tools/36.0.0"
    args.android_jar = args.sdk / "platforms/android-36/android.jar"
    args.out_dir = args.out_dir.resolve()
    for tool in ("java", "javac", "protoc"):
        if getattr(args, tool) is None:
            parser.error(f"{tool} is unavailable. Set --{tool} to the executable.")
    inputs = (args.aosp_root.resolve(), args.deps.resolve(), args.gradle_cache.resolve())
    outputs = [args.out_dir, args.depfile.resolve()] if args.depfile else [args.out_dir]
    if any(path.is_relative_to(source) for path in outputs for source in inputs):
        parser.error("Choose output paths outside the read-only source and dependency directories.")
    try:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".launcher3-", dir=args.out_dir) as stage:
            build(args, Path(stage))
    except subprocess.CalledProcessError as error:
        parser.exit(1, f"Launcher3 build failed: {Path(error.cmd[0]).name} exited with {error.returncode}\n")
    except (OSError, ValueError) as error:
        parser.exit(1, f"Launcher3 build failed: {error}\n")


if __name__ == "__main__":
    main()
