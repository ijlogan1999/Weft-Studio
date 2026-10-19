#!/usr/bin/env python3
"""Fails if a UI source draws with a colour or font size that isn't a design token (D-UI-13).

Every colour, typeface and type size lives in app/src/ui/Theme.{h,cpp}; components ask for
`colour (Token::...)` and `font (Type::...)`. This check keeps it that way.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UI = ROOT / "app" / "src" / "ui"
ALLOWED_FILES = {"Theme.h", "Theme.cpp"}

RULES = [
    (re.compile(r"0x[0-9a-fA-F]{8}\b"), "colour literal: use colour (Token::...)"),
    (re.compile(r"juce::Colours::(?!transparentBlack\b|transparentWhite\b)\w+"), "named colour: use colour (Token::...)"),
    (re.compile(r"juce::Colour\s*\(\s*\d"), "colour literal: use colour (Token::...)"),
    (re.compile(r"FontOptions\s*\(\s*[\d.]"), "font size literal: use font (Type::...)"),
    (re.compile(r"\bFont\s*\(\s*[\d.]"), "font size literal: use font (Type::...)"),
    (re.compile(r"setFont\s*\(\s*[\d.]"), "font size literal: use font (Type::...)"),
    (re.compile(r"withHeight\s*\(\s*[\d.]+f?\s*\)\s*\)"), "font size literal: use font (Type::...)"),
    (re.compile(r"withPointHeight\s*\("), "font size: use font (Type::...)"),
]


def main() -> int:
    problems = []
    for path in sorted(list(UI.glob("*.cpp")) + list(UI.glob("*.h"))):
        if path.name in ALLOWED_FILES:
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            code = line.split("//", 1)[0]
            for pattern, why in RULES:
                if pattern.search(code):
                    problems.append(f"{path.relative_to(ROOT)}:{number}: {why}\n    {line.strip()}")
    for p in problems:
        print(p)
    if problems:
        print(f"{len(problems)} UI drawing value(s) bypass the design tokens in app/src/ui/Theme.h")
        return 1
    print("UI sources use design tokens only")
    return 0


if __name__ == "__main__":
    sys.exit(main())
