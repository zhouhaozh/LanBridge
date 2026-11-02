#!/usr/bin/env python3
# SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
# SPDX-License-Identifier: MIT
"""Prepare verified release assets locally; publish only when explicitly requested."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys


OUTPUTS = {"RELEASE-MANIFEST.json", "SHA256SUMS", "NOTES.md"}


def parse_tag(tag):
    match = re.fullmatch(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?", tag)
    if not match:
        raise ValueError("标签格式无效：请使用 v1.28.0 或 v1.28.0-rc.1，不接受构建元数据。")
    prerelease = match.group(4)
    if prerelease and any(part.isdigit() and len(part) > 1 and part.startswith("0")
                          for part in prerelease.split(".")):
        raise ValueError("预发布版本中的纯数字标识不能有前导零。")
    return tag[1:], prerelease is not None


def regular_file(path):
    metadata = path.lstat()
    if not stat.S_ISREG(metadata.st_mode):
        raise ValueError("资产必须是普通文件，不能是目录或符号链接：{}".format(path))
    return metadata


def file_metadata(path):
    regular_file(path)
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(descriptor, "rb") as source:
        if not stat.S_ISREG(os.fstat(source.fileno()).st_mode):
            raise ValueError("资产类型已改变：{}".format(path))
        digest, size = hashlib.sha256(), 0
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
            size += len(chunk)
    if not size:
        raise ValueError("安装包不能为空：{}".format(path))
    return {"sha256": digest.hexdigest(), "bytes": size}


def write_output(path, content):
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o644)
    with os.fdopen(descriptor, "w", encoding="utf-8") as destination:
        destination.write(content)


def gh(*arguments, missing_ok=False, cwd=None):
    result = subprocess.run(["gh", *arguments], capture_output=True, text=True, check=False, cwd=cwd)
    if result.returncode:
        # gh api prints the real HTTP status in parentheses. Authentication and
        # network errors must never be mistaken for an absent release.
        if missing_ok and "(HTTP 404)" in result.stderr:
            return None
        raise RuntimeError((result.stderr or result.stdout or "GitHub 命令失败").strip())
    return result.stdout


def api(endpoint, *options, missing_ok=False):
    output = gh("api", endpoint, *options, missing_ok=missing_ok)
    return json.loads(output) if output is not None else None


def verify_tag(repo, tag, commit):
    reference = api(f"repos/{repo}/git/ref/tags/{tag}")
    seen = set()
    for _ in range(16):
        target = reference.get("object") if isinstance(reference, dict) else None
        if not isinstance(target, dict) or not re.fullmatch(r"[0-9a-fA-F]{40}", str(target.get("sha", ""))):
            raise ValueError("远端标签对象无效。")
        sha = target["sha"].lower()
        if target.get("type") == "commit":
            if sha != commit:
                raise ValueError("远端标签已移动或不属于当前构建提交，停止发布。")
            return
        if target.get("type") != "tag" or sha in seen:
            raise ValueError("远端标签未指向有效提交。")
        seen.add(sha)
        reference = api(f"repos/{repo}/git/tags/{sha}")
    raise ValueError("远端注解标签嵌套过深。")


def find_release(repo, tag):
    release = api(f"repos/{repo}/releases/tags/{tag}", missing_ok=True)
    if release is not None:
        return release
    # Some API responses hide drafts from the by-tag endpoint. List all pages
    # after a confirmed 404 so an interrupted draft can still be resumed safely.
    pages = api(f"repos/{repo}/releases?per_page=100", "--paginate", "--slurp")
    if not isinstance(pages, list) or any(not isinstance(page, list) for page in pages):
        raise ValueError("GitHub 发布列表格式无效。")
    releases = [release for page in pages for release in page]
    if any(not isinstance(release, dict) for release in releases):
        raise ValueError("GitHub 发布元数据格式无效。")
    matches = [release for release in releases if release.get("tag_name") == tag]
    if len(matches) > 1:
        raise ValueError("同一标签存在多个发布记录，停止发布。")
    return matches[0] if matches else None


def verify_draft(release, tag, commit):
    if not isinstance(release, dict) or release.get("tag_name") != tag:
        raise ValueError("未找到与当前标签一致的发布草稿。")
    if release.get("draft") is not True:
        raise ValueError("发布已公开或草稿状态无效，禁止覆盖。")
    if release.get("target_commitish") != commit:
        raise ValueError("发布草稿的目标提交不匹配，禁止复用。")


def verify_remote_assets(repo, release, expected, complete=False):
    release_id = release.get("id")
    if type(release_id) is not int or release_id <= 0:
        raise ValueError("发布草稿缺少有效 ID。")
    pages = api(f"repos/{repo}/releases/{release_id}/assets?per_page=100", "--paginate", "--slurp")
    if not isinstance(pages, list) or any(not isinstance(page, list) for page in pages):
        raise ValueError("GitHub 资产列表格式无效。")
    seen = set()
    for asset in (asset for page in pages for asset in page):
        name = asset.get("name") if isinstance(asset, dict) else None
        if not isinstance(name, str) or name not in expected or name in seen:
            raise ValueError("发布草稿存在未知或重复资产；不会删除这些文件。")
        seen.add(name)
        if complete:
            metadata = expected[name]
            if asset.get("state") != "uploaded" or type(asset.get("size")) is not int or asset["size"] != metadata["bytes"]:
                raise ValueError("远端资产未上传完整或大小不匹配：" + name)
            digest = asset.get("digest")
            if digest not in (None, "") and digest != "sha256:" + metadata["sha256"]:
                raise ValueError("远端资产 SHA256 不匹配：" + name)
    if complete and seen != set(expected):
        raise ValueError("远端资产缺失，禁止公开发布。")


def publish_release(tag, commit, repo, assets, manifest):
    verify_tag(repo, tag, commit)
    release = find_release(repo, tag)
    prerelease = str(manifest["prerelease"]).lower()
    latest = str(not manifest["prerelease"]).lower()
    flags = ["--repo", repo, "--title", "LanBridge " + manifest["version"],
             "--notes-file", str(assets / "NOTES.md"), "--prerelease=" + prerelease, "--latest=" + latest]
    if release is None:
        gh("release", "create", tag, "--draft", "--verify-tag", "--target", commit, *flags)
        release = find_release(repo, tag)
    verify_draft(release, tag, commit)
    for name, expected in manifest["assets"].items():
        if file_metadata(assets / name) != expected:
            raise ValueError("安装包在准备发布后已改变：" + name)
    expected_assets = dict(manifest["assets"])
    for name in ("SHA256SUMS", "RELEASE-MANIFEST.json"):
        expected_assets[name] = file_metadata(assets / name)
    verify_remote_assets(repo, release, expected_assets)
    # gh interprets '#' in upload arguments as an asset label. Passing only
    # controlled filenames also supports asset directories containing '#'.
    uploads = sorted(manifest["assets"]) + ["SHA256SUMS", "RELEASE-MANIFEST.json"]
    gh("release", "upload", tag, *uploads, "--repo", repo, "--clobber", cwd=assets)
    release = find_release(repo, tag)
    verify_draft(release, tag, commit)
    verify_remote_assets(repo, release, expected_assets, complete=True)
    verify_tag(repo, tag, commit)
    gh("release", "edit", tag, "--draft=false", "--verify-tag", *flags)


def prepare(tag, commit, repo, assets, publish=False):
    version, prerelease = parse_tag(tag)
    if not re.fullmatch(r"[0-9a-fA-F]{40}", commit):
        raise ValueError("构建提交必须是完整的 40 位十六进制 Git SHA。")
    commit = commit.lower()
    if not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?/[A-Za-z0-9_.-]+", repo) or repo.split("/")[-1] in {".", ".."}:
        raise ValueError("仓库必须使用 owner/name 格式。")
    assets = Path(assets).resolve(strict=True)
    if not assets.is_dir():
        raise ValueError("资产路径必须是目录。")
    names = {f"lanbridge-{version}-linux-x86_64.flatpak", f"lanbridge-{version}-macos-arm64.dmg"}
    present = {path.name for path in assets.iterdir()}
    if names - present or present - names - OUTPUTS:
        raise ValueError("资产目录必须恰有当前版本的 Linux Flatpak、macOS DMG 及可选的上次准备输出。")
    for name in present:
        regular_file(assets / name)
    manifest = {"schema": 1, "tag": tag, "version": version, "commit": commit,
                "repository": repo, "prerelease": prerelease,
                "assets": {name: file_metadata(assets / name) for name in sorted(names)}}
    manifest_text = json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    checksums = {name: metadata["sha256"] for name, metadata in manifest["assets"].items()}
    checksums["RELEASE-MANIFEST.json"] = hashlib.sha256(manifest_text.encode("utf-8")).hexdigest()
    notes = f"""# LanBridge {version}

来源标签：`{tag}`；源码提交：`{commit}`。

## Linux x86_64

面向 GNOME X11 会话。Flatpak 资产仅包含应用，不包含 KDE Platform 6.11 运行时；首次安装需联网准备运行时，并事先安装宿主 Flatpak：

```bash
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user flathub org.kde.Platform//6.11
flatpak install --user ./lanbridge-{version}-linux-x86_64.flatpak
flatpak run org.lanbridge.LanBridge
```

这份 Flatpak 不是完整离线安装器，不包含 SDK。当前工作流不支持 Wayland。

## macOS Apple Silicon（arm64）

下载 DMG，将 LanBridge.app 拖入“应用程序”并从固定位置启动。安装包采用 ad-hoc 本地签名，未使用 Developer ID，未公证。系统拦截时通过“系统设置 → 隐私与安全性”处理，并按提示授予辅助功能和局域网权限。

## 校验与使用

将两份安装包、`RELEASE-MANIFEST.json` 和 `SHA256SUMS` 下载到同一目录后校验：

```bash
sha256sum --check SHA256SUMS       # Linux
shasum -a 256 --check SHA256SUMS  # macOS
```

校验和验证文件完整性，不代替来源验证。Linux 作为服务端、Mac 作为客户端；在 Mac 填写实际 Linux 地址，默认 TCP 端口 24800。首次连接在两端核对 TLS 证书指纹；Ctrl + Alt + Esc 紧急返回 Linux。

更多使用、权限与平台限制见[本版本中文说明](https://github.com/{repo}/blob/{tag}/README.md)。安装包构建与自动化检查不代替真实双机验收。
"""
    write_output(assets / "RELEASE-MANIFEST.json", manifest_text)
    write_output(assets / "SHA256SUMS", "".join(f"{checksums[name]}  {name}\n" for name in sorted(checksums)))
    write_output(assets / "NOTES.md", notes)
    if publish:
        publish_release(tag, commit, repo, assets, manifest)
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    version_parser = commands.add_parser("version", help="验证标签并输出 GitHub Actions 变量")
    version_parser.add_argument("tag")
    prepare_parser = commands.add_parser("prepare", help="准备校验文件；仅 --publish 会操作 GitHub")
    prepare_parser.add_argument("--tag", required=True)
    prepare_parser.add_argument("--commit", required=True)
    prepare_parser.add_argument("--repo", required=True)
    prepare_parser.add_argument("--assets", type=Path, required=True)
    prepare_parser.add_argument("--publish", action="store_true")
    args = parser.parse_args(argv)
    if args.command == "version":
        version, prerelease = parse_tag(args.tag)
        print("version=" + version)
        print("prerelease=" + str(prerelease).lower())
    else:
        prepare(args.tag, args.commit, args.repo, args.assets, args.publish)
        print("已发布到 GitHub。" if args.publish else "发布材料已准备完成；未操作 GitHub。")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
