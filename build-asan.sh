#!/bin/bash
# build-asan.sh: simulator with the address sanitizer, detached; $S/build-asan.log ends with BUILD_EXIT=<code>
# It builds into ~/.cache/pio-asan, so the normal build stays as it is. The flags come from the
# environment: an edit of platformio.ini would make PlatformIO wipe every build directory.
source "$(dirname "$0")/env.sh"
setsid nohup bash -c "cd $FW && TMPDIR=$PIO_TMP PLATFORMIO_BUILD_DIR=$HOME/.cache/pio-asan PLATFORMIO_BUILD_FLAGS='-fsanitize=address -fno-omit-frame-pointer -g1' nice $PIO run -e native-tft-kb -j $JOBS > $S/build-asan.log 2>&1; echo BUILD_EXIT=\$? >> $S/build-asan.log" > /dev/null 2>&1 &
disown
echo "log: $S/build-asan.log, binary: $HOME/.cache/pio-asan/native-tft-kb/meshtasticd"
