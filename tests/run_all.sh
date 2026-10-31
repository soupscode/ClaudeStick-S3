#!/usr/bin/env bash
# Runs the whole test suite. Exit status 1 if anything fails.
#   tests/run_all.sh                 everything that can run here
#   tests/run_all.sh --no-browser    skip the headless-browser tests (they need: pip install playwright && playwright install chromium)
#   tests/run_all.sh --no-host       skip the C++ firmware tests (they need g++ and git)
#   tests/run_all.sh --browser-only  only the headless-browser tests
set -uo pipefail
cd "$(dirname "$0")/.."
browser=1; host=1; py=1
for a in "$@"; do case "$a" in --no-browser) browser=0;; --no-host) host=0;; --browser-only) host=0; py=0;; esac; done
fail=0

run() {   # run <label> <command...>: pass/fail from the exit code and any FAIL lines
  local label="$1"; shift
  local out rc p f
  out=$("$@" 2>&1); rc=$?
  p=$(printf '%s\n' "$out" | grep -c '^PASS'); f=$(printf '%s\n' "$out" | grep -c '^FAIL')
  if [ $rc -ne 0 ] || [ "$f" -gt 0 ]; then
    echo "FAIL $label ($p pass, $f fail, exit $rc)"; printf '%s\n' "$out" | tail -25; fail=1
  else
    echo "ok   $label${p:+ ($p checks)}"
  fi
}

if [ $py -eq 1 ]; then
  run py/hook_unit         python3 tests/py/hook_unit.py
  run py/e2e_hooks         python3 tests/py/e2e_hooks.py
  run py/e2e_usage         python3 tests/py/e2e_usage.py
  run py/e2e_statusline    python3 tests/py/e2e_statusline.py
  run py/selfupdate        python3 tests/py/selfupdate.py
  run tools/verify_release python3 tests/py/verify_release_test.py
fi

if [ $host -eq 1 ]; then
  out=$(tests/host/run.sh 2>&1); rc=$?
  printf '%s\n' "$out" | grep -E '^(ok|FAIL) '
  [ $rc -ne 0 ] && { printf '%s\n' "$out" | grep -v '^ok ' | tail -20; fail=1; }
fi

if [ $browser -eq 1 ]; then
  if python3 -c "import playwright" 2>/dev/null; then
    run browser/setup_page     python3 tests/browser/setup_page.py
    run browser/time_zone_step python3 tests/browser/time_zone_step.py
    run browser/setup_flow     python3 tests/browser/setup_flow.py
    run browser/phone_page     python3 tests/browser/phone_page.py
  else
    echo "skip browser tests (playwright not installed)"
  fi
fi

[ $fail -eq 0 ] && echo "ALL TESTS PASSED" || echo "TESTS FAILED"
exit $fail
