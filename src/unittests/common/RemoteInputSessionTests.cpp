/*
 * SPDX-FileCopyrightText: (C) 2026 LanBridge Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "common/RemoteInputSession.h"

#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <cstdio>

class RemoteInputSessionTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void markerPrecedesInputAndSurvivesUncleanDestruction()
  {
    QTemporaryDir directory;
    const auto path = directory.filePath("uncleanRemoteSession");
    {
      deskflow::RemoteInputSession session(path);
      QVERIFY(session.begin());
      QVERIFY(deskflow::RemoteInputSession::recoveryRequired(path));
      QVERIFY(session.begin());
    }
    QVERIFY(deskflow::RemoteInputSession::recoveryRequired(path));
  }

  void newInstanceCannotCleanPreviousInstance()
  {
    QTemporaryDir directory;
    const auto path = directory.filePath("uncleanRemoteSession");
    {
      deskflow::RemoteInputSession previous(path);
      QVERIFY(previous.begin());
    }
    deskflow::RemoteInputSession restarted(path);
    QVERIFY(!restarted.begin());
    bool cleanupCalled = false;
    QVERIFY(!restarted.finish([&cleanupCalled] {
      cleanupCalled = true;
      return true;
    }));
    QVERIFY(!cleanupCalled);
    QVERIFY(deskflow::RemoteInputSession::recoveryRequired(path));
  }

  void persistenceFailureDeniesInput()
  {
    QTemporaryDir directory;
    QFile parent(directory.filePath("not-a-directory"));
    QVERIFY(parent.open(QIODevice::WriteOnly));
    parent.close();
    deskflow::RemoteInputSession session(parent.fileName() + "/uncleanRemoteSession");
    QVERIFY(!session.begin());
    QVERIFY(!session.ownsMarker());
  }

  void failedCleanupKeepsMarker()
  {
    QTemporaryDir directory;
    const auto path = directory.filePath("uncleanRemoteSession");
    deskflow::RemoteInputSession session(path);
    QVERIFY(session.begin());
    QVERIFY(!session.finish([] { return false; }));
    QVERIFY(deskflow::RemoteInputSession::recoveryRequired(path));
    QVERIFY(session.ownsMarker());
    QVERIFY(!session.begin());
    QVERIFY(!session.finish([] { return true; }));
    QVERIFY(deskflow::RemoteInputSession::recoveryRequired(path));
  }

  void cleanFinishIsOrderedAndIdempotent()
  {
    QTemporaryDir directory;
    const auto path = directory.filePath("uncleanRemoteSession");
    deskflow::RemoteInputSession session(path);
    QVERIFY(session.begin());
    int cleanupCount = 0;
    QVERIFY(session.finish([&] {
      ++cleanupCount;
      return deskflow::RemoteInputSession::recoveryRequired(path);
    }));
    QVERIFY(!deskflow::RemoteInputSession::recoveryRequired(path));
    QVERIFY(session.finish([&] {
      ++cleanupCount;
      return true;
    }));
    QCOMPARE(cleanupCount, 1);
    QVERIFY(session.begin());
  }

  void replacedMarkerCannotBeClearedByOldOwner()
  {
    QTemporaryDir directory;
    const auto path = directory.filePath("uncleanRemoteSession");
    deskflow::RemoteInputSession session(path);
    QVERIFY(session.begin());
    QFile replacement(path);
    QVERIFY(replacement.open(QIODevice::WriteOnly | QIODevice::Truncate));
    replacement.write("another-session");
    replacement.close();
    QVERIFY(!session.finish([] { return true; }));
    QVERIFY(deskflow::RemoteInputSession::recoveryRequired(path));
  }

  void failedRemovalSyncRestoresRecoveryMarker()
  {
    QTemporaryDir directory;
    const auto path = directory.filePath("uncleanRemoteSession");
    deskflow::RemoteInputSession session(path);
    QVERIFY(session.begin());
    QVERIFY(!session.finish([] { return true; }, [](const QString &) { return false; }));
    QVERIFY(deskflow::RemoteInputSession::recoveryRequired(path));
    QVERIFY(!session.begin());
    QVERIFY(!session.finish([] { return true; }));
  }

  void localAcknowledgementAllowsNewSession()
  {
    QTemporaryDir directory;
    const auto path = directory.filePath("uncleanRemoteSession");
    {
      deskflow::RemoteInputSession crashed(path);
      QVERIFY(crashed.begin());
    }
    QVERIFY(deskflow::RemoteInputSession::acknowledgeRecovery(path));
    QVERIFY(deskflow::RemoteInputSession::acknowledgeRecovery(path));
    deskflow::RemoteInputSession restarted(path);
    QVERIFY(restarted.begin());
  }

  void localAcknowledgementCannotClearLiveReceiver()
  {
    QTemporaryDir directory;
    const auto path = directory.filePath("uncleanRemoteSession");
    {
      deskflow::RemoteInputSession liveReceiver(path);
      QVERIFY(liveReceiver.begin());
      QVERIFY(!deskflow::RemoteInputSession::acknowledgeRecovery(path));
      QVERIFY(deskflow::RemoteInputSession::recoveryRequired(path));
      QVERIFY(liveReceiver.finish([] { return true; }));
      // Keep the process lock even between cross-screen visits.
      QVERIFY(!deskflow::RemoteInputSession::acknowledgeRecovery(path));
    }
    QVERIFY(deskflow::RemoteInputSession::acknowledgeRecovery(path));
  }

  void killedProcessLeavesMarkerAndReleasesOnlyItsLock()
  {
    QTemporaryDir directory;
    const auto path = directory.filePath("uncleanRemoteSession");
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(), {"--hold-session-marker", path});
    QVERIFY(child.waitForStarted(5000));
    QVERIFY(child.waitForReadyRead(5000));
    QCOMPARE(child.readAllStandardOutput(), QByteArray("ready\n"));
    QVERIFY(!deskflow::RemoteInputSession::acknowledgeRecovery(path));
    child.kill();
    QVERIFY(child.waitForFinished(5000));
    QVERIFY(deskflow::RemoteInputSession::recoveryRequired(path));
    {
      deskflow::RemoteInputSession restarted(path);
      QVERIFY(!restarted.begin());
    }
    QVERIFY(deskflow::RemoteInputSession::acknowledgeRecovery(path));
    deskflow::RemoteInputSession acknowledged(path);
    QVERIFY(acknowledged.begin());
  }

  void buttonsReleaseOnlyRemoteHeldButtonsOnce()
  {
    deskflow::RemoteInputButtons<6> buttons;
    buttons.record(0, true);
    buttons.record(2, true);
    buttons.record(2, false);
    buttons.record(5, true);
    QList<size_t> released;
    const auto release = [&released](size_t index) {
      released.append(index);
      return true;
    };
    QVERIFY(buttons.releaseAll(release));
    QVERIFY(buttons.releaseAll(release));
    QCOMPARE(released, (QList<size_t>{0, 5}));
  }

  void failedButtonReleaseRemainsPending()
  {
    deskflow::RemoteInputButtons<6> buttons;
    buttons.record(1, true);
    QVERIFY(!buttons.releaseAll([](size_t) { return false; }));
    int released = 0;
    QVERIFY(buttons.releaseAll([&released](size_t index) {
      ++released;
      return index == 1;
    }));
    QCOMPARE(released, 1);
  }
};

int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  if (app.arguments().size() == 3 && app.arguments().at(1) == QStringLiteral("--hold-session-marker")) {
    deskflow::RemoteInputSession session(app.arguments().at(2));
    if (!session.begin())
      return 1;
    std::fputs("ready\n", stdout);
    std::fflush(stdout);
    return app.exec();
  }
  RemoteInputSessionTests tests;
  return QTest::qExec(&tests, argc, argv);
}
#include "RemoteInputSessionTests.moc"
