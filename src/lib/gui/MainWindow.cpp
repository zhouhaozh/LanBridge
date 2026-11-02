/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2024 - 2026 Chris Rizzitello <sithord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2024 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MainWindow.h"
#include "ui_MainWindow.h"

#include "Diagnostic.h"
#include "StyleUtils.h"

#include "dialogs/AboutDialog.h"
#include "dialogs/ClientConfigDialog.h"
#include "dialogs/FingerprintDialog.h"
#include "dialogs/HelpDialog.h"
#include "dialogs/ServerConfigDialog.h"
#include "dialogs/SettingsDialog.h"

#include "common/PlatformInfo.h"
#include "common/RemoteInputSession.h"
#include "common/Settings.h"
#include "common/UrlConstants.h"
#include "common/VersionInfo.h"
#include "gui/Messages.h"
#include "gui/StartupManager.h"
#include "gui/TlsUtility.h"
#include "gui/core/CoreProcess.h"
#include "gui/ipc/DaemonIpcClient.h"
#include "gui/widgets/LogDock.h"
#include "net/FingerprintDatabase.h"
#include "widgets/StatusBar.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSaveFile>
#include <QScreen>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <memory>

#if defined(Q_OS_MACOS)
#include <ApplicationServices/ApplicationServices.h>
#endif

using namespace deskflow::gui;

MainWindow::MainWindow()
    : ui{std::make_unique<Ui::MainWindow>()},
      m_coreProcess(m_serverConfig),
      m_trayIcon{new QSystemTrayIcon(this)},
      m_guiDupeChecker{new QLocalServer(this)},
      m_daemonIpcClient{new ipc::DaemonIpcClient(this)},
      m_logDock{new LogDock(this)},
      m_statusBar{new StatusBar(this)},
      m_menuFile{new QMenu(this)},
      m_menuEdit{new QMenu(this)},
      m_menuView{new QMenu(this)},
      m_menuHelp{new QMenu(this)},
      m_actionAbout{new QAction(this)},
      m_actionMinimize{new QAction(this)},
      m_actionQuit{new QAction(this)},
      m_actionTrayQuit{new QAction(this)},
      m_actionRestore{new QAction(this)},
      m_actionSettings{new QAction(this)},
      m_actionStartCore{new QAction(this)},
      m_actionRestartCore{new QAction(this)},
      m_actionStopCore{new QAction(this)},
      m_actionShowHelp{new QAction(this)},
      m_networkMonitor{new NetworkMonitor(this)}
{
  ui->setupUi(this);

  setWindowIcon(
      QIcon::fromTheme(kRevFqdnName, QIcon(QStringLiteral(":/icons/deskflow-dark/apps/64/org.deskflow.deskflow.svg")))
  );

  addDockWidget(Qt::BottomDockWidgetArea, m_logDock);

  // Setup Actions
  m_actionAbout->setMenuRole(QAction::AboutRole);
  m_actionAbout->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::HelpAbout));

  m_actionMinimize->setIcon(QIcon::fromTheme(QStringLiteral("window-minimize-pip")));
  m_actionRestore->setIcon(QIcon::fromTheme(QStringLiteral("window-restore-pip")));
  m_actionRestore->setObjectName(QStringLiteral("actionRestore"));
  m_actionQuit->setObjectName(QStringLiteral("actionQuit"));
  m_actionTrayQuit->setObjectName(QStringLiteral("actionTrayQuit"));

  if (!deskflow::platform::isWindows()) {
    m_actionQuit->setShortcut(QKeySequence::Quit);
    m_actionTrayQuit->setShortcut(QKeySequence::Quit);
  }

  m_actionQuit->setIcon(QIcon::fromTheme("application-exit"));
  m_actionQuit->setMenuRole(QAction::QuitRole);

  m_actionTrayQuit->setIcon(QIcon::fromTheme("application-exit"));
  m_actionTrayQuit->setMenuRole(QAction::NoRole);

  m_actionSettings->setIcon(QIcon::fromTheme(QStringLiteral("configure")));
  m_actionSettings->setMenuRole(QAction::PreferencesRole);
  m_actionSettings->setShortcut(QKeySequence::Preferences);

  m_actionStartCore->setIcon(QIcon::fromTheme(QStringLiteral("system-run")));
  m_actionStartCore->setMenuRole(QAction::NoRole);

  m_actionRestartCore->setVisible(false);
  m_actionRestartCore->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
  m_actionRestartCore->setMenuRole(QAction::NoRole);

  m_actionStopCore->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::ProcessStop));
  m_actionStopCore->setMenuRole(QAction::NoRole);

  m_actionShowHelp->setIcon(QIcon::fromTheme(QStringLiteral("question")));
  m_actionShowHelp->setMenuRole(QAction::NoRole);
  m_actionShowHelp->setShortcut(QKeySequence::HelpContents);

  // Setup the Instance Checking
  // In case of a previous crash remove first
  QLocalServer::removeServer(m_guiSocketName);
  m_guiDupeChecker->listen(m_guiSocketName);

  createMenuBar();
  setupControls();
  updateText();
  connectSlots();
  setupTrayIcon();
  updateComputerName();
  setHelpFilePath();

  qDebug().noquote() << "active settings path:" << Settings::settingsPath();

  // Force generation of a certificate on the host
  if (TlsUtility::isEnabled()) {
    if (Settings::value(Settings::Security::KeySize).toInt() < 2048) {
      QMessageBox::information(
          this, kAppName,
          tr("Your current TLS key is smaller than the minimum allowed size, A new key 2048-bit key will be generated.")
      );
      Settings::setValue(Settings::Security::KeySize, 2048);
    }
    if (!TlsUtility::isCertValid()) {
      generateCertificate();
    } else {
      m_fingerprint = {QCryptographicHash::Sha256, TlsUtility::certFingerprint()};
    }
  }

  applyConfig();
  setupLanBridgeControls();
  m_statusBar->setSecurityIcon(TlsUtility::isEnabled());
  restoreWindow();

#ifdef Q_OS_MACOS
  qApp->setQuitOnLastWindowClosed(false);
  qApp->installEventFilter(this);
  connect(qApp, &QCoreApplication::aboutToQuit, this, &MainWindow::prepareToQuit);
#endif
}
MainWindow::~MainWindow()
{
  // Stop network monitoring
  if (m_networkMonitor) {
    m_networkMonitor->stopMonitoring();
  }

  m_guiDupeChecker->close();
  m_coreProcess.cleanup();
}

void MainWindow::restoreWindow()
{
  auto windowGeometry = Settings::value(Settings::Gui::WindowGeometry).toRect();
  const auto totalGeometry = QGuiApplication::primaryScreen()->availableGeometry();
  if (!windowGeometry.isValid()) {
    adjustSize();
    windowGeometry = geometry();
  } else {
    setGeometry(windowGeometry);
  }
  m_expandedSize = geometry().size();

  if (!totalGeometry.contains(windowGeometry)) {
    QRect screenGeometry = QGuiApplication::primaryScreen()->geometry();
    move(screenGeometry.center() - rect().center());
  }

  if (!Settings::value(Settings::Gui::LogExpanded).toBool())
    setFixedSize(size());
}

void MainWindow::setupControls()
{
  secureSocket(false);

  ui->btnConfigureServer->setIcon(QIcon::fromTheme(QStringLiteral("configure")));
  ui->btnConfigureClient->setIcon(QIcon::fromTheme(QStringLiteral("configure")));

  if (Settings::value(Settings::Core::LastVersion).toString() != kVersion) {
    Settings::setValue(Settings::Core::LastVersion, kVersion);
  }

  if (!Settings::value(Settings::Gui::LogExpanded).toBool()) {
    m_logDock->hide();
  }

  ui->serverOptions->setVisible(false);
  ui->clientOptions->setVisible(false);

  const auto coreMode = Settings::value(Settings::Core::CoreMode).value<Settings::CoreMode>();
  ui->rbModeClient->setChecked(coreMode == Settings::CoreMode::Client);
  ui->rbModeServer->setChecked(coreMode == Settings::CoreMode::Server);

  ui->lineEditName->setValidator(new QRegularExpressionValidator(m_nameRegEx, this));
  ui->lineEditName->setVisible(false);
  ui->lineEditName->installEventFilter(this);

  if (deskflow::platform::isMac()) {
    ui->rbModeServer->setAttribute(Qt::WA_MacShowFocusRect, false);
    ui->rbModeClient->setAttribute(Qt::WA_MacShowFocusRect, false);
    ui->btnSaveServerConfig->setFixedWidth(ui->btnSaveServerConfig->height());
  } else {
    ui->btnSaveServerConfig->setIconSize(QSize(22, 22));
  }
  setStatusBar(m_statusBar);
}

void MainWindow::setupLanBridgeControls()
{
  auto *panel = new QGroupBox(QStringLiteral("LanBridge · 局域网键鼠共享"), this);
  auto *layout = new QVBoxLayout(panel);
  auto *intro = new QLabel(
      deskflow::platform::isMac() ? QStringLiteral("本机：Mac（mac）｜使用 Linux 的键鼠，连接 192.168.50.219:24800")
                                  : QStringLiteral("本机：Linux（linux）｜共享这台机器的键盘和鼠标给 Mac"),
      panel
  );
  intro->setWordWrap(true);
  layout->addWidget(intro);

  m_lanBridgeStatus = new QLabel(panel);
  m_lanBridgeStatus->setWordWrap(true);
  layout->addWidget(m_lanBridgeStatus);
  auto *hint = new QLabel(
      QStringLiteral(
          "鼠标越过边缘切换，键盘跟随鼠标。按住鼠标时锁定当前屏幕。\n"
          "紧急返回 Linux：Ctrl + Alt + Esc。操作 Mac 时可用 Ctrl+C / Ctrl+V 复制、粘贴。"
      ),
      panel
  );
  hint->setWordWrap(true);
  layout->addWidget(hint);

  auto *options = new QFormLayout;
  m_macPosition = new QComboBox(panel);
  m_macPosition->addItem(QStringLiteral("右侧（默认）"), QStringLiteral("right"));
  m_macPosition->addItem(QStringLiteral("左侧"), QStringLiteral("left"));
  m_macPosition->addItem(QStringLiteral("上方"), QStringLiteral("up"));
  m_macPosition->addItem(QStringLiteral("下方"), QStringLiteral("down"));
  m_macPosition->setCurrentIndex(qMax(0, m_macPosition->findData(Settings::value(Settings::LanBridge::MacPosition))));
  if (!deskflow::platform::isMac()) {
    options->addRow(QStringLiteral("Mac 在 Linux 的哪一侧："), m_macPosition);
  } else {
    m_macPosition->hide();
    auto *layoutHelp = new QLabel(QStringLiteral("屏幕位置请在 Linux 的 LanBridge 窗口中设置。"), panel);
    layoutHelp->setWordWrap(true);
    options->addRow(layoutHelp);
  }
  connect(m_macPosition, &QComboBox::currentIndexChanged, this, [this] {
    if (m_coreProcess.processState() != ProcessState::Stopped)
      return;
    const auto position = m_macPosition->currentData().toString();
    Settings::setValue(Settings::LanBridge::MacPosition, position);
    m_serverConfig.configureLanBridge(position);
    m_serverConfig.commit();
    Settings::save();
  });
  if (!deskflow::platform::isMac()) {
    m_clipboardSharing = new QCheckBox(QStringLiteral("共享剪贴板（两台机器间复制、粘贴）"), panel);
    m_clipboardSharing->setChecked(Settings::value(Settings::Server::EnableClipboard).toBool());
    m_clipboardSharing->setToolTip(QStringLiteral("停止共享后可调整，重新连接后生效。"));
    options->addRow(m_clipboardSharing);
    connect(m_clipboardSharing, &QCheckBox::toggled, this, [this](bool enabled) {
      if (m_coreProcess.processState() != ProcessState::Stopped || !Settings::isWritable())
        return;
      Settings::setValue(Settings::Server::EnableClipboard, enabled);
      if (enabled && Settings::value(Settings::Server::ClipboardSize).toUInt() == 0)
        Settings::setValue(Settings::Server::ClipboardSize, Settings::defaultValue(Settings::Server::ClipboardSize));
      m_serverConfig.commit();
      Settings::save();
    });
  } else {
    auto *clipboardHelp = new QLabel(QStringLiteral("剪贴板共享由 Linux 端设置，重新连接后生效。"), panel);
    clipboardHelp->setWordWrap(true);
    options->addRow(clipboardHelp);
  }
  m_diagnostics = new QCheckBox(QStringLiteral("性能诊断（只记录耗时，不记录按键内容）"), panel);
  m_diagnostics->setChecked(Settings::value(Settings::LanBridge::DiagnosticsEnabled).toBool());
  options->addRow(m_diagnostics);
  auto *diagnosticsReport = new QPushButton(QStringLiteral("查看 / 导出性能报告"), panel);
  connect(diagnosticsReport, &QPushButton::clicked, this, &MainWindow::showDiagnosticsReport);
  options->addRow(diagnosticsReport);
  connect(m_diagnostics, &QCheckBox::toggled, this, [](bool enabled) {
    Settings::setValue(Settings::LanBridge::DiagnosticsEnabled, enabled);
    Settings::save();
  });
  m_loginStartup = new QCheckBox(QStringLiteral("登录后启动 LanBridge"), panel);
  options->addRow(m_loginStartup);
  m_startupStatus = new QLabel(panel);
  m_startupStatus->setWordWrap(true);
  options->addRow(m_startupStatus);
  layout->addLayout(options);

  m_startupManager = new StartupManager(this);
  connect(
      m_startupManager, &StartupManager::stateChanged, this,
      [this](StartupManager::State state, const QString &detail) {
        const QSignalBlocker block(m_loginStartup);
        m_loginStartup->setChecked(state == StartupManager::State::Enabled);
        m_loginStartup->setEnabled(state != StartupManager::State::Busy && state != StartupManager::State::Unavailable);
        m_startupStatus->setText(detail);
      }
  );
  connect(m_loginStartup, &QCheckBox::toggled, m_startupManager, &StartupManager::setEnabled);
  m_startupManager->refresh();

  m_permissionStatus = new QLabel(panel);
  m_permissionStatus->setWordWrap(true);
  layout->addWidget(m_permissionStatus);
  auto *buttons = new QHBoxLayout;
  auto *help = new QPushButton(QStringLiteral("连接与使用帮助"), panel);
  connect(help, &QPushButton::clicked, this, &MainWindow::showLanBridgeHelp);
  buttons->addWidget(help);
#ifdef Q_OS_MACOS
  auto *permission = new QPushButton(QStringLiteral("打开辅助功能设置"), panel);
  connect(permission, &QPushButton::clicked, this, [] {
    QDesktopServices::openUrl(
        QUrl(QStringLiteral("x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"))
    );
  });
  buttons->addWidget(permission);
#endif
  m_recoveryButton = new QPushButton(QStringLiteral("我已在 Mac 本地恢复输入"), panel);
  connect(m_recoveryButton, &QPushButton::clicked, this, &MainWindow::acknowledgeInputRecovery);
  buttons->addWidget(m_recoveryButton);
  buttons->addStretch();
  layout->addLayout(buttons);
  qobject_cast<QVBoxLayout *>(ui->topLevelWidget->layout())->insertWidget(0, panel);
  ui->widgetModeSelection->hide();
  ui->btnEditName->hide();
  ui->btnRestartCore->setText(QStringLiteral("重新连接"));
  ui->btnConfigureServer->setText(QStringLiteral("高级屏幕设置"));
  ui->btnConfigureClient->setText(QStringLiteral("键盘与滚动设置"));
  // Keep the connection actions on their own row and preserve the address field's
  // native preferred width even when restoring an older, narrower window.
  ui->horizontalLayout_6->addStretch();
  ui->lineHostname->setMinimumWidth(ui->lineHostname->sizeHint().width());

  auto *statusTimer = new QTimer(this);
  statusTimer->setInterval(2000);
  connect(statusTimer, &QTimer::timeout, this, &MainWindow::updateLanBridgeStatus);
  statusTimer->start();
  updateLanBridgeStatus();
}

bool MainWindow::inputRecoveryRequired() const
{
#ifdef Q_OS_MACOS
  // A live receiver intentionally has a marker. Only a fully stopped process can be acknowledged.
  return m_coreProcess.processState() == ProcessState::Stopped && deskflow::RemoteInputSession::recoveryRequired();
#else
  return false;
#endif
}

void MainWindow::updateLanBridgeStatus()
{
  if (!m_lanBridgeStatus)
    return;
  const bool stopped = m_coreProcess.processState() == ProcessState::Stopped;
  const bool needsRecovery = inputRecoveryRequired();
  m_recoveryButton->setVisible(needsRecovery);
  m_recoveryButton->setEnabled(stopped);
  m_macPosition->setEnabled(stopped);
  m_diagnostics->setEnabled(stopped);
  if (m_clipboardSharing)
    m_clipboardSharing->setEnabled(stopped && Settings::isWritable());
  if (needsRecovery) {
    m_lanBridgeStatus->setText(QStringLiteral("需要本地恢复：先在 Mac 本地恢复输入，必要时注销登录，确认恢复后再连接。")
    );
    toggleCanRunCore(false);
  } else {
    QString text;
    switch (m_coreProcess.processState()) {
    case ProcessState::Stopped:
      text = QStringLiteral("已停止共享");
      break;
    case ProcessState::Starting:
      text = QStringLiteral("正在启动共享…");
      break;
    case ProcessState::Stopping:
      text = QStringLiteral("正在停止共享并释放输入…");
      break;
    case ProcessState::RetryPending:
      text = QStringLiteral("连接已断开，正在准备重试…");
      break;
    case ProcessState::Started:
      switch (m_coreProcess.connectionState()) {
      case ConnectionState::Connected:
        text = QStringLiteral("已连接，键鼠共享已就绪");
        break;
      case ConnectionState::Listening:
        text = m_connectedClients.isEmpty()
                   ? QStringLiteral("正在等待 Mac 连接")
                   : QStringLiteral("已连接：%1；键鼠共享已就绪").arg(m_connectedClients.join(QStringLiteral("、")));
        break;
      case ConnectionState::Connecting:
        text = QStringLiteral("正在连接 Linux…");
        break;
      case ConnectionState::Disconnected:
        text = QStringLiteral("连接已断开，等待恢复…");
        break;
      }
    }
    m_lanBridgeStatus->setText(text);
  }
#ifdef Q_OS_MACOS
  m_permissionStatus->setText(
      AXIsProcessTrusted()
          ? QStringLiteral("辅助功能权限：已授权。若系统询问局域网访问，请允许。")
          : QStringLiteral("辅助功能权限：未授权。请在系统设置中允许 LanBridge 控制本机，然后重新连接。")
  );
#else
  m_permissionStatus->setText(QStringLiteral("需要 GNOME X11 会话；键鼠连接在 Linux 上。"));
#endif
}

void MainWindow::acknowledgeInputRecovery()
{
#ifdef Q_OS_MACOS
  if (!inputRecoveryRequired())
    return;
  QMessageBox confirmation(
      QMessageBox::Warning, QStringLiteral("确认本地输入已恢复"),
      QStringLiteral(
          "请先使用 Mac 本机的键盘、鼠标确认没有卡住的按键或拖拽。必要时先注销登录。\n"
          "只有确认本地输入已恢复，才能解除暂停。"
      ),
      QMessageBox::NoButton, this
  );
  auto *confirm = confirmation.addButton(QStringLiteral("已恢复，解除暂停"), QMessageBox::AcceptRole);
  confirmation.addButton(QStringLiteral("暂不解除"), QMessageBox::RejectRole);
  confirmation.exec();
  if (confirmation.clickedButton() != confirm || !inputRecoveryRequired())
    return;
  if (!deskflow::RemoteInputSession::acknowledgeRecovery()) {
    QMessageBox::warning(
        this, QStringLiteral("无法解除暂停"),
        QStringLiteral("恢复标记无法清除，或仍有共享进程运行。请停止所有 LanBridge 共享进程后重试。")
    );
    return;
  }
  toggleCanRunCore(canRunCore());
  updateLanBridgeStatus();
#endif
}

void MainWindow::showDiagnosticsReport()
{
  QFile report(QDir(Settings::settingsPath()).filePath(QStringLiteral("diagnostics.json")));
  if (!report.open(QIODevice::ReadOnly)) {
    QMessageBox::information(
        this, QStringLiteral("尚无性能报告"),
        QStringLiteral("请停止共享，勾选性能诊断后重新连接，并操作键鼠至少 5 秒。报告会每 5 秒更新。")
    );
    return;
  }
  const auto contents = report.readAll();
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(contents, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) {
    QMessageBox::warning(
        this, QStringLiteral("无法读取报告"), QStringLiteral("性能报告格式无效，请重新开启诊断后重试。")
    );
    return;
  }
  const auto data = document.object();
  auto stageText = [](const QString &label, const QJsonObject &stage) {
    if (stage.value(QStringLiteral("samples")).toInteger() == 0)
      return QStringLiteral("%1：尚无样本").arg(label);
    return QStringLiteral("%1：%2 个样本\np50 %3 ms · p95 %4 ms · p99 %5 ms")
        .arg(label)
        .arg(stage.value(QStringLiteral("samples")).toInteger())
        .arg(stage.value(QStringLiteral("p50_ms")).toDouble(), 0, 'f', 2)
        .arg(stage.value(QStringLiteral("p95_ms")).toDouble(), 0, 'f', 2)
        .arg(stage.value(QStringLiteral("p99_ms")).toDouble(), 0, 'f', 2);
  };
  const auto summary =
      QStringLiteral(
          "报告时间（UTC）：%1\n\n%2\n\n%3\n\n发送队列峰值：%4 字节\n"
          "目标：本机各阶段 p95 ≤ 5 ms。此报告不包含网络往返或屏幕显示延迟。"
      )
          .arg(
              data.value(QStringLiteral("updated_utc")).toString(),
              stageText(QStringLiteral("捕获到发送"), data.value(QStringLiteral("capture_to_send")).toObject()),
              stageText(QStringLiteral("接收到注入"), data.value(QStringLiteral("receive_to_inject")).toObject())
          )
          .arg(data.value(QStringLiteral("socket_queue_peak_bytes")).toInteger());
  QMessageBox viewer(
      QMessageBox::Information, QStringLiteral("本机性能报告快照"), summary, QMessageBox::NoButton, this
  );
  viewer.setDetailedText(QString::fromUtf8(contents));
  auto *exportButton = viewer.addButton(QStringLiteral("导出报告…"), QMessageBox::AcceptRole);
  viewer.addButton(QStringLiteral("关闭"), QMessageBox::RejectRole);
  viewer.exec();
  if (viewer.clickedButton() != exportButton)
    return;
  const auto path = QFileDialog::getSaveFileName(
      this, QStringLiteral("导出性能报告"),
      QStringLiteral("lanbridge-%1-diagnostics.json").arg(Settings::value(Settings::Core::ComputerName).toString()),
      QStringLiteral("JSON (*.json)")
  );
  if (path.isEmpty())
    return;
  QSaveFile output(path);
  if (!output.open(QIODevice::WriteOnly) || output.write(contents) != contents.size() || !output.commit())
    QMessageBox::warning(this, QStringLiteral("导出失败"), QStringLiteral("无法保存报告，请选择可写入的位置。"));
}

void MainWindow::showLanBridgeHelp()
{
  QMessageBox::information(
      this, QStringLiteral("LanBridge 使用帮助"),
      QStringLiteral(
          "1. 在 Linux 的 GNOME X11 会话中启动共享；在 Mac 上允许辅助功能权限并连接。\n"
          "2. 首次连接请在两台机器上核对 TLS 证书指纹，再确认信任。证书变化会停止共享。\n"
          "3. 默认 Linux 在左、Mac 在右；停止共享后可在 Linux 窗口调整位置。\n"
          "4. 鼠标越过屏幕边缘切换；按住按钮时保持在当前机器，松开后可跨屏。\n"
          "5. Ctrl + Alt + Esc 紧急返回 Linux；中文输入使用当前机器自己的输入法。\n"
          "6. Linux 端开启“共享剪贴板”后，可在两台机器间复制、粘贴。操作 Mac 时支持 Ctrl+C / Ctrl+V。\n"
          "7. 使用“停止共享”或退出软件结束共享；“重新连接”用于普通连接故障。\n"
          "性能诊断需停止共享后启用，重新连接后生效；性能报告只包含耗时与队列统计，不包含按键内容。"
      )
  );
}

//////////////////////////////////////////////////////////////////////////////
// Begin slots
//////////////////////////////////////////////////////////////////////////////

// remember: using queued connection allows the render loop to run before
// executing the slot. the default is to instantly call the slot when the
// signal is emitted from the thread that owns the receiver's object.
void MainWindow::connectSlots()
{
  connect(Settings::instance(), &Settings::serverSettingsChanged, this, &MainWindow::serverConfigSaving);
  connect(Settings::instance(), &Settings::settingsChanged, this, &MainWindow::settingsChanged);

  connect(&m_coreProcess, &CoreProcess::error, this, &MainWindow::coreProcessError);
  connect(&m_coreProcess, &CoreProcess::logLine, this, &MainWindow::handleLogLine);
  connect(
      &m_coreProcess, &CoreProcess::processStateChanged, this, &MainWindow::coreProcessStateChanged,
      Qt::QueuedConnection
  );
  connect(&m_coreProcess, &CoreProcess::connectionStateChanged, this, &MainWindow::coreConnectionStateChanged);
  connect(&m_coreProcess, &CoreProcess::secureSocket, this, &MainWindow::secureSocket);
  connect(
      &m_coreProcess, &CoreProcess::daemonIpcClientConnectionFailed, this, &MainWindow::daemonIpcClientConnectionFailed
  );
  connect(&m_coreProcess, &CoreProcess::securityLevelChanged, m_statusBar, &StatusBar::setSecurityLevel);

  connect(m_actionAbout, &QAction::triggered, this, &MainWindow::openAboutDialog);
  connect(m_actionMinimize, &QAction::triggered, this, &MainWindow::hide);

#ifdef Q_OS_MACOS
  connect(m_actionQuit, &QAction::triggered, qApp, &QCoreApplication::quit);
  connect(m_actionTrayQuit, &QAction::triggered, qApp, &QCoreApplication::quit);
#else
  connect(m_actionQuit, &QAction::triggered, this, &MainWindow::close);
  connect(m_actionTrayQuit, &QAction::triggered, this, &MainWindow::close);
#endif
  connect(m_actionRestore, &QAction::triggered, this, &MainWindow::showAndActivate);
  connect(m_actionSettings, &QAction::triggered, this, &MainWindow::openSettings);
  connect(m_actionStartCore, &QAction::triggered, this, &MainWindow::startCore);
  connect(m_actionRestartCore, &QAction::triggered, this, &MainWindow::resetCore);
  connect(m_actionStopCore, &QAction::triggered, this, &MainWindow::stopCore);
  connect(m_actionShowHelp, &QAction::triggered, this, &MainWindow::showLanBridgeHelp);

  // Mac os tray will only show a menu
  if (!deskflow::platform::isMac())
    connect(m_trayIcon, &QSystemTrayIcon::activated, this, &MainWindow::trayIconActivated);

  connect(&m_coreProcess, &CoreProcess::connectedClientsChanged, this, &MainWindow::serverClientsChanged);
  connect(&m_coreProcess, &CoreProcess::unrecognisedClient, this, &MainWindow::handleUnrecognisedClient);
  connect(&m_coreProcess, &CoreProcess::connectionRefused, this, &MainWindow::handleConnectionRefused);
  connect(&m_coreProcess, &CoreProcess::retryIn, this, &MainWindow::updateTimeoutDelay);
  connect(&m_coreProcess, &CoreProcess::peerFingerprint, this, &MainWindow::handlePeerFingerprint);
  connect(&m_coreProcess, &CoreProcess::missingKeyboardLayouts, this, &MainWindow::handleMissingKeyboardLayouts);

  // Start from the actual stopped state; recovery may veto a saved autostart preference.
  connect(ui->btnToggleCore, &QPushButton::clicked, m_actionStartCore, &QAction::trigger, Qt::UniqueConnection);

  connect(ui->btnRestartCore, &QPushButton::clicked, this, &MainWindow::resetCore);

  connect(ui->lineHostname, &QLineEdit::returnPressed, ui->btnRestartCore, &QPushButton::click);
  connect(ui->lineHostname, &QLineEdit::textChanged, this, &MainWindow::remoteHostChanged);

  connect(ui->btnSaveServerConfig, &QPushButton::clicked, this, &MainWindow::saveServerConfig);
  connect(ui->btnConfigureServer, &QPushButton::clicked, this, [this] { showConfigureServer(""); });
  connect(ui->btnConfigureClient, &QPushButton::clicked, this, [this] { showConfigureClient(); });
  connect(ui->lblComputerName, &QLabel::linkActivated, this, &MainWindow::openSettings);

  connect(ui->rbModeServer, &QRadioButton::toggled, this, &MainWindow::coreModeToggled);
  connect(ui->rbModeClient, &QRadioButton::toggled, this, &MainWindow::coreModeToggled);

  connect(m_logDock->toggleViewAction(), &QAction::toggled, this, &MainWindow::toggleLogVisible);

  connect(m_statusBar, &StatusBar::requestShowMyFingerprints, this, &MainWindow::showMyFingerprint);
  connect(m_statusBar, &StatusBar::requestUpdateVersion, this, &MainWindow::openGetNewVersionUrl);
  connect(&m_versionChecker, &VersionChecker::updateFound, m_statusBar, &StatusBar::updateFound);

  connect(m_guiDupeChecker, &QLocalServer::newConnection, this, &MainWindow::showAndActivate);

  connect(ui->btnEditName, &QPushButton::clicked, this, &MainWindow::showHostNameEditor);

  connect(ui->lineEditName, &QLineEdit::editingFinished, this, &MainWindow::setHostName);

  connect(m_networkMonitor, &NetworkMonitor::ipAddressesChanged, this, &MainWindow::updateIpLabel);
}

void MainWindow::toggleLogVisible(bool visible)
{
  // When the main window is hidden e.g. close to tray / minimized), this also triggers the log visibility toggle,
  // but we don't want to hide the log in this case since we would need to un-hide it when the window is shown again.
  if (!isVisible() || isMinimized()) {
    qDebug() << "not toggling log, window not visible";
    return;
  }

  setFixedSize(16777215, 16777215);
  Settings::setValue(Settings::Gui::LogExpanded, visible);
  if (visible) {
    if (m_logDock->isFloating()) {
      adjustSize();
      setFixedSize(size());
    } else {
      QTimer::singleShot(15, this, [&] { resize(m_expandedSize); });
    }
  } else {
    if (!m_logDock->isFloating()) {
      m_expandedSize = geometry().size();
    }
    m_logDock->hide();
    if (!m_logDock->isFloating()) {
      adjustSize();
    }
    setFixedSize(size());
  }
  Settings::setValue(Settings::Gui::WindowGeometry, geometry());
}

void MainWindow::settingsChanged(const QString &key)
{
  if (key == Settings::Server::EnableClipboard && m_clipboardSharing) {
    const QSignalBlocker block(m_clipboardSharing);
    m_clipboardSharing->setChecked(Settings::value(key).toBool());
  }
  if (key == Settings::Log::Level) {
    m_coreProcess.applyLogLevel();
    return;
  }

  if (key == Settings::Core::ComputerName)
    updateComputerName();

  if ((key == Settings::Security::Certificate) || (key == Settings::Security::KeySize) ||
      (key == Settings::Security::TlsEnabled) || (key == Settings::Security::CheckPeers)) {
    if (TlsUtility::isEnabled()) {
      if (!TlsUtility::isCertValid()) {
        qWarning() << tr("invalid certificate, generating a new one");
        TlsUtility::generateCertificate();
      }
      m_fingerprint = {QCryptographicHash::Sha256, TlsUtility::certFingerprint()};
      updateFingerprintButton();
    }
    updateSecurityIcon(m_statusBar->securityIconVisible());
    return;
  }
}

void MainWindow::serverConfigSaving()
{
  m_serverConfig.commit();
}

void MainWindow::trayIconActivated(QSystemTrayIcon::ActivationReason reason)
{
  if (reason != QSystemTrayIcon::Trigger)
    return;
  isVisible() ? hide() : showAndActivate();
}

void MainWindow::coreProcessError(CoreProcess::Error error)
{
#ifdef Q_OS_MACOS
  if (m_quitPrepared)
    return;
#endif
  if (error == CoreProcess::Error::RecoveryRequired) {
    showAndActivate();
    updateLanBridgeStatus();
  } else if (error == CoreProcess::Error::AddressMissing) {
    QMessageBox::warning(
        this, tr("Address missing"), tr("Please enter the hostname or IP address of the other computer.")
    );
  } else if (error == CoreProcess::Error::StartFailed) {
    show();
    auto message = tr("The Core executable could not be started.\n"
                      "Please check if you have sufficient permissions to run %1.")
                       .arg(kCoreBinName);

    if (Settings::value(Settings::Core::CoreMode) == Settings::CoreMode::Server) {
      const auto mode =
          Settings::value(Settings::Server::ExternalConfigFile).toBool() ? tr("read") : tr("read and write");
      message.append(tr("\nAdditionally, check you are able to %1 the server config file: %2")
                         .arg(mode, Settings::serverConfigFile()));
    }
    QMessageBox::warning(this, kAppName, message);
  }
}

void MainWindow::startCore()
{
  if (inputRecoveryRequired()) {
    updateLanBridgeStatus();
    showAndActivate();
    return;
  }
  // Save current IP state when server starts
  if (m_coreProcess.mode() == CoreMode::Server && Settings::value(Settings::Core::Interface).toString().isEmpty()) {
    m_serverStartIPs = NetworkMonitor::validAddresses();
    m_serverStartSuggestedIP = m_serverStartIPs.isEmpty() ? "" : m_serverStartIPs.first();
  }

  m_actionStartCore->setVisible(false);
  m_actionRestartCore->setVisible(true);
  m_coreProcess.start();
}

void MainWindow::stopCore()
{
  qDebug() << "stopping core process";
  m_coreProcess.stop();
  m_actionStartCore->setVisible(true);
  m_actionRestartCore->setVisible(false);
}

void MainWindow::clearSettings()
{
  if (m_coreProcess.processState() != ProcessState::Stopped || inputRecoveryRequired()) {
    QMessageBox::warning(
        this, QStringLiteral("暂时不能重置"),
        QStringLiteral("请先停止共享；若提示本地恢复，完成 Mac 本地输入恢复后才能重置设置。")
    );
    return;
  }
  if (!diagnostic::clearSettings(true)) {
    QMessageBox::warning(
        this, QStringLiteral("无法重置设置"),
        QStringLiteral("仍有共享进程运行、需要本地恢复，或设置目录无法写入。请先停止所有共享进程并完成恢复。")
    );
    return;
  }

  m_networkMonitor->stopMonitoring();

  disconnect(&m_coreProcess, nullptr, this, nullptr);
  disconnect(&m_versionChecker, nullptr, this, nullptr);
  disconnect(m_guiDupeChecker, nullptr, this, nullptr);
  disconnect(m_trayIcon, nullptr, this, nullptr);
  disconnect(m_logDock->toggleViewAction(), nullptr, this, nullptr);

  m_coreProcess.clearSettings();
  m_saveOnExit = false;
}

bool MainWindow::saveServerConfig()
{
  QString fileName = QFileDialog::getSaveFileName(this, tr("Save server configuration as..."));

  if (!fileName.isEmpty() && !m_serverConfig.save(fileName)) {
    QMessageBox::warning(this, tr("Save failed"), tr("Could not save server configuration to file."));
    return true;
  }

  return false;
}

void MainWindow::openAboutDialog()
{
  AboutDialog about(this);
  about.exec();
}

void MainWindow::openGetNewVersionUrl() const
{
  QDesktopServices::openUrl(QUrl(kUrlDownload));
}

void MainWindow::openSettings()
{
  auto dialog = SettingsDialog(this, m_serverConfig);

  connect(&dialog, &SettingsDialog::requestRemoveAllSettings, this, &MainWindow::clearSettings, Qt::UniqueConnection);
  if (dialog.exec() == QDialog::Accepted) {
    Settings::save();
    disconnect(&dialog, &SettingsDialog::requestRemoveAllSettings, nullptr, nullptr);

    applyConfig();

    if (m_coreProcess.isStarted()) {
      m_coreProcess.restart();
    }
  }
}

void MainWindow::resetCore()
{
  m_coreProcess.restart();
}

void MainWindow::showMyFingerprint()
{
  FingerprintDialog fingerprintDialog(this, m_fingerprint);
  fingerprintDialog.exec();
}

void MainWindow::coreModeToggled(bool checked)
{
  // this method is called when rbClient or rbServer toggles
  // with both being in the same group one must be turned on if the other is turned off
  // only react to toggle on to avoid calling everything twice when the user switches modes
  if (!checked)
    return;

  Settings::CoreMode mode = Settings::CoreMode::None;

  if (ui->rbModeServer->isChecked())
    mode = Settings::CoreMode::Server;
  if (ui->rbModeClient->isChecked())
    mode = Settings::CoreMode::Client;

  qDebug() << QStringLiteral("change mode to: %1").arg(QVariant::fromValue(mode).toString());

  if (m_coreProcess.isStarted() && m_coreProcess.mode() != mode)
    m_coreProcess.stop();
  m_coreProcess.setMode(mode);

  Settings::setValue(Settings::Core::CoreMode, mode);
  Settings::save();

  updateModeControls();
}

void MainWindow::updateModeControls()
{
  const auto mode = m_coreProcess.mode();
  const bool isServer = mode == Settings::CoreMode::Server;
  const bool isClient = mode == Settings::CoreMode::Client;

  ui->serverOptions->setVisible(isServer);
  ui->clientOptions->setVisible(isClient);
  ui->lblNoMode->setVisible(!isServer && !isClient);
  toggleCanRunCore(canRunCore());

  ui->lblIpAddresses->setVisible(
      (isClient && !Settings::value(Settings::Core::Interface).toString().isEmpty()) || isServer
  );

  if (ui->lblIpAddresses->isVisible())
    updateNetworkInfo();

  if (isServer) {
    m_networkMonitor->startMonitoring();
  } else {
    m_networkMonitor->stopMonitoring();
  }

  if (isServer || isClient)
    updateModeControlLabels();
}

void MainWindow::updateModeControlLabels()
{
  const bool isServer = m_coreProcess.mode() == CoreMode::Server;
  const bool isStarted = m_coreProcess.processState() != ProcessState::Stopped;

  QString startText;
  QString stopText;
  QIcon startIcon;
  QIcon stopIcon;

  if (isServer) {
    startText = QStringLiteral("开始共享");
    stopText = QStringLiteral("停止共享");
    startIcon = QIcon::fromTheme(QStringLiteral("system-run"));
    stopIcon = QIcon::fromTheme(QIcon::ThemeIcon::ProcessStop);
  } else {
    startText = QStringLiteral("连接 Linux");
    stopText = QStringLiteral("停止共享");
    startIcon = QIcon::fromTheme(QStringLiteral("network-connect"));
    stopIcon = QIcon::fromTheme(QStringLiteral("network-disconnect"));
  }

  m_actionStartCore->setText(startText);
  m_actionStartCore->setIcon(startIcon);
  m_actionStopCore->setText(stopText);
  m_actionStopCore->setIcon(stopIcon);

  if (isStarted) {
    ui->btnToggleCore->setText(stopText);
    ui->btnToggleCore->setIcon(stopIcon);
  } else {
    ui->btnToggleCore->setText(startText);
    ui->btnToggleCore->setIcon(startIcon);
  }
}

void MainWindow::updateSecurityIcon(bool visible)
{
  m_statusBar->setSecurityIconVisible(visible);
  if (!visible)
    return;
  m_statusBar->setSecurityIcon(TlsUtility::isEnabled());
}

void MainWindow::updateNetworkInfo()
{
  const auto mode = m_coreProcess.mode();
  if (mode == CoreMode::None) {
    return;
  }

  if (mode == CoreMode::Server)
    updateIpLabel(NetworkMonitor::validAddresses());
  else
    updateIpLabel({Settings::value(Settings::Core::Interface).toString()});
}

void MainWindow::serverConnectionConfigureClient(const QString &clientName)
{
  m_serverConfigDialogVisible = true;
  ServerConfigDialog dialog(this, m_serverConfig);
  if (dialog.addClient(clientName) && dialog.exec() == QDialog::Accepted) {
    m_coreProcess.restart();
  }
  m_serverConfigDialogVisible = false;
}

//////////////////////////////////////////////////////////////////////////////
// End slots
//////////////////////////////////////////////////////////////////////////////

void MainWindow::open()
{
  if (!Settings::value(Settings::Gui::Autohide).toBool() || !QSystemTrayIcon::isSystemTrayAvailable() ||
      inputRecoveryRequired())
    showAndActivate();
  else if (deskflow::platform::isMac())
    // macOS to call hide after this function ends
    QTimer::singleShot(1, this, &MainWindow::hide);
  else
    hide();

  // if a critical error was shown just before the main window (i.e. on app
  // load), it will be hidden behind the main window. therefore we need to raise
  // it up in front of the main window.
  // HACK: because the `onShown` event happens just as the window is shown, the
  // message box has a chance of being raised under the main window. to solve
  // this we delay the error dialog raise by a split second. this seems a bit
  // hacky and fragile, so maybe there's a better approach.
  const auto kCriticalDialogDelay = 100;
  QTimer::singleShot(kCriticalDialogDelay, this, &messages::raiseCriticalDialog);

  if (!Settings::value(Settings::Gui::AutoUpdateCheck).isValid()) {
    Settings::setValue(Settings::Gui::AutoUpdateCheck, messages::showUpdateCheckOption(this));
  }

  if (Settings::value(Settings::Gui::AutoUpdateCheck).toBool()) {
    m_versionChecker.checkLatest();
  } else {
    qDebug() << "skipping check for new version, disabled";
  }

  if (!Settings::value(Settings::LanBridge::SetupComplete).toBool()) {
    showAndActivate();
    showLanBridgeHelp();
    Settings::setValue(Settings::LanBridge::SetupComplete, true);
  }
  updateLanBridgeStatus();
  if (Settings::value(Settings::Gui::AutoStartCore).toBool() && !inputRecoveryRequired()) {
    if (ui->rbModeClient->isChecked() && ui->lineHostname->text().isEmpty())
      return;
    startCore();
  }
}

void MainWindow::createMenuBar()
{
  m_menuFile->addAction(m_actionStartCore);
  m_menuFile->addAction(m_actionRestartCore);
  m_menuFile->addAction(m_actionStopCore);
  m_menuFile->addSeparator();
  m_menuFile->addAction(m_actionQuit);

  m_menuEdit->addAction(m_actionSettings);

  m_menuView->addAction(m_logDock->toggleViewAction());

  m_menuHelp->addAction(m_actionAbout);
  m_menuHelp->addAction(m_actionShowHelp);

  auto menuBar = new QMenuBar(this);
  menuBar->addMenu(m_menuFile);
  menuBar->addMenu(m_menuEdit);
  menuBar->addMenu(m_menuView);
  menuBar->addMenu(m_menuHelp);

  setMenuBar(menuBar);
}

void MainWindow::setupTrayIcon()
{
  auto trayMenu = new QMenu(this);
  trayMenu->addActions(
      {m_actionStartCore, m_actionRestartCore, m_actionStopCore, m_actionMinimize, m_actionRestore, m_actionTrayQuit}
  );
  trayMenu->insertSeparator(m_actionMinimize);
  trayMenu->insertSeparator(m_actionTrayQuit);
  m_trayIcon->setContextMenu(trayMenu);

  setTrayIcon();
  m_trayIcon->show();
}

void MainWindow::applyConfig()
{
  if (Settings::value(Settings::Gui::ShowVersionInTitle).toBool()) {
    setWindowTitle(QStringLiteral("%1 - %2").arg(kAppName, kDisplayVersion));
  } else {
    setWindowTitle(kAppName);
  }

  if (const auto host = Settings::value(Settings::Client::RemoteHost).toString(); !host.isEmpty())
    ui->lineHostname->setText(host);

  updateFingerprintButton();
  setTrayIcon();

  if (const auto ip = Settings::value(Settings::Core::Interface).toString(); !ip.isEmpty()) {
    m_serverStartIPs = {ip};
    m_serverStartSuggestedIP = ip;
  }

  coreModeToggled(true);
}

void MainWindow::saveSettings() const
{
  if (ui->rbModeClient->isChecked()) {
    Settings::setValue(Settings::Core::CoreMode, Settings::CoreMode::Client);
  } else if (ui->rbModeServer->isChecked()) {
    Settings::setValue(Settings::Core::CoreMode, Settings::CoreMode::Server);
  }
  if (!ui->lineHostname->text().isEmpty())
    Settings::setValue(Settings::Client::RemoteHost, ui->lineHostname->text());
  Settings::save();
}

void MainWindow::setTrayIcon()
{
  static const auto fallbackPath = QStringLiteral(":/icons/%1-%2/apps/64/%3.svg");

  // The fork keeps the upstream icon resources and their original identifiers.
  QString themeIcon = QStringLiteral("org.deskflow.deskflow");
  if (!Settings::value(Settings::Gui::SymbolicTrayIcon).toBool()) {
    if (deskflow::platform::isMac())
      m_trayIcon->setIcon(QIcon::fromTheme(themeIcon, QIcon(fallbackPath.arg(kAppId, iconMode(), themeIcon))));
    else
      m_trayIcon->setIcon(QIcon(fallbackPath.arg(kAppId, QStringLiteral("dark"), themeIcon)));
    return;
  }

  themeIcon.append(QStringLiteral("-symbolic"));

  if (deskflow::platform::isWindows()) {
    QSettings settings(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
        QSettings::NativeFormat
    );
    const QString theme = settings.value(QStringLiteral("SystemUsesLightTheme"), 1).toBool() ? QStringLiteral("light")
                                                                                             : QStringLiteral("dark");
    m_trayIcon->setIcon(QIcon(fallbackPath.arg(kAppId, theme, themeIcon)));
    return;
  }

  auto icon = QIcon::fromTheme(themeIcon, QIcon(fallbackPath.arg(kAppId, iconMode(), themeIcon)));
  icon.setIsMask(true);
  m_trayIcon->setIcon(icon);
}

void MainWindow::handleLogLine(const QString &line)
{
  m_logDock->appendLine(line);
}

void MainWindow::handleUnrecognisedClient(const QString &clientName)
{
  if (m_ignoredClients.contains(clientName)) {
    qDebug("ignoring %s:", qPrintable(clientName));
    return;
  }

  if (m_newClientPromptShowing || m_serverConfigDialogVisible)
    return;

  if (Settings::value(Settings::Server::ExternalConfig).toBool())
    return;

  if (m_serverConfig.isFull() || m_serverConfig.computerExists(clientName))
    return;

  m_newClientPromptShowing = true;

  showAndActivate();

  if (deskflow::gui::messages::showNewClientPrompt(this, clientName)) {
    serverConnectionConfigureClient(clientName);
  } else {
    m_ignoredClients.insert(clientName);
  }

  m_newClientPromptShowing = false;
}

void MainWindow::handleConnectionRefused(deskflow::core::ConnectionRefusal reason)
{
  if (reason != deskflow::core::ConnectionRefusal::AlreadyConnected)
    return;

  if (!isVisible() || m_clientErrorVisible)
    return;

  m_clientErrorVisible = true;
  showAndActivate();

  const auto address = Settings::value(Settings::Client::RemoteHost).toString();
  QMessageBox::warning(
      this, tr("%1 Connection Error").arg(kAppName),
      tr("<p>Failed to connect to the server '%1'.</p>"
         "<p>A Client with your name is already connected to the server.</p>"
         "Please ensure that you're using a unique name and that only a "
         "single instance of the client process is running.</p>")
          .arg(address)
  );

  m_clientErrorVisible = false;
}

void MainWindow::handleMissingKeyboardLayouts(const QString &layouts)
{
  if (Settings::value(Settings::Gui::IgnoreMissingKeyboardLayouts).toBool())
    return;

  QMessageBox msgBox(this);
  msgBox.setIcon(QMessageBox::Warning);
  msgBox.setWindowTitle(tr("Missing Keyboard Layouts"));
  msgBox.setText(tr("<p>Keyboard layout support requires matching layouts on all computers. "
                    "The following layouts from the other computer are not installed on this computer:</p>"
                    "<p><b>%1</b></p>"
                    "<p>Please install them to enable support for these layouts.</p>")
                     .arg(layouts));

  auto *checkBox = new QCheckBox(tr("Don't show this again"), &msgBox);
  msgBox.setCheckBox(checkBox);
  msgBox.exec();

  if (checkBox->isChecked()) {
    Settings::setValue(Settings::Gui::IgnoreMissingKeyboardLayouts, true);
  }
}

void MainWindow::handlePeerFingerprint(const QString &fingerprint)
{
  const auto sha256Text = QString(fingerprint).remove(':');
  const Fingerprint sha256 = {QCryptographicHash::Sha256, QByteArray::fromHex(sha256Text.toLatin1())};

  const bool isClient = m_coreProcess.mode() == CoreMode::Client;
  if ((isClient && m_checkedServers.contains(sha256Text)) || (!isClient && m_checkedClients.contains(sha256Text))) {
    qDebug("untrusted fingerprint already handled; keeping sharing stopped");
    m_coreProcess.stop();
    return;
  }

  FingerprintDatabase db;
  db.read(trustedFingerprintDatabase());

  if (db.isTrusted(sha256)) {
    qDebug("fingerprint is trusted");
    return;
  }

  if (isClient) {
    m_checkedServers.append(sha256Text);
    m_coreProcess.stop();
  } else {
    m_checkedClients.append(sha256Text);
  }

  // This two-machine product never silently expands an existing trust relationship.
  if (!db.fingerprints().isEmpty()) {
    m_coreProcess.stop();
    QMessageBox::critical(
        this, QStringLiteral("证书发生变化，已停止共享"),
        QStringLiteral("对方证书与之前信任的证书不同。请先在两台机器上核实证书变化原因，不能通过普通重连继续共享。")
    );
    return;
  }

  auto mode = isClient ? FingerprintDialogMode::Client : FingerprintDialogMode::Server;
  FingerprintDialog fingerprintDialog(this, m_fingerprint, mode, sha256);

  if (fingerprintDialog.exec() == QDialog::Accepted) {
    db.addTrusted(sha256);
    if (!db.write(trustedFingerprintDatabase())) {
      qCritical().noquote() << "unable to write fingerprint to db:" << trustedFingerprintDatabase();
      m_coreProcess.stop();
      return;
    }
    if (isClient) {
      m_checkedServers.removeAll(sha256Text);
      m_coreProcess.restart();
    } else {
      m_checkedClients.removeAll(sha256Text);
    }
  }
}

bool MainWindow::maybeHideToTray()
{
  if (!Settings::value(Settings::Gui::CloseToTray).toBool() || !QSystemTrayIcon::isSystemTrayAvailable()) {
    return false;
  }

  if (Settings::value(Settings::Gui::CloseReminder).toBool()) {
    messages::showCloseReminder(this);
    Settings::setValue(Settings::Gui::CloseReminder, false);
  }
  Settings::setValue(Settings::Gui::WindowGeometry, geometry());
  qDebug() << "hiding to tray";
  hide();
  return true;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
#ifdef Q_OS_MACOS
  if (!m_quitRequested) {
    Settings::setValue(Settings::Gui::WindowGeometry, geometry());
    hide();
    event->ignore();
    return;
  }
  // Qt's application-level Quit event owns termination after closing windows.
  event->accept();
#else
  if (event->spontaneous() && maybeHideToTray()) {
    event->ignore();
    return;
  }

  if (m_saveOnExit) {
    Settings::setValue(Settings::Gui::WindowGeometry, geometry());
    Settings::setValue(Settings::Gui::AutoStartCore, m_coreProcess.isStarted());
  }
  qDebug() << "quitting application";

  // any connected dock view acitons will be triggered
  // disconnect them before accepting the event
  disconnect(m_logDock->toggleViewAction(), &QAction::toggled, nullptr, nullptr);

  event->accept();
  QApplication::quit();
#endif
}

#ifdef Q_OS_MACOS
void MainWindow::prepareToQuit()
{
  if (m_quitPrepared)
    return;
  m_quitPrepared = true;

  if (m_saveOnExit) {
    Settings::setValue(Settings::Gui::WindowGeometry, geometry());
    Settings::setValue(Settings::Gui::AutoStartCore, m_coreProcess.isStarted());
  }
  m_networkMonitor->stopMonitoring();
  disconnect(&m_coreProcess, nullptr, this, nullptr);
  disconnect(m_logDock->toggleViewAction(), &QAction::toggled, nullptr, nullptr);
  if (!m_coreProcess.shutdown())
    qWarning("core did not shut down cleanly; input recovery state has been preserved");
}
#endif

void MainWindow::showFirstConnectedMessage()
{
  if (Settings::value(Settings::Gui::ShownFirstConnectedMessage).toBool())
    return;
  Settings::setValue(Settings::Gui::ShownFirstConnectedMessage, true);
  QMessageBox::information(
      this, QStringLiteral("键鼠共享已连接"),
      QStringLiteral("现在可以将 Linux 的鼠标移过屏幕边缘操作 Mac。Ctrl + Alt + Esc 可紧急返回 Linux。")
  );
}

void MainWindow::updateStatus()
{
  using enum ProcessState;
  const auto connection = m_coreProcess.connectionState();
  const auto process = m_coreProcess.processState();
  const bool isServer = (m_coreProcess.mode() == CoreMode::Server);
  if (process == Stopped || process == Started) {
    updateNetworkInfo();
    ui->btnEditName->setVisible(false);
  }
  m_statusBar->setStatus(connection, process, isServer);
  updateLanBridgeStatus();
}

void MainWindow::coreProcessStateChanged(ProcessState state)
{
#ifdef Q_OS_MACOS
  if (m_quitPrepared)
    return;
#endif
  using enum ProcessState;
  if (state == Stopped)
    m_connectedClients.clear();
  updateStatus();
  if (state == Started) {
    qDebug() << "recording that core has started";
    Settings::setValue(Settings::Gui::AutoStartCore, true);
    if (m_coreProcess.mode() == CoreMode::Server &&
        !Settings::value(Settings::Gui::ShownServerFirstStartMessage).toBool()) {
      qDebug() << "starting core server for first time";
      // The first-run LanBridge instructions already explain the server connection.

      Settings::setValue(Settings::Gui::ShownServerFirstStartMessage, true);
    }
  }

  if (state == Started || state == Starting || state == RetryPending) {
    disconnect(ui->btnToggleCore, &QPushButton::clicked, m_actionStartCore, &QAction::trigger);
    connect(ui->btnToggleCore, &QPushButton::clicked, m_actionStopCore, &QAction::trigger, Qt::UniqueConnection);

    ui->btnRestartCore->setEnabled(true);
    m_actionStartCore->setVisible(false);
    m_actionRestartCore->setVisible(true);
    m_actionStopCore->setEnabled(true);

    if (state == Starting) {
      saveSettings();
    }

  } else {
    disconnect(ui->btnToggleCore, &QPushButton::clicked, m_actionStopCore, &QAction::trigger);
    connect(ui->btnToggleCore, &QPushButton::clicked, m_actionStartCore, &QAction::trigger, Qt::UniqueConnection);

    ui->btnRestartCore->setEnabled(false);
    m_actionStartCore->setVisible(true);
    m_actionRestartCore->setVisible(false);
    m_actionStopCore->setEnabled(false);
  }
  updateModeControlLabels();
  updateLanBridgeStatus();
}

void MainWindow::coreConnectionStateChanged(ConnectionState state)
{
  qDebug() << "core connection state changed:" << static_cast<int>(state);

  updateStatus();

  // always assume connection is not secure when connection changes
  // to anything except connected. the only way the padlock shows is
  // when the correct TLS version string is detected.
  if (state != ConnectionState::Connected) {
    secureSocket(false);
  } else if (isVisible()) {
    showFirstConnectedMessage();
  }
}

void MainWindow::updateFingerprintButton()
{
  m_statusBar->setBtnFingerprintVisible(TlsUtility::isEnabled() && !m_fingerprint.data.isEmpty());
}

void MainWindow::hide()
{
#ifndef Q_OS_MACOS
  if (!QSystemTrayIcon::isSystemTrayAvailable()) {
    showMinimized();
    return;
  }
#endif
  QMainWindow::hide();
  m_actionRestore->setVisible(true);
  m_actionMinimize->setVisible(false);
}

void MainWindow::changeEvent(QEvent *e)
{
  QMainWindow::changeEvent(e);
  if (e->type() == QEvent::ActivationChange && isActiveWindow() && m_startupManager) {
    m_startupManager->refresh();
    updateLanBridgeStatus();
  } else if (e->type() == QEvent::PaletteChange) {
    updateIconTheme();
    setWindowIcon(
        QIcon::fromTheme(kRevFqdnName, QIcon(QStringLiteral(":/icons/deskflow-dark/apps/64/org.deskflow.deskflow.svg")))
    );
    setTrayIcon();
  } else if (e->type() == QEvent::LanguageChange) {
    ui->retranslateUi(this);
    updateModeControlLabels();
    updateNetworkInfo();
    updateStatus();
    serverClientsChanged(m_connectedClients);
    updateText();
  }
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
#ifdef Q_OS_MACOS
  if (obj == qApp && event->type() == QEvent::Quit) {
    m_quitRequested = true;
    // A different window may veto Quit; a later normal close must still hide.
    QTimer::singleShot(0, this, [this] {
      if (!m_quitPrepared)
        m_quitRequested = false;
    });
    return false;
  }
#endif
  if (obj != ui->lineEditName || event->type() != QEvent::KeyPress)
    return false;
  if (const auto keyEvent = static_cast<QKeyEvent *>(event); keyEvent->key() != Qt::Key_Escape)
    return false;
  ui->lineEditName->hide();
  ui->lblComputerName->show();
  ui->btnEditName->show();
  ui->lineEditName->setText(Settings::value(Settings::Core::ComputerName).toString());
  toggleCanRunCore(canRunCore());
  return true;
}

void MainWindow::updateText()
{
  m_menuFile->setTitle(tr("&File"));
  m_menuEdit->setTitle(tr("&Edit"));
  m_menuView->setTitle(tr("&View"));
  m_menuHelp->setTitle(tr("&Help"));

  m_actionMinimize->setText(tr("&Minimize to tray"));
  m_actionQuit->setText(tr("&Quit"));
  m_actionTrayQuit->setText(tr("&Quit"));
  //: %1 will be the replaced with the appname
  m_actionRestore->setText(tr("&Open %1").arg(kAppName));
  m_actionSettings->setText(tr("&Preferences"));
  m_actionStartCore->setText(tr("&Start"));
  m_actionRestartCore->setText(tr("Rest&art"));
  m_actionStopCore->setText(tr("S&top"));
  //: %1 will be the replaced with the appname
  m_actionAbout->setText(tr("About %1...").arg(kAppName));

  m_actionShowHelp->setText(tr("View &Help"));

  //: start / restart core shortcut
  m_actionStartCore->setShortcut(QKeySequence(tr("Ctrl+S")));
  m_actionRestartCore->setShortcut(QKeySequence(tr("Ctrl+S")));

  //: stop core shortcut
  m_actionStopCore->setShortcut(QKeySequence(tr("Ctrl+T")));

  if (deskflow::platform::isWindows()) {
    //: Quit shortcut
    m_actionQuit->setShortcut(QKeySequence(tr("Ctrl+Q")));
    m_actionTrayQuit->setShortcut(QKeySequence(tr("Ctrl+Q")));
  }
}

void MainWindow::showConfigureServer(const QString &message)
{
  ServerConfigDialog dialog(this, serverConfig());
  dialog.message(message);
  if ((dialog.exec() == QDialog::Accepted) && m_coreProcess.isStarted()) {
    m_coreProcess.restart();
  }
}

void MainWindow::showConfigureClient()
{
  ClientConfigDialog dialog(this);
  if ((dialog.exec() == QDialog::Accepted) && m_coreProcess.isStarted()) {
    m_coreProcess.restart();
  }
}

void MainWindow::secureSocket(bool secureSocket)
{
  m_secureSocket = secureSocket;
  updateSecurityIcon(m_statusBar->securityIconVisible());
}

void MainWindow::updateComputerName()
{
  const auto computerName = Settings::value(Settings::Core::ComputerName).toString();
  ui->lblComputerName->setText(computerName);
  ui->lineEditName->setText(computerName);
  m_serverConfig.updateServerName();
}

void MainWindow::showAndActivate()
{
  const auto wasVisible = isVisible();
#ifdef Q_OS_MACOS
  forceAppActive();
#endif
  showNormal();
  raise();
  activateWindow();
  m_actionRestore->setVisible(false);
  m_actionMinimize->setVisible(true);
  if (!wasVisible)
    restoreWindow();
}

void MainWindow::showHostNameEditor()
{
  ui->lineEditName->show();
  ui->lblComputerName->hide();
  ui->btnEditName->hide();
  toggleCanRunCore(false);
  ui->lineEditName->setFocus();
}

void MainWindow::setHostName()
{
  ui->lineEditName->hide();
  ui->lblComputerName->show();
  ui->btnEditName->show();
  toggleCanRunCore(canRunCore());

  QString text = ui->lineEditName->text();
  const auto computerName = Settings::value(Settings::Core::ComputerName).toString();

  if (text == computerName)
    return;

  const bool isServer = ui->rbModeServer->isChecked();
  bool existingComputer = false;
  if (isServer)
    existingComputer = serverConfig().computerExists(text);

  if (!ui->lineEditName->hasAcceptableInput() || text.isEmpty() || existingComputer) {
    blockSignals(true);
    ui->lineEditName->setText(computerName);
    blockSignals(false);

    const auto title = tr("Invalid Computer Name");
    QString body;
    if (existingComputer) {
      body = tr("Computer name already exists");
    } else {
      body =
          tr("The name you have chosen is invalid.\n\n"
             "Valid names:\n"
             "• Use letters and numbers\n"
             "• May also use _ or -\n"
             "• Are between 1 and 255 characters");
    }
    QMessageBox::information(this, title, body);
    return;
  }

  ui->lblComputerName->setText(ui->lineEditName->text());
  Settings::setValue(Settings::Core::ComputerName, ui->lineEditName->text());
  if (isServer)
    serverConfig().updateServerName();
  applyConfig();
}

QString MainWindow::trustedFingerprintDatabase() const
{
  const bool isClient = m_coreProcess.mode() == CoreMode::Client;
  return isClient ? Settings::tlsTrustedServersDb() : Settings::tlsTrustedClientsDb();
}

bool MainWindow::generateCertificate()
{
  const auto certificate = Settings::value(Settings::Security::Certificate).toString();
  if (!QFile::exists(certificate) && !TlsUtility::generateCertificate()) {
    return false;
  }

  m_fingerprint = {QCryptographicHash::Sha256, TlsUtility::certFingerprint()};
  updateFingerprintButton();
  return true;
}

void MainWindow::serverClientsChanged(const QStringList &clients)
{
  if (m_coreProcess.mode() != CoreMode::Server || !m_coreProcess.isStarted())
    return;
  m_connectedClients = clients;
  m_statusBar->setServerClients(clients);
  updateLanBridgeStatus();
}

void MainWindow::daemonIpcClientConnectionFailed()
{
  if (deskflow::gui::messages::showDaemonOffline(this)) {
    m_coreProcess.retryDaemon();
  }
}

void MainWindow::toggleCanRunCore(bool enableButtons)
{
  const bool isStarted = m_coreProcess.isStarted();
  ui->btnToggleCore->setEnabled(enableButtons);
  ui->btnRestartCore->setEnabled(enableButtons && isStarted);
  m_actionStartCore->setEnabled(enableButtons);
  m_actionStopCore->setEnabled(enableButtons && isStarted);
}

void MainWindow::remoteHostChanged(const QString &newRemoteHost)
{
  m_coreProcess.setAddress(newRemoteHost);
  toggleCanRunCore(!newRemoteHost.isEmpty() && ui->rbModeClient->isChecked());
  if (newRemoteHost.isEmpty()) {
    Settings::setValue(Settings::Client::RemoteHost);
  } else {
    Settings::setValue(Settings::Client::RemoteHost, newRemoteHost);
  }
}

void MainWindow::updateIpLabel(const QStringList &addresses)
{
  const auto mode = m_coreProcess.mode();
  if (mode == CoreMode::None) {
    return;
  }

  static const auto colorText = QStringLiteral(R"(<span style="color:%1;">%2</span>)");
  const bool serverStarted = m_coreProcess.isStarted();
  const bool fixedIP = !Settings::value(Settings::Core::Interface).toString().isEmpty();

  if (!fixedIP && addresses.isEmpty() && !serverStarted || (serverStarted && m_serverStartSuggestedIP.isEmpty())) {
    ui->lblIpAddresses->setText(colorText.arg(palette().linkVisited().color().name(), tr("No IP Detected")));
    ui->lblIpAddresses->setToolTip(tr("Unable to detect an IP address. Check your network connection is active."));
    return;
  }

  QString labelText = fixedIP ? tr("Using IP: ") : tr("Suggested IP: ");
  QString toolTipText = tr("<p>If connecting via the hostname fails, try %1</p>");

  const bool filterIpList = (serverStarted || fixedIP);
  const QRegularExpression ipListFilter(filterIpList ? QStringLiteral("(%1)").arg(m_serverStartIPs.join("|")) : "");
  const QStringList ipList = addresses.filter(ipListFilter);

  bool IPValid = true;
  if (filterIpList && (m_serverStartSuggestedIP != m_currentIpAddress) || !ipList.contains(m_serverStartSuggestedIP)) {
    IPValid = !ipList.isEmpty();
  }

  if (IPValid) {
    m_currentIpAddress = ipList.first();
    labelText.append(m_currentIpAddress);
  } else {
    labelText.append(colorText.arg(palette().linkVisited().color().name(), m_serverStartSuggestedIP));
    toolTipText.append(tr("\nA bound IP is now invalid, you may need to restart the server."));
  }

  if (ipList.count() < 2 || fixedIP) {
    toolTipText = toolTipText.arg(tr("the suggested IP."));
  } else {
    toolTipText = toolTipText.arg(tr("one of the following IPs:<br/>%1").arg(ipList.join("<br/>")));
  }

  ui->lblIpAddresses->setText(labelText);
  ui->lblIpAddresses->setToolTip(mode == CoreMode::Server ? toolTipText : QString());
}

void MainWindow::updateTimeoutDelay(int newDelay)
{
  m_statusBar->setConnectionInterval(newDelay);
}

void MainWindow::setHelpFilePath()
{
  const QString appPath = QCoreApplication::applicationDirPath();
  auto buildPath = QString("%1/../docs/HelpMain.md").arg(appPath);
  auto installPath = QString("%1/../share/doc/%2/HelpMain.md").arg(appPath, kAppId);
  if (deskflow::platform::isMac()) {
    installPath = QString("%1/../Resources/docs/HelpMain.md").arg(appPath);
    buildPath = QString("%1/../../../../docs/HelpMain.md").arg(appPath);
  } else if (deskflow::platform::isWindows()) {
    installPath = QString("%1/docs/HelpMain.md").arg(appPath);
  }

  buildPath = QDir::cleanPath(buildPath);
  installPath = QDir::cleanPath(installPath);

  if (QFile::exists(installPath))
    m_helpPath = QUrl::fromLocalFile(installPath);
  else if (QFile::exists(buildPath))
    m_helpPath = QUrl::fromLocalFile(buildPath);
  else
    m_helpPath = kUrlWiki;
}

void MainWindow::showHelpViewer() const
{
  if (m_helpPath.isLocalFile()) {
    HelpDialog dialogHelp(this->centralWidget(), m_helpPath);
    dialogHelp.exec();
  } else {
    QDesktopServices::openUrl(m_helpPath);
  }
}

bool MainWindow::canRunCore() const
{
  const auto mode = m_coreProcess.mode();
  const bool isServer = mode == Settings::CoreMode::Server;
  const bool isClient = mode == Settings::CoreMode::Client;
  return !inputRecoveryRequired() && (isServer || (isClient && !ui->lineHostname->text().isEmpty()));
}
