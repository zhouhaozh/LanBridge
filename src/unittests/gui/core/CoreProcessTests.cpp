/*
 * SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "CoreProcessTests.h"

#include "common/RemoteInputSession.h"
#include "gui/core/CoreProcess.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

using deskflow::core::ProcessState;
using deskflow::gui::CoreProcess;

void CoreProcessTests::staleReceiverBlocksStart()
{
#ifndef Q_OS_MACOS
  QSKIP("The receiver recovery gate is macOS-specific");
#else
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  Settings::setSettingsFile(directory.filePath("receiver.conf"));
  QFile marker(deskflow::RemoteInputSession::markerPath());
  QVERIFY(marker.open(QIODevice::WriteOnly));
  marker.write("unclean");
  marker.close();
  ServerConfig config;
  CoreProcess core(config);
  core.setMode(Settings::Client);
  QSignalSpy errors(&core, &CoreProcess::error);
  core.start();
  QCOMPARE(core.processState(), ProcessState::Stopped);
  QCOMPARE(errors.size(), 1);
  QCOMPARE(errors.first().first().value<CoreProcess::Error>(), CoreProcess::Error::RecoveryRequired);
  QVERIFY(deskflow::RemoteInputSession::recoveryRequired());
#endif
}

void CoreProcessTests::uncleanExitBlocksRetry()
{
#ifndef Q_OS_MACOS
  QSKIP("The receiver recovery gate is macOS-specific");
#else
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  Settings::setSettingsFile(directory.filePath("receiver.conf"));
  QFile marker(deskflow::RemoteInputSession::markerPath());
  QVERIFY(marker.open(QIODevice::WriteOnly));
  marker.write("unclean");
  marker.close();
  ServerConfig config;
  CoreProcess core(config);
  core.setMode(Settings::Client);
  QSignalSpy errors(&core, &CoreProcess::error);
  QVERIFY(
      QMetaObject::invokeMethod(
          &core, "onProcessFinished", Qt::DirectConnection, Q_ARG(int, 0),
          Q_ARG(QProcess::ExitStatus, QProcess::NormalExit)
      )
  );
  QCOMPARE(core.processState(), ProcessState::Stopped);
  QCOMPARE(errors.size(), 1);
  QCOMPARE(errors.first().first().value<CoreProcess::Error>(), CoreProcess::Error::RecoveryRequired);
  QVERIFY(deskflow::RemoteInputSession::recoveryRequired());
#endif
}

QTEST_MAIN(CoreProcessTests)
