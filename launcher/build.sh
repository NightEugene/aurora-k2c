#!/bin/sh
set -eu
TASK_LAUNCHER_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$TASK_LAUNCHER_DIR/build-rpm.sh"
