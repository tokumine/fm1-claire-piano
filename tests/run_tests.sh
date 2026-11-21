#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Pianola verification suite: OTA package check, update-loader transition test,
# host simulator build, and full interactive scenario check (app/tests/basic.txt).
set -u
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
PY="${PYTHON:-python3}"
export AC79_SDK="${AC79_SDK:-$HOME/fw-AC79_AIoT_SDK}"
B=app/build
OUT=$B/tests
FAIL=0
mkdir -p "$OUT" "$B/scenarios/basic"
run() {
    name="$1"; shift
    if "$@" > "$OUT/$name.log" 2>&1; then
        echo "  ok   $name"
    else
        echo "  FAIL $name (see $OUT/$name.log)"
        tail -20 "$OUT/$name.log" | sed 's/^/       /'
        FAIL=1
    fi
}

if [ -f "$B/pianola.fwsc" ]; then
    run ota-entry sh -c "$CC -w -o $OUT/ota_test base/tests/ota_test.c && $OUT/ota_test $B/pianola.fwsc"
    run update-loader sh -c "head -c 100000 $B/pianola.bin > $OUT/old_app.bin && \
        $PY base/tools/fm1pkg_make.py $OUT/old_app.bin $B/loader/ota.bin $OUT/old.fwsc >/dev/null && \
        $CC -w -o $OUT/ldr_test base/tests/ldr_test.c && $OUT/ldr_test $OUT/old.fwsc $B/pianola.fwsc"
else
    echo "  skip update path (no $B/pianola.fwsc: ./build.sh first)"
fi

run host-build sh build.sh --host
run scenario-basic "$B/host/pianola_host" app/tests/basic.txt "$B/scenarios/basic"

[ $FAIL -eq 0 ] && echo "all tests passed" || echo "TESTS FAILED"
exit $FAIL
