#!/bin/bash
set -euo pipefail

usage() {
    echo "Usage: $0 <working_folder> all|release" >&2
}

if [ $# -lt 2 ]; then
    usage
    exit 1
fi

WORKING_FOLDER=$1
BUILD_TYPE=$2
TARGET_FIRMWARE="target_firmware.h"

run_stcmd() {
    STCMD_NO_TTY=1 ST_WORKING_FOLDER="$WORKING_FOLDER" stcmd "$@"
}

run_stcmd make "$BUILD_TYPE"

FILENAME="./dist/FIRMWARE.IMG"
run_stcmd cp ./dist/BOOT.BIN "$FILENAME"

FILESIZE=$(run_stcmd stat -c %s "$FILENAME")
TARGET_SIZE=$((64 * 1024))

if [ "$FILESIZE" -gt "$TARGET_SIZE" ]; then
    echo "The firmware image exceeds 64 KB after build." >&2
    exit 2
fi

run_stcmd truncate -s "$TARGET_SIZE" "$FILENAME"

echo "Creating $TARGET_FIRMWARE"
python firmware.py --input="$FILENAME" --output="$TARGET_FIRMWARE" --array_name=target_firmware

cp "$TARGET_FIRMWARE" "../../rp/src/include/$TARGET_FIRMWARE"
echo "Copied $TARGET_FIRMWARE to rp/src/include/$TARGET_FIRMWARE"

rm "$TARGET_FIRMWARE"
echo "Removed temporary $TARGET_FIRMWARE"
