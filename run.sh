#!/bin/bash
# run.sh: start the simulator with the view's built-in demo data (no mesh behind it)
source "$(dirname "$0")/env.sh"
pkill -x meshtasticd; sleep 1
cd "$S/sim" && KBVIEW_DEMO=1 setsid nohup "$SIM" --sim -c "$S/sim/config.yaml" -d "$S/sim/fs" > "$S/kb/run.log" 2>&1 &
disown
