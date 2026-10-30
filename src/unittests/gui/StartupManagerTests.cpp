/*
 * SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "gui/StartupManager.h"
#include <QTest>

using deskflow::gui::StartupManager;

class StartupManagerTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void backgroundPermissionDoesNotEnableAutostart()
  {
    QCOMPARE(
        StartupManager::portalResult(0, {{"background", true}, {"autostart", false}}), StartupManager::State::Disabled
    );
    QCOMPARE(StartupManager::portalResult(0, {{"background", true}}), StartupManager::State::Error);
    QCOMPARE(StartupManager::portalResult(0, {{"autostart", "true"}}), StartupManager::State::Error);
    QCOMPARE(
        StartupManager::portalResult(0, {{"background", false}, {"autostart", true}}), StartupManager::State::Enabled
    );
  }
  void cancelledAndFailedRequestsNeverEnableAutostart()
  {
    QCOMPARE(StartupManager::portalResult(1, {{"autostart", true}}), StartupManager::State::Disabled);
    QCOMPARE(StartupManager::portalResult(2, {{"autostart", true}}), StartupManager::State::Error);
  }
  void deniedPermissionNeverOffersHostFallback()
  {
    QVERIFY(!StartupManager::missingPortal("org.freedesktop.DBus.Error.AccessDenied"));
    QVERIFY(!StartupManager::missingPortal("org.freedesktop.portal.Error.NotAllowed"));
    QVERIFY(!StartupManager::missingPortal("org.freedesktop.DBus.Error.NoReply"));
    QVERIFY(StartupManager::missingPortal("org.freedesktop.DBus.Error.UnknownInterface"));
    QVERIFY(StartupManager::missingPortal("org.freedesktop.DBus.Error.ServiceUnknown"));
  }
};

QTEST_GUILESS_MAIN(StartupManagerTests)
#include "StartupManagerTests.moc"
