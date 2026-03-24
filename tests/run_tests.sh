#!/bin/bash
# Test runner for machismo and wrapgen
cd "$(dirname "$0")/.."

PASS=0
FAIL=0
TOTAL=0

for test in tests/test_*.sh; do
    TOTAL=$((TOTAL+1))
    name=$(basename "$test" .sh)
    output=$(bash "$test" 2>&1)
    if [ $? -eq 0 ]; then
        echo "PASS: $name"
        PASS=$((PASS+1))
    else
        echo "FAIL: $name"
        echo "  $output" | head -5
        FAIL=$((FAIL+1))
    fi
done

echo "---"
echo "$PASS/$TOTAL passed, $FAIL failed"
[ $FAIL -eq 0 ]
