# kea-cb-cmds-hook

Clean-room implementation of Kea's Configuration Backend Commands hook. Builds
`libdhcp_cb_cmds.so`, implementing the public `remote-*` command surface
documented in the Kea ARM.

## Status

- Target API: Kea 3.0.0 Configuration Backend Commands documentation.
- Developed and tested against: Kea 3.0 and 3.1.
- Build modes: standalone Meson against installed Kea development packages
  (`kea-devel` from RHEL 10 and compatible distributions, or `isc-kea-devel`
  from ISC packages), or Kea source-tree overlay for full in-tree tests.
- License: MPL-2.0.

This project is not affiliated with or endorsed by Internet Systems
Consortium. Kea is a trademark of Internet Systems Consortium.

## Repository Layout

- `src/hooks/dhcp/cb_cmds/` contains the hook source, load tests, unit tests,
  DB-backed tests, and API coverage script.
- `meson.build` builds the hook against an installed Kea development package.
- `packaging/rpm/` contains the RPM spec for `kea-hooks-cb-cmds` and
  `isc-kea-hooks-cb-cmds`.
- `scripts/apply-overlay.sh` copies the hook into a Kea source tree and wires
  the Kea Meson parent file.
- `scripts/build-rpm-almalinux.sh` builds the RPM inside an AlmaLinux
  container using either distro `kea-*` packages or ISC `isc-kea-*` packages.
- `scripts/run-tests.sh` runs the recommended non-DB test set from a configured
  Kea build directory.
- `scripts/README.md` documents local test and ARM conformance harness setup.

## Build Against Installed Kea

No Kea source tree is required. Two package families are supported:

- Distro (`kea-*`, e.g. `kea-devel`): enable your distribution's Devel repo.
- ISC (`isc-kea-*`, e.g. `isc-kea-devel`): enable ISC's Cloudsmith
  [Kea 3.0 LTS repo](https://cloudsmith.io/~isc/repos/kea-3-0/groups/).

Install the matching Kea development package and the common build tools:

```sh
dnf install meson ninja-build gcc-c++ boost-devel log4cplus-devel openssl-devel libpq-devel mariadb-connector-c-devel

# Distro packages:
dnf install kea-devel

# ISC packages:
dnf install isc-kea-devel isc-kea-mysql isc-kea-pgsql
```

Then build the hook:

```sh
meson setup build-standalone -Dhooksdir=/usr/lib64/kea/hooks
meson compile -C build-standalone libdhcp_cb_cmds.so
meson install -C build-standalone
```

If Kea is installed outside the platform's standard include or library paths,
point Meson at those directories:

```sh
meson setup build-standalone \
  -Dhooksdir=/usr/lib64/kea/hooks \
  -Dkea_include_dir=/usr/include/kea \
  -Dkea_lib_dir=/usr/lib64
meson compile -C build-standalone libdhcp_cb_cmds.so
```

## Build RPM For RHEL 10 And Clones

The RPMs target RHEL 10 and compatible distributions. RPM builds use installed
Kea development packages, not a full Kea checkout.

There are two package-source modes:

- `KEA_PACKAGE_SOURCE=distro` builds against distro `kea-*` packages and
  produces `kea-hooks-cb-cmds`.
- `KEA_PACKAGE_SOURCE=isc` builds against ISC `isc-kea-*` packages and produces
  `isc-kea-hooks-cb-cmds`.

The helper builds in an AlmaLinux 10 container. Distro builds enable AlmaLinux's
CPU-specific Devel repo (where `kea-devel` lives): `x86_64` by default, or the
`x86_64_v2` repo when `CPU_BASELINE=x86_64_v2`.

```sh
# Default: ISC Kea 3.0 LTS packages in an AlmaLinux 10 container.
./scripts/build-rpm-almalinux.sh
```

Override via environment variables:

- `KEA_PACKAGE_SOURCE` — `isc` (default) or `distro`.
- `ISC_KEA_REPO` — ISC repo to use, e.g. `kea-3-0` (default) or `kea-dev`.
- `CPU_BASELINE` — `x86_64` (default) or `x86_64_v2`.
- `CONTAINER_ENGINE` — e.g. `podman`.

The resulting RPMs are written below `rpmbuild/RPMS/`. The `x86_64_v2` build
uses an RPM release suffix of `.x86_64_v2` so both variants can be published
side by side.

## Apply To A Kea Source Tree

From this repository:

```sh
./scripts/apply-overlay.sh /path/to/kea
```

Overlay options (environment variables):

- `KEA_CB_CMDS_SKIP_DOCS=1` — drop Kea's `subdir('doc')` before configuring.
- `KEA_CB_CMDS_KEEP_SUDO=1` — keep Kea's Meson-time sudo-test detection
  (disabled by default).

Then configure Kea with tests enabled:

```sh
cd /path/to/kea
meson setup build -Dtests=enabled
meson compile -C build libdhcp_cb_cmds.so
meson test -C build \
  dhcp-cb-cmds-tests \
  dhcp-cb-cmds-libload-tests \
  dhcp-cb-cmds-api-coverage \
  kea-config-backend-tests \
  --print-errorlogs
```

For MySQL/PostgreSQL-backed integration coverage, configure Kea with DB support
and run:

```sh
KEA_CB_CMDS_DB_TESTS=1 KEA_TEST_DB_WIPE_DATA_ONLY=false \
  meson test -C build-db dhcp-cb-cmds-db-tests --print-errorlogs
```

The DB test suite expects reachable Kea test databases using Kea's existing DB
test infrastructure.

For local leak/undefined-behavior checks, configure a sanitizer build and run
the cb_cmds C++ tests through the helper:

```sh
meson setup build-asan \
  -Dtests=enabled \
  -Dcpp_std=c++17 \
  -Db_sanitize=address,undefined

./scripts/run-tests.sh --leak-check build-asan
```

`--leak-check` sets `ASAN_OPTIONS`, `LSAN_OPTIONS`, and `UBSAN_OPTIONS` so any
finding fails the run, and scopes it to the cb_cmds test binaries (not the full
Kea suite). macOS lacks LeakSanitizer support, so use a Linux build for real
leak checks.

CI uploads sanitizer command output and Meson logs as the `sanitizer-results`
artifact.

## macOS/Homebrew Notes

Useful dependencies for a local Kea test build:

```sh
brew install meson ninja log4cplus googletest bison mysql-client libpq
export PATH="/opt/homebrew/opt/mysql-client/bin:/opt/homebrew/opt/libpq/bin:/opt/homebrew/opt/bison/bin:$PATH"
export PKG_CONFIG_PATH="/opt/homebrew/opt/mysql-client/lib/pkgconfig:/opt/homebrew/opt/libpq/lib/pkgconfig:$PKG_CONFIG_PATH"
```

## ARM Conformance Output

Clean local run of `scripts/arm_conformance.py` passes against both MySQL and
PostgreSQL config backends:

Set `KEA_CB_CMDS_ARM_CONFORMANCE_RUN_DIR=/path/to/results` to preserve daemon
logs, generated configs, PID files, lock files, and lease files. CI uploads
these files plus the conformance transcript as ARM conformance artifacts.

```text
=== DHCPv4 ===
  PASS server-set                                   result=0    server set
  PASS server-get  [ARM servers-list form]          result=0    server returned
  PASS server-get-all                               result=0    servers returned
  PASS subnet-set [id+relay]                        result=0    subnet set
  PASS subnet-get-by-id                             result=0    subnet returned
  PASS subnet-get-by-prefix                         result=0    subnet returned
  PASS subnet-list                                  result=0    subnets returned
  PASS global-parameter-set                         result=0    global parameters set
  PASS global-parameter-get                         result=0    global parameter returned
  PASS global-parameter-get-all                     result=0    global parameters returned
  PASS network-set                                  result=0    shared network set
  PASS network-get                                  result=0    shared network returned
  PASS network-list                                 result=0    shared networks returned
  PASS option-def-set [ARM: no array/record-types/encapsulate] result=0    option definition set
  PASS option-def-get                               result=0    option definition returned
  PASS option-def-get-all                           result=0    option definitions returned
  PASS option-global-set                            result=0    option set
  PASS option-global-get                            result=0    option returned
  PASS option-global-get-all                        result=0    options returned
  PASS option-subnet-set                            result=0    option set
  PASS option-pool-set                              result=0    option set
  PASS option-network-set                           result=0    option set
  PASS daemon serves CB subnet                      served=True
  PASS server-del [ARM servers-list form]           result=0    server deleted

=== DHCPv6 ===
  PASS server-set                                   result=0    server set
  PASS server-get  [ARM servers-list form]          result=0    server returned
  PASS server-get-all                               result=0    servers returned
  PASS subnet-set [id+relay]                        result=0    subnet set
  PASS subnet-get-by-id                             result=0    subnet returned
  PASS subnet-get-by-prefix                         result=0    subnet returned
  PASS subnet-list                                  result=0    subnets returned
  PASS global-parameter-set                         result=0    global parameters set
  PASS global-parameter-get                         result=0    global parameter returned
  PASS global-parameter-get-all                     result=0    global parameters returned
  PASS network-set                                  result=0    shared network set
  PASS network-get                                  result=0    shared network returned
  PASS network-list                                 result=0    shared networks returned
  PASS option-def-set [ARM: no array/record-types/encapsulate] result=0    option definition set
  PASS option-def-get                               result=0    option definition returned
  PASS option-def-get-all                           result=0    option definitions returned
  PASS option-global-set                            result=0    option set
  PASS option-global-get                            result=0    option returned
  PASS option-global-get-all                        result=0    options returned
  PASS option-subnet-set                            result=0    option set
  PASS option-pool-set                              result=0    option set
  PASS option-network-set                           result=0    option set
  PASS option-pd-pool-set                           result=0    option set
  PASS daemon serves CB subnet                      served=True
  PASS server-del [ARM servers-list form]           result=0    server deleted

=== SUMMARY ===
checks: 49   passed: 49   DEVIATIONS: 0
```
