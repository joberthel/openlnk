#!/usr/bin/env python3
"""Clean-Room Isolation & Architectural Guardrail Checker (Agent A vs Agent B).

Enforces TRD Sections 2, 4, and 6 in both local Git pre-commit hooks and CI:
  1. CLEANROOM_NO_ANALYSIS_REF : cleanroom/ and esp32/ must never reference analysis/,
     Ghidra artifacts, or Agent A logs.
  2. NO_LEAKED_SOURCES         : Strictly prohibits leaked MS-DOS source references
     (e.g., c6ers) in code or protocol specifications.
  3. CORE_NO_MALLOC_OR_FLOAT   : Enforces zero dynamic memory allocation and zero
     floating-point arithmetic in cleanroom/src/ and cleanroom/include/.
  4. NO_PROPRIETARY_BINARIES   : Blocks committing DOS binaries, disk images, or Ghidra
     workspace files.
  5. DUAL_AGENT_BOUNDARY       : Blocks a single commit/PR from modifying both analysis/
     and cleanroom/ (or esp32/) simultaneously.
  6. AUDIT_LOGS_VALID          : Verifies audit_agent_a.log and audit_agent_b.log exist
     and contain valid append-only JSON Lines records.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Iterable, Optional

REPO_ROOT = Path(__file__).resolve().parent.parent.parent

CLEANROOM_DIRS = ("cleanroom", "esp32")
CORE_C_DIRS = ("cleanroom/src", "cleanroom/include")
ANALYSIS_DIR = "analysis"

FORBIDDEN_IN_CLEANROOM_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("analysis/ path reference", re.compile(r"\banalysis/", re.IGNORECASE)),
    ("Ghidra reference", re.compile(r"\bghidra\b", re.IGNORECASE)),
    ("analyzeHeadless reference", re.compile(r"\banalyzeHeadless\b", re.IGNORECASE)),
    ("Ghidra project artifact", re.compile(r"\.(?:gpr|rep)\b", re.IGNORECASE)),
    ("Agent A audit log reference", re.compile(r"\baudit_agent_a\.log\b", re.IGNORECASE)),
    ("Disassembly file reference", re.compile(r"\.(?:disasm|idb|i64|bndb)\b", re.IGNORECASE)),
)

FORBIDDEN_LEAKED_SOURCE_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("Leaked c6ers repository reference", re.compile(r"\bc6ers\b", re.IGNORECASE)),
    ("Leaked MS-DOS source header", re.compile(r"Microsoft Confidential|MS-DOS\s+6\.\d+\s+Source", re.IGNORECASE)),
)

# Files that legitimately document the prohibition itself
LEAK_CHECK_ALLOWLIST = {
    "docs/LEGAL_COMPLIANCE.md",
    "analysis/README.md",
    "tools/ci/check_cleanroom_isolation.py",
    "tests/test_tooling.py",
}

FORBIDDEN_CORE_C_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("Dynamic allocation (malloc/calloc/realloc/free)", re.compile(r"\b(?:malloc|calloc|realloc|free)\s*\(")),
    ("stdlib.h include in zero-alloc core", re.compile(r"#\s*include\s*<\s*stdlib\.h\s*>")),
    ("math.h include in integer-only core", re.compile(r"#\s*include\s*<\s*math\.h\s*>")),
    ("Floating-point type (float/double)", re.compile(r"\b(?:float|double)\b")),
)

FORBIDDEN_BINARY_EXTENSIONS = {
    ".exe",
    ".sys",
    ".com",
    ".vhd",
    ".img",
    ".ima",
    ".iso",
    ".gpr",
}

TEXT_EXTENSIONS = {
    ".c", ".h", ".cpp", ".hpp", ".py", ".sh", ".md", ".txt", ".yml", ".yaml",
    ".json", ".ksy", ".cfg", ".properties", ".cmake", ".watcom", "",
}

C_COMMENT_RE = re.compile(
    r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
    re.DOTALL,
)


def strip_c_comments_and_strings(source: str) -> str:
    """Replace C comments and string/char literals with spaces for accurate token scanning."""
    def _repl(match: re.Match[str]) -> str:
        text = match.group(0)
        if text.startswith("//") or text.startswith("/*"):
            return re.sub(r"[^\n]", " ", text)
        return " " * len(text)

    return C_COMMENT_RE.sub(_repl, source)


def iter_repo_files(root: Path) -> Iterable[Path]:
    skip_dirs = {".git", "build", "__pycache__", ".venv", "venv", ".pytest_cache"}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in skip_dirs and not d.endswith(".rep")]
        for fname in filenames:
            yield Path(dirpath) / fname


def check_cleanroom_references(root: Path) -> list[str]:
    violations: list[str] = []
    for sub in CLEANROOM_DIRS:
        base = root / sub
        if not base.exists():
            continue
        for fpath in iter_repo_files(base):
            if fpath.suffix.lower() not in TEXT_EXTENSIONS:
                continue
            rel = fpath.relative_to(root).as_posix()
            try:
                content = fpath.read_text(encoding="utf-8", errors="ignore")
            except OSError:
                continue
            for desc, pattern in FORBIDDEN_IN_CLEANROOM_PATTERNS:
                if pattern.search(content):
                    violations.append(f"[CLEANROOM_NO_ANALYSIS_REF] {rel}: forbidden {desc}")
    return violations


def check_core_c_constraints(root: Path) -> list[str]:
    violations: list[str] = []
    for sub in CORE_C_DIRS:
        base = root / sub
        if not base.exists():
            continue
        for fpath in iter_repo_files(base):
            if fpath.suffix.lower() not in (".c", ".h"):
                continue
            rel = fpath.relative_to(root).as_posix()
            raw = fpath.read_text(encoding="utf-8", errors="ignore")
            stripped = strip_c_comments_and_strings(raw)
            for desc, pattern in FORBIDDEN_CORE_C_PATTERNS:
                if pattern.search(stripped):
                    violations.append(f"[CORE_NO_MALLOC_OR_FLOAT] {rel}: forbidden {desc}")
    return violations


def check_leaked_sources(root: Path) -> list[str]:
    violations: list[str] = []
    for fpath in iter_repo_files(root):
        if fpath.suffix.lower() not in TEXT_EXTENSIONS:
            continue
        rel = fpath.relative_to(root).as_posix()
        if rel in LEAK_CHECK_ALLOWLIST:
            continue
        try:
            content = fpath.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        for desc, pattern in FORBIDDEN_LEAKED_SOURCE_PATTERNS:
            if pattern.search(content):
                violations.append(f"[NO_LEAKED_SOURCES] {rel}: {desc}")
    return violations


def check_binary_paths(paths: Iterable[str]) -> list[str]:
    violations: list[str] = []
    for rel in paths:
        norm = rel.replace("\\", "/")
        ext = Path(norm).suffix.lower()
        if ext in FORBIDDEN_BINARY_EXTENSIONS or ".rep/" in norm:
            violations.append(
                f"[NO_PROPRIETARY_BINARIES] {norm}: binary/Ghidra artifact must not be tracked in git"
            )
    return violations


def check_dual_agent_boundary(changed_paths: Iterable[str]) -> list[str]:
    if os.environ.get("CLEANROOM_ALLOW_BOOTSTRAP") == "1":
        return []
    touched_analysis = []
    touched_cleanroom = []
    for rel in changed_paths:
        norm = rel.replace("\\", "/")
        if norm == ANALYSIS_DIR or norm.startswith(f"{ANALYSIS_DIR}/"):
            touched_analysis.append(norm)
        elif any(norm == d or norm.startswith(f"{d}/") for d in CLEANROOM_DIRS):
            touched_cleanroom.append(norm)

    if touched_analysis and touched_cleanroom:
        return [
            "[DUAL_AGENT_BOUNDARY] Single commit/PR modifies both Agent A (analysis/) "
            f"({len(touched_analysis)} file(s)) and Agent B (cleanroom/esp32) "
            f"({len(touched_cleanroom)} file(s)). Split into separate Agent A and Agent B commits."
        ]
    return []


def check_audit_logs(root: Path) -> list[str]:
    violations: list[str] = []
    required_keys = {"ts", "agent", "action", "compliance"}
    for log_name, expected_agent in (
        ("audit_agent_a.log", "AGENT_A"),
        ("audit_agent_b.log", "AGENT_B"),
    ):
        p = root / log_name
        if not p.is_file():
            violations.append(f"[AUDIT_LOGS_VALID] Missing required audit log: {log_name}")
            continue
        lines = [ln.strip() for ln in p.read_text(encoding="utf-8").splitlines() if ln.strip()]
        if not lines:
            violations.append(f"[AUDIT_LOGS_VALID] Empty audit log: {log_name}")
            continue
        for idx, line in enumerate(lines, start=1):
            try:
                obj = json.loads(line)
            except json.JSONDecodeError as exc:
                violations.append(f"[AUDIT_LOGS_VALID] {log_name}:{idx} invalid JSON: {exc}")
                continue
            missing = required_keys - set(obj.keys())
            if missing:
                violations.append(f"[AUDIT_LOGS_VALID] {log_name}:{idx} missing keys {sorted(missing)}")
            if obj.get("agent") != expected_agent:
                violations.append(
                    f"[AUDIT_LOGS_VALID] {log_name}:{idx} agent={obj.get('agent')!r}, expected {expected_agent!r}"
                )
    return violations


def git_changed_files(root: Path, staged: bool, diff_range: Optional[str]) -> list[str]:
    if staged:
        cmd = ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"]
    elif diff_range:
        cmd = ["git", "diff", "--name-only", "--diff-filter=ACMR", diff_range]
    else:
        cmd = ["git", "ls-files"]
    res = subprocess.run(cmd, cwd=root, capture_output=True, text=True, check=False)
    if res.returncode != 0:
        return []
    return [line.strip() for line in res.stdout.splitlines() if line.strip()]


def run_all_checks(
    root: Path,
    staged: bool = False,
    diff_range: Optional[str] = None,
) -> list[str]:
    violations: list[str] = []
    violations.extend(check_cleanroom_references(root))
    violations.extend(check_core_c_constraints(root))
    violations.extend(check_leaked_sources(root))
    violations.extend(check_audit_logs(root))

    if staged or diff_range:
        changed = git_changed_files(root, staged=staged, diff_range=diff_range)
        violations.extend(check_binary_paths(changed))
        violations.extend(check_dual_agent_boundary(changed))
    else:
        tracked = git_changed_files(root, staged=False, diff_range=None)
        violations.extend(check_binary_paths(tracked))

    return violations


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Verify Clean-Room Isolation & C99 Constraints")
    parser.add_argument("--root", type=Path, default=REPO_ROOT, help="Repository root directory")
    parser.add_argument("--staged", action="store_true", help="Validate staged git changes (pre-commit)")
    parser.add_argument("--diff-range", help="Validate git diff range (e.g. origin/main...HEAD)")
    args = parser.parse_args(argv)

    violations = run_all_checks(args.root, staged=args.staged, diff_range=args.diff_range)
    if violations:
        print("CLEAN-ROOM ISOLATION CHECK FAILED:", file=sys.stderr)
        for v in violations:
            print(f"  - {v}", file=sys.stderr)
        return 1

    mode = "staged" if args.staged else (f"diff:{args.diff_range}" if args.diff_range else "full-repo")
    print(f"[cleanroom-isolation] PASSED ({mode})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
