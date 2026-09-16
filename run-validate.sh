#!/usr/bin/env bash
# Run the engine and summarise Vulkan validation output.
#
# Usage: ./run-validate.sh [project] [label]
#   Logs to /tmp/fjell-validate-<label>.log and prints a per-VUID count, so
#   two runs can be diffed to tell a new error from one that was already there.
set -u
project="${1:-$HOME/test_project}"
label="${2:-run}"
log="/tmp/fjell-validate-${label}.log"

./build/src/fjell "$project" 2>&1 | tee "$log"

echo
echo "──────── validation summary (${label}) ────────"
if grep -q "Validation:" "$log"; then
    grep -oP 'VUID-[A-Za-z0-9-]+' "$log" | sort | uniq -c | sort -rn
    echo "  total validation lines: $(grep -c 'Validation:' "$log")"
    echo "  errors: $(grep -c '\[error\] Validation:' "$log")"
    echo "  warnings: $(grep -c '\[warning\] Validation:' "$log")"
else
    echo "  clean — no validation messages"
fi
echo "  log: $log"
