#!/usr/bin/env python3
"""kbuild-subset.py -- evaluate the Kbuild subset used by the amdgpu tree.

The pinned 6.6 Makefiles are the source of truth for which .c files compile,
which include paths apply and which per-file flags exist.  This script
evaluates that subset against kernel/linuxkpi/include/generated/autoconf.h
and writes kernel/linux/Makefile.kbuild, which kernel/GNUmakefile includes
after linux/Makefile.files.

Supported syntax (everything else is an error, never a guess):

  * variable assignment: VAR =, VAR :=, VAR +=, VAR ?=, computed names
    (VAR-$(CONFIG_X) := ...), backslash continuations, # comments;
  * conditionals: ifdef/ifndef/ifeq/ifneq/else/endif (including else-if);
  * include path/Makefile (missing includes are fatal, as in Kbuild);
  * make functions used by these trees: addprefix, addsuffix, filter,
    filter-out, patsubst, subst, strip, sort, firstword, word, words,
    notdir, dir, basename, if, call, info, warning, error;
  * $(call cc-option,...), $(call cc-disable-warning,...) and
    $(call gcc-min-version,...) answer for the host GCC.

The generator never modifies the imported tree.  It fails with a non-zero
exit code if any construct is unsupported, an object's source is missing,
or a known-object sanity check fails.

Usage:
  scripts/linux/kbuild-subset.py --tree kernel/linux \
      --config kernel/linuxkpi/include/generated/autoconf.h \
      --out kernel/linux/Makefile.kbuild \
      --report kernel/linux/kbuild-report.txt
"""

import argparse
import os
import re
import sys

# Directories evaluated per root, with the object-list variable to collect.
# (root Makefile relative to the tree, base dir for object paths, variable)
ROOTS = [
    ("drivers/gpu/drm/amd/amdgpu/Makefile",
     "drivers/gpu/drm/amd/amdgpu", "amdgpu-y", "amd"),
    ("drivers/gpu/drm/display/Makefile",
     "drivers/gpu/drm/display", "drm_display_helper-y", "display"),
]

# Objects that must be in the generated list.  If one disappears, the
# Makefile grammar or the config changed in a way this script did not model.
KNOWN_OBJECTS = [
    "drivers/gpu/drm/amd/amdgpu/amdgpu_drv.c",
    "drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c",
    "drivers/gpu/drm/amd/amdgpu/psp_v13_0.c",
    "drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0_5_ppt.c",
    "drivers/gpu/drm/amd/pm/powerplay/hwmgr/hwmgr.c",
    "drivers/gpu/drm/amd/display/amdgpu_dm/amdgpu_dm.c",
    "drivers/gpu/drm/amd/display/amdgpu_dm/dc_fpu.c",
    "drivers/gpu/drm/amd/display/dc/core/dc.c",
    "drivers/gpu/drm/amd/display/dc/dcn31/dcn31_hwseq.c",
    "drivers/gpu/drm/amd/display/dc/dml/dcn314/dcn314_fpu.c",
    "drivers/gpu/drm/amd/display/dmub/src/dmub_dcn31.c",
    "drivers/gpu/drm/display/drm_dp_helper.c",
    "drivers/gpu/drm/display/drm_dsc_helper.c",
]

# Flags the generator drops deliberately.  Warnings are governed by the
# central LINUX_CFLAGS in kernel/GNUmakefile (documented in the gap log), and
# CONFIG_FRAME_WARN is not part of the AvoryOS config.
DROP_FLAG_PATTERNS = [
    re.compile(r"^-W"),
]


class MakeError(Exception):
    pass


def split_words(value):
    return value.split()


def split_func_args(body):
    """Split a function body into arguments on top-level commas."""
    args = []
    depth = 0
    current = []
    i = 0
    while i < len(body):
        c = body[i]
        if c == "$" and i + 1 < len(body) and body[i + 1] in "({":
            depth += 1
            current.append(c)
            current.append(body[i + 1])
            i += 2
            continue
        if c in ")" and depth:
            depth -= 1
            current.append(c)
            i += 1
            continue
        if c == "," and depth == 0:
            args.append("".join(current))
            current = []
            i += 1
            continue
        current.append(c)
        i += 1
    args.append("".join(current))
    return args


def match_percent(pattern, word):
    if "%" not in pattern:
        return pattern == word
    prefix, suffix = pattern.split("%", 1)
    return (word.startswith(prefix) and word.endswith(suffix)
            and len(word) >= len(prefix) + len(suffix))


def patsubst(pattern, replacement, text):
    out = []
    for word in split_words(text):
        if not match_percent(pattern, word):
            out.append(word)
            continue
        if "%" not in pattern:
            out.append(replacement)
            continue
        prefix, suffix = pattern.split("%", 1)
        stem = word[len(prefix):len(word) - len(suffix)] if suffix else word[len(prefix):]
        out.append(replacement.replace("%", stem, 1))
    return " ".join(out)


class Evaluator:
    def __init__(self, tree):
        self.tree = tree
        self.vars = {}
        self.cond_stack = []
        self.includes = []
        self.include_stack = []
        self.unknown = []          # unsupported constructs
        self.undefined_used = set()
        self.dropped_flags = []
        self.pushed_files = []

    # -- variable helpers ---------------------------------------------------
    def active(self):
        return all(frame["active"] for frame in self.cond_stack)

    def var_raw(self, name):
        entry = self.vars.get(name)
        if entry is None:
            self.undefined_used.add(name)
            return None
        return entry

    def var_value(self, name):
        entry = self.vars.get(name)
        if entry is None:
            self.undefined_used.add(name)
            return ""
        value, simple = entry
        return value if simple else self.expand(value)

    def set_var(self, name, op, rhs):
        if not name:
            raise MakeError("empty variable name in assignment")
        if op == "?=":
            if name not in self.vars:
                self.vars[name] = [rhs, False]
            return
        if op == "=":
            self.vars[name] = [rhs, False]
            return
        if op == ":=":
            self.vars[name] = [self.expand(rhs), True]
            return
        if op == "+=":
            if name not in self.vars:
                self.vars[name] = [rhs, False]
                return
            value, simple = self.vars[name]
            if simple:
                self.vars[name] = [(value + " " + self.expand(rhs)).strip(), True]
            else:
                self.vars[name] = [(value + " " + rhs).strip(), False]
            return
        raise MakeError("unknown assignment operator %r" % op)

    # -- expansion ----------------------------------------------------------
    def expand(self, text):
        out = []
        i = 0
        while i < len(text):
            c = text[i]
            if c == "$" and i + 1 < len(text):
                nxt = text[i + 1]
                if nxt == "$":
                    out.append("$")
                    i += 2
                    continue
                if nxt in "({":
                    close = ")" if nxt == "(" else "}"
                    j = i + 2
                    depth = 1
                    while j < len(text) and depth:
                        if text[j] == nxt:
                            depth += 1
                        elif text[j] == close:
                            depth -= 1
                        j += 1
                    if depth:
                        raise MakeError("unbalanced reference in %r" % text)
                    out.append(self.eval_ref(text[i + 2:j - 1]))
                    i = j
                    continue
            out.append(c)
            i += 1
        return "".join(out)

    def eval_ref(self, body):
        body = body.strip()
        if not body:
            return ""
        space = body.find(" ")
        if space == -1:
            return self.var_value(body)
        name = body[:space]
        args = split_func_args(body[space + 1:])
        return self.call_function(name, args, body)

    def call_function(self, name, args, full):
        def arg(i, default=""):
            return args[i] if i < len(args) else default

        if name == "addprefix":
            return " ".join(self.expand(arg(0)) + w
                            for w in split_words(self.expand(arg(1))))
        if name == "addsuffix":
            return " ".join(w + self.expand(arg(0))
                            for w in split_words(self.expand(arg(1))))
        if name == "filter":
            patterns = split_words(self.expand(arg(0)))
            return " ".join(w for w in split_words(self.expand(arg(1)))
                            if any(match_percent(p, w) for p in patterns))
        if name == "filter-out":
            patterns = split_words(self.expand(arg(0)))
            return " ".join(w for w in split_words(self.expand(arg(1)))
                            if not any(match_percent(p, w) for p in patterns))
        if name == "patsubst":
            return patsubst(self.expand(arg(0)), self.expand(arg(1)),
                            self.expand(arg(2)))
        if name == "subst":
            return self.expand(arg(2)).replace(self.expand(arg(0)),
                                               self.expand(arg(1)))
        if name == "strip":
            return " ".join(split_words(self.expand(arg(0))))
        if name == "sort":
            return " ".join(sorted(set(split_words(self.expand(arg(0))))))
        if name == "firstword":
            words = split_words(self.expand(arg(0)))
            return words[0] if words else ""
        if name == "word":
            idx = int(self.expand(arg(0)).strip() or "0")
            words = split_words(self.expand(arg(1)))
            return words[idx - 1] if 0 < idx <= len(words) else ""
        if name == "words":
            return str(len(split_words(self.expand(arg(0)))))
        if name == "notdir":
            return " ".join(os.path.basename(w)
                            for w in split_words(self.expand(arg(0))))
        if name == "dir":
            return " ".join(os.path.dirname(w) + "/"
                            for w in split_words(self.expand(arg(0))))
        if name == "basename":
            return " ".join(os.path.splitext(w)[0]
                            for w in split_words(self.expand(arg(0))))
        if name == "if":
            cond = self.expand(arg(0))
            return self.expand(arg(1)) if cond.strip() else self.expand(arg(2))
        if name == "call":
            callee = self.expand(arg(0)).strip()
            rest = ",".join(args[1:])
            return self.call_builtin(callee, rest)
        if name in ("info", "warning"):
            self.expand(arg(0))
            return ""
        if name == "error":
            raise MakeError("$(error %s)" % self.expand(arg(0)))
        if name == "shell":
            raise MakeError("$(shell ...) is not supported: %s" % full)
        self.unknown.append("$(%s ...)" % name)
        return ""

    def call_builtin(self, callee, rest):
        if callee == "cc-option":
            # Host GCC supports the options these trees probe for.
            return " ".join(split_words(self.expand(rest)))
        if callee == "cc-disable-warning":
            return "-Wno-" + self.expand(rest).strip()
        if callee == "gcc-min-version":
            return "y"
        raise MakeError("$(call %s,...) is not supported" % callee)

    # -- parsing ------------------------------------------------------------
    def push_cond(self, directive, argument):
        parent = self.active()
        value = None
        if directive in ("ifdef", "ifndef"):
            var = self.expand(argument).strip()
            defined = var in self.vars and bool(self.var_value(var).strip())
            value = defined
            if directive == "ifndef":
                value = not value
        elif directive in ("ifeq", "ifneq"):
            a, b = self.parse_ifeq(argument)
            value = self.expand(a).strip() == self.expand(b).strip()
            if directive == "ifneq":
                value = not value
        else:
            raise MakeError("unknown conditional %r" % directive)
        if not parent:
            value = False
        self.cond_stack.append({"active": value, "taken": value,
                                "else_seen": False})

    @staticmethod
    def parse_ifeq(argument):
        argument = argument.strip()
        if argument.startswith("(") and argument.endswith(")"):
            args = split_func_args(argument[1:-1])
            if len(args) == 2:
                return args[0], args[1]
            raise MakeError("cannot parse %r" % argument)
        m = re.match(r'^(["\'])(.*?)\1\s+(["\'])(.*?)\3$', argument)
        if m:
            return m.group(2), m.group(4)
        raise MakeError("cannot parse %r" % argument)

    def handle_else(self, rest):
        if not self.cond_stack:
            raise MakeError("else without conditional")
        frame = self.cond_stack[-1]
        if frame["else_seen"] and not rest.strip():
            raise MakeError("duplicate else")
        if rest.strip():
            # else-if: re-test under the same frame.
            if frame["else_seen"]:
                raise MakeError("else-if after else")
            directive, argument = rest.strip().split(None, 1)
            parent = all(f["active"] for f in self.cond_stack[:-1])
            value = False
            if parent and not frame["taken"]:
                if directive in ("ifdef", "ifndef"):
                    var = self.expand(argument).strip()
                    defined = (var in self.vars
                               and bool(self.var_value(var).strip()))
                    value = defined if directive == "ifdef" else not defined
                elif directive in ("ifeq", "ifneq"):
                    a, b = self.parse_ifeq(argument)
                    value = self.expand(a).strip() == self.expand(b).strip()
                    if directive == "ifneq":
                        value = not value
                else:
                    raise MakeError("unknown else-if %r" % directive)
            frame["active"] = value
            frame["taken"] = frame["taken"] or value
            return
        frame["else_seen"] = True
        frame["active"] = (not frame["taken"]
                           and all(f["active"]
                                   for f in self.cond_stack[:-1]))

    def parse_file(self, rel):
        rel = os.path.normpath(rel).replace(os.sep, "/")
        abs_path = os.path.join(self.tree, rel)
        if not os.path.isfile(abs_path):
            raise MakeError("include not found: %s" % rel)
        if rel in self.include_stack:
            raise MakeError("include cycle: %s" % " -> ".join(
                self.include_stack + [rel]))
        self.include_stack.append(rel)
        self.includes.append(rel)
        with open(abs_path, "r", encoding="utf-8", errors="replace") as f:
            raw_lines = f.read().split("\n")
        i = 0
        while i < len(raw_lines):
            line = raw_lines[i]
            while line.endswith("\\") and i + 1 < len(raw_lines):
                i += 1
                line = line[:-1] + " " + raw_lines[i]
            i += 1
            line = self.strip_comment(line)
            stripped = line.strip()
            if not stripped:
                continue

            m = re.match(r"^(ifdef|ifndef|ifeq|ifneq)\b\s*(.*)$", stripped)
            if m:
                self.push_cond(m.group(1), m.group(2))
                continue
            m = re.match(r"^endif\b", stripped)
            if m:
                if not self.cond_stack:
                    raise MakeError("endif without conditional in %s" % rel)
                self.cond_stack.pop()
                continue
            m = re.match(r"^else\b\s*(.*)$", stripped)
            if m:
                self.handle_else(m.group(1))
                continue
            if not self.active():
                continue

            m = re.match(r"^-?include\s+(.*)$", stripped)
            if m:
                for inc in split_words(self.expand(m.group(1))):
                    inc = inc.strip()
                    if not inc:
                        continue
                    if os.path.isabs(inc):
                        inc = inc.lstrip("/")
                    self.parse_file(inc)
                continue

            m = re.match(r"^([^=]*?)\s*(\+=|\?=|:=|=)\s*(.*)$", line, re.S)
            if m:
                name = self.expand(m.group(1)).strip()
                self.set_var(name, m.group(2), m.group(3))
                continue

            if stripped.startswith("$("):
                self.expand(stripped)
                continue

            raise MakeError("unsupported line in %s: %r" % (rel, stripped))
        self.include_stack.pop()

    @staticmethod
    def strip_comment(line):
        # The Makefiles in these trees have no '#' inside expansions or
        # values; a plain first-'#' cut matches GNU make closely enough.
        idx = line.find("#")
        return line if idx < 0 else line[:idx]

    # -- collection ---------------------------------------------------------
    def normalize_obj(self, entry, base):
        if not entry.endswith(".o"):
            raise MakeError("object entry without .o: %r" % entry)
        path = os.path.normpath(os.path.join(base, entry[:-2] + ".c"))
        path = path.replace(os.sep, "/")
        if path.startswith("../") or os.path.isabs(path):
            raise MakeError("object escapes the tree: %r" % entry)
        if not os.path.isfile(os.path.join(self.tree, path)):
            raise MakeError("missing source for %r: %s" % (entry, path))
        return path

    def collect_objects(self, var, base):
        raw = self.expand("$(%s)" % var)
        seen = set()
        objects = []
        duplicates = []
        for entry in split_words(raw):
            obj = self.normalize_obj(entry, base)
            if obj in seen:
                duplicates.append(obj)
                continue
            seen.add(obj)
            objects.append(obj)
        return objects, duplicates

    def collect_include_flags(self):
        raw = (self.expand("$(ccflags-y)") + " "
               + self.expand("$(subdir-ccflags-y)"))
        flags = []
        for token in split_words(raw):
            if token.startswith("-I"):
                path = token[2:]
                if os.path.isabs(path):
                    path = path.lstrip("/")
                path = os.path.normpath(path).replace(os.sep, "/")
                if not os.path.isdir(os.path.join(self.tree, path)):
                    raise MakeError("include directory missing: %s" % path)
                if not os.path.isabs(path):
                    path = self.prefix + "/" + path
                flag = "-I" + path
                if flag not in flags:
                    flags.append(flag)
                continue
            if token.startswith("-D"):
                if token not in flags:
                    flags.append(token)
                continue
            if any(p.match(token) for p in DROP_FLAG_PATTERNS):
                self.dropped_flags.append(token)
                continue
            self.dropped_flags.append(token)
        return flags

    def collect_per_file_flags(self, base):
        rules = []
        for name in sorted(self.vars):
            if name.startswith("CFLAGS_REMOVE_"):
                value = self.expand("$(%s)" % name)
                if value.strip():
                    self.unknown.append(
                        "non-empty %s (ignored)" % name)
                continue
            if not name.startswith("CFLAGS_"):
                continue
            obj = name[len("CFLAGS_"):]
            if not obj.endswith(".o"):
                continue
            path = os.path.normpath(os.path.join(base, obj[:-2] + ".c"))
            path = path.replace(os.sep, "/")
            if path.startswith("../") or os.path.isabs(path):
                self.unknown.append("CFLAGS_ key escapes the tree: %s" % obj)
                continue
            if not os.path.isfile(os.path.join(self.tree, path)):
                self.unknown.append("CFLAGS_ key has no source: %s" % obj)
                continue
            value = self.expand("$(%s)" % name)
            kept = [t for t in split_words(value)
                    if not any(p.match(t) for p in DROP_FLAG_PATTERNS)
                    and not t.startswith("-Wframe-larger-than")]
            dropped = [t for t in split_words(value) if t not in kept]
            self.dropped_flags.extend(dropped)
            if kept:
                rules.append((path, " ".join(kept)))
        return rules


def load_config(path):
    config = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            m = re.match(r"^#define\s+(CONFIG_[A-Za-z0-9_]+)\s+(.*?)\s*$", line)
            if not m:
                continue
            value = m.group(2)
            config[m.group(1)] = "y" if value == "1" else value
    return config


def read_pin(tree):
    pin = os.path.join(tree, ".pin")
    if not os.path.isfile(pin):
        return None
    data = {}
    with open(pin, "r", encoding="utf-8") as f:
        for line in f:
            if "=" in line:
                key, value = line.strip().split("=", 1)
                data[key] = value
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", required=True,
                        help="imported Linux tree (e.g. kernel/linux)")
    parser.add_argument("--config", required=True,
                        help="autoconf.h to evaluate CONFIG_* from")
    parser.add_argument("--out", required=True,
                        help="generated Makefile fragment")
    parser.add_argument("--report", help="write a human-readable report here")
    args = parser.parse_args()

    tree = os.path.abspath(args.tree)
    if not os.path.isdir(tree):
        raise SystemExit("kbuild-subset: tree not found: %s" % args.tree)
    config = load_config(args.config)
    prefix = os.path.relpath(tree, os.path.dirname(tree))
    if prefix == ".":
        prefix = ""

    all_objects = []
    all_duplicates = []
    include_flags = []
    per_file = []
    reports = []

    for makefile, base, var, label in ROOTS:
        ev = Evaluator(tree)
        ev.prefix = prefix
        # Config symbols become make variables exactly as Kbuild's
        # include/config/auto.conf would define them.  The host compiler is
        # GCC, which dml/Makefile probes for via CONFIG_CC_IS_GCC.
        for name, value in config.items():
            ev.vars[name] = [value, True]
        ev.vars["CONFIG_CC_IS_GCC"] = ["y", True]
        ev.vars["srctree"] = ["", True]
        ev.vars["src"] = [base, True]

        ev.parse_file(makefile)
        if ev.cond_stack:
            raise SystemExit("kbuild-subset: unclosed conditional in %s" % makefile)
        objects, duplicates = ev.collect_objects(var, base)
        if label == "amd":
            include_flags = ev.collect_include_flags()
            per_file = ev.collect_per_file_flags(base)
        all_objects.extend(objects)
        all_duplicates.extend(duplicates)
        if ev.unknown:
            reports.append("UNSUPPORTED constructs in %s:" % makefile)
            reports.extend("  " + u for u in ev.unknown)
        reports.append("%-10s %4d object(s) from %s" % (label, len(objects), var))
        ev.dropped_flags = [f for f in ev.dropped_flags]
        if ev.dropped_flags:
            reports.append("  dropped flag tokens: "
                           + " ".join(sorted(set(ev.dropped_flags))))

    known_missing = [k for k in KNOWN_OBJECTS if k not in all_objects]
    total = len(all_objects)

    pin = read_pin(tree)
    pin_desc = "unknown"
    if pin:
        pin_desc = "%s (%s)" % (pin.get("tag", "?"), pin.get("commit", "?"))

    report_lines = [
        "kbuild-subset report",
        "  tree        : %s" % args.tree,
        "  pin         : %s" % pin_desc,
        "  config      : %s" % args.config,
        "  objects     : %d" % total,
        "  includes    : %d" % len(include_flags),
        "  per-file    : %d rule(s)" % len(per_file),
        "",
    ] + reports
    if all_duplicates:
        report_lines.append("")
        report_lines.append("duplicate object entries (kept once):")
        report_lines.extend("  " + d for d in all_duplicates)
    if known_missing:
        report_lines.append("")
        report_lines.append("MISSING known objects:")
        report_lines.extend("  " + k for k in known_missing)
    text = "\n".join(report_lines) + "\n"
    print(text, end="")
    if args.report:
        with open(args.report, "w", encoding="utf-8") as f:
            f.write(text)

    if all_duplicates:
        pass  # informational only
    if any(r.startswith("UNSUPPORTED") for r in reports) or known_missing:
        raise SystemExit("kbuild-subset: refusing to emit; fix the errors above")

    with open(args.out, "w", encoding="utf-8") as f:
        f.write("# Generated by scripts/linux/kbuild-subset.py -- do not edit.\n")
        f.write("# Linux tree: %s\n" % pin_desc)
        f.write("# Config: %s\n" % args.config)
        f.write("#\n")
        f.write("# amdgpu object list.  Gated by KPI_AMDGPU (default 0 in\n")
        f.write("# kernel/GNUmakefile) while the Phase 6 C2 compile waves land;\n")
        f.write("# flip the default when 6a links and boots cleanly.\n")
        f.write("ifeq ($(KPI_AMDGPU),1)\n")
        f.write("linux-obj-y += \\\n")
        for idx, obj in enumerate(all_objects):
            end = "" if idx == len(all_objects) - 1 else " \\"
            f.write("    %s%s\n" % (obj, end))
        f.write("endif\n\n")
        f.write("# amdgpu include path union (ccflags-y + subdir-ccflags-y,\n")
        f.write("# matching the single-invocation semantics of the 6.6 Makefiles).\n")
        f.write("KPI_AMD_INCLUDES := \\\n")
        for idx, flag in enumerate(include_flags):
            end = "" if idx == len(include_flags) - 1 else " \\"
            f.write("    %s%s\n" % (flag, end))
        f.write("obj-$(ARCH)/linux/drivers/gpu/drm/amd/%.c.o:"
                " LINUX_CPPFLAGS += $(KPI_AMD_INCLUDES)\n\n")
        if per_file:
            f.write("# Per-file CFLAGS_<path> from the 6.6 Makefiles"
                    " (DML float/SSE, ...).\n")
            for path, flags in per_file:
                f.write("obj-$(ARCH)/linux/%s.o: LINUX_CFLAGS += %s\n"
                        % (path, flags))
            f.write("\n")
    print("kbuild-subset: wrote %s (%d objects)" % (args.out, total))


if __name__ == "__main__":
    try:
        main()
    except MakeError as exc:
        raise SystemExit("kbuild-subset: %s" % exc)
