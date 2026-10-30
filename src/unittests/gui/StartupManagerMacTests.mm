/*
 * SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "gui/StartupManager.h"

#include <QSignalSpy>
#include <QTest>

#import <ServiceManagement/ServiceManagement.h>

using deskflow::gui::StartupManager;

// Synthetic status values only: these tests never query, register or unregister a system login item.
class StartupManagerMacTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void missingServiceKeepsRegistrationAvailable()
  {
    StartupManager manager;
    QSignalSpy changes(&manager, &StartupManager::stateChanged);
    manager.updateMacStatus(SMAppServiceStatusNotFound);
    QCOMPARE(manager.state(), StartupManager::State::Error);
    QVERIFY(manager.state() != StartupManager::State::Unavailable);
    QVERIFY(manager.detail().contains(QStringLiteral("尝试系统注册")));
    QVERIFY(!manager.detail().contains(QStringLiteral("安装到应用程序")));
    QVERIFY(!manager.fallbackAvailable());
    QCOMPARE(changes.size(), 1);
  }

  void permissionAndRegistrationStatesRemainDistinct()
  {
    StartupManager manager;
    manager.updateMacStatus(SMAppServiceStatusRequiresApproval);
    QCOMPARE(manager.state(), StartupManager::State::NeedsApproval);
    manager.updateMacStatus(SMAppServiceStatusNotRegistered);
    QCOMPARE(manager.state(), StartupManager::State::Disabled);
    manager.updateMacStatus(SMAppServiceStatusEnabled);
    QCOMPARE(manager.state(), StartupManager::State::Enabled);
    manager.updateMacStatus(SMAppServiceStatusNotFound);
    QCOMPARE(manager.state(), StartupManager::State::Error);
  }

  void unknownStatusDoesNotClaimUnsupportedMacOS()
  {
    StartupManager manager;
    manager.updateMacStatus(98765);
    QCOMPARE(manager.state(), StartupManager::State::Error);
    QVERIFY(manager.detail().contains(QStringLiteral("98765")));
    QVERIFY(!manager.detail().contains(QStringLiteral("macOS 13")));
  }
};

QTEST_GUILESS_MAIN(StartupManagerMacTests)
#include "StartupManagerMacTests.moc"
