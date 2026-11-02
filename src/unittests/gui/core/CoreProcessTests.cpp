/*
 * SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "CoreProcessTests.h"

#include "common/RemoteInputSession.h"
#include "gui/core/CoreProcess.h"
#include "gui/ipc/DaemonIpcClient.h"

#include <QElapsedTimer>
#include <QFile>
#include <QMutexLocker>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

using deskflow::core::ProcessState;
using deskflow::gui::CoreProcess;

void CoreProcessTests::initTestCase()
{
  QVERIFY(m_directory.isValid());
  qputenv("XDG_CONFIG_HOME", m_directory.filePath("config").toUtf8());
  qputenv("XDG_STATE_HOME", m_directory.filePath("state").toUtf8());
}

void CoreProcessTests::init()
{
  Settings::setSettingsFile(m_directory.filePath("profile.conf"));
  Settings::setStateFile(m_directory.filePath("profile.state"));
  Settings::setValue(Settings::Core::ProcessMode, QVariant::fromValue(Settings::ProcessMode::Desktop));
  QFile::remove(deskflow::RemoteInputSession::markerPath());
}

bool CoreProcessTests::startOwnedProcess(CoreProcess &core, bool ignoreTerminate, int exitCode)
{
  const auto python = QStandardPaths::findExecutable(QStringLiteral("python3"));
  if (python.isEmpty())
    return false;
  auto *process = new QProcess(&core);
  core.m_process = process;
  core.m_lastProcessMode = Settings::ProcessMode::Desktop;
  connect(process, &QProcess::finished, &core, &CoreProcess::onProcessFinished);
  const auto handler = ignoreTerminate ? QStringLiteral("signal.SIG_IGN")
                                       : QStringLiteral("lambda *args: (time.sleep(0.08), sys.exit(%1))").arg(exitCode);
  const auto program = QStringLiteral(
                           "import signal, sys, time\nsignal.signal(signal.SIGTERM, %1)\n"
                           "print('ready', flush=True)\ntime.sleep(60)\n"
  )
                           .arg(handler);
  process->start(python, {QStringLiteral("-u"), QStringLiteral("-c"), program});
  const bool ready = process->waitForStarted(1000) && process->waitForReadyRead(1000) &&
                     process->readAllStandardOutput().contains("ready");
  if (!ready) {
    process->kill();
    process->waitForFinished(1000);
    return false;
  }
  core.m_processState = ProcessState::Started;
  return true;
}

void CoreProcessTests::stopRetryWithoutProcessIsSynchronous()
{
  ServerConfig config;
  CoreProcess core(config);
  core.m_processState = ProcessState::RetryPending;
  core.m_retryTimer.start(10000);
  core.stop(Settings::ProcessMode::Desktop);
  QCOMPARE(core.processState(), ProcessState::Stopped);
  QVERIFY(!core.m_retryTimer.isActive());
}

void CoreProcessTests::shutdownWithoutRunningProcess_data()
{
  QTest::addColumn<ProcessState>("state");
  QTest::addColumn<bool>("hasProcess");
  for (const auto state :
       {ProcessState::Stopped, ProcessState::Starting, ProcessState::Started, ProcessState::Stopping,
        ProcessState::RetryPending}) {
    for (const bool hasProcess : {false, true}) {
      const auto name = QStringLiteral("%1-%2").arg(static_cast<int>(state)).arg(hasProcess).toUtf8();
      QTest::newRow(name.constData()) << state << hasProcess;
    }
  }
}

void CoreProcessTests::shutdownWithoutRunningProcess()
{
  QFETCH(ProcessState, state);
  QFETCH(bool, hasProcess);
  ServerConfig config;
  CoreProcess core(config);
  core.m_processState = state;
  core.m_connectionState = deskflow::core::ConnectionState::Connecting;
  core.m_retryTimer.start(10000);
  if (hasProcess)
    core.m_process = new QProcess(&core);
  QVERIFY(core.shutdown(50));
  QVERIFY(core.shutdown(50));
  QCOMPARE(core.processState(), ProcessState::Stopped);
  QCOMPARE(core.connectionState(), deskflow::core::ConnectionState::Disconnected);
  QVERIFY(!core.m_retryTimer.isActive());
}

void CoreProcessTests::shutdownOwnedProcess_data()
{
  QTest::addColumn<ProcessState>("state");
  for (const auto state :
       {ProcessState::Starting, ProcessState::Started, ProcessState::Stopping, ProcessState::RetryPending}) {
    QTest::newRow(qPrintable(QString::number(static_cast<int>(state)))) << state;
  }
}

void CoreProcessTests::shutdownOwnedProcess()
{
#ifndef Q_OS_UNIX
  QSKIP("The controlled child uses POSIX SIGTERM");
#else
  QFETCH(ProcessState, state);
  ServerConfig config;
  CoreProcess core(config);
  QVERIFY(startOwnedProcess(core, false));
  core.m_processState = state;
  core.m_retryTimer.start(10000);
  bool timerProcessed = false;
  QTimer::singleShot(20, &core, [&timerProcessed] { timerProcessed = true; });
  QVERIFY(core.shutdown(1000));
  QVERIFY(timerProcessed);
  QCOMPARE(core.m_process->state(), QProcess::NotRunning);
  QCOMPARE(core.processState(), ProcessState::Stopped);
  QVERIFY(!core.m_retryTimer.isActive());
  QVERIFY(core.shutdown(1000));
#endif
}

void CoreProcessTests::shutdownFromAboutToQuit()
{
#ifndef Q_OS_UNIX
  QSKIP("The controlled child uses POSIX SIGTERM");
#else
  ServerConfig config;
  CoreProcess core(config);
  QVERIFY(startOwnedProcess(core, false));
  bool shutdownCompleted = false;
  bool timerProcessed = false;
  const auto connection = connect(qApp, &QCoreApplication::aboutToQuit, &core, [&] {
    QTimer::singleShot(20, &core, [&timerProcessed] { timerProcessed = true; });
    shutdownCompleted = core.shutdown(1000);
  });
  QTimer::singleShot(0, qApp, &QCoreApplication::quit);
  QCOMPARE(QCoreApplication::exec(), 0);
  disconnect(connection);
  QVERIFY(shutdownCompleted);
  QVERIFY(timerProcessed);
  QCOMPARE(core.m_process->state(), QProcess::NotRunning);
  QCOMPARE(core.processState(), ProcessState::Stopped);
#endif
}

void CoreProcessTests::shutdownTimeoutPreservesRecoveryMarker()
{
#ifndef Q_OS_UNIX
  QSKIP("The controlled child uses POSIX SIGTERM");
#else
  ServerConfig config;
  CoreProcess core(config);
  QVERIFY(startOwnedProcess(core, true));
  QFile marker(deskflow::RemoteInputSession::markerPath());
  QVERIFY(marker.open(QIODevice::WriteOnly));
  QCOMPARE(marker.write("owned test marker"), qint64(17));
  marker.close();
  QSignalSpy errors(&core, &CoreProcess::error);
  QElapsedTimer elapsed;
  elapsed.start();
  QVERIFY(!core.shutdown(80));
  QVERIFY(elapsed.elapsed() < 1500);
  QCOMPARE(core.m_process->state(), QProcess::NotRunning);
  QCOMPARE(core.processState(), ProcessState::Stopped);
  QVERIFY(marker.exists());
  QVERIFY(errors.isEmpty());
  QVERIFY(!core.shutdown(80));
#endif
}

void CoreProcessTests::shutdownRejectsQueuedRestart()
{
#ifndef Q_OS_UNIX
  QSKIP("The controlled child uses POSIX SIGTERM");
#else
  ServerConfig config;
  CoreProcess core(config);
  QVERIFY(startOwnedProcess(core, false));
  core.setMode(Settings::Client);
  QFile marker(deskflow::RemoteInputSession::markerPath());
  QVERIFY(marker.open(QIODevice::WriteOnly));
  marker.write("owned test marker");
  marker.close();
  // The recovery gate prevents any OS permission request or actual core start
  // even if the shutdown terminal guard regresses.
  QSignalSpy errors(&core, &CoreProcess::error);
  bool restartAttempted = false;
  connect(core.m_process, &QProcess::finished, &core, [&] {
    QTimer::singleShot(0, &core, [&] {
      restartAttempted = true;
      core.start();
      core.restart();
    });
  });
  QVERIFY(core.shutdown(1000));
  QCoreApplication::processEvents();
  QVERIFY(restartAttempted);
  QVERIFY(errors.isEmpty());
  QVERIFY(!core.m_retryTimer.isActive());
  QCOMPARE(core.m_process->state(), QProcess::NotRunning);
  QCOMPARE(core.processState(), ProcessState::Stopped);
  QVERIFY(marker.exists());
#endif
}

void CoreProcessTests::shutdownReportsAbnormalExit()
{
#ifndef Q_OS_UNIX
  QSKIP("The controlled child uses POSIX SIGTERM");
#else
  ServerConfig config;
  CoreProcess core(config);
  QVERIFY(startOwnedProcess(core, false, 7));
  QVERIFY(!core.shutdown(1000));
  QCOMPARE(core.m_process->state(), QProcess::NotRunning);
  QCOMPARE(core.m_process->exitCode(), 7);
  QCOMPARE(core.processState(), ProcessState::Stopped);
  QVERIFY(!core.shutdown(1000));
#endif
}

void CoreProcessTests::shutdownDuringLockedTransition()
{
#ifndef Q_OS_UNIX
  QSKIP("The controlled child uses POSIX SIGTERM");
#else
  ServerConfig config;
  CoreProcess core(config);
  QVERIFY(startOwnedProcess(core, false));
  core.m_processState = ProcessState::Starting;
  QMutexLocker locker(&core.m_processMutex);
  QVERIFY(core.shutdown(1000));
  QCOMPARE(core.m_process->state(), QProcess::NotRunning);
  QCOMPARE(core.processState(), ProcessState::Stopped);
#endif
}

void CoreProcessTests::shutdownFromStartingSignalStopsStartImmediately()
{
  ServerConfig config;
  CoreProcess core(config);
  core.setMode(Settings::Client);
  core.m_appPath = Settings::settingsPath() + QStringLiteral("/never-run-core");
  bool stopped = false;
  connect(&core, &CoreProcess::processStateChanged, &core, [&](ProcessState state) {
    if (state == ProcessState::Starting)
      stopped = core.shutdown(50);
  });
  core.start(Settings::ProcessMode::Desktop);
  QVERIFY(stopped);
  QVERIFY(core.m_process == nullptr);
  QCOMPARE(core.processState(), ProcessState::Stopped);
  QCOMPARE(core.connectionState(), deskflow::core::ConnectionState::Disconnected);
}

void CoreProcessTests::shutdownServiceDoesNotContactDaemon()
{
  ServerConfig config;
  CoreProcess core(config);
  // A regression must fail inside this fixture, never contact a real daemon.
  delete core.m_daemonIpcClient;
  core.m_daemonIpcClient = nullptr;
  Settings::setValue(Settings::Core::ProcessMode, QVariant::fromValue(Settings::ProcessMode::Service));
  core.m_lastProcessMode = Settings::ProcessMode::Service;
  core.m_processState = ProcessState::Started;
  core.m_retryTimer.start(10000);
  QVERIFY(core.shutdown(50));
  QVERIFY(!core.m_retryTimer.isActive());
}

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
