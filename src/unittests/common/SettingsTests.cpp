/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "SettingsTests.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

void SettingsTests::initTestCase()
{
  QFile oldSettings(m_settingsFile);
  if (oldSettings.exists())
    oldSettings.remove();
}

void SettingsTests::setSettingsFile()
{
  Settings::setSettingsFile(m_settingsFile);
}

void SettingsTests::setStateFile()
{
  Settings::setStateFile(m_stateFile);
}

void SettingsTests::settingsFile()
{
  QVERIFY(Settings::settingsFile().endsWith(m_settingsFile));
}

void SettingsTests::settingsPath()
{
  QVERIFY(Settings::settingsPath().endsWith(m_settingsPath));
}

void SettingsTests::tlsDir()
{
  QVERIFY(Settings::tlsDir().endsWith(m_expectedTlsDir));
}

void SettingsTests::tlsTrustedServersDb()
{
  QVERIFY(Settings::tlsTrustedServersDb().endsWith(m_expectedTlsServerDB));
}

void SettingsTests::tlsTrustedClientsDb()
{
  QVERIFY(Settings::tlsTrustedClientsDb().endsWith(m_expectedTlsClientDB));
}

void SettingsTests::checkValidSettings()
{
  QSignalSpy spy(Settings::instance(), &Settings::settingsChanged);
  QVERIFY(spy.isValid());

  const auto keysToCheck = QRegularExpression(QLatin1String("[^%1]").arg(Settings::Core::ComputerName));
  const auto validKeys = Settings::validKeys().filter(keysToCheck);
  for (const auto &setting : validKeys) {
    if (setting == Settings::Security::TlsEnabled || setting == Settings::Security::CheckPeers ||
        setting == Settings::Client::DynamicConnectionRetry)
      continue; // These safety constraints are deliberately immutable.
    const auto value = Settings::value(setting).toString();
    QCOMPARE(Settings::defaultValue(setting).toString(), value);

    Settings::setValue(setting, "NEW_VALUE");
    QCOMPARE(spy.count(), 1);
    QCOMPARE(qvariant_cast<QString>(spy.first().at(0)), setting);
    QCOMPARE(Settings::value(setting).toString(), "NEW_VALUE");

    Settings::setValue(setting);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(Settings::value(setting).toString(), value);

    // Reset the spy for the next loop
    spy.clear();
    QCOMPARE(spy.count(), 0);
  }
}

void SettingsTests::checkCleanComputerName()
{
  const auto input = QStringLiteral("--!_ _-C@o#mpu$t%e^&*(r)= +Name\n[1]2|3?4--5>6<,7`~/8*90\\.lan--..    ..");
  const auto expected = QStringLiteral("Computer_Name_1234--567890.lan");

  Settings::setValue(Settings::Core::ComputerName, input);

  QCOMPARE(Settings::value(Settings::Core::ComputerName).toString(), expected);
}

void SettingsTests::checkCleanComputerName_LongName()
{
  QString input;
  input.fill('f', 300);
  input.prepend('.');

  QString expected;
  expected.fill('f', 255);

  Settings::setValue(Settings::Core::ComputerName, input);

  QCOMPARE(Settings::value(Settings::Core::ComputerName).toString(), expected);
}

void SettingsTests::checkClearState()
{
  Settings::setValue(Settings::Gui::WindowGeometry, QRect(1, 2, 3, 4));
  QVERIFY(Settings::value(Settings::Gui::WindowGeometry).toRect().isValid());

  Settings::clearState();

  QVERIFY(!Settings::value(Settings::Gui::WindowGeometry).toRect().isValid());
}

void SettingsTests::lanBridgeDefaults()
{
  Settings::setValue(Settings::Server::EnableClipboard);
  QCOMPARE(Settings::defaultValue(Settings::Server::EnableClipboard).toBool(), true);
  QCOMPARE(Settings::value(Settings::Server::EnableClipboard).toBool(), true);
  QCOMPARE(Settings::defaultValue(Settings::Server::EnableHeartbeat).toBool(), true);
  QCOMPARE(Settings::defaultValue(Settings::Server::Heartbeat).toInt(), 1000);
  QCOMPARE(Settings::defaultValue(Settings::Security::TlsEnabled).toBool(), true);
  QCOMPARE(Settings::defaultValue(Settings::Security::CheckPeers).toBool(), true);
  QCOMPARE(Settings::defaultValue(Settings::Client::LanguageSync).toBool(), false);
  QCOMPARE(Settings::defaultValue(Settings::Client::RemoteHost).toString(), QString("192.168.50.219"));
  QCOMPARE(Settings::defaultValue(Settings::LanBridge::DiagnosticsEnabled).toBool(), false);
  QCOMPARE(Settings::defaultValue(Settings::LanBridge::MacPosition).toString(), QString("right"));
#ifdef Q_OS_MACOS
  QCOMPARE(Settings::defaultValue(Settings::Core::ComputerName).toString(), QString("mac"));
  QCOMPARE(Settings::defaultValue(Settings::Core::CoreMode).toInt(), int(Settings::Client));
#else
  QCOMPARE(Settings::defaultValue(Settings::Core::ComputerName).toString(), QString("linux"));
  QCOMPARE(Settings::defaultValue(Settings::Core::CoreMode).toInt(), int(Settings::Server));
#endif
}

void SettingsTests::clipboardCanBeEnabledAndDisabled()
{
  const auto key = Settings::Server::EnableClipboard;
  for (const bool enabled : {true, false, true}) {
    Settings::setValue(key, enabled);
    QCOMPARE(Settings::value(key).toBool(), enabled);
    QSettings stored(Settings::settingsFile(), QSettings::IniFormat);
    QCOMPARE(stored.value(key).toBool(), enabled);
  }
}

void SettingsTests::clipboardImportPreservesPreference_data()
{
  QTest::addColumn<bool>("enabled");
  QTest::newRow("enabled") << true;
  QTest::newRow("disabled") << false;
}

void SettingsTests::clipboardImportPreservesPreference()
{
  QFETCH(bool, enabled);
  QTemporaryDir importDirectory;
  QVERIFY(importDirectory.isValid());
  const auto key = Settings::Server::EnableClipboard;
  const auto originalSettings = Settings::settingsFile();
  QSettings imported(importDirectory.filePath("imported.conf"), QSettings::IniFormat);
  imported.setValue(key, enabled);
  imported.sync();

  Settings::setSettingsFile(imported.fileName());
  const bool actual = Settings::value(key).toBool();
  Settings::save(false);
  imported.sync();
  const bool persisted = imported.value(key).toBool();
  Settings::setSettingsFile(originalSettings);

  QCOMPARE(actual, enabled);
  QCOMPARE(persisted, enabled);
}

void SettingsTests::restoreLanBridgeDefaults()
{
  Settings::setValue(Settings::Server::EnableClipboard, false);
  QCOMPARE(Settings::value(Settings::Server::EnableClipboard).toBool(), false);
  Settings::setValue(Settings::Server::EnableHeartbeat, false);
  Settings::setValue(Settings::Server::Heartbeat, 5000);
  Settings::setValue(Settings::LanBridge::DiagnosticsEnabled, true);
  Settings::restoreDefaultSettings();
  QCOMPARE(Settings::value(Settings::Server::EnableClipboard).toBool(), true);
  QCOMPARE(Settings::value(Settings::Server::EnableHeartbeat).toBool(), true);
  QCOMPARE(Settings::value(Settings::Server::Heartbeat).toInt(), 1000);
  QCOMPARE(Settings::value(Settings::LanBridge::DiagnosticsEnabled).toBool(), false);
}

void SettingsTests::tlsCannotBeDisabled()
{
  for (const auto &key : {Settings::Security::TlsEnabled, Settings::Security::CheckPeers}) {
    Settings::setValue(key, false);
    QCOMPARE(Settings::value(key).toBool(), true);
    QSettings imported(Settings::settingsFile(), QSettings::IniFormat);
    imported.setValue(key, false);
    imported.sync();
    QCOMPARE(Settings::value(key).toBool(), true);
    Settings::restoreDefaultSettings();
    QCOMPARE(Settings::value(key).toBool(), true);
  }
}

void SettingsTests::dynamicRetryCannotBeEnabled()
{
  const auto key = Settings::Client::DynamicConnectionRetry;
  QCOMPARE(Settings::defaultValue(key).toBool(), false);
  Settings::setValue(key, true);
  QCOMPARE(Settings::value(key).toBool(), false);
  QSettings imported(Settings::settingsFile(), QSettings::IniFormat);
  imported.setValue(key, true);
  imported.sync();
  QCOMPARE(Settings::value(key).toBool(), false);
  Settings::restoreDefaultSettings();
  QCOMPARE(Settings::value(key).toBool(), false);
}

QTEST_MAIN(SettingsTests)
