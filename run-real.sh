#!/bin/bash
# run-real.sh [stress nodes] [binary]: simulator without demo data, the view talks to the real native firmware
# KBVIEW_SAFE=1 makes the view skip actions that restart the device.
# glibc's heap checker is on: a double free or a write past a block aborts, freed memory is overwritten.
source "$(dirname "$0")/env.sh"
BIN=${2:-$SIM}
pkill -x meshtasticd; sleep 1
CHECK="LD_PRELOAD=libc_malloc_debug.so.0 MALLOC_CHECK_=3 MALLOC_PERTURB_=165"
# a sanitizer build brings its own allocator
[ -n "$2" ] && CHECK=""
cd "$S/sim" && env $CHECK ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:log_path=$S/kb/asan KBVIEW_SAFE=1 KBVIEW_STRESS=${1:-0} setsid nohup "$BIN" --sim -c "$S/sim/config.yaml" -d "$S/sim/fs" > "$S/kb/run.log" 2>&1 &
disown
echo "log: $S/kb/run.log (ready when it shows: DeviceTelemetry] Send packet to phone)"
