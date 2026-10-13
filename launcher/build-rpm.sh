#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SDK=${AURORA_SDK_ROOT:-"$HOME/.local/share/aurora-sdk/sdk/5.2.1.200"}
if [ ! -f "$ROOT/launcher/rpm-source/runtime/lib/arm64-v8a/libunity.so" ]; then
    echo 'Prepare the runtime first: python3 launcher/prepare-runtime.py /path/to/game.apk' >&2
    exit 1
fi
export WORKSPACE_DIR="$ROOT"
mkdir -p "$ROOT/launcher/rpm-source/native"
cp "$ROOT/launcher/main.cpp" "$ROOT/launcher/rpm-source/main.cpp"
cp "$ROOT/launcher/Launcher.qml" "$ROOT/launcher/rpm-source/Launcher.qml"
cp "$ROOT/shim/src/"*.h "$ROOT/launcher/rpm-source/native/"
for source in m3 jni_fake hooks k2c_audio k2c_display k2c_window xdg_shell_min android_compat android_compat_library; do
    cp "$ROOT/shim/src/$source.c" "$ROOT/launcher/rpm-source/native/"
done
if [ -n "${AURORA_BUILD_IMAGE:-}" ]; then
    docker run --rm -v "$ROOT:/workspace" -w /workspace "$AURORA_BUILD_IMAGE" \
        apptool build --arm64 \
        --specfile=/workspace/launcher/rpm/ru.nighteugene.k2clauncher.spec \
        --srcdir=/workspace/launcher/rpm-source \
        --dstdir=/workspace/launcher/build-rpm
else
    VAR_SPECFILE="$ROOT/launcher/rpm/ru.nighteugene.k2clauncher.spec" \
        "$SDK/tools/apptool" build --arm64 \
        --srcdir=/workspace/launcher/rpm-source \
        --dstdir=/workspace/launcher/build-rpm
fi
