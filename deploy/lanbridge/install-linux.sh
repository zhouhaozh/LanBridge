#!/usr/bin/env bash
# SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
# SPDX-License-Identifier: MIT
set -euo pipefail
if [[ $# != 1 || ! -f $1 ]]; then
  printf '用法：%s /完整路径/lanbridge.flatpak\n' "$0" >&2
  exit 2
fi
if [[ -e /.flatpak-info ]]; then
  printf '请在 Linux 本机终端运行，不能在 Flatpak 沙箱中安装。\n' >&2
  exit 1
fi
flatpak install --user "$1"
printf '已安装。运行 flatpak run org.lanbridge.LanBridge 打开软件。\n'
