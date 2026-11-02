#!/usr/bin/env bash
# SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
# SPDX-License-Identifier: MIT
set -euo pipefail
if [[ $(uname -s) != Linux ]]; then
  printf '请在 Linux 本机运行此构建脚本。\n' >&2
  exit 1
fi
task_version=${LANBRIDGE_VERSION-1.27.0}
task_version_regex='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$'
if [[ ! $task_version =~ $task_version_regex ]]; then
  printf '无效版本；请使用 X.Y.Z 或 X.Y.Z-rc.1，不含 v 前缀或构建元数据。\n' >&2
  exit 1
fi
if [[ $task_version == *-* ]]; then
  IFS='.' read -r -a task_prerelease <<< "${task_version#*-}"
  for task_part in "${task_prerelease[@]}"; do
    if [[ $task_part =~ ^0[0-9]+$ ]]; then
      printf '无效版本：预发布数字不能有前导零。\n' >&2
      exit 1
    fi
  done
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
task_bundle="$task_output/lanbridge-$task_version-linux-$task_arch.flatpak"
if [[ -e $task_bundle ]]; then
  printf '已存在安装包；请先移动它再运行：%s\n' "$task_bundle" >&2
  exit 1
fi
task_manifest="$task_root/deploy/linux/flatpak/org.lanbridge.LanBridge.yml"
if [[ ${LANBRIDGE_VERSION+x} ]]; then
  command -v python3 >/dev/null || { printf '发布版本构建需要 python3。\n' >&2; exit 1; }
  task_release_manifest="$task_output/lanbridge-$task_version-manifest.json"
  python3 - "$task_manifest" "$task_release_manifest" "$task_root" "$task_version" <<'PY'
import json
from pathlib import Path
import subprocess
import sys

source, destination, root, version = sys.argv[1:]
result = subprocess.run(["flatpak-builder", "--show-manifest", source],
                        capture_output=True, text=True, check=True)
manifest = json.loads(result.stdout)
module = manifest["modules"][-1]
if module.get("name") != "lanbridge":
    raise SystemExit("发布清单最后一个模块必须为 lanbridge。")
sources = module.get("sources", [])
if len(sources) != 1 or sources[0].get("type") != "dir":
    raise SystemExit("LanBridge 发布必须使用当前本地源码目录。")
sources[0]["path"] = root
module["config-opts"] = [option for option in module.get("config-opts", [])
                         if not option.startswith("-DLANBRIDGE_VERSION=")]
module["config-opts"].append("-DLANBRIDGE_VERSION=" + version)
Path(destination).write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
PY
  task_manifest="$task_release_manifest"
fi
# Build from this working tree so local LanBridge changes are included, never from an official binary.
flatpak-builder --user --force-clean --disable-rofiles-fuse \
  --state-dir="$task_output/state" --repo="$task_output/repo" "$task_output/build" \
  "$task_manifest"
flatpak build-bundle --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
  "$task_output/repo" "$task_bundle" org.lanbridge.LanBridge stable
printf '已构建：%s\n' "$task_bundle"
