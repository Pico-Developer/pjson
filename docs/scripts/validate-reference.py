#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate that Doxygen emitted pjson's complete public API surface."""

from __future__ import annotations

import argparse
import collections
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


# ---- Required public documentation surface -----------------------------

REQUIRED_COMPOUNDS = {
    "ByteDance",
    "ByteDance::pjson",
    "ByteDance::pjson::Allocator",
    "ByteDance::pjson::PointerError",
    "ByteDance::pjson::PatchError",
    "ByteDance::pjson::PatchOptions",
    "ByteDance::pjson::SerializeOptions",
    "ByteDance::pjson::SerializeError",
    "ByteDance::pjson::StringView",
    "ByteDance::pJsonParser",
    "ByteDance::pJsonParser::Options",
    "ByteDance::pJsonParser::Error",
    "ByteDance::pJsonParser::SaxHandler",
    "ByteDance::pJsonSchemaValidator",
    "ByteDance::pJsonSchemaValidator::Error",
    "ByteDance::pJsonSchemaValidator::Options",
}

# Baseline overload counts make accidental omissions visible. APIs whose exact
# shape is part of the breaking-contract check are also listed below by type.
REQUIRED_MEMBERS = {
    "getVersion": 1,
    "toString": 3,
    "write": 3,
    "getType": 1,
    "isNull": 1,
    "isString": 1,
    "isNumber": 1,
    "isInt": 1,
    "isDouble": 1,
    "isBool": 1,
    "isArray": 1,
    "isObject": 1,
    "isUInt": 1,
    "isInteger": 1,
    "getAllocator": 1,
    "canSwap": 1,
    "tryGet": 24,
    "size": 1,
    "empty": 1,
    "clear": 1,
    "keys": 1,
    "hasKey": 2,
    "contains": 2,
    "hasIndex": 1,
    "find": 6,
    "forEachMember": 2,
    "forEachElement": 2,
    "at": 4,
    "null": 1,
    "object": 1,
    "array": 1,
    "pushBack": 2,
    "insertOrAssign": 2,
    "reserve": 1,
    "escapePointerToken": 1,
    "findPointer": 8,
    "operator[]": 4,
    "operator=": 14,
    "operator+=": 11,
    "erase": 3,
    "applyPatch": 2,
    "applyMergePatch": 2,
    "operator==": 1,
    "operator!=": 1,
}

REQUIRED_PARSER_MEMBERS = {
    "pJsonParser": 2,
    "options": 1,
    "allocator": 1,
    "parse": 4,
    "parseStream": 2,
    "parseSax": 4,
    "parseSaxStream": 2,
}

REQUIRED_SCHEMA_VALIDATOR_MEMBERS = {
    "pJsonSchemaValidator": 1,
    "validate": 2,
    "documentedSubsetDialectUri": 1,
    "documentedSubsetVocabularyUri": 1,
    "isSchemaValid": 1,
    "schemaErrors": 1,
    "dialect": 1,
    "schema": 1,
    "options": 1,
}

REQUIRED_SCHEMA_OPTIONS_MEMBERS = {
    "Options": 1,
    "trustedRegex": 1,
    "strict": 1,
    "modernSubset": 1,
}

REMOVED_PUBLIC_MEMBERS = {
    "PJSONARRAY",
    "PJSONMAP",
    "ArrayStorage",
    "ObjectStorage",
    "getInt64",
    "getDouble",
    "getBool",
    "getString",
    "getArray",
    "getMap",
    "getIfExist",
    "getArrayValues",
    "getInt64Or",
    "getDoubleOr",
    "getBoolOr",
    "getStringOr",
    "EncodeForJSON",
    "EncodeBase64ForJSON",
    "DecodeFromJSON",
    "DecodeBase64FromJSON",
    "parse",
    "parseStream",
    "parseSax",
    "parseSaxStream",
}

EXPECTED_PUBLIC_ENUMS = {
    ("ByteDance::pJsonSchemaValidator::Error", "Category"): {
        "InstanceValidation",
        "SchemaCompilation",
    },
    ("ByteDance::pJsonSchemaValidator::Error", "Code"): {
        "None",
        "FalseSchema",
        "TypeMismatch",
        "ConstMismatch",
        "EnumMismatch",
        "NumericConstraint",
        "StringConstraint",
        "ArrayConstraint",
        "ObjectConstraint",
        "FormatMismatch",
        "CombinatorMismatch",
        "ReferenceFailure",
        "ReferenceCycle",
        "RegexFailure",
        "UnsupportedKeyword",
        "InvalidSchema",
        "UnsupportedDialect",
        "UnsupportedVocabulary",
        "ResolverFailure",
        "ResourceLimit",
        "AllocationFailure",
        "InternalError",
    },
    ("ByteDance::pjson", "jsonType"): {
        "jsonNull",
        "jsonString",
        "jsonNumberInt",
        "jsonNumberDouble",
        "jsonBoolean",
        "jsonArray",
        "jsonObject",
        "jsonNumberUInt",
    },
    ("ByteDance::pjson::Allocator", "AllocationKind"): {
        "NodeAllocation",
        "StringAllocation",
        "ArrayAllocation",
        "ObjectAllocation",
    },
    ("ByteDance::pJsonParser::Options", "DuplicateKeyPolicy"): {
        "RejectDuplicateKeys",
        "KeepFirstDuplicate",
        "KeepLastDuplicate",
    },
    ("ByteDance::pJsonParser::Options", "NumberPolicy"): {
        "RejectUnrepresentableNumbers",
        "AllowLossyNumbers",
    },
    ("ByteDance::pJsonParser::Error", "Code"): {
        "None",
        "Syntax",
        "InvalidEncoding",
        "DuplicateKey",
        "NumberRange",
        "DepthLimit",
        "InputLimit",
        "NodeLimit",
        "AllocationFailure",
        "StreamError",
        "CallbackError",
        "InvalidArgument",
    },
    ("ByteDance::pjson::PointerError", "Code"): {
        "Ok",
        "InvalidSyntax",
        "InvalidEscape",
        "MissingTarget",
        "ExpectedContainer",
        "InvalidArrayIndex",
        "ArrayIndexOutOfRange",
        "AppendTokenNotAllowed",
        "AllocationFailure",
        "InternalError",
    },
    ("ByteDance::pjson::PatchError", "Code"): {
        "Ok",
        "InvalidPatchDocument",
        "OperationNotObject",
        "MissingOp",
        "MissingPath",
        "MissingFrom",
        "MissingValue",
        "InvalidOp",
        "InvalidPath",
        "InvalidFrom",
        "TargetMissing",
        "InvalidArrayIndex",
        "ArrayIndexOutOfRange",
        "MoveRootNotAllowed",
        "MoveIntoDescendant",
        "TestFailed",
        "ResourceLimit",
        "AllocationFailure",
        "InternalError",
    },
    ("ByteDance::pjson::SerializeOptions", "KeyOrder"): {
        "AscendingKeys",
        "DescendingKeys",
    },
    ("ByteDance::pjson::SerializeOptions", "NonFinitePolicy"): {
        "RejectNonFinite",
        "NonFiniteToNull",
        "NonFiniteToString",
    },
    ("ByteDance::pjson::SerializeError", "Code"): {
        "None",
        "InvalidUtf8",
        "NonFiniteNumber",
        "OutputLimit",
        "AllocationFailure",
        "StreamFailure",
        "InternalError",
    },
}

EXPECTED_PARAMETER_TYPES = {
    "operator[]": {
        ("const std::string&",),
        ("const char*",),
        ("int",),
        ("size_t",),
    },
    "tryGet": {
        ("int64_t&",),
        ("uint64_t&",),
        ("double&",),
        ("bool&",),
        ("std::string&",),
        ("StringView&",),
        ("const std::string&", "int64_t&"),
        ("const std::string&", "uint64_t&"),
        ("const std::string&", "double&"),
        ("const std::string&", "bool&"),
        ("const std::string&", "std::string&"),
        ("const std::string&", "StringView&"),
        ("const char*", "int64_t&"),
        ("const char*", "uint64_t&"),
        ("const char*", "double&"),
        ("const char*", "bool&"),
        ("const char*", "std::string&"),
        ("const char*", "StringView&"),
        ("int", "int64_t&"),
        ("int", "uint64_t&"),
        ("int", "double&"),
        ("int", "bool&"),
        ("int", "std::string&"),
        ("int", "StringView&"),
    },
    "toString": {
        (),
        ("const SerializeOptions&",),
        ("std::string&", "SerializeError&", "const SerializeOptions&"),
    },
    "write": {
        ("std::ostream&",),
        ("std::ostream&", "const SerializeOptions&"),
        ("std::ostream&", "SerializeError&", "const SerializeOptions&"),
    },
    "applyPatch": {
        ("const pjson&", "const PatchOptions&"),
        ("const pjson&", "PatchError&", "const PatchOptions&"),
    },
    "applyMergePatch": {
        ("const pjson&", "const PatchOptions&"),
        ("const pjson&", "PatchError&", "const PatchOptions&"),
    },
    "operator=": {
        ("const pjson&",),
        ("pjson&&",),
        ("std::nullptr_t",),
        ("const std::string&",),
        ("const char*",),
        ("const bool",),
        ("const int64_t",),
        ("const uint64_t",),
        ("const double",),
        ("const std::vector<std::string>&",),
        ("const std::vector<bool>&",),
        ("const std::vector<int64_t>&",),
        ("const std::vector<uint64_t>&",),
        ("const std::vector<double>&",),
    },
    "operator+=": {
        ("const std::string&",),
        ("const char*",),
        ("const bool",),
        ("const int64_t",),
        ("const uint64_t",),
        ("const double",),
        ("const std::vector<std::string>&",),
        ("const std::vector<bool>&",),
        ("const std::vector<int64_t>&",),
        ("const std::vector<uint64_t>&",),
        ("const std::vector<double>&",),
    },
}

REQUIRED_PUBLIC_FIELDS = {
    "ByteDance::pJsonParser::Options": {
        "maxDepth",
        "maxNodes",
        "maxInputBytes",
        "duplicateKeys",
        "numberPolicy",
    },
    "ByteDance::pJsonParser::Error": {
        "ok",
        "code",
        "offset",
        "line",
        "column",
        "message",
    },
    "ByteDance::pjson::SerializeError": {
        "code",
        "message",
    },
    "ByteDance::pJsonSchemaValidator::Error": {
        "code",
        "category",
        "instanceLocation",
        "schemaLocation",
        "keyword",
        "message",
        "causes",
    },
    "ByteDance::pJsonSchemaValidator::Options": {
        "maxRegexPatternBytes",
        "maxRegexSubjectBytes",
        "allowUnsafeRegex",
        "maxValidationDepth",
        "maxRefResolutions",
        "maxValidationWork",
        "maxErrors",
        "stopAfterFirstError",
        "collectNestedCauses",
        "validateFormats",
        "strictSubset",
        "refSiblings",
        "retrievalUri",
        "defaultDialectUri",
        "resolver",
        "resolverContext",
        "maxResolvedDocuments",
        "maxResolvedBytes",
    },
    "ByteDance::pjson::PatchOptions": {
        "maxOperations",
        "maxClonedNodes",
        "maxClonedBytes",
        "maxWork",
    },
    "ByteDance::pjson::SerializeOptions": {
        "pretty",
        "indentWidth",
        "indentCharacter",
        "escapeNonAscii",
        "keyOrder",
        "nonFinite",
        "maxOutputBytes",
    },
}

REQUIRED_GUIDE_PAGES = {
    "custom-allocators",
    "migration-nlohmann-json",
    "migration-rapidjson",
}
REQUIRED_DEFINES = {
    "PJSON_VERSION",
    "PJSON_VERSION_MAJOR",
    "PJSON_VERSION_MINOR",
    "PJSON_VERSION_PATCH",
}
REQUIRED_ALLOCATOR_MEMBERS = {
    "AllocationKind": 1,
    "allocate": 1,
    "deallocate": 1,
}


def fail(messages: list[str]) -> int:
    """Emit all validation failures together and return a failing status."""
    for message in messages:
        print(f"documentation validation: {message}", file=sys.stderr)
    return 1


def undocumented_public_members(definition):
    """List public XML members that have neither brief nor detailed prose."""
    undocumented = []
    for member in definition.findall(".//memberdef[@prot='public']"):
        if member.get("kind") not in {"function", "typedef", "enum", "variable"}:
            continue
        prose = "".join(member.find("briefdescription").itertext()).strip()
        prose += "".join(member.find("detaileddescription").itertext()).strip()
        if not prose:
            undocumented.append(member.findtext("name", default="?"))
    return undocumented


def normalized_xml_type(node) -> str:
    """Return a stable spelling for a Doxygen XML type element."""
    if node is None:
        return ""
    value = " ".join("".join(node.itertext()).split())
    for before, after in (("< ", "<"), (" >", ">"), (" &&", "&&"),
                          (" &", "&"), (" *", "*")):
        value = value.replace(before, after)
    return value


def parameter_types(member) -> tuple[str, ...]:
    """Return the normalized parameter-type tuple for one XML member."""
    return tuple(normalized_xml_type(param.find("type")) for param in member.findall("param"))


def signature(name: str, parameters: tuple[str, ...]) -> str:
    """Format a normalized signature for a validation diagnostic."""
    return f"{name}({', '.join(parameters)})"


def main() -> int:
    """Validate generated Doxygen XML/HTML against the required API surface."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--xml", required=True, type=Path)
    parser.add_argument("--html", required=True, type=Path)
    args = parser.parse_args()

    index_path = args.xml / "index.xml"
    html_index = args.html / "index.html"
    missing_files = [str(path) for path in (index_path, html_index) if not path.is_file()]
    if missing_files:
        return fail(["missing generated file " + path for path in missing_files])

    index = ET.parse(index_path).getroot()
    compounds = {
        node.findtext("name", default=""): node
        for node in index.findall("compound")
    }
    errors: list[str] = []

    for name in sorted(REQUIRED_COMPOUNDS - compounds.keys()):
        errors.append(f"missing public compound {name}")

    pjson_node = compounds.get("ByteDance::pjson")
    members: collections.Counter[str] = collections.Counter()
    if pjson_node is not None:
        members.update(node.findtext("name", default="") for node in pjson_node.findall("member"))
        refid = pjson_node.get("refid", "")
        class_xml = args.xml / f"{refid}.xml"
        class_html = args.html / f"{refid}.html"
        if not class_xml.is_file():
            errors.append(f"missing class XML {class_xml.name}")
        if not class_html.is_file():
            errors.append(f"missing class HTML {class_html.name}")
        if class_xml.is_file():
            definition = ET.parse(class_xml).getroot()
            undocumented = undocumented_public_members(definition)
            if undocumented:
                errors.append("undocumented public members: " + ", ".join(undocumented))

    for name, minimum in REQUIRED_MEMBERS.items():
        if members[name] < minimum:
            errors.append(f"{name}: expected at least {minimum} overload(s), found {members[name]}")

    parser_node = compounds.get("ByteDance::pJsonParser")
    parser_members: collections.Counter[str] = collections.Counter()
    if parser_node is not None:
        parser_members.update(
            node.findtext("name", default="") for node in parser_node.findall("member")
        )
    for name, minimum in REQUIRED_PARSER_MEMBERS.items():
        if parser_members[name] < minimum:
            errors.append(
                f"pJsonParser::{name}: expected at least {minimum}, "
                f"found {parser_members[name]}"
            )

    schema_validator_node = compounds.get("ByteDance::pJsonSchemaValidator")
    schema_validator_members: collections.Counter[str] = collections.Counter()
    if schema_validator_node is not None:
        schema_validator_members.update(
            node.findtext("name", default="")
            for node in schema_validator_node.findall("member")
        )
    for name, minimum in REQUIRED_SCHEMA_VALIDATOR_MEMBERS.items():
        if schema_validator_members[name] < minimum:
            errors.append(
                f"pJsonSchemaValidator::{name}: expected at least {minimum}, "
                f"found {schema_validator_members[name]}"
            )

    schema_options_node = compounds.get("ByteDance::pJsonSchemaValidator::Options")
    schema_options_members: collections.Counter[str] = collections.Counter()
    if schema_options_node is not None:
        schema_options_members.update(
            node.findtext("name", default="") for node in schema_options_node.findall("member")
        )
    for name, minimum in REQUIRED_SCHEMA_OPTIONS_MEMBERS.items():
        if schema_options_members[name] < minimum:
            errors.append(
                f"pJsonSchemaValidator::Options::{name}: expected at least {minimum}, "
                f"found {schema_options_members[name]}"
            )

    def compound_definition(name: str):
        """Load one compound XML definition when its index entry exists."""
        node = compounds.get(name)
        if node is None:
            return None
        path = args.xml / f"{node.get('refid', '')}.xml"
        return ET.parse(path).getroot() if path.is_file() else None

    allocator_definition = compound_definition("ByteDance::pjson::Allocator")
    if allocator_definition is not None:
        undocumented = undocumented_public_members(allocator_definition)
        if undocumented:
            errors.append(
                "undocumented Allocator members: " + ", ".join(undocumented)
            )
        allocator_members = collections.Counter(
            member.findtext("name", default="")
            for member in allocator_definition.findall(".//memberdef[@prot='public']")
        )
        for name, minimum in REQUIRED_ALLOCATOR_MEMBERS.items():
            if allocator_members[name] < minimum:
                errors.append(
                    f"Allocator::{name}: expected at least {minimum}, "
                    f"found {allocator_members[name]}"
                )
    for compound_name, expected_fields in REQUIRED_PUBLIC_FIELDS.items():
        definition = compound_definition(compound_name)
        if definition is None:
            continue
        actual_fields = {
            member.findtext("name", default="")
            for member in definition.findall(".//memberdef[@kind='variable'][@prot='public']")
        }
        for field in sorted(expected_fields - actual_fields):
            errors.append(f"missing public field {compound_name}::{field}")

    if members["pjson"] < 6:
        errors.append("pjson: expected six allocator/default constructors")
    if members["swap"] < 1 or members["copyFrom"] < 1:
        errors.append("missing allocator-sensitive swap/copyFrom API")

    pjson_definition = compound_definition("ByteDance::pjson")
    if pjson_definition is not None:
        public_members = pjson_definition.findall(".//memberdef[@prot='public']")
        public_names = collections.Counter(
            member.findtext("name", default="") for member in public_members
        )
        for name in sorted(REMOVED_PUBLIC_MEMBERS):
            if public_names[name]:
                errors.append(f"removed public member is still documented: {name}")

        for name, expected in EXPECTED_PARAMETER_TYPES.items():
            actual = {
                parameter_types(member)
                for member in public_members
                if member.findtext("name", default="") == name
            }
            for parameters in sorted(expected - actual):
                errors.append(f"missing public signature {signature(name, parameters)}")
            for parameters in sorted(actual - expected):
                errors.append(f"unexpected public signature {signature(name, parameters)}")

        for member in public_members:
            if member.findtext("name", default="") == "tryGet":
                result_type = normalized_xml_type(member.find("type"))
                if result_type != "bool":
                    errors.append(
                        f"{signature('tryGet', parameter_types(member))} "
                        f"returns {result_type}, expected bool"
                    )

        constructors = [
            member.findtext("argsstring", default="")
            for member in pjson_definition.findall(".//memberdef[@prot='public']")
            if member.findtext("name", default="") == "pjson"
        ]
        allocator_constructors = [signature for signature in constructors if "Allocator &" in signature]
        if len(allocator_constructors) < 3:
            errors.append(
                f"allocator-aware constructors: expected three signatures, "
                f"found {len(allocator_constructors)}"
            )


    parser_definition = compound_definition("ByteDance::pJsonParser")
    if parser_definition is not None:
        undocumented = undocumented_public_members(parser_definition)
        if undocumented:
            errors.append(
                "undocumented pJsonParser members: " + ", ".join(undocumented)
            )
        parser_public_members = parser_definition.findall(".//memberdef[@prot='public']")
        for member in parser_public_members:
            if member.findtext("name", default="") in {"parse", "parseStream"}:
                result_type = normalized_xml_type(member.find("type"))
                if result_type != "pjson":
                    name = member.findtext("name", default="")
                    errors.append(
                        f"pJsonParser::{signature(name, parameter_types(member))} returns "
                        f"{result_type}, expected pjson"
                    )

    # Pin every public enum nested anywhere under pjson. Scanning all public
    # pjson compounds, rather than only the currently expected owners, also
    # makes a newly added enum fail until the reference contract is updated.
    actual_public_enums: dict[tuple[str, str], set[str]] = {}
    for compound_name in sorted(compounds):
        if (
            compound_name != "ByteDance::pjson"
            and not compound_name.startswith("ByteDance::pjson::")
            and compound_name != "ByteDance::pJsonParser"
            and not compound_name.startswith("ByteDance::pJsonParser::")
            and compound_name != "ByteDance::pJsonSchemaValidator"
            and not compound_name.startswith("ByteDance::pJsonSchemaValidator::")
        ):
            continue
        definition = compound_definition(compound_name)
        if definition is None:
            continue
        compound = definition.find("compounddef")
        if compound is None or compound.get("prot") != "public":
            continue
        for member in definition.findall(".//memberdef[@kind='enum'][@prot='public']"):
            enum_name = member.findtext("name", default="")
            actual_public_enums[(compound_name, enum_name)] = {
                value.findtext("name", default="")
                for value in member.findall("enumvalue")
            }

    for owner, enum_name in sorted(EXPECTED_PUBLIC_ENUMS.keys() - actual_public_enums.keys()):
        errors.append(f"missing public enum {owner}::{enum_name}")
    for owner, enum_name in sorted(actual_public_enums.keys() - EXPECTED_PUBLIC_ENUMS.keys()):
        errors.append(f"unexpected public enum {owner}::{enum_name}")
    for key in sorted(EXPECTED_PUBLIC_ENUMS.keys() & actual_public_enums.keys()):
        owner, enum_name = key
        expected_values = EXPECTED_PUBLIC_ENUMS[key]
        actual_values = actual_public_enums[key]
        for value in sorted(expected_values - actual_values):
            errors.append(f"missing {owner}::{enum_name} value {value}")
        for value in sorted(actual_values - expected_values):
            errors.append(f"unexpected {owner}::{enum_name} value {value}")

    pages = {name for name, node in compounds.items() if node.get("kind") == "page"}
    for name in sorted(REQUIRED_GUIDE_PAGES - pages):
        errors.append(f"missing guide page {name}")

    all_members = collections.Counter(
        member.findtext("name", default="")
        for compound in index.findall("compound")
        for member in compound.findall("member")
    )
    for name in sorted(REQUIRED_DEFINES):
        if all_members[name] == 0:
            errors.append(f"missing public version macro {name}")

    if errors:
        return fail(errors)

    public_count = sum(members.values())
    print(
        f"documentation validation: OK ({public_count} pjson index entries, "
        f"{len(REQUIRED_COMPOUNDS)} required compounds, "
        f"{len(REQUIRED_GUIDE_PAGES)} guide pages)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
