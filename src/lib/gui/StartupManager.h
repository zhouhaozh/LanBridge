/*
 * SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantMap>

class StartupManagerTests;
class StartupManagerMacTests;

namespace deskflow::gui {

class StartupManager : public QObject
{
  Q_OBJECT
public:
  enum class State
  {
    Disabled,
    Enabled,
    NeedsApproval,
    Busy,
    Unavailable,
    Error
  };
  Q_ENUM(State)

  explicit StartupManager(QObject *parent = nullptr);
  ~StartupManager() override;
  State state() const
  {
    return m_state;
  }
  QString detail() const
  {
    return m_detail;
  }
  bool fallbackAvailable() const
  {
    return m_fallbackAvailable;
  }

public Q_SLOTS:
  void refresh();
  void setEnabled(bool enabled);

Q_SIGNALS:
  void stateChanged(deskflow::gui::StartupManager::State state, const QString &detail);

private Q_SLOTS:
  void portalResponse(uint response, const QVariantMap &results);

private:
  friend class ::StartupManagerTests;
  friend class ::StartupManagerMacTests;
  static State portalResult(uint response, const QVariantMap &results);
  static bool missingPortal(const QString &errorName);
  void update(State state, const QString &detail);
  void clearPortalRequest(bool close);
  void probePortal();
  void requestPortal(bool enabled);
  void refreshMac();
  void updateMacStatus(int status);
  void setMacEnabled(bool enabled);

  State m_state = State::Disabled;
  QString m_detail;
  bool m_fallbackAvailable = false;
  QString m_requestPath;
  QTimer m_requestTimeout;
};

} // namespace deskflow::gui
