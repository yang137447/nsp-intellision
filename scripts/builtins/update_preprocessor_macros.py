#!/usr/bin/env python3
"""Generate the preprocessor macro preset resource from shadercompiler data.

The upstream `builtin_macros.py` file is Python 2 and includes generated helper
code after the data table, so this script parses the literal table/update calls
instead of importing the module.
"""

from __future__ import annotations

import argparse
import ast
import json
import re
from collections import OrderedDict
from pathlib import Path
from typing import Dict, Iterable, MutableMapping, Optional, Set, Tuple


LEGACY_COMPILER_CONTEXT_MACROS = {
    # G66 shaderlib/function.hlsl uses this only as `#if !GL3_PROFILE`.
    # The real shadercompiler/NeoX sources do not define it, so the compile
    # behavior follows standard preprocessor semantics: undefined in #if is 0.
    "GL3_PROFILE",
}

LEGACY_UNDEFINED_ZERO_MACROS = OrderedDict(
    [
        # These macros are intentionally not material-family constants here.
        # They model shadercompiler/preprocessor behavior for legacy shader
        # source that uses the name in #if before any source/profile/provider
        # definition exists: undefined identifiers in #if evaluate to 0. A real
        # source #define, active-unit profile row, nsf.defines, or user preset
        # value still overrides this lowest-priority preset input.
        ("COLOR_CHANGE_PICKER", "0"),
        ("COLOR_CHANGE_MULTIPLE", "0"),
        ("COLOR_CHANGE_GRADIENT", "0"),
        ("CHANNEL_COLOR_CHANGE", "0"),
        ("CHANNEL_COLOR_CHANGE_GRADIENT", "0"),
        ("CHANNEL_COLOR_CHANGE_ID", "0"),
        ("EMISSIVE_FLOW", "0"),
        ("EMISSIVE_FLOW_UV1", "0"),
        ("EMISSIVE_PEARL", "0"),
        ("EMISSIVE_DISSOLVE_DISSORT", "0"),
        ("EMISSIVE_THIN_FILM", "0"),
        ("RENDER_VELOCITY", "0"),
        ("HAS_THIN_TRANSLUCENT", "0"),
        ("DYNAMIC_GI_TYPE", "0"),
        ("IS_MEADOW_LOD", "0"),
    ]
)

VERIFIED_STABLE_SOURCE_CONSTANT_MACROS = OrderedDict(
    [
        # G66 shader-source parameter files and generated shader outputs agree
        # on EMISSIVE_COLOR=1. Other EMISSIVE_* values are family-specific and
        # intentionally stay out of the global preset.
        ("EMISSIVE_COLOR", "1"),
        # shaderlib/foliage_anim_functions.hlsl and generated foliage shaders
        # agree on these foliage enum constants.
        ("FOLIAGE_TREE_BRANCH", "1"),
        ("FOLIAGE_TREE_LEAF", "2"),
        ("FOLIAGE_GRASS_BRANCH", "3"),
        ("FOLIAGE_GRASS_LEAF", "4"),
    ]
)


def find_balanced_object(text: str, start: int) -> str:
    expr_start = text.index("{", start)
    depth = 0
    quote = ""
    escaped = False
    for index, ch in enumerate(text[expr_start:], expr_start):
        if quote:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == quote:
                quote = ""
            continue
        if ch in ("'", '"'):
            quote = ch
            continue
        if ch == "{":
            depth += 1
            continue
        if ch == "}":
            depth -= 1
            if depth == 0:
                return text[expr_start : index + 1]
    raise ValueError("Unterminated object literal")


def parse_literal_dict_at(text: str, start: int) -> OrderedDict:
    value = ast.literal_eval(find_balanced_object(text, start))
    return OrderedDict(value)


def parse_builtin_macro_maps(
    text: str,
) -> Tuple[OrderedDict[str, dict], OrderedDict[str, dict], Iterable[str]]:
    base = parse_literal_dict_at(text, text.index("MACROS ="))
    for match in re.finditer(r"MACROS\.update\s*\(", text):
        base.update(parse_literal_dict_at(text, match.end()))

    mobile_high = OrderedDict(base)
    for match in re.finditer(r"MACROS_MOBILE_HIGH\.update\s*\(", text):
        mobile_high.update(parse_literal_dict_at(text, match.end()))

    quality_names = []
    quality_match = re.search(r"QUALITY_LEVEL_NAMES\s*=\s*(\[[^\]]+\])", text)
    if quality_match:
        quality_names = ast.literal_eval(quality_match.group(1))
    return base, mobile_high, quality_names


def parse_const_defines(text: str) -> Dict[str, str]:
    defines: Dict[str, str] = {}
    define_re = re.compile(r"^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s*(.*?)\s*(?://.*)?$")
    for line in text.splitlines():
        match = define_re.match(line)
        if not match:
            continue
        name = match.group(1)
        value = match.group(2).strip()
        if not value:
            value = "1"
        defines[name] = value
    return defines


def add_macro_values(
    target: MutableMapping[str, str],
    source: MutableMapping[str, dict],
    *,
    overwrite: bool,
) -> None:
    for name, info in source.items():
        if not overwrite and name in target:
            continue
        value = str(info.get("value", ""))
        target[name] = value


def collect_dependency_names(maps: Iterable[MutableMapping[str, dict]]) -> Set[str]:
    macro_names = set()
    dependencies = set()
    for mapping in maps:
        macro_names.update(mapping.keys())
    for mapping in maps:
        for info in mapping.values():
            for dependency in info.get("dependents", []):
                if dependency not in macro_names:
                    dependencies.add(str(dependency))
    return dependencies


def parse_named_string_literals(text: str, name: str) -> Set[str]:
    match = re.search(rf"\b{re.escape(name)}\s*=", text)
    if not match:
        return set()
    object_text = find_balanced_object(text, match.end())
    return set(re.findall(r"['\"]([A-Za-z_][A-Za-z0-9_]*)['\"]", object_text))


def parse_compiler_context_macros(text: str) -> Set[str]:
    names: Set[str] = set()
    for object_name in (
        "SYSTEM_SUPPORT_MACROS",
        "DEVICE_SUPPORT_MACRO",
        "API_SUPPORT_MACRO",
        "API_SUPPORT_MACRO_APPEND_IN_FUTURE_HIGH",
        "API_PLATFORM_QUALITY_MACROS",
    ):
        names.update(parse_named_string_literals(text, object_name))
    return names


def build_preset(
    builtin_macros: Path,
    const_macros: Optional[Path],
    compiler_context: Optional[Path],
) -> OrderedDict[str, str]:
    text = builtin_macros.read_text(encoding="utf-8")
    base, mobile_high, quality_names = parse_builtin_macro_maps(text)

    preset: OrderedDict[str, str] = OrderedDict()
    add_macro_values(preset, base, overwrite=True)
    for index, name in enumerate(quality_names):
        preset.setdefault(str(name), str(index))

    # Preserve the base preset when base and mobile-high disagree, but include
    # mobile-only helper macros so references such as QUALITY_SUPPORT_ULTRA_HIGH
    # are available to users.
    for name, info in mobile_high.items():
        if name not in preset:
            preset[name] = str(info.get("value", ""))

    const_values = parse_const_defines(const_macros.read_text(encoding="utf-8")) if const_macros else {}
    for dependency in sorted(collect_dependency_names([base, mobile_high])):
        if dependency in preset:
            continue
        preset[dependency] = const_values.get(dependency, "0")

    if compiler_context:
        context_text = compiler_context.read_text(encoding="utf-8")
        for name in sorted(parse_compiler_context_macros(context_text)):
            preset.setdefault(name, "0")
    for name in sorted(LEGACY_COMPILER_CONTEXT_MACROS):
        preset.setdefault(name, "0")
    for name, value in LEGACY_UNDEFINED_ZERO_MACROS.items():
        preset.setdefault(name, value)
    for name, value in VERIFIED_STABLE_SOURCE_CONSTANT_MACROS.items():
        preset.setdefault(name, value)

    return OrderedDict(sorted(preset.items()))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("builtin_macros", type=Path)
    parser.add_argument(
        "--const-macros",
        type=Path,
        default=None,
        help="Optional shaderlib/const_macros.hlsl used for dependent enum constants.",
    )
    parser.add_argument(
        "--compiler-context",
        type=Path,
        default=None,
        help="Optional shadercompiler hlsl_process.py used for compile-context macro names.",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=Path("server_cpp/resources/language/preprocessor_macros/base.json"),
    )
    args = parser.parse_args()

    preset = build_preset(args.builtin_macros, args.const_macros, args.compiler_context)
    data = {
        "version": 1,
        "entries": [
            {"name": name, "replacement": replacement}
            for name, replacement in preset.items()
        ],
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"wrote {len(data['entries'])} preprocessor macros to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
