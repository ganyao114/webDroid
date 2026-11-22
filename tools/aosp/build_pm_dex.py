#!/usr/bin/env python3
"""Compile the AOSP 17 lite package parser offline with JDK 17 and Android SDK 36."""

import argparse
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import zipfile


SOURCE_TAG = "android-17.0.0_r1"
FRAMEWORK_JAVA = Path("frameworks/base/core/java")
PM = "android/content/pm"
PARSE = f"{PM}/parsing"
INTERNAL_PM = "com/android/internal/pm/pkg"
PROGRAM_SOURCES = (
    f"{PARSE}/ApkLite.java",
    f"{PARSE}/ApkLiteParseUtils.java",
    f"{PARSE}/FrameworkParsingPackageUtils.java",
    f"{PARSE}/PackageLite.java",
    f"{PARSE}/result/ParseInput.java",
    f"{PARSE}/result/ParseResult.java",
    f"{PARSE}/result/ParseTypeImpl.java",
    f"{INTERNAL_PM}/parsing/ParsingUtils.java",
    f"{PM}/SharedLibraryInfo.java",
    f"{PM}/VerifierInfo.java",
    f"{INTERNAL_PM}/component/ParsedIntentInfo.java",
    f"{INTERNAL_PM}/component/ParsedIntentInfoImpl.java",
)
LIBCORE_SOURCES = ("libcore/luni/src/main/java/libcore/util/HexEncoding.java",)
DEX, PROFILE, CLASSES, API = (
    "aosp17_pm_core.dex", "pm_boot_profile.txt", "pm_classes.jar", "pm_compile_api.jar"
)
ACC_PUBLIC, ACC_STATIC, ACC_FINAL, ACC_NATIVE = 0x0001, 0x0008, 0x0010, 0x0100


def run(*command):
    env = os.environ.copy()
    for key in ("JAVA_TOOL_OPTIONS", "_JAVA_OPTIONS", "JDK_JAVA_OPTIONS", "JDK_JAVAC_OPTIONS"):
        env.pop(key, None)
    result = subprocess.run(command, check=False, env={**env, "LC_ALL": "C", "TZ": "UTC"})
    if result.returncode:
        raise ValueError(f"{command[0]} exited with status {result.returncode}")


def write_jar(path, entries, *, compression=zipfile.ZIP_STORED):
    # Explicitly write entries in lexical order with fixed ZIP metadata.
    with zipfile.ZipFile(path, "w") as jar:
        for name, content in sorted(entries.items()):
            jar.writestr(zipfile.ZipInfo(name), content, compress_type=compression)


def sdk_declarations(data, fields=(), methods=()):
    """Keep the SDK class and append only missing field and native method declarations."""
    cp_count, = struct.unpack_from(">H", data, 8)
    utf8 = {}
    pos = 10
    slot = 1
    while slot < cp_count:
        tag = data[pos]
        pos += 1
        if tag == 1:
            length, = struct.unpack_from(">H", data, pos)
            utf8[slot] = data[pos + 2:pos + 2 + length]
            pos += 2 + length
        else:
            pos += {3: 4, 4: 4, 5: 8, 6: 8, 7: 2, 8: 2, 9: 4, 10: 4, 11: 4,
                    12: 4, 15: 3, 16: 2, 17: 4, 18: 4, 19: 2, 20: 2}[tag]
            if tag in (5, 6):  # long and double use two constant pool slots.
                slot += 1
        slot += 1
    cp_end = pos
    interface_count, = struct.unpack_from(">H", data, pos + 6)
    pos += 8 + 2 * interface_count
    new_constants = bytearray()

    def constant(tag, payload, slots=1):
        nonlocal cp_count
        index = cp_count
        cp_count += slots
        new_constants.extend(bytes([tag]) + payload)
        return index

    def string(value):
        encoded = value.encode("ascii")
        return constant(1, struct.pack(">H", len(encoded)) + encoded)

    def members(additions):
        nonlocal pos
        count, = struct.unpack_from(">H", data, pos)
        start = pos + 2
        pos = start
        present = set()
        for _ in range(count):
            _, name, descriptor, nattrs = struct.unpack_from(">4H", data, pos)
            present.add((utf8[name], utf8[descriptor]))
            pos += 8
            for _ in range(nattrs):
                _, length = struct.unpack_from(">HI", data, pos)
                pos += 6 + length
        extra = bytearray()
        for name, descriptor, access, value in additions:
            if (name.encode("ascii"), descriptor.encode("ascii")) in present:
                continue
            extra.extend(struct.pack(">4H", access, string(name), string(descriptor), value is not None))
            if value is not None:
                if descriptor == "J":
                    index = constant(5, struct.pack(">q", value), 2)
                elif descriptor == "I":
                    index = constant(3, struct.pack(">i", value))
                else:
                    index = constant(8, struct.pack(">H", string(value)))
                extra.extend(struct.pack(">HIH", string("ConstantValue"), 2, index))
            count += 1
        return struct.pack(">H", count) + data[start:pos] + extra

    class_header = data[cp_end:pos]
    field_table = members((name, desc, ACC_PUBLIC | ACC_STATIC | ACC_FINAL, value)
                          for name, desc, value in fields)
    method_table = members((name, desc, ACC_PUBLIC | ACC_NATIVE | (ACC_STATIC if static else 0), None)
                           for name, desc, static in methods)
    return (data[:8] + struct.pack(">H", cp_count) + data[10:cp_end] + new_constants
            + class_header + field_table + method_table + data[pos:])


def extract(source, pattern):
    matches = re.findall(pattern, source.read_text(encoding="utf-8"), re.MULTILINE)
    if len(matches) != 1:
        raise ValueError(f"{source}: expected exactly one declaration matching {pattern}")
    return matches[0]


def compile_api(root, sdk_jar, stage, inputs):
    """Prepare SDK member overlays and declarations for a couple of internal parser edges."""
    base = root / FRAMEWORK_JAVA

    def upstream(path):
        inputs.add(base / path)
        return base / path

    def int_fields(path, names):
        return [
            (name, "I", int(extract(upstream(path), rf"^    public static final int {name} = (-?\d+);$")))
            for name in names
        ]

    manager_fields = int_fields(f"{PM}/PackageManager.java", (
        "INSTALL_SUCCEEDED", "INSTALL_FAILED_OLDER_SDK", "INSTALL_FAILED_NEWER_SDK",
        "INSTALL_PARSE_FAILED_NOT_APK", "INSTALL_PARSE_FAILED_BAD_MANIFEST",
        "INSTALL_PARSE_FAILED_UNEXPECTED_EXCEPTION", "INSTALL_PARSE_FAILED_INCONSISTENT_CERTIFICATES",
        "INSTALL_PARSE_FAILED_BAD_PACKAGE_NAME", "INSTALL_PARSE_FAILED_MANIFEST_MALFORMED",
        "INSTALL_PARSE_FAILED_SKIPPED",
    ))
    overlay = {
        f"{PM}/PackageManager": (manager_fields, ()),
        f"{PM}/ApplicationInfo": (int_fields(f"{PM}/ApplicationInfo.java", (
            "PAGE_SIZE_APP_COMPAT_FLAG_UNDEFINED",)), ()),
        f"{PM}/PackageInfo": (int_fields(f"{PM}/PackageInfo.java", ("INSTALL_LOCATION_UNSPECIFIED",)),
                              (("composeLongVersionCode", "(II)J", True),)),
        f"{PM}/Signature": ((), (("areExactMatch", f"(L{PM}/SigningDetails;L{PM}/SigningDetails;)Z", True),)),
        "android/content/Context": (
            (("PLATFORM_COMPAT_SERVICE", "Ljava/lang/String;", "platform_compat"),), ()),
        "android/os/Build$VERSION": ((("ACTIVE_CODENAMES", "[Ljava/lang/String;", None),), ()),
        "android/os/FileUtils": ((), (("isValidExtFilename", "(Ljava/lang/String;)Z", True),)),
        "android/os/Trace": ((("TRACE_TAG_PACKAGE_MANAGER", "J", 1 << 18),), (
            ("traceBegin", "(JLjava/lang/String;)V", True), ("traceEnd", "(J)V", True))),
        "android/util/ArrayMap": ((), (("erase", "()V", False),)),
        "android/os/Parcel": ((), (
            ("readString8", "()Ljava/lang/String;", False),
            ("writeString8", "(Ljava/lang/String;)V", False),
            ("createString8Array", "()[Ljava/lang/String;", False),
            ("writeString8Array", "([Ljava/lang/String;)V", False),
            ("readCharSequence", "()Ljava/lang/CharSequence;", False),
            ("writeCharSequence", "(Ljava/lang/CharSequence;)V", False),
        )),
    }
    with zipfile.ZipFile(sdk_jar) as sdk:
        overlays = {name + ".class": sdk_declarations(sdk.read(name + ".class"), *members)
                    for name, members in overlay.items()}
    overlay_jar = stage / "sdk-hidden-members.jar"
    write_jar(overlay_jar, overlays)

    package = f"{INTERNAL_PM.replace('/', '.')}.parsing"
    prefix = f"{INTERNAL_PM}/parsing"
    declarations = {
        "ParsingPackage": ("interface", extract(upstream(f"{prefix}/ParsingPackage.java"),
                                               r"^    (String getBaseApkPath\(\);)$")),
        "ParsingPackageUtils": ("class", extract(upstream(f"{prefix}/ParsingPackageUtils.java"),
                                                 r"^    (public static final boolean RIGID_PARSER = (?:true|false);)$")),
    }
    generated = []
    for name, (kind, declaration) in declarations.items():
        path = stage / "declarations" / prefix / f"{name}.java"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"package {package};\npublic {kind} {name} {{\n    {declaration}\n}}\n",
                        encoding="utf-8")
        generated.append(path)
    return overlay_jar, overlays, generated


def uleb128(data, offset):
    result = 0
    for shift in range(0, 35, 7):
        byte = data[offset]
        offset += 1
        result |= (byte & 0x7f) << shift
        if byte < 0x80:
            return result, offset
    raise ValueError("Invalid unsigned LEB128 in D8 output")


def dex_profile(data, expected_classes, library_classes):
    """Count encoded method definitions and DEX instruction bytes per class."""
    if data[:4] != b"dex\n" or len(data) != struct.unpack_from("<I", data, 32)[0]:
        raise ValueError("D8 did not produce one standard DEX file")
    string_ids, _, type_ids = struct.unpack_from("<3I", data, 60)
    method_ids, _, class_count, class_defs = struct.unpack_from("<4I", data, 88)
    classes = {}
    for idx in range(class_count):
        type_idx, _, _, _, _, _, class_data, _ = struct.unpack_from("<8I", data, class_defs + 32 * idx)
        string_idx, = struct.unpack_from("<I", data, type_ids + 4 * type_idx)
        str_data, = struct.unpack_from("<I", data, string_ids + 4 * string_idx)
        _, str_data = uleb128(data, str_data)
        descriptor = data[str_data:data.index(0, str_data)].decode("ascii")
        methods = code_bytes = 0
        if class_data:
            counts = []
            for _ in range(4):
                count, class_data = uleb128(data, class_data)
                counts.append(count)
            fields = counts[0] + counts[1]
            methods = counts[2] + counts[3]
            for _ in range(2 * fields):
                _, class_data = uleb128(data, class_data)
            for _ in range(methods):
                _, class_data = uleb128(data, class_data)  # Method index delta.
                _, class_data = uleb128(data, class_data)  # Access flags.
                code, class_data = uleb128(data, class_data)
                if code:
                    code_units, = struct.unpack_from("<I", data, code + 12)
                    code_bytes += 2 * code_units
        classes[descriptor] = methods, code_bytes
    missing = expected_classes - classes.keys()
    if missing:
        raise ValueError(f"D8 omitted program class definitions: {', '.join(sorted(missing))}")
    leaked = library_classes & classes.keys()
    if leaked:
        raise ValueError(f"D8 packaged compile-time declarations: {', '.join(sorted(leaked))}")
    rows = [
        "aosp17_pm_boot_profile 1",
        f"source_tag {SOURCE_TAG}",
        f"dex_version {data[4:7].decode('ascii')}",
        f"dex_sha256 {hashlib.sha256(data).hexdigest()}",
        f"dex_bytes {len(data)}",
        f"class_count {class_count}",
        f"method_ids {method_ids}",
        f"defined_methods {sum(m for m, _ in classes.values())}",
        f"code_bytes {sum(n for _, n in classes.values())}",
        "# descriptor defined_methods code_bytes",
    ]
    rows.extend(f"{descriptor} {m} {n}" for descriptor, (m, n) in sorted(classes.items()))
    return "\n".join(rows) + "\n"


def build(args, stage):
    program = [args.aosp_root / FRAMEWORK_JAVA / path for path in PROGRAM_SOURCES]
    helpers = sorted(Path(__file__).with_name("pm_build").rglob("*.java"))
    libcore = [args.aosp_root / path for path in LIBCORE_SOURCES]
    inputs = {Path(__file__).resolve(), args.javac, args.d8, args.android_jar,
              *program, *helpers, *libcore}
    d8_jar = args.d8.parent / "lib/d8.jar"
    if d8_jar.is_file():
        inputs.add(d8_jar)
    for path in sorted(inputs):
        if not path.is_file():
            raise ValueError(f"Required input does not exist: {path}")
    overlay, library, declarations = compile_api(args.aosp_root, args.android_jar, stage, inputs)
    classes = stage / "classes"
    classes.mkdir()
    run(args.javac, "--release", "17", "-encoding", "UTF-8", "-proc:none", "-g:source,lines",
        "-sourcepath", "", "-classpath", os.pathsep.join(map(str, (overlay, args.android_jar))),
        "-d", classes, *sorted(program + helpers + libcore + declarations))

    top_levels = {path.removesuffix(".java") for path in PROGRAM_SOURCES}
    bytecode = {}
    for path in sorted(classes.rglob("*.class")):
        name = path.relative_to(classes).as_posix()
        outer = name.removesuffix(".class").split("$", 1)[0]
        archive = bytecode if outer in top_levels else library
        archive[name] = path.read_bytes()
    missing = {path + ".class" for path in top_levels} - bytecode.keys()
    if missing:
        raise ValueError(f"javac omitted program classes: {', '.join(sorted(missing))}")
    write_jar(stage / CLASSES, bytecode)
    write_jar(stage / API, library)
    run(args.d8, "--release", "--min-api", "36", "--lib", args.android_jar,
        "--classpath", stage / API, "--output", stage, stage / CLASSES)
    if sorted(path.name for path in stage.glob("classes*.dex")) != ["classes.dex"]:
        raise ValueError("Expected exactly one classes.dex from SDK 36 d8")
    dex = stage / "classes.dex"
    dex.rename(stage / DEX)
    descriptors = {f"L{name.removesuffix('.class')};" for name in bytecode}
    library_descriptors = {f"L{name.removesuffix('.class')};" for name in library}
    profile = dex_profile((stage / DEX).read_bytes(), descriptors, library_descriptors)
    (stage / PROFILE).write_text(profile, encoding="utf-8")
    return inputs


def write_depfile(path, output, inputs):
    def escape(item):
        return str(item).replace("$", "$$").replace("#", "\\#").replace(" ", "\\ ")

    path.write_text(f"{escape(output)}: {' '.join(escape(p) for p in sorted(inputs))}\n",
                    encoding="utf-8")


def main():
    default_sdk = (Path.home() / "Library/Android/sdk" if sys.platform == "darwin"
                   else Path.home() / "Android/Sdk")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aosp-root", required=True, type=Path, help=f"read-only AOSP {SOURCE_TAG} tree")
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--android-sdk", type=Path, default=os.environ.get("ANDROID_SDK_ROOT")
                        or os.environ.get("ANDROID_HOME") or default_sdk)
    parser.add_argument("--javac", type=Path, default=Path("/usr/bin/javac"))
    parser.add_argument("--android-jar", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--depfile", type=Path, help="write build dependencies for CMake")
    parser.add_argument("--check", action="store_true", help="rebuild and compare with existing artifacts")
    args = parser.parse_args()
    args.android_jar = args.android_jar or args.android_sdk / "platforms/android-36/android.jar"
    args.d8 = args.d8 or args.android_sdk / "build-tools/36.0.0/d8"
    for attr in ("aosp_root", "out_dir", "android_jar", "d8", "javac"):
        setattr(args, attr, getattr(args, attr).resolve())
    if args.out_dir.is_relative_to(args.aosp_root) or (
            args.depfile and args.depfile.resolve().is_relative_to(args.aosp_root)):
        parser.error("Build outputs must be outside the read-only AOSP tree")
    if args.check and args.depfile:
        parser.error("--check compares artifacts and cannot write a depfile")

    try:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".pm-build-", dir=args.out_dir) as temp:
            stage = Path(temp)
            inputs = build(args, stage)
            outputs = (DEX, PROFILE, CLASSES, API)
            if args.check:
                for name in outputs:
                    existing = args.out_dir / name
                    if not existing.is_file() or existing.read_bytes() != (stage / name).read_bytes():
                        raise ValueError(f"Reproducibility check failed for {existing}")
                print(f"Byte-for-byte reproducibility verified for {args.out_dir / DEX} and its companion artifacts")
            else:
                for name in outputs:
                    (stage / name).replace(args.out_dir / name)
                if args.depfile:
                    write_depfile(args.depfile, args.out_dir / DEX, inputs)
                print(f"Built {args.out_dir / DEX}")
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    main()
