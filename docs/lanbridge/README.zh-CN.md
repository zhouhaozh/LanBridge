# LanBridge 使用与构建说明

LanBridge 基于 Deskflow v1.27.0，面向 Ubuntu GNOME X11 与 Apple Silicon Mac 的局域网双机工作场景：Linux 共享本机键鼠，Mac 接收控制，两端可共享剪贴板。它不传输画面或文件，当前 LanBridge 工作流不支持 Wayland。

界面名称为 LanBridge，应用标识为 `org.lanbridge.LanBridge`。为保留上游兼容性，Linux GUI 与核心程序名称仍为 `deskflow`、`deskflow-core`。仓库当前为开发版本，安装包名称中的 `1.27.0` 继承自上游基线。

## 一、检查环境

两台机器需要在局域网内互相访问，Linux 必须登录 GNOME X11 会话。Ubuntu 登录页面可选择“Ubuntu on Xorg”。需要检查 Linux 环境时，在 Linux 本机终端、项目根目录运行：

```bash
bash deploy/lanbridge/probe-linux.sh
```

该脚本只读系统信息，会输出登录名、系统版本、CPU、会话、显示器、Flatpak、Background portal 和 SSH 主机公钥指纹。分享检查结果前，请移除与问题无关的机器信息。

构建最低要求为 CMake 3.24、C++20 编译器、Qt 6.7.0、OpenSSL 3。Linux 原生构建还涉及 X11、xkbcommon、xkbregistry、libei 和 libportal；Flatpak 构建通过清单准备沙箱内依赖。

## 二、构建与安装

以下构建命令均在项目根目录执行。安装包和验证报告由本地生成，不能仅凭文件名判断它们来自当前源码。

### Linux Flatpak

在 Linux 安装 `flatpak`、`flatpak-builder`，并准备与本机架构一致的 KDE Platform / SDK 6.11：

```bash
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user flathub org.kde.Platform//6.11 org.kde.Sdk//6.11
bash deploy/lanbridge/build-flatpak.sh
bash deploy/lanbridge/install-linux.sh "dist/linux/lanbridge-1.27.0-linux-$(flatpak --default-arch).flatpak"
flatpak run org.lanbridge.LanBridge
```

构建使用当前工作区源码，需要联网下载构建清单指定的依赖。脚本在导出安装包前运行非交互测试；未发现测试或测试失败时停止，已有同名包时拒绝覆盖。

请在 Linux 本机以普通用户执行安装。配置保存在该 Flatpak 的独立用户目录中。

### Mac 应用与 DMG

在 Apple Silicon Mac 安装 Xcode 命令行工具、CMake、Qt 和 OpenSSL 3，然后运行：

```bash
bash deploy/lanbridge/build-macos.sh
```

脚本读取已安装的 Homebrew 依赖位置，不自动安装依赖。它依次构建、运行非交互测试、部署运行库、检查架构与签名，再生成：

```text
 dist/macos/LanBridge.app
 dist/macos/lanbridge-1.27.0-macos-arm64.dmg
```

默认构建目录为 `build/lanbridge-macos/`。如需保留已有产物，可指定新目录：

```bash
bash deploy/lanbridge/build-macos.sh build/macos-dev dist/macos-dev
```

已有同名 DMG 时，脚本在重新构建或替换配套应用前停止。当前应用采用本地临时签名，未做 Developer ID 公证。将应用拖入“应用程序”，从固定位置启动；系统拦截时，通过“系统设置 → 隐私与安全性”处理。

按应用与系统提示授予“辅助功能”和系统要求的局域网权限。应用位置或签名改变后，应重新检查授权。

### Linux 离线安装器

`deploy/lanbridge/package-offline-linux.py` 用于将已准备的应用包、精确锁定的运行时及宿主依赖打包成离线安装器。它需要提前准备并核实这些载荷；源码仓库不包含生成的安装器或依赖包。

如已获得与目标系统、CPU 架构匹配的安装器，在 Linux 本机普通用户终端执行：

```bash
bash ./LanBridge-offline.run --verify-only
bash ./LanBridge-offline.run
```

示例文件名需替换为实际安装器名称。安装器会校验完整载荷及逐文件 SHA256，再执行安装；缺少宿主 Flatpak 依赖时可能请求系统管理员授权，不要在整个命令前加 `sudo`。若依赖方案需要改变已有系统包，安装器会停止。具体要求以安装器配套清单为准。

## 三、连接两台机器

1. 在 Linux 打开 LanBridge，点击“开始共享”。Linux 默认作为服务端，名称为 `linux`。
2. 在 Mac 打开 LanBridge，客户端名称默认为 `mac`。将服务器地址栏中的预设值改为实际 Linux 主机名或局域网地址，确认服务端 TCP 端口为 `24800`，再点击“连接 Linux”。当前顶部说明仍可能显示预设地址，应以实际填写的服务器地址为准。
3. 首次连接时，在两台机器上核对证书指纹后再信任。TLS 加密与对端验证保持开启；证书变化后应重新核实。
4. 在 Linux 主窗口设置 Mac 的相对方向。默认 Linux 在左、Mac 在右；鼠标越过连接边缘时切换，键盘随鼠标跟随。

不需要手动切换角色或改计算机名称。两端防火墙与网络策略需要允许客户端访问服务端端口。

## 四、键鼠、剪贴板与恢复

- 按住鼠标按钮时，不自动跨屏；松开后可以再次移到边缘。
- 在 Linux 按 `Ctrl + Alt + Esc` 紧急返回 Linux，拖动时也应可用。
- 操作 Mac 时，`Ctrl+C` / `Ctrl+V` 分别执行 `Command+C` / `Command+V`。在 Mac 终端中 `Ctrl+C` 同样执行复制，中断快捷键需按终端设置使用；其他 Ctrl 组合保持原有处理。
- 如需共享剪贴板，先在 Linux 停止共享，再调整“共享剪贴板”选项并重新连接。两端需使用支持该功能的版本；曾经关闭过的配置不会因为更新自动开启。
- 中文输入使用当前机器自己的输入法。Retina 缩放、中文输入与多显示器布局需要实机检查。

断网后程序会断开并尝试重连。重连后操作位置保持在 Linux，需要主动再次跨屏。

如果 Mac 接收进程异常退出并提示本地恢复，请先在 Mac 本机恢复键盘与鼠标状态，必要时注销登录；确认恢复后再按界面提示继续。连接按钮不能代替恢复确认。

## 五、登录自启

在主窗口开启“登录后启动 LanBridge”。

- **Mac**：使用 `SMAppService.mainAppService` 注册主应用，需要 macOS 13 或更新版本。“等待批准”时在“系统设置 → 通用 → 登录项”允许 LanBridge。
- **Linux**：使用 `org.freedesktop.portal.Background.RequestBackground` 申请授权。只有系统返回 `autostart=true` 才表示获准，单独获得后台权限不等于开启自启。
- Linux Background portal 无法读取当前自启状态，界面中的“上次系统确认”仅记录上一次授权结果。在系统设置修改权限后，应重新授权，并通过重新登录检查实际结果。

只有系统缺少 Background 接口时，才在 Linux 本机终端、项目根目录执行：

```bash
bash deploy/lanbridge/login-startup-linux.sh install
```

脚本会再次检查接口；已有接口、权限拒绝和连接超时均不会触发此回退。使用 `remove` 参数撤销脚本创建的自启项。

“登录后启动”和“启动后开始共享”是独立设置，应分别确认。

## 六、诊断与验证

“查看 / 导出性能报告”展示样本数、p50/p95/p99、队列峰值和更新时间，也可导出 JSON。Linux 的“捕获到发送”与 Mac 的“接收到注入”是各自本机的阶段统计，不等于网络往返或端到端显示延迟。

Mac 打包流程会检查 Mach-O 架构、运行库路径和签名，并在干净临时目录运行 GUI 与核心程序的版本检查。默认报告位于 `build/verification/bundle-audit.json`，可在项目根目录手动重跑：

```bash
python3 deploy/lanbridge/verify-macos-bundle.py dist/macos/LanBridge.app
```

开发构建后的普通回归：

```bash
ctest --test-dir build/lanbridge-macos/src/unittests -LE interactive --output-on-failure --no-tests=error
python3 -m unittest discover -s deploy/lanbridge/tests -p 'test_*.py'
```

CTest 目录需与实际构建目录对应。部分 Python 离线安装器测试仅在 Linux 执行。`OSXClipboardTests` 会替换剪贴板，`OSXKeyStateTests` 会注入真实按键，均标为 `interactive`，默认排除；应在专用测试环境中另行执行。

构建成功、自动化测试通过与真实双机验收是不同阶段。实际使用前仍需检查跨屏、拖动、快捷键、剪贴板、断网恢复、系统权限及登录自启。

## 七、卸载

在 Linux 本机、项目根目录运行：

```bash
bash deploy/lanbridge/uninstall-linux.sh
```

脚本保留用户配置，并移除自身创建的自启回退文件。Mac 先在软件内关闭登录自启，退出后移除应用；辅助功能权限与登录项可在系统设置中检查。

## 八、源码快照

在项目根目录生成当前工作区源码归档和 SHA256 清单：

```bash
python3 deploy/lanbridge/package-source.py --source . \
  --archive dist/lanbridge-source.tar.gz \
  --manifest dist/SOURCE-MANIFEST.json
```

快照包含已修改的跟踪文件及未忽略的新文件，排除 Git 数据、构建与安装包目录，保留执行权限和符号链接。输出存在时拒绝覆盖。归档与清单应配对保留，生成的快照不提交到源码仓库。
