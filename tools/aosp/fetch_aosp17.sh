#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BASE="${1:-$ROOT/third_party/aosp17}"
TAG="android-17.0.0_r1"
URL="https://android.googlesource.com"
mkdir -p "$BASE/system" "$BASE/external" "$BASE/frameworks" "$BASE/packages/apps" "$BASE/packages/modules"

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
clone_full platform/external/lz4 external/lz4 &
clone_full platform/libnativehelper libnativehelper &
clone_sparse platform/system/unwinding system/unwinding libunwindstack/include &
clone_sparse platform/libcore libcore ojluni/src/main/java/java/lang ojluni/src/main/java/java/util ojluni/src/main/java/java/io ojluni/src/main/java/java/nio ojluni/src/main/java/java/math ojluni/src/main/java/java/text ojluni/src/main/java/java/sql luni/src/main/java libart/src/main/java \
  dalvik/src/main/java/dalvik/annotation/compat ojluni/src/main/java/java/net ojluni/src/main/java/sun/util/calendar \
  ojluni/src/main/java/javax/net ojluni/src/main/java/sun/nio/fs &
clone_sparse platform/bionic bionic libc/kernel/uapi/linux/android libc/kernel/uapi/asm-generic libc/include/android libc/include/sys &
clone_sparse platform/system/core system/core libcutils libutils include libprocessgroup libsystem &
clone_sparse platform/frameworks/native frameworks/native libs/binder cmds/servicemanager include headers &
clone_sparse platform/frameworks/base frameworks/base libs/androidfw core/jni core/res graphics/java packages/SystemUI packages/SettingsLib libs/WindowManager/Shell core/java/android/app core/java/android/content core/java/android/graphics core/java/android/os core/java/android/util core/java/android/view core/java/android/widget core/java/android/text core/java/android/animation core/java/android/database core/java/android/net core/java/android/webkit core/java/com/android/internal/pm core/java/com/android/internal/util core/java/com/android/internal/policy core/java/com/android/internal/os services/core/java/com/android/server/pm services/core/java/com/android/server/am services/core/java/com/android/server/wm \
  core/java/android/media media/java/android/media apex/jobscheduler/framework/java/android/app data/keyboards &
clone_full platform/frameworks/libs/systemui frameworks/libs/systemui &
clone_sparse platform/art art libartbase libdexfile compiler runtime &
clone_full platform/packages/apps/Launcher3 packages/apps/Launcher3 &
clone_sparse platform/packages/modules/Connectivity packages/modules/Connectivity \
  framework/src/android/net framework-t/src/android/net staticlibs/framework/com/android/net/module/util &
connectivity_pid=$!
wait "$connectivity_pid"
wait
echo "Fetched minimal AOSP 17 ($TAG) components into $BASE"
