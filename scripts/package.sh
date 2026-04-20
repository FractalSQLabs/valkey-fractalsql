#!/usr/bin/env bash
#
# scripts/package.sh — valkey-fractalsql packaging.
#
# Assumes ./build.sh ${ARCH} has produced:
#   dist/${ARCH}/fractalsql.so
#
# Emits one .deb and one .rpm per arch into dist/packages/:
#   dist/packages/valkey-fractalsql-amd64.deb
#   dist/packages/valkey-fractalsql-amd64.rpm
#   dist/packages/valkey-fractalsql-arm64.deb
#   dist/packages/valkey-fractalsql-arm64.rpm
#
# One binary covers Valkey 7.2 / 8.0 / 8.1 — the Modules ABI
# (REDISMODULE_APIVER_1 / VALKEYMODULE_APIVER_1) was preserved across
# the April 2024 fork from Redis, so the package depends on
# valkey-server / valkey generically rather than pinning a major.
#
# Usage:
#   scripts/package.sh [amd64|arm64]     # default: amd64

set -euo pipefail

cd "$(dirname "$0")/.."

VERSION="1.0.0"
ITERATION="1"
DIST_DIR="dist/packages"
PKG_NAME="valkey-fractalsql"
mkdir -p "${DIST_DIR}"

# Absolute repo root, captured before any -C chdir'd fpm invocation.
REPO_ROOT="$(pwd)"
for f in LICENSE LICENSE-THIRD-PARTY; do
    if [ ! -f "${REPO_ROOT}/${f}" ]; then
        echo "missing ${REPO_ROOT}/${f} — refusing to package without it" >&2
        exit 1
    fi
done

PKG_ARCH="${1:-amd64}"
case "${PKG_ARCH}" in
    amd64|arm64) ;;
    *)
        echo "unknown arch '${PKG_ARCH}' — expected amd64 or arm64" >&2
        exit 2
        ;;
esac

case "${PKG_ARCH}" in
    amd64) RPM_ARCH="x86_64"  ;;
    arm64) RPM_ARCH="aarch64" ;;
esac

SO="dist/${PKG_ARCH}/fractalsql.so"
if [ ! -f "${SO}" ]; then
    echo "missing ${SO} — run ./build.sh ${PKG_ARCH} first" >&2
    exit 1
fi

DEB_OUT="${DIST_DIR}/${PKG_NAME}-${PKG_ARCH}.deb"
RPM_OUT="${DIST_DIR}/${PKG_NAME}-${PKG_ARCH}.rpm"

# Build a staging root that mirrors the on-disk layout so fpm can
# just tar it up.
#
# LICENSE ledger: staged into /usr/share/doc/<pkg>/ via install -Dm0644
# BEFORE running fpm. Explicit fpm src=dst mappings break here — fpm's
# -C chroots absolute source paths too, so ${REPO_ROOT}/LICENSE gets
# resolved as ${STAGE}${REPO_ROOT}/LICENSE and fpm bails with
# "Cannot chdir to ...".
STAGE="$(mktemp -d)"
trap 'rm -rf "${STAGE}"' EXIT

install -Dm0755 "${SO}" \
    "${STAGE}/usr/lib/valkey/modules/fractalsql.so"
install -Dm0644 "${REPO_ROOT}/scripts/load_module.conf" \
    "${STAGE}/etc/valkey/modules-available/fractalsql.conf"
install -Dm0644 "${REPO_ROOT}/LICENSE" \
    "${STAGE}/usr/share/doc/${PKG_NAME}/LICENSE"
install -Dm0644 "${REPO_ROOT}/LICENSE-THIRD-PARTY" \
    "${STAGE}/usr/share/doc/${PKG_NAME}/LICENSE-THIRD-PARTY"

echo "------------------------------------------"
echo "Packaging ${PKG_NAME} (${PKG_ARCH})"
echo "------------------------------------------"

DESC="FractalSQL: Stochastic Fractal Search module for Valkey 7.2 / 8.0 / 8.1"

# LuaJIT is statically linked into fractalsql.so — no libluajit-5.1-2
# (Debian) or luajit (RPM) runtime dependency is declared.
#
# Depends on valkey-server (Debian-family) / valkey (RHEL-family).
# Valkey is a drop-in Redis replacement but the upstream package name
# differs — this repo targets the Valkey-native package name rather
# than the redis-server compat alias some distros ship.
#
# The post-install hint advises the operator to add the loadmodule
# directive to valkey.conf. We deliberately do NOT auto-edit the
# server config — silently rewriting the operator's valkey.conf is
# an unwelcome surprise on a shared host.
fpm -s dir -t deb \
    -n "${PKG_NAME}" \
    -v "${VERSION}" \
    -a "${PKG_ARCH}" \
    --iteration "${ITERATION}" \
    --description "${DESC}" \
    --license "MIT" \
    --depends "libc6 (>= 2.38)" \
    --depends "valkey-server" \
    --config-files /etc/valkey/modules-available/fractalsql.conf \
    --after-install "${REPO_ROOT}/packaging/debian/postinst" \
    -C "${STAGE}" \
    -p "${DEB_OUT}" \
    usr etc

fpm -s dir -t rpm \
    -n "${PKG_NAME}" \
    -v "${VERSION}" \
    -a "${RPM_ARCH}" \
    --iteration "${ITERATION}" \
    --description "${DESC}" \
    --license "MIT" \
    --depends "valkey" \
    --config-files /etc/valkey/modules-available/fractalsql.conf \
    --after-install "${REPO_ROOT}/packaging/debian/postinst" \
    -C "${STAGE}" \
    -p "${RPM_OUT}" \
    usr etc

rm -rf "${STAGE}"
trap - EXIT

echo
echo "Done. Packages in ${DIST_DIR}:"
ls -l "${DIST_DIR}"
