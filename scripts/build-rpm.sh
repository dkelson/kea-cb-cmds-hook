#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
REPO_DIR=$(CDPATH= cd "$SCRIPT_DIR/.." && pwd)
SPEC_FILE="$REPO_DIR/packaging/rpm/isc-kea-hooks-cb-cmds.spec"
KEA_PACKAGE_SOURCE=${KEA_PACKAGE_SOURCE:-isc}

if ! command -v rpmbuild >/dev/null 2>&1; then
    echo "rpmbuild is required" >&2
    exit 1
fi

case "$KEA_PACKAGE_SOURCE" in
    distro)
        NAME=kea-hooks-cb-cmds
        ;;
    isc)
        NAME=isc-kea-hooks-cb-cmds
        ;;
    *)
        echo "KEA_PACKAGE_SOURCE must be distro or isc" >&2
        exit 2
        ;;
esac

default_version() {
    if [ -f "$REPO_DIR/VERSION" ]; then
        tr -d '[:space:]' < "$REPO_DIR/VERSION"
        return
    fi

    awk '
        /^%{!\?cb_cmds_version:/ {
            gsub(/[}]/, "")
            print $NF
            exit
        }
        /^Version:/ {
            print $2
            exit
        }
    ' "$SPEC_FILE"
}

if [ -z "${VERSION:-}" ]; then
    TAG=
    if git -C "$REPO_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        TAG=$(git -C "$REPO_DIR" describe --exact-match --tags HEAD 2>/dev/null || true)
    fi

    case "$TAG" in
        v[0-9]*)
            VERSION=${TAG#v}
            ;;
        *)
            VERSION=$(default_version)
            ;;
    esac
fi

TOPDIR=${RPM_TOPDIR:-"$REPO_DIR/rpmbuild"}
ARCHIVE="$TOPDIR/SOURCES/$NAME-$VERSION.tar.gz"
CLEANUP_DIR=

cleanup() {
    if [ -n "$CLEANUP_DIR" ] && [ -d "$CLEANUP_DIR" ]; then
        rm -rf "$CLEANUP_DIR"
    fi
}

trap cleanup EXIT INT TERM

for dir in BUILD BUILDROOT RPMS SOURCES SPECS SRPMS; do
    mkdir -p "$TOPDIR/$dir"
done

create_archive_from_worktree() {
    CLEANUP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/$NAME-$VERSION.XXXXXX")
    mkdir -p "$CLEANUP_DIR/$NAME-$VERSION"

    tar \
        --exclude './.git' \
        --exclude './build' \
        --exclude './build-*' \
        --exclude './rpmbuild' \
        --exclude './*.rpm' \
        --exclude './*.src.rpm' \
        -cf - \
        -C "$REPO_DIR" . | tar -xf - -C "$CLEANUP_DIR/$NAME-$VERSION"

    tar -czf "$ARCHIVE" -C "$CLEANUP_DIR" "$NAME-$VERSION"
}

if git -C "$REPO_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    if ! git -C "$REPO_DIR" diff --quiet --ignore-submodules HEAD; then
        echo "warning: uncommitted tracked changes are not included in the source archive" >&2
    fi

    if [ -n "$(git -C "$REPO_DIR" ls-files --others --exclude-standard)" ]; then
        echo "warning: untracked files are not included in the source archive" >&2
    fi

    git -C "$REPO_DIR" archive \
        --format=tar.gz \
        --prefix="$NAME-$VERSION/" \
        --output="$ARCHIVE" \
        HEAD
else
    echo "warning: git metadata unavailable; archiving current working tree" >&2
    create_archive_from_worktree
fi

cp "$SPEC_FILE" "$TOPDIR/SPECS/"

if [ "${CPU_BASELINE:-x86_64}" = "x86_64_v2" ]; then
    rpmbuild -ba "$TOPDIR/SPECS/$(basename "$SPEC_FILE")" \
        --define "_topdir $TOPDIR" \
        --define "cb_cmds_package_name $NAME" \
        --define "cb_cmds_version $VERSION" \
        --define "cb_cmds_x86_64_v2 1" \
        --define "cb_cmds_release_suffix .x86_64_v2" \
        "$@"
else
    rpmbuild -ba "$TOPDIR/SPECS/$(basename "$SPEC_FILE")" \
        --define "_topdir $TOPDIR" \
        --define "cb_cmds_package_name $NAME" \
        --define "cb_cmds_version $VERSION" \
        "$@"
fi
