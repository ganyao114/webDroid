#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BASE="${1:-$ROOT/third_party/aosp17}"
TAG="android-17.0.0_r1"
URL="https://android.googlesource.com"
mkdir -p "$BASE/system" "$BASE/external" "$BASE/frameworks"

clone_full() {
  local repo="$1" dst="$2"
  if [ ! -d "$BASE/$dst/.git" ]; then
    git clone --depth=1 --branch "$TAG" --single-branch "$URL/$repo" "$BASE/$dst"
  fi
}

clone_sparse() {
  local repo="$1" dst="$2"
  shift 2
  if [ ! -d "$BASE/$dst/.git" ]; then
    git clone --depth=1 --branch "$TAG" --single-branch --filter=blob:none --no-checkout "$URL/$repo" "$BASE/$dst"
    git -C "$BASE/$dst" sparse-checkout init --cone
    git -C "$BASE/$dst" sparse-checkout set "$@"
    git -C "$BASE/$dst" checkout "$TAG"
  fi
}

clone_full platform/system/libbase system/libbase &
clone_full platform/system/logging system/logging &
clone_full platform/system/libziparchive system/libziparchive &
clone_full platform/system/incremental_delivery system/incremental_delivery &
clone_full platform/system/libartpalette system/libartpalette &
clone_full platform/external/zlib external/zlib &
clone_full platform/external/fmtlib external/fmtlib &
clone_sparse platform/bionic bionic libc/kernel/uapi/linux/android libc/kernel/uapi/asm-generic libc/include/android libc/include/sys &
clone_sparse platform/system/core system/core libcutils libutils include libprocessgroup libsystem &
clone_sparse platform/frameworks/native frameworks/native libs/binder cmds/servicemanager include headers &
clone_sparse platform/frameworks/base frameworks/base libs/androidfw &
clone_sparse platform/art art libartbase libdexfile compiler runtime &
wait
echo "Fetched minimal AOSP 17 ($TAG) components into $BASE"
