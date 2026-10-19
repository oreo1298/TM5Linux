#!/usr/bin/env bash
# Checks that the right CPU / RAM temperature sensors are picked from fake /sys trees.
# Usage: tests/sensors-test.sh [path/to/tm5linux-cli]
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
CLI="${1:-../build/tm5linux-cli}"
fail=0

check() {   # fixture, expected CPU °C, expected sensor name, expected DIMM list
    local got
    got="$(TM5LINUX_SYSFS="fixtures/$1" "$CLI" --sensors --json)"
    if python3 -c '
import json, sys
ev = json.loads(sys.argv[1])
want_cpu, want_name, want_dimms = float(sys.argv[2]), sys.argv[3], json.loads(sys.argv[4])
ok = (abs(ev["cpu_temp"] - want_cpu) < 0.01 and ev["cpu_sensor"] == want_name
      and [round(d, 1) for d in ev["dimm_temps"]] == want_dimms)
sys.exit(0 if ok else 1)' "$got" "$2" "$3" "$4"; then
        echo "ok    $1"
    else
        echo "FAIL  $1: $got"
        fail=1
    fi
}

check amd-ddr5          61.25 "k10temp Tctl"                     "[45.0, 46.5, 47.2]"
check intel-ddr4        55    "coretemp Package id 0"            "[]"
check intel-no-coretemp 52    "x86_pkg_temp"                     "[]"
check dual-socket       66    "coretemp, hottest of 2 CPUs"      "[32.5, 33.5]"

exit $fail
