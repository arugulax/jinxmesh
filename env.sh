# Paths shared by the scripts here; source it, do not run it.
# KBVIEW_WORK holds everything the scripts write: build logs, the simulator's file system,
# run.log and the screenshots. It is on disk on purpose: /tmp is often a small memory file system.
HARNESS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
S=${KBVIEW_WORK:-$HOME/.cache/kbview}
FW=${KBVIEW_FIRMWARE:-$HOME/github_forks/firmware}
UI=${KBVIEW_DEVICE_UI:-$HOME/github_forks/device-ui}
PIO=${KBVIEW_PIO:-$HOME/.platformio-venv/bin/pio}
# the linker's temporary files do not fit into /tmp
PIO_TMP=$HOME/.cache/pio-tmp
# parallel compile jobs: one per core, but no more than one per 1.2 GB of memory (2 on the 3.9 GB box)
JOBS=${KBVIEW_JOBS:-$(awk -v n="$(nproc)" '/MemTotal/ { m = int($2 / 1258291); if (m < 2) m = 2; print (m < n ? m : n) }' /proc/meminfo)}
SIM=$FW/.pio/build/native-tft-kb/meshtasticd
export DISPLAY=${KBVIEW_DISPLAY:-:99}

mkdir -p "$S/kb" "$S/sim/fs" "$S/sim/config.d" "$S/sim/available.d" "$PIO_TMP"
# the simulator wants absolute paths in its config
sed "s|@WORK@|$S|g" "$HARNESS/sim/config.yaml" > "$S/sim/config.yaml"
