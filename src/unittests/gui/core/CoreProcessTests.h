/*
 * SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <QTemporaryDir>
#include <QTest>

class QProcess;
namespace deskflow::gui {
class CoreProcess;
}

class CoreProcessTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void init();
  void staleReceiverBlocksStart();
  void uncleanExitBlocksRetry();
  void stopRetryWithoutProcessIsSynchronous();
  void shutdownWithoutRunningProcess_data();
  void shutdownWithoutRunningProcess();
  void shutdownOwnedProcess_data();
  void shutdownOwnedProcess();
  void shutdownFromAboutToQuit();
  void shutdownDuringLockedTransition();
  void shutdownFromStartingSignalStopsStartImmediately();
  void shutdownRejectsQueuedRestart();
  void shutdownTimeoutPreservesRecoveryMarker();
  void shutdownReportsAbnormalExit();
  void shutdownServiceDoesNotContactDaemon();

private:
  bool startOwnedProcess(deskflow::gui::CoreProcess &core, bool ignoreTerminate, int exitCode = 0);
  QTemporaryDir m_directory;
};
