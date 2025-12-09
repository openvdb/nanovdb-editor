#!/usr/bin/env python3
# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

"""Check if current nanovdb-editor version satisfies fvdb-reality-capture requirement."""

import itertools
import os
import pathlib
import re
import sys
import urllib.request

try:
    import tomllib
except ModuleNotFoundError:
    import tomli as tomllib

URL = "https://raw.githubusercontent.com/openvdb/fvdb-reality-capture/main/pyproject.toml"
DEPENDENCY = "nanovdb-editor"


def parse_numeric(version: str):
    """Parse numeric components from a version string."""
    parts = []
    for token in re.split(r"[.+-]", version):
        if token.isdigit():
            parts.append(int(token))
        else:
            break
    return parts


def compare(lhs, rhs):
    """Compare two version tuples. Returns -1 if lhs < rhs, 1 if lhs > rhs, 0 if equal."""
    for l, r in itertools.zip_longest(lhs, rhs, fillvalue=0):
        if l < r:
            return -1
        if l > r:
            return 1
    return 0


def main():
    """Main entry point."""
    # Determine repo root (script is in scripts/ subdirectory)
    repo_root = pathlib.Path(__file__).parent.parent
    version_file = repo_root / "pymodule" / "VERSION.txt"

    with urllib.request.urlopen(URL, timeout=30) as response:
        pyproject = tomllib.loads(response.read().decode())

    dependencies = pyproject.get("project", {}).get("dependencies", [])
    constraint = next(
        (dep for dep in dependencies if dep.replace("_", "-").startswith(DEPENDENCY)),
        None,
    )
    if constraint is None:
        raise SystemExit(f"Unable to locate dependency constraint for {DEPENDENCY!r}")

    # Extract lower bound (>=)
    lower_match = re.search(r">=\s*([0-9][^\",]*)", constraint)
    lower_bound = lower_match.group(1).strip() if lower_match else None

    # Extract upper bound (<)
    upper_match = re.search(r"<\s*([0-9][^\",]*)", constraint)
    if not upper_match:
        raise SystemExit(f"Unable to locate upper bound for {DEPENDENCY!r}")
    upper_bound = upper_match.group(1).strip()

    package_version = version_file.read_text().strip()
    package_parts = parse_numeric(package_version)

    # Check lower bound if present
    lower_ok = True
    if lower_bound:
        lower_ok = compare(package_parts, parse_numeric(lower_bound)) >= 0

    # Check upper bound
    upper_ok = compare(package_parts, parse_numeric(upper_bound)) < 0

    compatible = lower_ok and upper_ok

    constraint_str = f">= {lower_bound}, " if lower_bound else ""
    constraint_str += f"< {upper_bound}"
    print(f"Package version {package_version}, fvdb requirement {constraint_str}: {compatible}")

    # Write to GITHUB_OUTPUT if running in GitHub Actions
    if "GITHUB_OUTPUT" in os.environ:
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as fh:
            fh.write(f"compatible={'true' if compatible else 'false'}\n")

    # Exit with appropriate code for local testing
    sys.exit(0 if compatible else 1)


if __name__ == "__main__":
    main()
