"""Generate the SDK-only annotations and LauncherAtom value types used by the Java slice."""

from pathlib import Path
import re


ATOM_SCHEMAS = (
    Path("protos/launcher_atom.proto"),
    Path("protos_overrides/launcher_atom_extension.proto"),
)

# Compile-time annotations used by the audited SDK-only sources.
ANNOTATION_MEMBERS = {
    "FloatRange": (
        "double from() default Double.NEGATIVE_INFINITY; "
        "double to() default Double.POSITIVE_INFINITY; "
        "boolean fromInclusive() default true; boolean toInclusive() default true;"
    ),
    "IntDef": 'long[] value() default {}; boolean flag() default false; String[] prefix() default {};',
    "IntRange": "long from() default Long.MIN_VALUE; long to() default Long.MAX_VALUE;",
    "RequiresApi": "int value() default 1; int api() default 1;",
    "RequiresPermission": (
        'String value() default ""; String[] allOf() default {}; String[] anyOf() default {}; '
        "boolean conditional() default false;"
    ),
    "RestrictTo": "Scope[] value(); enum Scope { LIBRARY, LIBRARY_GROUP, LIBRARY_GROUP_PREFIX, TESTS, SUBCLASSES }",
    "StringDef": 'String[] value() default {}; String[] prefix() default {};',
    "VisibleForTesting": (
        "int PRIVATE = 2, PACKAGE_PRIVATE = 3, PROTECTED = 4, NONE = 5; "
        "int otherwise() default PRIVATE;"
    ),
}

MARKER_ANNOTATIONS = """
    AnimRes AnyRes AnyThread AttrRes BoolRes CallSuper CheckResult ColorInt ColorRes
    DimenRes Dimension DrawableRes FontRes IdRes LayoutRes MainThread NonNull Nullable
    PluralsRes Px RawRes StringRes StyleRes UiThread WorkerThread XmlRes
""".split()


def write_annotations(output):
    folder = output / "androidx/annotation"
    folder.mkdir(parents=True)
    for name, members in sorted({**dict.fromkeys(MARKER_ANNOTATIONS, ""), **ANNOTATION_MEMBERS}.items()):
        (folder / f"{name}.java").write_text(
            "package androidx.annotation;\n\n"
            "@java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.SOURCE)\n"
            f"public @interface {name} {{ {members} }}\n", encoding="utf-8")


def blocks(source, kind):
    """Read named declarations and their balanced bodies from the input schema."""
    matches = re.compile(rf"\b{kind}\s+(\w+)\s*\{{")
    position = 0
    while match := matches.search(source, position):
        end = match.end()
        depth = 1
        while end < len(source) and depth > 0:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        if depth != 0:
            raise ValueError(f"Unclosed protobuf {kind} {match[1]}")
        yield match[1], source[match.end():end - 1], match.start(), end
        position = end


def camel(name):
    return "".join(word.capitalize() for word in name.split("_"))


def declaration_body(source, kind):
    for _, _, start, end in reversed(list(blocks(source, kind))):
        source = source[:start] + source[end:]
    return source


def java_enum(name, body):
    values = re.findall(r"^\s*(\w+)\s*=\s*(-?\d+)\s*;", body, re.M)
    entries = ", ".join(f"{key}({value})" for key, value in values)
    return (
        f"public enum {name} {{ {entries};\n"
        "    private final int number;\n"
        f"    {name}(int number) {{ this.number = number; }}\n"
        "    public int getNumber() { return number; }\n"
        f"    public static {name} forNumber(int value) {{\n"
        f"        for ({name} candidate : values()) if (candidate.number == value) return candidate;\n"
        "        return null;\n"
        "    }\n"
        "}\n"
    )


def java_message(name, body, enum_defaults):
    # A LauncherAtom dependency of the offline slice. Expose the schema's typed values
    # and builder semantics for local model code without pulling in Android's stats daemon.
    field_pattern = r"(\w+)\s+(\w+)\s*=\s*(\d+)\s*(?:\[([^\]]*)\])?\s*;"
    groups = {group: re.findall(field_pattern, members) for group, members, _, _ in blocks(body, "oneof")}
    plain = declaration_body(declaration_body(body, "oneof"), "enum")
    fields = [(repeat == "repeated", "", *field) for repeat, *field in
              re.findall(rf"\b(optional|repeated|required)\s+{field_pattern}", plain)]
    fields += [(False, group, *field) for group, members in groups.items() for field in members]
    known_types = {"int32": ("int", "Integer", "0"), "bool": ("boolean", "Boolean", "false"),
                   "string": ("String", "String", '""')}
    lines = [
        f"public static final class {name} {{",
        f"    private static final {name} DEFAULT = new {name}(null);",
        "    private final Map<Integer, Object> fields;",
        f"    private {name}(Map<Integer, Object> fields) {{ this.fields = fields; }}",
        f"    public static {name} getDefaultInstance() {{ return DEFAULT; }}",
        "    public static Builder newBuilder() { return new Builder(); }",
        "    @Override public String toString() { return fields == null ? \"{}\" : fields.toString(); }",
    ]
    builder = ["public static final class Builder {",
               "    private final Map<Integer, Object> fields = new LinkedHashMap<>();"]
    for group, members in groups.items():
        case = group + "Case"
        entries = ", ".join(field.upper() for _, field, _, _ in members)
        lines.append(f"    public enum {case} {{ {entries}, {group.upper()}_NOT_SET }}")
        lines.append(f"    public {case} get{case}() {{")
        for _, field, number, _ in members:
            lines.append(f"        if (fields != null && fields.containsKey({number})) return {case}.{field.upper()};")
        lines.extend([f"        return {case}.{group.upper()}_NOT_SET;", "    }"])
    for repeated, group, kind, field, number, options in fields:
        suffix = camel(field)
        is_message = kind not in known_types and kind not in enum_defaults
        java, boxed, default = known_types.get(kind, (kind, kind, f"{kind}.getDefaultInstance()"))
        if kind in enum_defaults:
            default = f"{kind}.{enum_defaults[kind]}"
        if match := re.search(r"\bdefault\s*=\s*(\S+)", options):
            default = match[1]
        if repeated:
            lines.extend([
                "    @SuppressWarnings(\"unchecked\")",
                f"    public List<{boxed}> get{suffix}List() {{",
                f"        return fields == null ? Collections.emptyList() : (List<{boxed}>) "
                f"fields.getOrDefault({number}, Collections.emptyList());",
                "    }",
            ])
            builder.extend([
                f"    public Builder add{suffix}({java} value) {{",
                f"        return addAll{suffix}(Collections.singletonList(value));",
                "    }",
                "    @SuppressWarnings(\"unchecked\")",
                f"    public Builder addAll{suffix}(Iterable<{boxed}> values) {{",
                f"        List<{boxed}> items = new ArrayList<>((List<{boxed}>) "
                f"fields.getOrDefault({number}, Collections.emptyList()));",
                f"        for ({boxed} value : values) items.add(value);",
                f"        fields.put({number}, Collections.unmodifiableList(items));",
                "        return this;",
                "    }",
            ])
        else:
            lines.extend([
                f"    public {java} get{suffix}() {{",
                f"        if (fields != null && fields.containsKey({number})) return ({boxed}) fields.get({number});",
                f"        return {default};",
                "    }",
            ])
            builder.append(f"    public Builder set{suffix}({java} value) {{")
            for _, _, alternative, _ in groups.get(group, ()):
                if alternative != number:
                    builder.append(f"        fields.remove({alternative});")
            builder.extend([f"        fields.put({number}, value);", "        return this;", "    }"])
            if is_message:
                builder.append(f"    public Builder set{suffix}({java}.Builder value) {{ return set{suffix}(value.build()); }}")

    for enum, members, _, _ in blocks(body, "enum"):
        lines.extend("    " + line for line in java_enum(enum, members).splitlines())
    builder.extend([f"    public {name} build() {{ return new {name}(new LinkedHashMap<>(fields)); }}", "}"])
    return "\n".join(lines + ["    " + line for line in builder] + ["}"]) + "\n"


def write_launcher_atoms(launcher, output):
    folder = output / "com/android/launcher3/logger"
    folder.mkdir()
    for schema in reversed(ATOM_SCHEMAS):
        source = (launcher / schema).read_text(encoding="utf-8")
        clean = re.sub(r"//[^\n]*|/\*.*?\*/", "", source, flags=re.S)
        outer = re.search(r'\bjava_outer_classname\s*=\s*"([^"]+)"', clean)[1]
        enum_defaults = {name: re.search(r"\b(\w+)\s*=\s*-?\d+", body)[1]
                         for name, body, _, _ in blocks(clean, "enum")}
        lines = [
            "package com.android.launcher3.logger;",
            "import java.util.ArrayList;",
            "import java.util.Collections;",
            "import java.util.LinkedHashMap;",
            "import java.util.List;",
            "import java.util.Map;",
        ]
        if outer == "LauncherAtom":
            lines.append("import com.android.launcher3.logger.LauncherAtomExtensions.ExtendedContainers;")
        lines.append(f"public final class {outer} {{")
        for name, body, _, _ in blocks(declaration_body(clean, "message"), "enum"):
            lines.extend("    " + line for line in java_enum(name, body).splitlines())
        for name, body, _, _ in blocks(clean, "message"):
            lines.extend("    " + line for line in java_message(name, body, enum_defaults).splitlines())
        lines.append("}")
        (folder / f"{outer}.java").write_text("\n".join(lines) + "\n", encoding="utf-8")
