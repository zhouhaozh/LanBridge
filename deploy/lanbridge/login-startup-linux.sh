#!/usr/bin/env bash
# SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
# SPDX-License-Identifier: MIT
set -euo pipefail
if [[ $(uname -s) != Linux || -e /.flatpak-info ]]; then
  printf '此脚本只能在 Linux 本机终端运行，不能在沙箱中写入宿主设置。\n' >&2
  exit 1
fi
task_action=${1:-}
task_directory=${XDG_CONFIG_HOME:-"$HOME/.config"}/autostart
task_file="$task_directory/org.lanbridge.LanBridge.desktop"
case "$task_action" in
  install)
    command -v gdbus >/dev/null || { printf '缺少 gdbus，无法确认系统接口是否缺失。\n' >&2; exit 1; }
    task_error=$(gdbus call --session --dest org.freedesktop.portal.Desktop \
      --object-path /org/freedesktop/portal/desktop \
      --method org.freedesktop.DBus.Properties.Get org.freedesktop.portal.Background version 2>&1) && {
      printf '系统支持 Background 接口，请在 LanBridge 窗口中开启登录自启。\n' >&2
      exit 1
    }
    case "$task_error" in
      *org.freedesktop.DBus.Error.UnknownInterface*|*org.freedesktop.DBus.Error.UnknownProperty*|*org.freedesktop.DBus.Error.ServiceUnknown*|*org.freedesktop.DBus.Error.NameHasNoOwner*) ;;
      *) printf '无法确认接口缺失；不会绕过授权失败：%s\n' "$task_error" >&2; exit 1 ;;
    esac
    flatpak info --user org.lanbridge.LanBridge >/dev/null
    if [[ -e $task_file ]]; then
      printf '自启文件已存在，不覆盖：%s\n' "$task_file" >&2
      exit 1
    fi
    mkdir -p -- "$task_directory"
    umask 077
    cat > "$task_file" <<'DESKTOP'
# LanBridge host fallback; created only when the Background portal is unavailable.
[Desktop Entry]
Type=Application
Name=LanBridge
Exec=flatpak run org.lanbridge.LanBridge
Icon=org.lanbridge.LanBridge
Terminal=false
X-GNOME-Autostart-enabled=true
DESKTOP
    printf '已创建登录自启：%s\n' "$task_file"
    ;;
  remove)
    if [[ -f $task_file ]]; then
      if ! head -n 1 "$task_file" | grep -q '^# LanBridge host fallback;'; then
        printf '该文件不由本脚本创建，请在系统设置中移除：%s\n' "$task_file" >&2
        exit 1
      fi
      rm -- "$task_file"
    fi
    ;;
  *) printf '用法：%s install|remove\n' "$0" >&2; exit 2 ;;
esac
