"""The source extractor must fail closed rather than test a truncated function."""

import tempfile
import unittest
from pathlib import Path

from source import Source


class ExtractionTest(unittest.TestCase):
    def source(self, text):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        path = Path(temporary.name) / "sample.c"
        path.write_text(text, encoding="utf-8")
        return Source(path)

    def test_comments_literals_nested_blocks_and_prototypes(self):
        definition = '''static void run(void)
{
    // } run(void) {
    const char *s = "escaped \\\" }";
    char c = '{'; /* } */
    if (s) { use(c); }
}'''
        source = self.source("void run(void);\n" + definition + "\nvoid other() {}")
        extracted = source.function("run")
        self.assertIn("#line 2 ", extracted)
        self.assertTrue(extracted.endswith(definition + "\n"))
        self.assertNotIn("other()", extracted)

    def test_missing_or_duplicate_definition(self):
        for text in ("void run(void);", "void run() {}\nvoid run() {}"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                self.source(text).function("run")

    def test_unterminated_definition(self):
        with self.assertRaises(ValueError):
            self.source("void run() { if (1) {}\n").function("run")

    def test_shader_semantic(self):
        self.assertIn("float4 run(VSOut i) : SV_Target", self.source(
            "float4 run(VSOut i) : SV_Target\n{ return i.pos; }").function("run"))

    def test_production_enum(self):
        text = "typedef enum\n{ FIRST, /* } */ SECOND } mode_t;\nint unrelated;"
        self.assertIn("SECOND } mode_t;", self.source(text).enum("mode_t"))
        with self.assertRaises(ValueError):
            self.source(text).enum("missing_t")

    def test_sections_require_unique_start_and_end(self):
        source = self.source("// start\nint state;\n// end\nint other;")
        self.assertIn("int state;", source.section("// start", "// end"))
        self.assertNotIn("int other;", source.section("// start", "// end"))
        with self.assertRaises(ValueError):
            source.section("absent", "// end")
        with self.assertRaises(ValueError):
            source.section("// start", "absent")
        with self.assertRaises(ValueError):
            self.source("// start\n// start\n// end").section("// start", "// end")


if __name__ == "__main__":
    unittest.main()
