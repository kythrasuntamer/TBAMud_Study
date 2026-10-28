"""Every DG class comparison in lib/world/trg names a class that exists.

%actor.class% (and %self.class%, %victim.class%, ...) expands through
sprinttype(GET_CLASS(c), pc_class_types, ...) in src/dg_variables.c, so a
trigger that compares it against a name missing from pc_class_types[] in
src/class.c is silently always-false (==) or always-true (!=). That is how
renaming "Magic User" to "Wizard" closed every Mage guild to wizards without a
single diagnostic. This test parses pc_class_types[] and every trigger file
and fails on any comparison whose class name is not a current class.

Comparisons checked: '%<var>.class% <op> <name>' and '<name> <op> %<var>.class%'
for ==, !=, and /= (substring). DG compares with str_cmp(), so case does not
matter. %<var>.class(<name>)% subfields go through get_class_by_name() in
src/utils.c, which also accepts abbreviations and the legacy aliases in
legacy_class_aliases[]; they are checked against that rule.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
TRG_DIR = ROOT / "lib/world/trg"

# Stock TBA guildguards for classes this MUD never had. Each '!=' is always
# true, so the guild is closed to everyone; that is pre-existing world content,
# not a rename casualty. Entries are exact (file, trigger vnum, name) so a new
# comparison against an unknown class still fails, and a stale entry fails
# test_known_nonexistent_class_guards_are_still_present.
KNOWN_NONEXISTENT_CLASS_GUARDS = {
    ("257.trg", 25702, "ranger"),
    ("316.trg", 31601, "ranger"),
    ("316.trg", 31605, "death knight"),
    ("316.trg", 31606, "paladin"),
}

CLASS_VAR = r"%[A-Za-z_][A-Za-z0-9_]*\.class%"
OP = r"(==|!=|/=)"
# A class name operand: words and spaces up to ')', '&&', '||' or end of line.
NAME = r"([A-Za-z][A-Za-z ]*?)"
END = r"\s*(?:\)|&&|\|\||$)"
START = r"(?:^|\(|&&|\|\||\bif\b|\belseif\b|\bwhile\b)\s*"
LHS_VAR = re.compile(CLASS_VAR + r"\s*" + OP + r"\s*" + NAME + END)
RHS_VAR = re.compile(START + NAME + r"\s*" + OP + r"\s*" + CLASS_VAR)
SUBFIELD = re.compile(r"%[A-Za-z_][A-Za-z0-9_]*\.class\(([^)%]*)\)%")


def c_string_table(source, table):
    """Return the strings of 'const char *<table>[] = { ... "\\n" };'."""
    match = re.search(r"\b" + re.escape(table) + r"\[\]\s*=\s*\{(.*?)\};",
                      source, re.S)
    if match is None:
        raise AssertionError("cannot find %s[] in source" % table)
    names = re.findall(r'"((?:[^"\\]|\\.)*)"', match.group(1))
    if not names or names[-1] != "\\n":
        raise AssertionError("%s[] is not '\\n'-terminated" % table)
    return names[:-1]


def pc_class_names():
    return c_string_table((ROOT / "src/class.c").read_text(), "pc_class_types")


def legacy_aliases():
    source = (ROOT / "src/utils.c").read_text()
    match = re.search(r"legacy_class_aliases\[\]\s*=\s*\{(.*?)\};", source, re.S)
    if match is None:
        raise AssertionError("cannot find legacy_class_aliases[] in src/utils.c")
    return re.findall(r'\{\s*"([^"]+)"\s*,', match.group(1))


def trigger_lines():
    """Yield (file name, trigger vnum, line number, line) for script lines."""
    for path in sorted(TRG_DIR.glob("*.trg")):
        vnum = None
        for lineno, line in enumerate(path.read_text(errors="replace")
                                      .splitlines(), 1):
            header = re.match(r"#(\d+)\s*$", line)
            if header:
                vnum = int(header.group(1))
                continue
            stripped = line.strip()
            if stripped.startswith("*"):  # DG comment
                continue
            yield path.name, vnum, lineno, line


def class_comparisons():
    """Yield (file, vnum, lineno, op, name) for every class comparison."""
    for name, vnum, lineno, line in trigger_lines():
        for match in LHS_VAR.finditer(line):
            yield name, vnum, lineno, match.group(1), match.group(2).strip()
        for match in RHS_VAR.finditer(line):
            yield name, vnum, lineno, match.group(2), match.group(1).strip()


def is_abbrev(arg, full):
    return bool(arg) and full.lower().startswith(arg.lower())


class TriggerClassNameContract(unittest.TestCase):
    def setUp(self):
        self.classes = [c.lower() for c in pc_class_names()]
        self.assertIn("wizard", self.classes)

    def test_parser_sees_the_guildguard_comparisons(self):
        found = {(f, v, n.lower()) for f, v, _, _, n in class_comparisons()}
        # Midgaard's Mage guildguard: the trigger the rename broke.
        self.assertIn(("30.trg", 3000, "wizard"), found)
        self.assertIn(("30.trg", 3003, "warrior"), found)
        self.assertIn(("30.trg", 3003, "monk"), found)
        self.assertGreater(len(found), 40)

    def test_every_class_comparison_names_a_current_class(self):
        bad = []
        for fname, vnum, lineno, op, name in class_comparisons():
            lowered = name.lower()
            if (fname, vnum, lowered) in KNOWN_NONEXISTENT_CLASS_GUARDS:
                continue
            if op == "/=":
                ok = any(lowered in c for c in self.classes)
            else:
                ok = lowered in self.classes
            if not ok:
                bad.append("lib/world/trg/%s:%d (trigger #%s): %%class%% %s "
                           "'%s' is not in pc_class_types %r"
                           % (fname, lineno, vnum, op, name, self.classes))
        self.assertEqual([], bad, "\n" + "\n".join(bad))

    def test_class_subfields_resolve_through_get_class_by_name(self):
        names = self.classes + [a.lower() for a in legacy_aliases()]
        bad = []
        for fname, vnum, lineno, line in trigger_lines():
            for match in SUBFIELD.finditer(line):
                arg = match.group(1).strip()
                if "%" in arg:  # resolved at run time
                    continue
                if not any(is_abbrev(arg, n) for n in names):
                    bad.append("lib/world/trg/%s:%d (trigger #%s): "
                               "%%class(%s)%% matches no class or alias"
                               % (fname, lineno, vnum, arg))
        self.assertEqual([], bad, "\n" + "\n".join(bad))

    def test_known_nonexistent_class_guards_are_still_present(self):
        found = {(f, v, n.lower()) for f, v, _, _, n in class_comparisons()}
        stale = sorted(KNOWN_NONEXISTENT_CLASS_GUARDS - found)
        self.assertEqual([], stale, "remove stale allowlist entries")
        for _, _, name in KNOWN_NONEXISTENT_CLASS_GUARDS:
            self.assertNotIn(name, self.classes,
                             "%s is a class now; drop it from the allowlist"
                             % name)

    def test_legacy_magic_user_aliases_are_registered(self):
        aliases = [a.lower() for a in legacy_aliases()]
        self.assertIn("magic user", aliases)
        self.assertIn("mage", aliases)


if __name__ == "__main__":
    unittest.main()
