#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)

usage() {
    cat <<USAGE
Usage: $0 [board_type] [build_type] <app_uuid_key>

Arguments:
  board_type   Optional. Defaults to pico_w. Supported: pico, pico_w
  build_type   Optional. Defaults to release. Supported: release, debug
  app_uuid_key Required. UUID that matches the entry in desc/app.json

Examples:
  $0 pico release 123e4567-e89b-12d3-a456-426614174000
  $0 pico_w debug 5e8d2c5c-7caa-4b64-aa8f-7aaf84c1c111
  $0 5e8d2c5c-7caa-4b64-aa8f-7aaf84c1c111   # uses defaults pico_w/release
USAGE
}

BOARD_TYPE=${1:-pico_w}
BUILD_TYPE=${2:-release}
APP_UUID_KEY=${3:-}

# If only two args provided assume they meant board/build+uuid; if only uuid
# was provided, shift the parameters down.
if [[ -z "$APP_UUID_KEY" ]]; then
    if [[ "$BOARD_TYPE" =~ ^[0-9a-fA-F-]{36}$ ]] && [[ $# -eq 1 ]]; then
        APP_UUID_KEY=$BOARD_TYPE
        BOARD_TYPE=pico_w
        BUILD_TYPE=release
    elif [[ "$BUILD_TYPE" =~ ^[0-9a-fA-F-]{36}$ ]] && [[ $# -eq 2 ]]; then
        APP_UUID_KEY=$BUILD_TYPE
        BUILD_TYPE=release
    else
        usage
        exit 1
    fi
fi

case "$BOARD_TYPE" in
  pico|pico_w)
    ;;
  *)
    echo "Unsupported board type '$BOARD_TYPE'. Use pico or pico_w." >&2
    exit 1
    ;;
esac

BUILD_TYPE_LOWER=$(echo "$BUILD_TYPE" | tr '[:upper:]' '[:lower:]')
case "$BUILD_TYPE_LOWER" in
  release|debug)
    ;;
  *)
    echo "Unsupported build type '$BUILD_TYPE'. Use release or debug." >&2
    exit 1
    ;;
esac

if [[ ! -f "$SCRIPT_DIR/version.txt" ]]; then
    echo "version.txt not found next to build.sh" >&2
    exit 1
fi

GIT_ROOT=$SCRIPT_DIR
git -C "$GIT_ROOT" submodule update --init --recursive >/dev/null

echo "Copy version.txt to component projects"
cp "$SCRIPT_DIR/version.txt" "$SCRIPT_DIR/rp/"
cp "$SCRIPT_DIR/version.txt" "$SCRIPT_DIR/target/"

VERSION=$(tr -d '\r\n ' < "$SCRIPT_DIR/version.txt")
echo "Version: $VERSION"
echo "Board type: $BOARD_TYPE"
echo "Build type: $BUILD_TYPE_LOWER"
echo "App UUID Key: $APP_UUID_KEY"

DIST_DIR="$SCRIPT_DIR/dist"
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

echo "Building Atari ST target payload"
(
  cd "$SCRIPT_DIR/target/atarist"
  ./build.sh "$SCRIPT_DIR/target/atarist" release
)

echo "Building RP firmware"
(
  cd "$SCRIPT_DIR/rp"
  ./build.sh "$BOARD_TYPE" "$BUILD_TYPE_LOWER"
  if [ "$BUILD_TYPE_LOWER" = "release" ]; then
    cp "./dist/rp-$BOARD_TYPE.uf2" "$DIST_DIR/rp.uf2"
  else
    cp "./dist/rp-$BOARD_TYPE-$BUILD_TYPE_LOWER.uf2" "$DIST_DIR/rp.uf2"
  fi
)

BINARY_MD5=$(python3 - <<'PY'
import hashlib, pathlib, sys
path = pathlib.Path(sys.argv[1])
data = path.read_bytes()
print(hashlib.md5(data).hexdigest())
PY "$DIST_DIR/rp.uf2")

echo "$BINARY_MD5  rp.uf2" > "$DIST_DIR/rp.uf2.md5sum"

APP_JSON_TEMPLATE="$SCRIPT_DIR/desc/app.json"
if [ ! -f "$APP_JSON_TEMPLATE" ]; then
    echo "app.json file not found in the desc directory. Please create one." >&2
    exit 1
fi

FINAL_UF2="$DIST_DIR/$APP_UUID_KEY-$VERSION.uf2"
mv "$DIST_DIR/rp.uf2" "$FINAL_UF2"
cp "$APP_JSON_TEMPLATE" "$DIST_DIR/app.json"

# Portable replacements (sed -i differences) handled via temp file
replace_token() {
  local token=$1
  local value=$2
  python3 - "$DIST_DIR/app.json" "$token" "$value" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
token, value = sys.argv[2], sys.argv[3]
data = path.read_text()
path.write_text(data.replace(token, value))
PY
}

replace_token "<APP_UUID>" "$APP_UUID_KEY"
replace_token "<BINARY_MD5_HASH>" "$BINARY_MD5"
replace_token "<APP_VERSION>" "$VERSION"

FINAL_JSON="$DIST_DIR/$APP_UUID_KEY.json"
mv "$DIST_DIR/app.json" "$FINAL_JSON"

cat <<SUMMARY
Build completed successfully.
  UF2 : $(basename "$FINAL_UF2")
  JSON: $(basename "$FINAL_JSON")
  MD5 : $BINARY_MD5
SUMMARY

exit 0
