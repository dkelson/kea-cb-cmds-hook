#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
REPO_DIR=$(CDPATH= cd "$SCRIPT_DIR/.." && pwd)
ALMALINUX_IMAGE=${ALMALINUX_IMAGE:-quay.io/almalinuxorg/almalinux:10}
ISC_KEA_REPO=${ISC_KEA_REPO:-kea-3-0}
KEA_PACKAGE_SOURCE=${KEA_PACKAGE_SOURCE:-isc}
CPU_BASELINE=${CPU_BASELINE:-x86_64}
CONTAINER_ENGINE=${CONTAINER_ENGINE:-podman}
CONTAINER_PLATFORM=${CONTAINER_PLATFORM:-}

if ! command -v "$CONTAINER_ENGINE" >/dev/null 2>&1; then
    echo "$CONTAINER_ENGINE is required; set CONTAINER_ENGINE=docker to use Docker" >&2
    exit 1
fi

case "$KEA_PACKAGE_SOURCE" in
    distro|isc) ;;
    *)
        echo "KEA_PACKAGE_SOURCE must be distro or isc" >&2
        exit 2
        ;;
esac

case "$CPU_BASELINE" in
    x86_64)
        ALMALINUX_DEVEL_ARCH=${ALMALINUX_DEVEL_ARCH:-x86_64}
        ;;
    x86_64_v2)
        ALMALINUX_DEVEL_ARCH=${ALMALINUX_DEVEL_ARCH:-x86_64_v2}
        ;;
    *)
        echo "CPU_BASELINE must be x86_64 or x86_64_v2" >&2
        exit 2
        ;;
esac

volume_suffix=
if [ "$CONTAINER_ENGINE" = "podman" ]; then
    volume_suffix=:Z
fi

platform_args=
if [ -n "$CONTAINER_PLATFORM" ]; then
    platform_args="--platform $CONTAINER_PLATFORM"
fi

# shellcheck disable=SC2086
"$CONTAINER_ENGINE" run --rm \
    $platform_args \
    --volume "$REPO_DIR:/work$volume_suffix" \
    --workdir /work \
    --env ISC_KEA_REPO="$ISC_KEA_REPO" \
    --env KEA_PACKAGE_SOURCE="$KEA_PACKAGE_SOURCE" \
    --env CPU_BASELINE="$CPU_BASELINE" \
    --env ALMALINUX_DEVEL_ARCH="$ALMALINUX_DEVEL_ARCH" \
    "$ALMALINUX_IMAGE" \
    /bin/bash -lc '
        set -euo pipefail
        dnf install -y dnf-plugins-core curl ca-certificates epel-release
        dnf config-manager --set-enabled crb
        cat >"/etc/yum.repos.d/almalinux-devel-${ALMALINUX_DEVEL_ARCH}.repo" <<EOF
[almalinux-devel-${ALMALINUX_DEVEL_ARCH}]
name=AlmaLinux 10 Devel ${ALMALINUX_DEVEL_ARCH}
baseurl=https://repo.almalinux.org/almalinux/10/devel/${ALMALINUX_DEVEL_ARCH}/os/
enabled=1
gpgcheck=1
countme=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-AlmaLinux-10
EOF
        if [ "$KEA_PACKAGE_SOURCE" = "isc" ]; then
            curl -1sLf "https://dl.cloudsmith.io/public/isc/${ISC_KEA_REPO}/setup.rpm.sh" | bash
            kea_devel_packages="isc-kea-devel isc-kea-mysql isc-kea-pgsql"
        else
            kea_devel_packages=kea-devel
        fi
        # shellcheck disable=SC2086
        dnf install -y \
            boost-devel \
            gcc-c++ \
            git \
            libpq-devel \
            mariadb-connector-c-devel \
            $kea_devel_packages \
            meson \
            ninja-build \
            pkgconf-pkg-config \
            redhat-rpm-config \
            rpm-build
        pkg-config --modversion kea || true
        if [ -d build-rpm-smoke ]; then
            meson setup --wipe build-rpm-smoke \
                -Dhooksdir=/usr/lib64/kea/hooks \
                -Dkea_include_dir=/usr/include/kea \
                -Dkea_lib_dir=/usr/lib64
        else
            meson setup build-rpm-smoke \
                -Dhooksdir=/usr/lib64/kea/hooks \
                -Dkea_include_dir=/usr/include/kea \
                -Dkea_lib_dir=/usr/lib64
        fi
        meson compile -C build-rpm-smoke
        ./scripts/build-rpm.sh
        rpm -qpl rpmbuild/RPMS/*/*kea-hooks-cb-cmds-*.rpm
    '
