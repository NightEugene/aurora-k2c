#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SDK=${AURORA_SDK_ROOT:-"$HOME/.local/share/aurora-sdk/sdk/5.2.1.200"}
if [ "$#" -ne 1 ]; then
    echo 'Usage: sh launcher/build-builtin-rpm.sh /path/to/compatible-game.apk' >&2
    exit 1
fi
export WORKSPACE_DIR="$ROOT"
SOURCE="$ROOT/launcher/builtin-source"
mkdir -p "$SOURCE/native" "$SOURCE/game"
# Prepare the native runtime from the same APK that is embedded below.
# Validate it before replacing the previous generated source directory.
TASK_RUNTIME=$(mktemp -d "$ROOT/launcher/.builtin-runtime-XXXXXX")
trap 'rm -rf "$TASK_RUNTIME"' EXIT HUP INT TERM
python3 "$ROOT/launcher/prepare-runtime.py" "$1" --output "$TASK_RUNTIME/runtime"
rm -rf "$SOURCE/runtime"
mv "$TASK_RUNTIME/runtime" "$SOURCE/runtime"
cp "$ROOT/launcher/main.cpp" "$SOURCE/main.cpp"
cp "$ROOT/launcher/Game.qml" "$SOURCE/Game.qml"
cp "$ROOT/launcher/game.qrc" "$SOURCE/game.qrc"
cp "$ROOT/launcher/rpm-source/ru.nighteugene.k2clauncher.pro" "$SOURCE/ru.nighteugene.kingdom.pro"
cp "$ROOT/launcher/rpm-source/hybris_bridge.c" "$SOURCE/hybris_bridge.c"
for size in 86x86 108x108 128x128 172x172; do
    mkdir -p "$SOURCE/icons/$size"
    cp "$ROOT/launcher/rpm-source/icons/$size/ru.nighteugene.k2clauncher.png" "$SOURCE/icons/$size/ru.nighteugene.kingdom.png"
done
sed -e 's/ru.nighteugene.k2clauncher/ru.nighteugene.kingdom/g' -e 's/^Name=.*/Name=Kingdom/' -e 's/^Name\[ru\]=.*/Name[ru]=Kingdom/' -e 's/^ApplicationName=.*/ApplicationName=kingdom/' -e 's/^Permissions=.*/Permissions=Audio;/' "$ROOT/launcher/rpm-source/ru.nighteugene.k2clauncher.desktop" > "$SOURCE/ru.nighteugene.kingdom.desktop"
cp "$ROOT/shim/src/"*.h "$SOURCE/native/"
for source in m3 jni_fake hooks k2c_audio k2c_display k2c_window xdg_shell_min android_compat android_compat_library; do
    cp "$ROOT/shim/src/$source.c" "$SOURCE/native/"
done
cp --reflink=auto "$1" "$SOURCE/game/base.apk"
if [ -n "${AURORA_BUILD_IMAGE:-}" ]; then
    docker run --rm -v "$ROOT:/workspace" -w /workspace "$AURORA_BUILD_IMAGE" \
        apptool build --arm64 \
        --specfile=/workspace/launcher/rpm/ru.nighteugene.kingdom.spec \
        --srcdir=/workspace/launcher/builtin-source \
        --dstdir=/workspace/launcher/build-builtin-rpm
else
    VAR_SPECFILE="$ROOT/launcher/rpm/ru.nighteugene.kingdom.spec" \
        "$SDK/tools/apptool" build --arm64 \
        --srcdir=/workspace/launcher/builtin-source \
        --dstdir=/workspace/launcher/build-builtin-rpm
fi
