#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DEVICE=${1:-daq}
"$ROOT/launcher/build-rpm.sh"
TASK_VERSION=$(sed -n 's/^Version: //p' "$ROOT/launcher/rpm/ru.nighteugene.k2clauncher.spec")
TASK_RELEASE=$(sed -n 's/^Release: //p' "$ROOT/launcher/rpm/ru.nighteugene.k2clauncher.spec")
RPM="$ROOT/launcher/build-rpm/RPMS/ru.nighteugene.k2clauncher-$TASK_VERSION-$TASK_RELEASE.aarch64.rpm"
test -f "$RPM"
ssh -F "$HOME/.ssh/config" "$DEVICE" 'test "$(id -un)" = defaultuser && test "$(uname -m)" = aarch64'
scp -F "$HOME/.ssh/config" "$RPM" "$DEVICE:/home/defaultuser/Downloads/"
ssh -F "$HOME/.ssh/config" "$DEVICE" 'pkill -TERM -f "^/usr/bin/ru.nighteugene.k2clauncher$" || true'
ssh -F "$HOME/.ssh/config" "$DEVICE" 'sdk-deploy-rpm --silent --keepUserData ~/Downloads/'"$(basename "$RPM")"
