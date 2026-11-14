#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build with AddressSanitizer + LeakSanitizer and run the tests, then summarise leaks per test.
#
#   scripts/check_leaks.sh [build dir] [ctest -R pattern]
#
# build dir  default build-asan (configured with -DKK_SANITIZE=address on first use)
# pattern    default: the tests that own memory (DEFAULT below); "all" runs every test
# Environment: KK_JOBS (default 2), KK_LEAK_TIMEOUT per test in seconds (default 900),
#              KK_LEAK_LOGS (default <build dir>/leak-logs), KK_SANITIZE (default address;
#              address,undefined also works), KK_NO_BUILD=1 skips the build.
# Run it in the kk-arch container:  podman exec -w "$PWD" kk-arch scripts/check_leaks.sh
# Exit code: 0 clean, 1 leak / sanitizer error / failed test, 2 build or setup failure.
# Details: docs/contributing.md, "Memory leaks".
set -u
cd "$(dirname "$0")/.." || exit 2
BUILD=${1:-build-asan}
PATTERN=${2:-}
JOBS=${KK_JOBS:-2}
TMO=${KK_LEAK_TIMEOUT:-900}
SAN=${KK_SANITIZE:-address}
LOGS=$(realpath -m "${KK_LEAK_LOGS:-$BUILD/leak-logs}")
DEFAULT='^test_(memorysoak|mediacache|videodecoder|playbackengine|playbacklogic|playbackgap|varispeed|viewerpanel|pageturnplayback|editplaying|bogplayback|cutlookahead|compviewerview|compositingpage|comprender|rendermemory|export$)'

if [ ! -f "$BUILD/build.ninja" ] && [ ! -f "$BUILD/Makefile" ]; then
  cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKK_SANITIZE="$SAN" || exit 2
fi
case "$PATTERN" in
  all) RARGS=() ;;
  "")  RARGS=(-R "$DEFAULT") ;;
  *)   RARGS=(-R "$PATTERN") ;;
esac

if [ -z "${KK_NO_BUILD:-}" ]; then
  # An ASan build of everything takes hours: build the application and the selected tests only.
  if [ "$PATTERN" = all ]; then
    TARGETS=()
  else
    mapfile -t TARGETS < <(ctest --test-dir "$BUILD" -N "${RARGS[@]}" | sed -n 's/^ *Test *#[0-9]*: \(test_[A-Za-z0-9_]*\)$/\1/p')
    TARGETS+=(kinoklip)
    [ ${#TARGETS[@]} -gt 1 ] || { echo "no test matches the pattern"; exit 2; }
  fi
  cmake --build "$BUILD" -j"$JOBS" ${TARGETS[@]:+--target "${TARGETS[@]}"} || { echo "build failed"; exit 2; }
fi

rm -rf "$LOGS"; mkdir -p "$LOGS"
export KK_LEAK_LOGS="$LOGS"
export ASAN_OPTIONS="detect_leaks=1:exitcode=23:symbolize=1:fast_unwind_on_malloc=0:malloc_context_size=30"
export LSAN_OPTIONS="suppressions=$(pwd)/scripts/lsan.supp:exitcode=23:max_leaks=50"
export UBSAN_OPTIONS="print_stacktrace=1"
export QT_QPA_PLATFORM=offscreen KK_NO_AUDIO=1


START=$(date +%s)
ctest --test-dir "$BUILD" -j"$JOBS" --timeout "$TMO" --output-on-failure "${RARGS[@]}" \
  > "$LOGS/ctest.log" 2>&1
CT=$?
tail -n 6 "$LOGS/ctest.log"

# ---- Summary: per test, each leak with its first frames in our code ----
found=0
{
  echo "== Sanitizer reports (logs in $LOGS) =="
  for f in "$LOGS"/test_*; do
    [ -s "$f" ] || continue
    found=1
    echo
    echo "--- $(basename "$f")"
    grep -m1 "SUMMARY:" "$f"
    awk '
      /^(Direct|Indirect) leak of/ { print $0; n=0; next }
      /^ *#[0-9]+ / && /(\/src\/|\/tests\/)/ && n<3 { sub(/^ */,""); print "    " $0; n++ }
      /^ERROR: AddressSanitizer/ { print }
      /runtime error/ { print }
    ' "$f" | head -n 40
  done
  [ $found = 0 ] && echo "none"
  echo
  echo "== Tests that failed =="
  grep -E "Test +#[0-9]+: .*(Failed|Timeout|Exception|\*\*\*)" "$LOGS/ctest.log" || echo "none"
  echo "== Took $(( $(date +%s) - START )) s =="
} > "$LOGS/summary.txt"
cat "$LOGS/summary.txt"

if [ $found = 1 ] || [ $CT -ne 0 ]; then
  echo "check_leaks: FAILED (ctest exit $CT); logs in $LOGS"; exit 1
fi
echo "check_leaks: clean"; exit 0
