#!/usr/bin/env bash
# SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
# SPDX-License-Identifier: MIT
set -euo pipefail
if [[ $(uname -s) != Linux ]]; then
  printf '请在 Linux 本机运行此构建脚本。\n' >&2
  exit 1
fi
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
task_output=${1:-"$task_root/dist/linux"}
mkdir -p -- "$task_output"
task_output=$(cd -- "$task_output" && pwd -P)
if [[ "$task_output/" == "$task_root/"* && "$task_output/" != "$task_root/dist/"* && "$task_output/" != "$task_root/build/"* ]]; then
  printf '源码目录内的构建输出必须位于 dist/ 或 build/，避免把输出递归复制到源码。\n' >&2
  exit 1
fi
for task_tool in flatpak flatpak-builder; do
  command -v "$task_tool" >/dev/null || { printf '缺少 %s。\n' "$task_tool" >&2; exit 1; }
done
task_arch=$(flatpak --default-arch)
task_bundle="$task_output/lanbridge-1.27.0-linux-$task_arch.flatpak"
if [[ -e $task_bundle ]]; then
  printf '已存在安装包；请先移动它再运行：%s\n' "$task_bundle" >&2
  exit 1
fi
# Build from this working tree so local LanBridge changes are included, never from an official binary.
flatpak-builder --user --force-clean --disable-rofiles-fuse \
  --state-dir="$task_output/state" --repo="$task_output/repo" "$task_output/build" \
  "$task_root/deploy/linux/flatpak/org.lanbridge.LanBridge.yml"
flatpak build-bundle "$task_output/repo" "$task_bundle" org.lanbridge.LanBridge stable
printf '已构建：%s\n' "$task_bundle"
