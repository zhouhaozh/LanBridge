#!/usr/bin/env bash
# SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
# SPDX-License-Identifier: MIT
set -euo pipefail
if [[ $(uname -s) != Darwin || $(uname -m) != arm64 ]]; then
  printf '请在 Apple Silicon Mac 本机运行。\n' >&2
  exit 1
fi
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
task_build=${1:-"$task_root/build/lanbridge-macos"}
task_output=${2:-"$task_root/dist/macos"}
mkdir -p -- "$task_build" "$task_output"
task_build=$(cd -- "$task_build" && pwd -P)
task_output=$(cd -- "$task_output" && pwd -P)
task_dmg="$task_output/lanbridge-1.27.0-macos-arm64.dmg"
# Refuse before rebuilding or replacing the app paired with an existing image.
if [[ -e $task_dmg ]]; then
  printf '已存在镜像；请先移动它或选择新的输出目录：%s\n' "$task_dmg" >&2
  exit 1
fi
task_cmake_args=()
if command -v brew >/dev/null 2>&1; then
  task_brew=$(brew --prefix)
  task_ssl=$(brew --prefix openssl@3 2>/dev/null || true)
  # Current Homebrew can install Qt as separate qtbase/qttools formulae.
  export PATH="$task_brew/bin:$task_brew/opt/qtbase/bin:$task_brew/opt/qttools/bin:$PATH"
  task_cmake_args+=("-DCMAKE_PREFIX_PATH=$task_brew")
  if [[ -n $task_ssl ]]; then
    task_cmake_args+=("-DOPENSSL_ROOT_DIR=$task_ssl")
  fi
fi
cmake -S "$task_root" -B "$task_build" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTS=ON -DSKIP_BUILD_TESTS=ON \
  -DBUILD_OSX_BUNDLE=ON -DBUILD_INSTALLER=ON "${task_cmake_args[@]}"
cmake --build "$task_build" --parallel "$(sysctl -n hw.ncpu)"
ctest --test-dir "$task_build/src/unittests" -LE interactive --output-on-failure
cmake --install "$task_build" --prefix "$task_output"
/usr/bin/python3 "$task_root/deploy/lanbridge/verify-macos-bundle.py" "$task_output/LanBridge.app" \
  --report "$task_root/build/verification/bundle-audit.json"
task_stage=$(mktemp -d "${TMPDIR:-/tmp}/lanbridge-dmg.XXXXXX")
trap 'rm -rf -- "$task_stage"' EXIT
ditto "$task_output/LanBridge.app" "$task_stage/LanBridge.app"
ln -s /Applications "$task_stage/Applications"
hdiutil create -volname LanBridge -srcfolder "$task_stage" -ov -format UDZO "$task_dmg"
hdiutil verify "$task_dmg"
printf '已生成本地签名（未公证）的应用和镜像：%s\n%s\n' "$task_output/LanBridge.app" "$task_dmg"
