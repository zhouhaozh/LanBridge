#!/usr/bin/env bash
# SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
# SPDX-License-Identifier: MIT
set -eu
printf 'LanBridge Linux 环境检查（只读）\n'
printf '登录名: %s\n' "$(id -un)"
printf 'CPU: %s\n' "$(uname -m)"
if [[ -r /etc/os-release ]]; then
  cat /etc/os-release
fi
printf '会话类型: %s\n桌面: %s\nDISPLAY: %s\n' "${XDG_SESSION_TYPE:-未知}" "${XDG_CURRENT_DESKTOP:-未知}" "${DISPLAY:-未设置}"
if [[ ${XDG_SESSION_TYPE:-} != x11 ]]; then
  printf '注意：首版需要在登录页面选择 Ubuntu on Xorg，当前会话未确认是 X11。\n'
fi
if command -v xrandr >/dev/null 2>&1 && [[ -n ${DISPLAY:-} ]]; then
  xrandr --listmonitors || true
fi
if command -v flatpak >/dev/null 2>&1; then
  flatpak --version
  flatpak list --runtime --columns=application,branch,arch
else
  printf '未安装 flatpak。\n'
fi
if command -v flatpak-builder >/dev/null 2>&1; then
  flatpak-builder --version
else
  printf '未安装 flatpak-builder（本机编译需要）。\n'
fi
if command -v gdbus >/dev/null 2>&1; then
  gdbus call --session --dest org.freedesktop.portal.Desktop \
    --object-path /org/freedesktop/portal/desktop \
    --method org.freedesktop.DBus.Properties.Get org.freedesktop.portal.Background version || true
fi
if [[ -r /etc/ssh/ssh_host_ed25519_key.pub ]]; then
  ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub
fi
