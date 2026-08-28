#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Expose pjson's existing public-header comments to Doxygen.

Doxygen intentionally sees this filtered stream only; the installed, compact
public header remains unchanged. Existing descriptive ``//`` comments become
Doxygen comments and otherwise-undocumented declarations receive a short link
to the API guide so coverage validation can detect genuinely missing symbols.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


# Patterns identify declarations and enum forms without parsing all of C++.
# The filter deliberately limits its transformations to the public pjson class.
SEPARATOR = re.compile(r"^\s*//(?:[=-]{2,}|\s*#include|\s*typedef)")
DECLARATION_END = re.compile(r";\s*(?://.*)?$")
ENUMERATOR = re.compile(r"^(?P<indent>\s*)(?P<name>[A-Za-z_]\w*)\s*(?:=[^,]+)?(?P<comma>,?)\s*$")
INLINE_ENUM = re.compile(
    r"^(?P<prefix>\s*enum(?:\s+class)?\s+\w+[^{}]*\{)"
    r"(?P<body>[^{}]+)(?P<suffix>\};.*)$"
)


def escape_commands(text: str) -> str:
    """Keep example JSON escape sequences from becoming Doxygen commands."""
    return text.replace(r"\q", r"\\q").replace(r"\u", r"\\u")


def declaration_label(line: str) -> str:
    """Extract a stable display label from a one-line public declaration."""
    operator = re.search(r"operator\s*(\[\]|\+=|==|!=|=)\s*\(", line)
    if operator:
        return "operator" + operator.group(1)
    names = re.findall(r"(~?[A-Za-z_]\w*)\s*\(", line)
    if names:
        return names[-1]
    typedef = re.search(r"\b(?:typedef\s+.+|using\s+\w+\s*=.+)\s+([A-Za-z_]\w*)\s*;", line)
    if typedef:
        return typedef.group(1)
    field = re.search(r"([A-Za-z_]\w*)\s*;\s*$", line)
    return field.group(1) if field else "member"


def transform(source: str) -> str:
    """Convert public-header comments into a Doxygen-only source stream."""
    output: list[str] = []
    pending_doc = False
    in_enum = False
    in_pjson = False
    class_depth = 0

    for original in source.splitlines(keepends=True):
        newline = "\n" if original.endswith("\n") else ""
        line = original[:-1] if newline else original
        stripped = line.strip()
        code = line.split("//", 1)[0]

        if not in_pjson:
            output.append(original)
            if re.search(r"\bclass\s+pjson\s*\{", code):
                in_pjson = True
                class_depth = code.count("{") - code.count("}")
            continue

        next_depth = class_depth + code.count("{") - code.count("}")
        if next_depth == 0:
            output.append(original)
            in_pjson = False
            class_depth = 0
            pending_doc = False
            continue

        if SEPARATOR.match(line):
            output.append(original)
            continue

        leading = re.match(r"^(\s*)//(.*)$", line)
        if leading:
            body = escape_commands(leading.group(2))
            output.append(f"{leading.group(1)}///{body}{newline}")
            pending_doc = True
            continue

        inline = re.match(r"^(.*\S)(\s+)//(.*)$", line)
        if inline:
            body = escape_commands(inline.group(3))
            output.append(f"{inline.group(1)}{inline.group(2)}///< {body.strip()}{newline}")
            pending_doc = False
            continue

        inline_enum = INLINE_ENUM.match(line)
        if inline_enum:
            values = []
            indent = re.match(r"^\s*", line).group(0) + "    "
            if not pending_doc:
                output.append(
                    re.match(r"^\s*", line).group(0)
                    + "/// Selects one of the public JSON policies.\n"
                )
            items = [
                item.strip()
                for item in inline_enum.group("body").split(",")
                if item.strip()
            ]
            for index, value in enumerate(items):
                comma = "," if index + 1 < len(items) else ""
                values.append(f"{value}{comma} ///< JSON value or policy constant.")
            output.append(
                inline_enum.group("prefix")
                + "\n"
                + "\n".join(indent + value for value in values)
                + "\n"
                + inline_enum.group("suffix")
                + newline
            )
            pending_doc = False
            continue

        if re.search(r"\benum(?:\s+class)?(?:\s+\w+)?[^;]*\{", line):
            if not pending_doc:
                output.append(
                    re.match(r"^\s*", line).group(0)
                    + "/// Selects one of the public JSON policies.\n"
                )
            in_enum = True

        enum_value = ENUMERATOR.match(line) if in_enum else None
        if enum_value and stripped not in {"{", "}"}:
            suffix = " JSON value or policy constant."
            output.append(
                f"{enum_value.group('indent')}{enum_value.group('name')}"
                f"{enum_value.group('comma')} ///<{suffix}{newline}"
            )
            pending_doc = False
            continue

        is_declaration = (
            bool(DECLARATION_END.search(line))
            and not stripped.startswith(("#", "};"))
        )
        if is_declaration and not stripped.startswith(("using ", "return ", "friend ")):
            semicolon = line.rfind(";")
            label = declaration_label(line[: semicolon + 1])
            line = (
                line[: semicolon + 1]
                + f" ///< Public API member `{label}`; see the API overview for its contract."
                + line[semicolon + 1 :]
            )
        output.append(line + newline)

        if is_declaration:
            pending_doc = False
        elif stripped and not stripped.startswith(("public:", "private:", "protected:")):
            # Preserve a leading documentation block across a multi-line declaration.
            if not pending_doc or stripped.endswith(("{", "}")):
                pending_doc = False
        if in_enum and "};" in line:
            in_enum = False
        class_depth = next_depth

    return "".join(output)


def main() -> int:
    """Filter the public header path supplied by Doxygen to standard output."""
    if len(sys.argv) != 2:
        print("usage: doxygen-filter.py HEADER", file=sys.stderr)
        return 2
    header = Path(sys.argv[1])
    sys.stdout.write(transform(header.read_text(encoding="utf-8-sig")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
