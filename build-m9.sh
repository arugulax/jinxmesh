#!/bin/bash
# build-m9.sh: build the M9 firmware (env thinknode_m9-tft-kb) detached; $S/build-m9.log ends with BUILD_EXIT=<code>
# TMPDIR is on disk: the LTO link needs more than the tmpfs on /tmp has free
source "$(dirname "$0")/env.sh"
# the About page shows when the build was made: recompile the file that holds the date
touch "$UI/source/graphics/TFT/KeyboardViewBuildStamp.cpp"
setsid nohup bash -c "cd $FW && TMPDIR=$PIO_TMP $PIO run -e thinknode_m9-tft-kb -j $JOBS > $S/build-m9.log 2>&1; echo BUILD_EXIT=\$? >> $S/build-m9.log" > /dev/null 2>&1 &
disown
echo "log: $S/build-m9.log"
