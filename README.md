# LanBridge

LanBridge 是一款局域网键盘、鼠标与剪贴板共享工具，面向 **Ubuntu GNOME X11 + Apple Silicon Mac** 的双机工作场景：在 Linux 上使用一套键鼠，将鼠标移过屏幕边缘即可操作 Mac。

本项目基于 **Deskflow v1.27.0**（上游提交 `081f6478e654a112168879e67a41ce7d7e4ae3af`）进行定制开发，保留上游版权、许可证和核心程序命名。应用显示名称为 **LanBridge**，应用标识为 `org.lanbridge.LanBridge`；Linux GUI 和核心程序仍分别使用 `deskflow`、`deskflow-core`。

当前仓库保存的是基于 1.27.0 的开发版本。构建脚本中的安装包版本仍为 `1.27.0`，不代表已经发布独立的正式版本；自动化测试结果也不能替代真实双机验收。

## 主要功能

- **跨屏控制**：Linux 作为服务端、Mac 作为客户端，键盘跟随鼠标切换；可在 Linux 界面调整 Mac 位于左、右、上、下哪个方向。
- **操作恢复**：拖动时锁定当前屏幕；`Ctrl + Alt + Esc` 紧急返回 Linux；连接异常后提供恢复状态与重连提示。
- **剪贴板共享**：支持两台机器之间复制、粘贴，由 Linux 界面启用或关闭，重新连接后生效。
- **Mac 常用快捷键**：远程操作 Mac 时，将 `Ctrl+C`、`Ctrl+V` 映射为 `Command+C`、`Command+V`。
- **加密连接**：使用 TLS，首次连接核对并确认对端证书指纹。
- **登录自启**：Mac 使用系统登录项服务；Linux 使用 Background portal 申请授权。
- **性能诊断**：展示阶段耗时、分位数、队列峰值等统计，并可导出 JSON；不记录按键内容。
- **本地打包**：提供 macOS 应用与 DMG、Linux Flatpak、Linux 离线安装器和源码快照的构建工具。

本项目不提供画面传输或文件共享；当前 LanBridge 工作流不支持 Wayland。Windows、BSD 等上游平台代码仍保留，但不属于本项目当前定制支持范围。

## 环境要求

| 项目 | 要求 |
| --- | --- |
| 使用场景 | 两台能够互相访问的局域网计算机 |
| Linux | Ubuntu GNOME **X11** 会话；Flatpak 构建需要 `flatpak`、`flatpak-builder` |
| macOS | 当前打包脚本要求 Apple Silicon（arm64）、Xcode 命令行工具 |
| 编译工具链 | C++20 编译器、CMake **3.24+** |
| Qt | **6.7.0+**；Core、Widgets、Network、LinguistTools；构建测试还需要 Test，Linux 需要 DBus |
| OpenSSL | **3.0+** |
| Linux 原生构建依赖 | X11 开发库、`xkbcommon`、`xkbregistry`；libei **1.3+**、libportal **0.9.1+** |
| 打包辅助工具 | Bash、Python 3；macOS 打包使用系统签名与磁盘镜像工具 |

Mac 登录自启功能需要 macOS 13 或更新版本。Linux Flatpak 清单使用 KDE Platform / SDK **6.11**，依赖版本以[构建清单](deploy/linux/flatpak/org.lanbridge.LanBridge.yml)为准。

## 获取源码

```bash
git clone https://github.com/zhouhaozh/LanBridge.git
cd LanBridge
```

私有仓库需要使用有访问权限的 GitHub 账户认证。以下命令均在项目根目录执行。详细操作见[中文使用与构建说明](docs/lanbridge/README.zh-CN.md)。

仓库保存源码、构建脚本、测试和必要资源；安装包、构建目录、个人配置、日志和本地验收材料不作为源码提交。具体规则见 [.gitignore](.gitignore)。

## 构建与安装

### macOS（Apple Silicon）

先安装 Xcode 命令行工具、CMake、Qt 和 OpenSSL 3。使用 Homebrew 时，脚本会读取已经安装的依赖位置，并将 Qt 工具加入构建路径，不会自动安装依赖。

```bash
bash deploy/lanbridge/build-macos.sh
```

脚本配置 Release 构建，编译后执行非交互测试，部署 Qt 与运行库，再检查应用架构、加载路径和签名，最后生成：

```text
dist/macos/LanBridge.app
dist/macos/lanbridge-1.27.0-macos-arm64.dmg
```

默认构建目录为 `build/lanbridge-macos/`。也可以为构建与输出分别指定目录：

```bash
bash deploy/lanbridge/build-macos.sh build/macos-dev dist/macos-dev
```

输出目录已有同名 DMG 时，脚本拒绝覆盖。当前产物采用本地临时签名，未做 Developer ID 公证。安装时将应用放入“应用程序”，从固定位置启动，并按照系统提示授予“辅助功能”及系统要求的局域网权限。若系统拦截，通过“系统设置 → 隐私与安全性”处理。

### Linux（Flatpak）

先安装 `flatpak`、`flatpak-builder`，并准备与本机架构一致的运行时和 SDK：

```bash
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user flathub org.kde.Platform//6.11 org.kde.Sdk//6.11
bash deploy/lanbridge/build-flatpak.sh
```

脚本使用当前源码构建，运行非交互测试，再导出安装包；构建过程中需要下载清单中指定的依赖。输出位于 `dist/linux/`，已有同名包时拒绝覆盖。

```bash
bash deploy/lanbridge/install-linux.sh "dist/linux/lanbridge-1.27.0-linux-$(flatpak --default-arch).flatpak"
flatpak run org.lanbridge.LanBridge
```

请在 Linux 本机普通用户终端执行安装命令，并登录 X11 会话；Ubuntu 登录页面可选择“Ubuntu on Xorg”。

### 开发构建

已安装对应平台的依赖后，可以直接使用 CMake。Qt 的 CMake 配置与部署工具需要能够被找到；必要时设置 `CMAKE_PREFIX_PATH`、`OPENSSL_ROOT_DIR` 和工具搜索路径。

```bash
cmake -S . -B build/dev \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON \
  -DSKIP_BUILD_TESTS=ON \
  -DBUILD_INSTALLER=OFF
cmake --build build/dev --parallel
ctest --test-dir build/dev/src/unittests -LE interactive --output-on-failure --no-tests=error
```

`SKIP_BUILD_TESTS=ON` 仅关闭构建结束时的自动测试，测试程序仍会编译，随后由上述 CTest 命令显式执行。更多上游构建选项见[开发构建说明](docs/dev/build.md)；其中 Deskflow 的平台说明和名称需要结合本项目定制情况使用。

## 首次连接与日常操作

1. 在 Linux 打开 LanBridge，确认是 X11 会话，点击“开始共享”。当前服务端名称为 `linux`，Mac 客户端名称为 `mac`。
2. 确认两端网络可达，服务端 TCP 端口默认为 **24800**。当前版本仍包含预设服务端地址，部署时需要核对实际 Linux 地址。
3. 在 Mac 安装并打开 LanBridge，按提示授予系统权限，将服务器地址栏改为实际 Linux 主机名或局域网地址，再点击“连接 Linux”。顶部说明可能仍显示预设地址，以实际填写的服务器地址为准。首次连接时，在两端核对证书指纹后确认信任；证书变化时重新核对。
4. 在 Linux 设置 Mac 的相对位置，将鼠标移过相应边缘即可跨屏。按住鼠标按钮时保持当前屏幕；`Ctrl + Alt + Esc` 可紧急返回 Linux。
5. 如需共享剪贴板，在 Linux 停止共享后调整“共享剪贴板”，然后重新连接。两端均需使用支持该功能的版本。

远程控制 Mac 时，`Ctrl+C` 会执行复制，包括在终端内；终端中断操作需使用应用自身配置的快捷键。中文输入使用当前机器自己的输入法，Retina 缩放、多显示器布局和输入法表现需要在实际设备验证。

如 Mac 接收进程异常退出并提示恢复，请先在 Mac 本机恢复键鼠状态，再按界面提示确认。重连后需要主动再次跨屏。

### 登录自启

在主窗口启用“登录后启动 LanBridge”。Mac 出现“等待批准”时，前往“系统设置 → 通用 → 登录项”允许；Linux 需要 Background portal 返回自启授权。Linux 界面显示的“上次系统确认”不等于实时读取系统自启状态，变更权限后应重新授权并通过重新登录验证。

只有系统缺少 Background portal 接口时，才使用 [Linux 自启辅助脚本](deploy/lanbridge/login-startup-linux.sh)。登录自启与“启动后开始共享”是两项独立设置。

## 测试与验证

非交互 C++ / Qt 测试覆盖输入状态、事件队列、连接与协议、剪贴板、配置、诊断和自启逻辑。使用前述 CTest 命令执行。

打包工具的 Python 回归测试可单独运行：

```bash
python3 -m unittest discover -s deploy/lanbridge/tests -p 'test_*.py'
```

部分离线安装器测试仅在 Linux 执行，在其他系统会跳过。`OSXClipboardTests` 和 `OSXKeyStateTests` 标记为 `interactive`，会改变系统剪贴板或注入真实按键，普通回归和打包流程默认排除；需要在专用测试环境中另行验证。

macOS 打包后也可手动重新检查应用依赖和签名：

```bash
python3 deploy/lanbridge/verify-macos-bundle.py dist/macos/LanBridge.app
```

性能报告中的 Linux“捕获到发送”和 Mac“接收到注入”属于各自本机阶段统计，不代表端到端显示延迟。真实跨屏、拖动、断网恢复、中文输入和系统授权仍需双机验收。

## 项目结构

```text
.
├── CMakeLists.txt           # 项目配置与依赖版本
├── cmake/                  # 通用构建辅助模块
├── src/
│   ├── apps/               # GUI、核心与后台程序入口
│   ├── lib/                # 输入、网络、协议、GUI 与平台实现
│   └── unittests/          # C++ / Qt 单元测试
├── deploy/
│   ├── lanbridge/          # LanBridge 构建、安装、验证与打包工具
│   ├── linux/              # Linux 桌面与 Flatpak 元数据
│   ├── mac/                # macOS 部署配置
│   └── windows/            # 保留的上游 Windows 部署配置
├── docs/                   # 配置、协议与开发文档
├── translations/           # Qt 翻译资源
├── LICENSE                 # 项目主许可证
└── LICENSES/               # 各文件许可证及 OpenSSL 例外
```

`build/`、`dist/` 等目录由本地构建生成，安装包和验证报告保存在本地，不上传到源码仓库。Linux 卸载可运行 `bash deploy/lanbridge/uninstall-linux.sh`，脚本保留用户配置；Mac 卸载前先关闭登录自启，再退出并移除应用。

## 开发约定

- 修改前阅读对应模块和测试；保持变更范围清晰，行为变化应增加适当的回归验证。
- 遵循现有 C++20、Qt、CMake 结构以及 [.editorconfig](.editorconfig)、[.clang-format](.clang-format)。
- 不提交个人配置、证书私钥、环境变量文件、日志、缓存或构建产物；复用现有构建和测试工具。
- 提交说明应交代修改原因、验证方式和已知限制，保留第三方版权和 SPDX 标记。
- 报告问题时提供系统、CPU 架构、X11 会话状态、复现步骤和必要日志，并移除地址、证书及其他个人信息。

## 来源与许可证

LanBridge 衍生自 [Deskflow](https://github.com/deskflow/deskflow)，不是上游官方发行版。

仓库保留上游的 [GNU GPL v2 主许可证](LICENSE)和 [OpenSSL 链接例外](LICENSES/LicenseRef-OpenSSL-Exception.txt)。各文件还可能适用 MIT、LGPL 等许可证，以文件 SPDX 标记、[REUSE.toml](REUSE.toml) 和 [LICENSES](LICENSES/) 中的文本为准；再分发时请保留相应版权与许可证说明。
