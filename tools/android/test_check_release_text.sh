#!/bin/bash
# Tests check_release_text.sh (tools/android/README.md) with two install texts in tools/android/testdata: one still
# "in testing" (in other letter case, to show the match ignores case), one with a final result (and the word
# "Testing" alone, which must not trip the guard).
#
# usage: tools/android/test_check_release_text.sh
# Exit 0 when every case gives the expected result.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
CHECK=tools/android/check_release_text.sh
IN_TESTING=tools/android/testdata/install-in-testing.txt
TESTED=tools/android/testdata/install-tested.txt

PASS=0
FAILS=0
# expect <name> <exit status> <args...>
expect() {
    local name=$1 want=$2 out status=0
    shift 2
    out=$(bash "$CHECK" "$@" 2>&1) || status=$?
    if [ "$status" = "$want" ]; then
        echo "PASS $name: exit $status"
        PASS=$((PASS + 1))
    else
        echo "FAIL $name: exit $status, expected $want"
        FAILS=$((FAILS + 1))
    fi
    sed 's/^/    /' <<< "$out"
}

expect "'in testing', tag build" 1 "$IN_TESTING" refs/tags/v0.9.0
expect "'in testing', dry run on a branch" 0 "$IN_TESTING" refs/heads/feature/android-build
expect "'in testing', dry run on master" 0 "$IN_TESTING" refs/heads/master
expect "final result, tag build" 0 "$TESTED" refs/tags/v0.9.0
expect "final result, branch" 0 "$TESTED" refs/heads/master
expect "missing file" 2 tools/android/testdata/none.txt refs/tags/v0.9.0
expect "no ref" 2 "$IN_TESTING" ""
expect "one argument" 2 "$IN_TESTING"

echo "test_check_release_text: $PASS passed, $FAILS failed"
[ "$FAILS" = 0 ]
