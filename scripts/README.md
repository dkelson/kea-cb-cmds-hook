# Scripts

This directory contains helper scripts for building and testing the
`libdhcp_cb_cmds.so` hook.

## run-tests.sh

`run-tests.sh` runs the recommended local Meson tests from a configured Kea
source-tree overlay build:

```sh
./scripts/run-tests.sh /path/to/kea/build
```

Default coverage:

- `dhcp-cb-cmds-tests`
- `dhcp-cb-cmds-libload-tests`
- `dhcp-cb-cmds-api-coverage`
- `kea-config-backend-tests`

Optional database-backed tests:

```sh
KEA_CB_CMDS_DB_TESTS=1 KEA_TEST_DB_WIPE_DATA_ONLY=false \
  ./scripts/run-tests.sh /path/to/kea/build-db
```

Optional sanitizer run:

```sh
./scripts/run-tests.sh --leak-check /path/to/kea/build-asan
```

On Linux, `--leak-check` enables LeakSanitizer. On macOS, Apple AddressSanitizer
does not support LeakSanitizer, so the helper runs ASan/UBSan checks with leak
detection disabled.

If `CB_PG_PASSWORD` is set, or `KEA_CB_CMDS_ARM_CONFORMANCE=1` is set,
`run-tests.sh` also invokes `arm_conformance.py`.

## arm_conformance.py

`arm_conformance.py` is a clean-room ARM conformance harness. It starts isolated
`kea-dhcp4` and `kea-dhcp6` processes, wires them to a PostgreSQL configuration
backend, loads `libdhcp_pgsql.so` and `libdhcp_cb_cmds.so`, drives the ARM 3.0
canonical request shapes, tears down rows it creates, and exits non-zero on any
deviation.

The harness writes daemon PID files, lock files, logs, generated configs, and
memfile leases to a run directory. By default this is a temporary directory
under `/tmp` and is removed when the run exits. Set
`KEA_CB_CMDS_ARM_CONFORMANCE_RUN_DIR=/path/to/results` to preserve those files.

Required environment:

- `CB_PG_PASSWORD`

Common overrides:

- `KEA_BIN4`, `KEA_BIN6` — Kea daemon binaries.
- `KEA_HOOKS_DIR` — directory containing both `libdhcp_pgsql.so` and
  `libdhcp_cb_cmds.so`.
- `CB_PG_HOST`, `CB_PG_PORT`, `CB_PG_NAME`, `CB_PG_USER` — PostgreSQL
  connection settings.
- `CB_PORT4`, `CB_PORT6` — HTTP control socket ports for the temporary daemons.
- `KEA_CB_CMDS_ARM_CONFORMANCE_RUN_DIR` — run directory to preserve daemon
  logs, generated configs, PID files, lock files, and lease files.

Example with an existing PostgreSQL Kea config-backend database:

```sh
CB_PG_PASSWORD=secret \
KEA_BIN4=/path/to/kea-dhcp4 \
KEA_BIN6=/path/to/kea-dhcp6 \
KEA_HOOKS_DIR=/path/to/kea/hooks \
python3 scripts/arm_conformance.py
```

Example disposable PostgreSQL setup with Podman:

```sh
podman run -d --rm --name kea-cb-cmds-pg \
  -e POSTGRES_USER=kea \
  -e POSTGRES_PASSWORD=kea \
  -e POSTGRES_DB=kea \
  -p 127.0.0.1:55432:5432 \
  postgres:18-alpine

# Wait until PostgreSQL accepts connections, then initialize the Kea schema.
kea-admin db-init pgsql \
  -h 127.0.0.1 \
  -P 55432 \
  -u kea \
  -p kea \
  -n kea \
  -d /path/to/kea/database/scripts

CB_PG_HOST=127.0.0.1 \
CB_PG_PORT=55432 \
CB_PG_NAME=kea \
CB_PG_USER=kea \
CB_PG_PASSWORD=kea \
KEA_BIN4=/path/to/kea-dhcp4 \
KEA_BIN6=/path/to/kea-dhcp6 \
KEA_HOOKS_DIR=/path/to/kea/hooks \
python3 scripts/arm_conformance.py

podman stop kea-cb-cmds-pg
```

`KEA_HOOKS_DIR` must contain both hook libraries. If they are built in different
directories, create a temporary directory and symlink or copy both `.so` files
there before running the harness.
