#!/usr/bin/env python3
"""Patch lv_font_conv (LVGL 8 format) output into LVGL 9 form.

`lv_font_conv` still emits the LVGL 8 layout: an include guard, a bunch of
`#if LVGL_VERSION_MAJOR ...` branches, and a `.cache` field on the font
descriptor. LVGL 9 chokes on the stale `.cache` field and renders the font
invisible (see CLAUDE.md gotcha #4). This script resolves those conditionals
for LVGL 9.2, drops the guard + cache, and adds the LVGL 9 font fields so the
result matches the hand-patched fonts already in firmware/src/.

Usage:
    lv_font_patch.py <in.c> <out.c>

Idempotent on already-patched files (no LVGL_VERSION_MAJOR lines → no-op).
"""
import re
import sys

LV_MAJOR, LV_MINOR = 9, 2


def lv_version_check(maj, minr, patch):
    return (LV_MAJOR, LV_MINOR) >= (maj, minr)


def eval_cond(expr: str) -> bool:
    """Evaluate the small set of preprocessor expressions lv_font_conv emits."""
    e = expr.strip()
    if m := re.fullmatch(r"LVGL_VERSION_MAJOR\s*(==|>=)\s*(\d+)", e):
        op, n = m.group(1), int(m.group(2))
        return LV_MAJOR == n if op == "==" else LV_MAJOR >= n
    if e.startswith("!(") and e.endswith(")"):
        return not eval_cond(e[2:-1])
    if "&&" in e:
        return all(eval_cond(p) for p in e.split("&&"))
    if "||" in e:
        return any(eval_cond(p) for p in e.split("||"))
    if m := re.fullmatch(r"LVGL_VERSION_MAJOR\s*==\s*(\d+)\s*&&\s*"
                         r"LVGL_VERSION_MINOR\s*==\s*(\d+)", e):
        return LV_MAJOR == int(m.group(1)) and LV_MINOR == int(m.group(2))
    if m := re.fullmatch(r"LV_VERSION_CHECK\((\d+),\s*(\d+),\s*(\d+)\)", e):
        return lv_version_check(int(m.group(1)), int(m.group(2)), int(m.group(3)))
    raise ValueError(f"unhandled preprocessor expr: {expr!r}")


def resolve_conditionals(lines):
    """Resolve #if/#ifdef/#ifndef/#else/#endif against LVGL 9.2.

    Treats the lv_font_conv include guard (#ifndef <NAME>/#if <NAME>) and
    LV_LVGL_H_INCLUDE_SIMPLE as known-undefined so we keep the plain
    `#include "lvgl.h"` branch and drop the guard wrapper.
    """
    out, stack = [], []  # stack of (keep_branch, taken_already)
    for line in lines:
        s = line.strip()
        if m := re.match(r"#if(n?)def\s+(\w+)", s):
            negate, name = m.group(1) == "n", m.group(2)
            # All macros here are undefined (guard token, INCLUDE_SIMPLE).
            cond = negate  # #ifndef <undefined> → true; #ifdef <undefined> → false
            stack.append([cond and all(f for f, _ in stack), cond])
            continue
        if m := re.match(r"#if\s+(.*)", s):
            try:
                cond = eval_cond(m.group(1))
            except ValueError:
                cond = True
            stack.append([cond and all(f for f, _ in stack), cond])
            continue
        if s.startswith("#else"):
            frame = stack[-1]
            frame[0] = (not frame[1]) and all(f for f, _ in stack[:-1])
            frame[1] = True
            continue
        if s.startswith("#endif"):
            stack.pop()
            continue
        if re.match(r"#define\s+\w+\s+1\s*$", s) and stack and not all(
                f for f, _ in stack):
            continue  # the guard's own #define, inside a dropped branch
        if all(f for f, _ in stack) if stack else True:
            out.append(line)
    return out


def main():
    src, dst = sys.argv[1], sys.argv[2]
    with open(src) as f:
        text = f.read()

    text = "\n".join(resolve_conditionals(text.splitlines()))

    # Drop the now-orphaned guard #define that sits before the first #if.
    text = re.sub(r"^#define\s+\w+\s+1\s*\n", "", text, flags=re.M)
    # Drop the LVGL 8 glyph cache declaration if it survived.
    text = re.sub(r"static\s+lv_font_fmt_txt_glyph_cache_t\s+cache;\s*\n", "", text)
    # Collapse the doubled `#include "lvgl.h"` the conditional resolve can leave.
    text = re.sub(r'(#include "lvgl.h"\n)+', '#include "lvgl.h"\n', text)
    # Drop a trailing `.cache = &cache` line if present.
    text = re.sub(r"\s*\.cache\s*=\s*&cache\s*\n", "\n", text)

    # Add the LVGL 9 font fields the converter omits, matching the existing
    # hand-patched fonts (inserted right after the .subpx line).
    text = re.sub(
        r"(\.subpx = LV_FONT_SUBPX_NONE,\n)",
        r"\1    .release_glyph = NULL,\n    .kerning = 0,\n    .static_bitmap = 0,\n",
        text, count=1)

    # Squeeze 3+ blank lines down to 2.
    text = re.sub(r"\n{3,}", "\n\n\n", text)
    with open(dst, "w") as f:
        f.write(text)


if __name__ == "__main__":
    main()
