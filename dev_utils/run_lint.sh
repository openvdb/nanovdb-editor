#!/usr/bin/env bash
# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Keep in sync with .github/workflows/workflow-security.yml
ZIZMOR_VERSION="1.26.1"
ACTIONLINT_VERSION="1.7.7"

step() {
  echo ""
  echo "==> $1"
}

pass() {
  if [[ $# -gt 0 ]]; then
    echo "    OK — $*"
  else
    echo "    OK"
  fi
}

actionlint_platform() {
  local os=""
  local arch=""
  case "$(uname -s)" in
    Linux) os="linux" ;;
    Darwin) os="darwin" ;;
    MINGW*|MSYS*|CYGWIN*)
      echo "actionlint: Windows is not supported by run_lint.sh; use CI or WSL." >&2
      return 1
      ;;
    *)
      echo "actionlint: unsupported OS: $(uname -s)" >&2
      return 1
      ;;
  esac

  case "$(uname -m)" in
    x86_64|amd64) arch="amd64" ;;
    arm64|aarch64) arch="arm64" ;;
    *)
      echo "actionlint: unsupported architecture: $(uname -m)" >&2
      return 1
      ;;
  esac

  echo "${os}_${arch}"
}

ensure_actionlint() {
  local platform=""
  platform="$(actionlint_platform)"
  local cache_dir="${REPO_ROOT}/.cache/actionlint-${ACTIONLINT_VERSION}"
  local actionlint_bin="${cache_dir}/actionlint"

  if [[ ! -x "${actionlint_bin}" ]]; then
    echo "    downloading actionlint ${ACTIONLINT_VERSION}..."
    mkdir -p "${cache_dir}"
    curl -fsSL \
      "https://github.com/rhysd/actionlint/releases/download/v${ACTIONLINT_VERSION}/actionlint_${ACTIONLINT_VERSION}_${platform}.tar.gz" \
      -o "${cache_dir}/actionlint.tar.gz"
    tar xzf "${cache_dir}/actionlint.tar.gz" -C "${cache_dir}" actionlint
    rm -f "${cache_dir}/actionlint.tar.gz"
  fi

  echo "${actionlint_bin}"
}

check_whitespace() {
  step "Whitespace (fix_whitespace.py --check)"
  local output=""
  local processed=""
  local would_fix=""

  if ! output="$(python dev_utils/fix_whitespace.py . --check 2>&1)"; then
    echo "${output}" | grep '^Would fix:' >&2 || true
    would_fix="$(echo "${output}" | awk '/^Would fix / {print $3}')"
    echo "    FAIL — ${would_fix:-?} file(s) need whitespace fixes" >&2
    echo "    Run: ./dev_utils/run_format.sh" >&2
    exit 1
  fi

  processed="$(echo "${output}" | awk '/^Processed / {print $2}')"
  pass "${processed:-0} files checked, no fixes needed"
}

check_clang_format() {
  step "clang-format"
  local -a sources=()
  while IFS= read -r -d '' file; do
    sources+=("${file}")
  done < <(
    find . -type f \
      \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) \
      -not -path "*/build/*" \
      -not -path "*/.cache/*" \
      -not -path "*/.cpm_cache/*" \
      -not -path "*/vcpkg_installed/*" \
      -not -path "*/_skbuild/*" \
      -not -path "*/dist/*" \
      -not -path "*/__pycache__/*" \
      -not -path "*/*.egg-info/*" \
      -print0
  )

  if [[ ${#sources[@]} -eq 0 ]]; then
    pass "no C/C++ sources found"
    return
  fi

  clang-format --dry-run --Werror "${sources[@]}"
  pass "${#sources[@]} files checked"
}

check_black() {
  step "black"
  python -m black ./pymodule ./pytests --check --quiet \
    --target-version=py311 --line-length=120 --extend-exclude='_skbuild|dist|\.egg-info'
  pass "Python sources"
}

run_actionlint() {
  step "actionlint ${ACTIONLINT_VERSION}"
  local actionlint_bin=""
  actionlint_bin="$(ensure_actionlint)"
  if ! command -v shellcheck >/dev/null 2>&1; then
    echo "    WARNING: shellcheck not found; actionlint will skip shell script checks (CI will not)."
    echo "             Install it (e.g. apt install shellcheck / brew install shellcheck) for full parity."
  fi
  "${actionlint_bin}" -color
  pass "workflows and composite actions"
}

run_zizmor() {
  step "zizmor ${ZIZMOR_VERSION}"
  local -a zizmor_cmd=()
  if command -v uvx >/dev/null 2>&1; then
    zizmor_cmd=(uvx "zizmor==${ZIZMOR_VERSION}")
  elif command -v zizmor >/dev/null 2>&1; then
    zizmor_cmd=(zizmor)
  else
    echo "    FAIL — zizmor is not installed" >&2
    echo "    Install with: pip install 'zizmor==${ZIZMOR_VERSION}'" >&2
    echo "    Or install uv and rerun (uses: uvx zizmor==${ZIZMOR_VERSION})." >&2
    exit 1
  fi

  if [[ -z "${GH_TOKEN:-}" ]] && command -v gh >/dev/null 2>&1; then
    GH_TOKEN="$(gh auth token 2>/dev/null || true)"
  fi
  local pass_note=""
  if [[ -n "${GH_TOKEN:-}" ]]; then
    export GH_TOKEN
    pass_note="online audits enabled"
  else
    pass_note="offline audits only (set GH_TOKEN or run gh auth login for online checks)"
  fi

  local output=""
  if ! output="$("${zizmor_cmd[@]}" -q --persona=regular .github/workflows .github/actions 2>&1)"; then
    echo "${output}" >&2
    exit 1
  fi

  if [[ -n "${output}" ]]; then
    pass "${output}; ${pass_note}"
  else
    pass "${pass_note}"
  fi
}

cd "${REPO_ROOT}"
echo "Running lint checks from ${REPO_ROOT}"

check_whitespace
check_clang_format
check_black
run_actionlint
run_zizmor

echo ""
echo "All lint checks passed."
