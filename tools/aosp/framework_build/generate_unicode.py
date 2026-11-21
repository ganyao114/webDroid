"""Generate packed Unicode data used by the framework's normalization routines."""

from itertools import groupby
import unicodedata as unicode

# Numeric category values follow java.lang.Character.
CATEGORIES = ("Cn Lu Ll Lt Lm Lo Mn Me Mc Nd Nl No Zs Zl Zp Cc Cf -- "
              "Co Cs Pd Ps Pe Pc Po Sm Sc Sk So Pi Pf").split()
CODES = {category: index for index, category in enumerate(CATEGORIES)}
CHUNK = 12000


def ranges(values):
    result = []
    offset = 0
    for value, group in groupby(values):
        result.extend((offset, value))
        offset += sum(1 for _ in group)
    return result


def packed(name, values):
    units = [part for value in values for part in (value >> 11, value & 2047)]
    chunks = (units[i:i + CHUNK] for i in range(0, len(units), CHUNK))
    strings = (''.join(f"\\u{part + 0x800:04x}" for part in chunk) for chunk in chunks)
    return f'    static final String[] {name} = {{\n' + ''.join(f'        "{s}",\n' for s in strings) + '    };\n'


def generate(output):
    categories, classes = [], []
    decomp, tokens, compositions = [], [], []
    for code in range(0x110000):
        char = chr(code)
        categories.append(CODES[unicode.category(char)])
        classes.append(unicode.combining(char))
        text = unicode.decomposition(char)
        if text:
            pieces = text.split()
            compatibility = pieces[0].startswith('<')
            group = [int(item, 16) for item in pieces[compatibility:]]
            decomp.extend((code, len(tokens), len(group) | (32 if compatibility else 0)))
            tokens.extend(group)
            if not compatibility and len(group) == 2 and unicode.normalize('NFC', ''.join(map(chr, group))) == char:
                compositions.append((*group, code))
    composites = [value for entry in sorted(compositions) for value in entry]
    output.write_text(
        f"package java.text;\n\n// Unicode {unicode.unidata_version}, generated from Python's Unicode Database.\n"
        "final class UnicodeData {\n"
        + "".join(packed(name, values) for name, values in {
            "CATEGORY": ranges(categories),
            "COMBINING": ranges(classes),
            "DECOMPOSITIONS": decomp,
            "DECOMPOSED_POINTS": tokens,
            "COMPOSITIONS": composites,
        }.items())
        + "}\n", encoding="utf-8")
