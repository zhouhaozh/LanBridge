#!/usr/bin/env bash
# SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
# SPDX-License-Identifier: MIT
set -euo pipefail
if [[ -e /.flatpak-info ]]; then
  printf '请在 Linux 本机终端运行。\n' >&2
  exit 1
fi
task_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
"$task_dir/login-startup-linux.sh" remove
flatpak uninstall --user org.lanbridge.LanBridge
printf '已卸载程序；用户配置仍保留，以便重新安装时恢复。\n'
