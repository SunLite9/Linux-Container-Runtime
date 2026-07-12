#!/usr/bin/env bash
# Downloads the official Alpine minirootfs tarball and extracts it to
# ./rootfs/alpine, for use as a container root filesystem.
# Superseded by the registry client (src/registry/), which pulls
# arbitrary images from Docker Hub instead of just Alpine; kept here as
# the original bootstrap path.
set -euo pipefail

ARCH="x86_64"
INDEX_URL="https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/${ARCH}/"
DEST_DIR="rootfs/alpine"

echo "Finding latest Alpine minirootfs tarball for ${ARCH}..."
TARBALL=$(curl -fsSL "$INDEX_URL" | grep -o "alpine-minirootfs-[0-9.]*-${ARCH}\.tar\.gz" | sort -u | tail -1)

if [[ -z "$TARBALL" ]]; then
    echo "Could not find a minirootfs tarball at $INDEX_URL" >&2
    exit 1
fi

echo "Downloading $TARBALL..."
curl -fsSL -o "/tmp/${TARBALL}" "${INDEX_URL}${TARBALL}"

echo "Extracting to ${DEST_DIR}..."
mkdir -p "$DEST_DIR"
tar -xzf "/tmp/${TARBALL}" -C "$DEST_DIR"
rm -f "/tmp/${TARBALL}"

echo "Alpine rootfs ready at ${DEST_DIR}"
