#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Test launcher for sanitizer builds (KK_SANITIZE): lsan_run.sh <test name> <command...>
# Gives every test its own sanitizer log, <KK_LEAK_LOGS>/<test name>.<pid>, so
# scripts/check_leaks.sh can say which test leaked. Without KK_LEAK_LOGS it just runs the test.
name=$1; shift
if [ -n "${KK_LEAK_LOGS:-}" ]; then
  ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}log_path=$KK_LEAK_LOGS/$name"
  UBSAN_OPTIONS="${UBSAN_OPTIONS:+$UBSAN_OPTIONS:}log_path=$KK_LEAK_LOGS/$name.ubsan"
  export ASAN_OPTIONS UBSAN_OPTIONS
fi
exec "$@"
