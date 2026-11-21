#!/usr/bin/env python3
"""Expose typed calls for AOSP's descriptor / void-pointer JNI registration tables."""

import argparse
import json
from pathlib import Path
import re


def read_table(source):
    text = source.read_text(encoding="utf-8")
    tables = re.findall(r"static const JNINativeMethod \w+\[\] = \{(.*?)\n\};", text, re.S)
    entry = re.compile(r'\{\s*"(\w+)"\s*,\s*((?:"[^"\n]+"\s*)+),\s*'
                       r'\(void\s*\*\)\s*(\w+)\s*\}')
    methods = []
    for table in tables:
        table = re.sub(r"//[^\n]*|/\*.*?\*/", "", table, flags=re.S)
        if entry.sub("", table).strip(" \t\n,"):
            raise ValueError(f"Unrecognized JNI registration entry in {source}")
        for name, signature, function in entry.findall(table):
            methods.append((name, "".join(re.findall(r'"([^"]+)"', signature)), function))
    if not methods:
        raise ValueError(f"No JNI registration entries in {source}")
    return methods


def wrapper(source, methods):
    symbol = f"Resolve_{source.stem}"
    lines = [f"#include {json.dumps(str(source.resolve()))}", '#include "native_thunk.h"',
             f"wa::interp::NativeThunk wa::interp::{symbol}(void* function) {{"]
    for name in dict.fromkeys(function for _, _, function in methods):
        lines.extend((f"  if (function == reinterpret_cast<void*>(&android::{name})) {{",
                      f"    return &NativeCall<&android::{name}>::Invoke;", "  }"))
    return "\n".join(lines + ["  return nullptr;", "}", ""])


def java_types(descriptors):
    primitive = dict(zip("VZBCSIJFD",
                         "void boolean byte char short int long float double".split()))
    while descriptors:
        arrays = len(descriptors) - len(descriptors.lstrip("["))
        descriptors = descriptors[arrays:]
        if descriptors.startswith("L"):
            end = descriptors.index(";")
            name = descriptors[1:end].replace("/", ".").replace("$", ".")
            descriptors = descriptors[end + 1:]
        else:
            name = primitive[descriptors[0]]
            descriptors = descriptors[1:]
        yield name + "[]" * arrays


def java_asset_manager(template, methods):
    declarations = []
    for name, signature, _ in methods:
        parameters, result = signature[1:].split(")")
        arguments = ", ".join(f"{typ} arg{i}" for i, typ in enumerate(java_types(parameters)))
        visibility = "public" if name.startswith("get") else "private"
        declarations.append(
            f"    {visibility} static native {next(java_types(result))} {name}({arguments});")
    return template.read_text(encoding="utf-8").replace(
        "@native_declarations@", "\n".join(declarations))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    output = parser.add_mutually_exclusive_group(required=True)
    output.add_argument("--wrapper", type=Path)
    output.add_argument("--asset-manager", type=Path)
    parser.add_argument("--template", type=Path)
    args = parser.parse_args()
    methods = read_table(args.source)
    if args.wrapper:
        path = args.wrapper
        content = wrapper(args.source, methods)
    else:
        path = args.asset_manager
        content = java_asset_manager(args.template, methods)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


if __name__ == "__main__":
    main()
