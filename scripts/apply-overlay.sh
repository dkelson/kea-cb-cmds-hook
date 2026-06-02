#!/usr/bin/env sh
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /path/to/kea" >&2
    exit 2
fi

KEA_TREE=$1
if [ ! -f "$KEA_TREE/src/hooks/dhcp/meson.build" ]; then
    echo "not a Kea source tree: $KEA_TREE" >&2
    exit 1
fi

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
REPO_DIR=$(CDPATH= cd "$SCRIPT_DIR/.." && pwd)

mkdir -p "$KEA_TREE/src/hooks/dhcp"
rm -rf "$KEA_TREE/src/hooks/dhcp/cb_cmds"
cp -R "$REPO_DIR/src/hooks/dhcp/cb_cmds" "$KEA_TREE/src/hooks/dhcp/cb_cmds"

python3 - "$KEA_TREE/src/hooks/dhcp/meson.build" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()

if "subdir('cb_cmds')" not in text:
    marker = "subdir('bootp')\n"
    if marker not in text:
        raise SystemExit("could not find bootp insertion marker")
    text = text.replace(marker, marker + "subdir('cb_cmds')\n", 1)

if "subdir('cb_cmds/dbtests')" not in text:
    marker = "subdir('pgsql')\n"
    if marker not in text:
        raise SystemExit("could not find pgsql insertion marker")
    text = text.replace(marker, marker + "subdir('cb_cmds/dbtests')\n", 1)

path.write_text(text)
PY

if [ "${KEA_CB_CMDS_SKIP_DOCS:-0}" = "1" ]; then
    python3 - "$KEA_TREE/meson.build" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
text = text.replace("subdir('doc')\n", "", 1)
path.write_text(text)
PY
fi

if [ "${KEA_CB_CMDS_KEEP_SUDO:-0}" != "1" ]; then
    python3 - "$KEA_TREE/meson.build" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
text = text.replace("SUDO = find_program('sudo', required: false)\n", "SUDO = disabler()\n", 1)
path.write_text(text)
PY
fi

echo "cb_cmds overlay applied to $KEA_TREE"
