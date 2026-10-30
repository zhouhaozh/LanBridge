/*
 * SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "StartupManager.h"

#include <QSettings>
#include <QUuid>

#if defined(Q_OS_LINUX)
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#endif

namespace deskflow::gui {
namespace {
constexpr auto kPortalService = "org.freedesktop.portal.Desktop";
constexpr auto kPortalPath = "/org/freedesktop/portal/desktop";
constexpr auto kBackground = "org.freedesktop.portal.Background";
constexpr auto kRequest = "org.freedesktop.portal.Request";

QSettings startupSettings()
{
  return QSettings(QSettings::IniFormat, QSettings::UserScope, "LanBridge", "startup");
}
} // namespace

StartupManager::StartupManager(QObject *parent) : QObject(parent)
{
  m_requestTimeout.setSingleShot(true);
  m_requestTimeout.setInterval(120000);
  connect(&m_requestTimeout, &QTimer::timeout, this, [this] {
    clearPortalRequest(true);
    update(State::Error, tr("系统授权未完成，请重试；尚未确认登录自启。"));
  });
}

StartupManager::~StartupManager()
{
  clearPortalRequest(true);
}

void StartupManager::update(State state, const QString &detail)
{
  m_state = state;
  m_detail = detail;
  Q_EMIT stateChanged(state, detail);
}

void StartupManager::refresh()
{
  if (m_state == State::Busy)
    return;
  m_fallbackAvailable = false;
#if defined(Q_OS_MACOS)
  refreshMac();
#elif defined(Q_OS_LINUX)
  probePortal();
#else
  update(State::Unavailable, tr("此系统暂不支持登录自启。"));
#endif
}

void StartupManager::setEnabled(bool enabled)
{
  if (m_state == State::Busy)
    return;
  m_fallbackAvailable = false;
  update(State::Busy, tr("正在等待系统确认…"));
#if defined(Q_OS_MACOS)
  setMacEnabled(enabled);
#elif defined(Q_OS_LINUX)
  requestPortal(enabled);
#else
  Q_UNUSED(enabled);
  update(State::Unavailable, tr("此系统暂不支持登录自启。"));
#endif
}

StartupManager::State StartupManager::portalResult(uint response, const QVariantMap &results)
{
  if (response == 1)
    return State::Disabled;
  if (response != 0 || results.value("autostart").metaType().id() != QMetaType::Bool)
    return State::Error;
  // Background permission alone is never evidence of an autostart grant.
  return results.value("autostart").toBool() ? State::Enabled : State::Disabled;
}

bool StartupManager::missingPortal(const QString &errorName)
{
  return errorName == "org.freedesktop.DBus.Error.UnknownInterface" ||
         errorName == "org.freedesktop.DBus.Error.UnknownMethod" ||
         errorName == "org.freedesktop.DBus.Error.UnknownProperty" ||
         errorName == "org.freedesktop.DBus.Error.ServiceUnknown" ||
         errorName == "org.freedesktop.DBus.Error.NameHasNoOwner";
}

void StartupManager::probePortal()
{
#if defined(Q_OS_LINUX)
  update(State::Busy, tr("正在检查系统登录自启接口…"));
  auto message = QDBusMessage::createMethodCall(kPortalService, kPortalPath, "org.freedesktop.DBus.Properties", "Get");
  message << kBackground << "version";
  auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
  connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
    const QDBusPendingReply<QDBusVariant> reply = *watcher;
    watcher->deleteLater();
    if (reply.isError()) {
      m_fallbackAvailable = missingPortal(reply.error().name());
      update(
          m_fallbackAvailable ? State::Unavailable : State::Error,
          m_fallbackAvailable ? tr("系统没有 Background 接口，可在 Linux 本机使用安装包附带的自启脚本。")
                              : tr("无法查询系统登录自启接口：%1").arg(reply.error().message())
      );
      return;
    }
    // The portal has no read-status API. Never issue a new authorization prompt just to refresh UI.
    const bool lastGrant = startupSettings().value("portal/lastConfirmedAutostart", false).toBool();
    update(
        lastGrant ? State::Enabled : State::Disabled,
        lastGrant ? tr("上次系统确认已开启；若在系统设置中修改，请重新授权并注销登录验证。") : tr("登录自启未开启。")
    );
  });
#endif
}

void StartupManager::requestPortal(bool enabled)
{
#if defined(Q_OS_LINUX)
  auto bus = QDBusConnection::sessionBus();
  auto sender = bus.baseService();
  sender.remove(':');
  sender.replace('.', '_');
  const auto token = "lanbridge_" + QUuid::createUuid().toString(QUuid::Id128);
  m_requestPath = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
  if (!bus.connect(
          kPortalService, m_requestPath, kRequest, "Response", this, SLOT(portalResponse(uint, QVariantMap))
      )) {
    clearPortalRequest(false);
    update(State::Error, tr("无法接收系统授权结果。"));
    return;
  }
  QVariantMap options{
      {"handle_token", token},
      {"reason", tr("登录后启动 LanBridge，在局域网中共享键盘和鼠标。")},
      {"autostart", enabled},
      {"commandline", QStringList{"deskflow"}},
      {"dbus-activatable", false}
  };
  auto message = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kBackground, "RequestBackground");
  message << QString() << options;
  m_requestTimeout.start();
  const auto expectedPath = m_requestPath;
  auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message), this);
  connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, expectedPath] {
    const QDBusPendingReply<QDBusObjectPath> reply = *watcher;
    watcher->deleteLater();
    if (m_requestPath != expectedPath)
      return; // A quick Response or a timeout can precede this reply; do not disturb a later request.
    if (reply.isError()) {
      clearPortalRequest(false);
      m_fallbackAvailable = missingPortal(reply.error().name());
      update(
          m_fallbackAvailable ? State::Unavailable : State::Error,
          m_fallbackAvailable ? tr("系统没有 Background 接口，可在 Linux 本机运行附带的自启脚本。")
                              : tr("系统未确认登录自启：%1").arg(reply.error().message())
      );
      return;
    }
    if (reply.value().path() != m_requestPath) {
      auto bus = QDBusConnection::sessionBus();
      bus.disconnect(
          kPortalService, m_requestPath, kRequest, "Response", this, SLOT(portalResponse(uint, QVariantMap))
      );
      m_requestPath = reply.value().path();
      if (!bus.connect(
              kPortalService, m_requestPath, kRequest, "Response", this, SLOT(portalResponse(uint, QVariantMap))
          )) {
        clearPortalRequest(true);
        update(State::Error, tr("无法接收系统授权结果。"));
      }
    }
  });
#else
  Q_UNUSED(enabled);
#endif
}

void StartupManager::portalResponse(uint response, const QVariantMap &results)
{
  if (m_requestPath.isEmpty())
    return;
  clearPortalRequest(false);
  const auto result = portalResult(response, results);
  auto settings = startupSettings();
  settings.setValue("portal/lastConfirmedAutostart", result == State::Enabled);
  settings.sync();
  update(
      result, result == State::Enabled    ? tr("系统已确认开启登录自启。")
              : result == State::Disabled ? tr("登录自启已关闭，或系统未授予自启权限。")
                                          : tr("系统未返回有效的自启授权结果，请重试。")
  );
}

void StartupManager::clearPortalRequest(bool close)
{
  m_requestTimeout.stop();
#if defined(Q_OS_LINUX)
  if (!m_requestPath.isEmpty()) {
    auto bus = QDBusConnection::sessionBus();
    bus.disconnect(kPortalService, m_requestPath, kRequest, "Response", this, SLOT(portalResponse(uint, QVariantMap)));
    if (close)
      bus.asyncCall(QDBusMessage::createMethodCall(kPortalService, m_requestPath, kRequest, "Close"));
  }
#else
  Q_UNUSED(close);
#endif
  m_requestPath.clear();
}

} // namespace deskflow::gui
