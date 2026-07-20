# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import argparse
import os
import subprocess
import sys
from pathlib import Path

# Keep in sync with .github/workflows/codestyle.yml (trailingspaces / spacesnottabs jobs).
TRAILING_SPACE_SKIP_SUFFIXES = (".wlt",)

BINARY_SKIP_PATTERNS = {
    ".svg",
    ".cmd",
    ".png",
    ".jpg",
    ".gif",
    ".mp4",
    ".pt",
    ".pth",
    ".nvdb",
    ".npz",
    ".wlt",
}

TAB_FIX_SKIP_PATTERNS = BINARY_SKIP_PATTERNS | {".gitmodules"}


def get_git_root():
    try:
        git_root = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            stderr=subprocess.DEVNULL,
            universal_newlines=True,
        ).strip()
        return Path(git_root)
    except subprocess.CalledProcessError:
        return None


def get_git_files(git_root: Path) -> set[Path]:
    try:
        git_files = subprocess.check_output(
            ["git", "ls-files"],
            cwd=git_root,
            stderr=subprocess.DEVNULL,
            universal_newlines=True,
        )
        return {Path(f.strip()) for f in git_files.splitlines()}
    except subprocess.CalledProcessError:
        return set()


def is_ignored_by_git(git_root: Path, relative_path: Path) -> bool:
    try:
        subprocess.run(
            ["git", "check-ignore", "-q", "--", str(relative_path)],
            cwd=git_root,
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return True
    except subprocess.CalledProcessError:
        return False


def normalize_path(file_path: Path) -> str:
    return str(file_path).replace("\\", "/")


def matches_pattern(path: str, patterns: set[str]) -> bool:
    return any(pattern in path for pattern in patterns)


def should_skip_file(relative_path: Path, git_root: Path, git_files: set[Path]) -> bool:
    path = normalize_path(relative_path)
    if path.endswith(TRAILING_SPACE_SKIP_SUFFIXES):
        return True
    if matches_pattern(path, BINARY_SKIP_PATTERNS):
        return True

    if relative_path in git_files:
        return False

    # Include untracked (but not gitignored) files under .github/, e.g. new workflow YAML.
    if path.startswith(".github/") and not is_ignored_by_git(git_root, relative_path):
        return False

    return True


def should_fix_tabs(file_path: Path) -> bool:
    path = normalize_path(file_path)
    if path.endswith("codestyle.yml"):
        return False
    return not matches_pattern(path, TAB_FIX_SKIP_PATTERNS)


def transform_line(line: str, fix_tabs: bool) -> str:
    new_line = line.rstrip()
    if fix_tabs:
        new_line = new_line.replace("\t", "    ")
    return new_line


def file_needs_fix(file_path: Path, fix_tabs: bool) -> bool:
    try:
        with open(file_path, "r", encoding="utf-8") as f:
            content = f.read()
    except OSError:
        return False

    if "\0" in content:
        return False

    for line in content.splitlines():
        if transform_line(line, fix_tabs) != line:
            return True
    return False


def fix_whitespace(file_path: Path, fix_tabs: bool) -> bool:
    try:
        with open(file_path, "r", encoding="utf-8") as f:
            content = f.read()

        if "\0" in content:
            return False

        fixed_lines = []
        modified = False

        for line in content.splitlines():
            new_line = transform_line(line, fix_tabs)
            if new_line != line:
                modified = True
            fixed_lines.append(new_line)

        if modified:
            with open(file_path, "w", encoding="utf-8", newline="\n") as f:
                f.write("\n".join(fixed_lines) + "\n")
            print(f"Fixed: {file_path}")
            return True

        return False

    except Exception as e:
        print(f"Error processing {file_path}: {e}")
        return False


def check_line_length(file_path, max_length=100):
    try:
        with open(file_path, "r", encoding="utf-8") as f:
            for line_num, line in enumerate(f, 1):
                if len(line.rstrip("\n")) > max_length:
                    line_length = len(line.rstrip("\n"))
                    print(
                        f"{file_path}:{line_num}: Line length {line_length} exceeds {max_length} characters"
                    )
    except Exception as e:
        print(f"Error checking line length in {file_path}: {e}")


def main():
    parser = argparse.ArgumentParser(
        description="Fix whitespace issues in git-tracked files (matches codestyle CI checks)"
    )
    parser.add_argument(
        "directory",
        nargs="?",
        default=".",
        help="Directory to process (default: current directory)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Show files that would be modified without making changes",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Exit with status 1 if any file would need fixes",
    )
    parser.add_argument(
        "--check-length",
        action="store_true",
        help="Check for lines exceeding 100 characters",
    )

    args = parser.parse_args()
    preview_only = args.dry_run or args.check

    git_root = get_git_root()
    if not git_root:
        print("Error: Not a git repository", file=sys.stderr)
        return 1

    work_dir = Path(args.directory).resolve()
    git_files = get_git_files(git_root)
    if not git_files:
        print("Error: No git-tracked files found", file=sys.stderr)
        return 1

    fixed_count = 0
    processed_count = 0

    for root, _, files in os.walk(work_dir):
        for file in files:
            file_path = Path(root) / file
            try:
                relative_path = file_path.resolve().relative_to(git_root)
            except ValueError:
                continue

            if should_skip_file(relative_path, git_root, git_files):
                continue

            processed_count += 1
            fix_tabs = should_fix_tabs(relative_path)

            if args.check_length:
                check_line_length(file_path)

            if preview_only:
                if file_needs_fix(file_path, fix_tabs):
                    print(f"Would fix: {relative_path}")
                    fixed_count += 1
            elif fix_whitespace(file_path, fix_tabs):
                fixed_count += 1

    print(f"\nProcessed {processed_count} files")
    if preview_only:
        print(f"Would fix {fixed_count} files")
    elif fixed_count == 0:
        print("No whitespace fixes needed.")
    else:
        print(f"Fixed {fixed_count} files")

    if args.check and fixed_count > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
