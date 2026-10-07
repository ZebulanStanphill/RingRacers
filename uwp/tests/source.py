"""Extract production code without treating braces in comments/strings as syntax.

Generated harnesses include the engine's real headers. Only platform dependencies
are stubbed; implementations under test always come from RR_SOURCE_DIR.
"""

import json
import re
from pathlib import Path


def syntax(text):
    """Blank comments and literals while preserving offsets and line numbers."""
    pattern = r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\[\s\S]|[^"\\])*"|\'(?:\\[\s\S]|[^\'\\])*\''
    return re.sub(pattern, lambda m: re.sub(r"[^\n]", " ", m[0]), text)


class Source:
    def __init__(self, path):
        self.path = Path(path)
        self.text = self.path.read_text(encoding="utf-8")
        self.code = syntax(self.text)

    def annotated(self, start, end):
        line = self.text.count("\n", 0, start) + 1
        return f"#line {line} {json.dumps(self.path.as_posix())}\n" + self.text[start:end] + "\n"

    def function(self, name):
        # These functions have ordinary, non-template definitions. Prototypes and
        # call sites cannot match; changed/ambiguous signatures fail generation.
        pattern = rf"^[\w \t*]+\b{re.escape(name)}\([^;{{}}]*\)(?:\s*:\s*SV_Target)?\s*\{{"
        matches = list(re.finditer(pattern, self.code, re.MULTILINE))
        if len(matches) != 1:
            raise ValueError(f"{self.path}: expected one definition of {name}, found {len(matches)}")
        match = matches[0]
        brace = self.code.index("{", match.start())
        depth = 0
        for end in range(brace, len(self.code)):
            depth += (self.code[end] == "{") - (self.code[end] == "}")
            if depth == 0:
                return self.annotated(match.start(), end + 1)
        raise ValueError(f"{self.path}: unterminated definition of {name}")

    def section(self, first, after):
        if self.text.count(first) != 1:
            raise ValueError(f"{self.path}: ambiguous or missing section start {first!r}")
        start = self.text.index(first)
        end = self.text.index(after, start + len(first))
        return self.annotated(start, end)

    def enum(self, name):
        pattern = rf"^typedef\s+enum\s*\{{[^{{}}]*\}}\s*{re.escape(name)}\s*;"
        matches = list(re.finditer(pattern, self.code, re.MULTILINE))
        if len(matches) != 1:
            raise ValueError(f"{self.path}: expected one enum {name}, found {len(matches)}")
        return self.annotated(matches[0].start(), matches[0].end())
