// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "common/RemoteInputSession.h"
#include "gui/Diagnostic.h"
#include <QFile>
#include <QLockFile>
#include <QTemporaryDir>
#include <QTest>

class DiagnosticTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void resetPreservesUncleanMarker()
  {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Settings::setSettingsFile(directory.filePath("profile.conf"));
    Settings::setStateFile(directory.filePath("profile.state"));
    QFile marker(deskflow::RemoteInputSession::markerPath());
    QVERIFY(marker.open(QIODevice::WriteOnly));
    marker.write("unclean");
    marker.close();
    Settings::setValue(Settings::Core::ComputerName, "preserve-me");
    QVERIFY(!deskflow::gui::diagnostic::clearSettings(false));
    QVERIFY(deskflow::RemoteInputSession::recoveryRequired());
    QCOMPARE(Settings::value(Settings::Core::ComputerName).toString(), QString("preserve-me"));
  }
  void resetRefusesLiveReceiverLock()
  {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Settings::setSettingsFile(directory.filePath("profile.conf"));
    Settings::setStateFile(directory.filePath("profile.state"));
    QLockFile active(deskflow::RemoteInputSession::markerPath() + ".lock");
    active.setStaleLockTime(0);
    QVERIFY(active.tryLock());
    Settings::setValue(Settings::Core::ComputerName, "preserve-me");
    QVERIFY(!deskflow::gui::diagnostic::clearSettings(false));
    QCOMPARE(Settings::value(Settings::Core::ComputerName).toString(), QString("preserve-me"));
    QVERIFY(active.isLocked());
  }
  void cleanStoppedProfileCanReset()
  {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Settings::setSettingsFile(directory.filePath("profile.conf"));
    Settings::setStateFile(directory.filePath("profile.state"));
    QFile sentinel(directory.filePath("old-certificate"));
    QVERIFY(sentinel.open(QIODevice::WriteOnly));
    sentinel.close();
    QVERIFY(deskflow::gui::diagnostic::clearSettings(false));
    QVERIFY(!sentinel.exists());
    QVERIFY(!QFile::exists(deskflow::RemoteInputSession::markerPath() + ".lock"));
  }
};
QTEST_MAIN(DiagnosticTests)
#include "DiagnosticTests.moc"
