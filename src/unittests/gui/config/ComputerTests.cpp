/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2024 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ComputerTests.h"

#include "common/Settings.h"
#include "gui/config/Computer.h"
#include "gui/config/ServerConfig.h"

void ComputerTests::initTestCase()
{
  QDir dir;
  QVERIFY(dir.mkpath(m_settingsPath));

  QFile oldSettings(m_settingsFile);
  if (oldSettings.exists())
    oldSettings.remove();

  Settings::setSettingsFile(m_settingsFile);
  Settings::setStateFile(m_stateFile);
}

void ComputerTests::basicFunctionality()
{
  Computer computer;
  QVERIFY(computer.isNull());

  computer.setName("stub");
  QVERIFY(!computer.isNull());

  computer.saveSettings(Settings::proxy());
  computer.loadSettings(Settings::proxy());
  QCOMPARE("stub", computer.name());
}

void ComputerTests::lanBridgeLayout_data()
{
  QTest::addColumn<QString>("position");
  QTest::addColumn<QString>("reverse");
  QTest::newRow("right") << "right" << "left";
  QTest::newRow("left") << "left" << "right";
  QTest::newRow("up") << "up" << "down";
  QTest::newRow("down") << "down" << "up";
}

void ComputerTests::lanBridgeLayout()
{
  QFETCH(QString, position);
  QFETCH(QString, reverse);
  Settings::setValue(Settings::Core::ComputerName, "linux");
  ServerConfig config;
  config.configureLanBridge(position);
  QCOMPARE(config.numComputers(), 2);
  QVERIFY(config.computerExists("linux"));
  QVERIFY(config.computerExists("mac"));
  QString text;
  QTextStream stream(&text);
  stream << config;
  QVERIFY(text.contains(QString("linux:\n\t\t%1 = mac").arg(position)));
  QVERIFY(text.contains(QString("mac:\n\t\t%1 = linux").arg(reverse)));
  QVERIFY(text.contains("keystroke(Control+Alt+Escape) = switchToScreen(linux)"));
  config.commit();
  const ServerConfig recalled;
  QString afterReload;
  QTextStream reloaded(&afterReload);
  reloaded << recalled;
  QCOMPARE(afterReload, text);
}

void ComputerTests::layoutPreservesModifiers()
{
  Settings::setValue(Settings::Core::ComputerName, "linux");
  ServerConfig config;
  config.configureLanBridge("right");
  config.commit();
  Settings::setValue(Settings::Computer::ModifierCtrl.arg("mac"), "Meta");
  ServerConfig custom;
  custom.configureLanBridge("left");
  custom.commit();
  const ServerConfig recalled;
  for (const auto &computer : recalled.computers()) {
    if (computer.name() == "mac") {
      QCOMPARE(computer.modifier(static_cast<int>(KeyboardModifier::Ctrl)), static_cast<int>(KeyboardModifier::Meta));
      return;
    }
  }
  QFAIL("Mac screen missing after layout change");
}

QTEST_MAIN(ComputerTests)
