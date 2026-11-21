"""Generate diagnostic stream operators from ART's enums for the sparse checkout."""

import json
from pathlib import Path
import re
import sys


ENUMS = {
    "libartbase": {
        "arch/instruction_set.h": ("art", ["InstructionSet"]),
        "base/allocator.h": ("art", ["AllocatorTag"]),
        "base/unix_file/fd_file.h": ("unix_file", ["FdFile::GuardState"]),
    },
    "libdexfile": {
        "dex/dex_file.h": ("art", ["EncodedArrayValueIterator::ValueType"]),
        "dex/dex_instruction.h": ("art", ["Instruction::Format"]),
        "dex/dex_instruction_utils.h": ("art", ["DexMemAccessType"]),
        "dex/invoke_type.h": ("art", ["InvokeType"]),
    },
}


def stream_operator(header, namespace, enum):
    name = enum.split("::")[-1]
    body = re.search(rf"\benum\s+(?:class\s+)?{name}\b[^{{;]*\{{(.*?)\}};",
                     header, re.DOTALL)
    if body is None:
        raise ValueError(f"Missing enum {enum}")
    enumerators = {}
    for line in body[1].splitlines():
        code, _, comment = line.partition("//")
        value, _, initializer = code.strip().rstrip(",").partition("=")
        value = value.strip()
        if not value:
            continue
        if not re.fullmatch(r"\w+", value):
            raise ValueError(f"Unexpected {enum} enumerator {code!r}")
        alias = initializer.strip()
        if alias in enumerators:
            continue
        label = re.search(r"<<(.*?)>>", comment)
        enumerators[value] = label[1] if label else value
    if not enumerators:
        raise ValueError(f"Empty enum {enum}")
    lines = [f"namespace {namespace} {{",
             f"std::ostream& operator<<(std::ostream& out, {enum} value) {{",
             "  switch (value) {"]
    for value, label in enumerators.items():
        lines.append(f"    case {enum}::{value}: return out << {json.dumps(label)};")
    lines += ["  }", f"  return out << +static_cast<std::underlying_type_t<{enum}>>(value);",
              "}", f"}}  // namespace {namespace}", ""]
    return lines


def main(root, library, output):
    specs = ENUMS[library]
    lines = ["// Generated from AOSP ART headers.", "#include <ostream>", "#include <type_traits>"]
    lines += [f'#include "{name}"' for name in specs]
    lines.append("")
    for name, (namespace, enums) in specs.items():
        header = (Path(root) / library / name).read_text(encoding="utf-8")
        for enum in enums:
            lines += stream_operator(header, namespace, enum)
    Path(output).write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    main(*sys.argv[1:])
