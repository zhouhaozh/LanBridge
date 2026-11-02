/*
 * SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "common/Settings.h"
#include "gui/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

// Keep Cocoa activation and notifications out of this isolated Qt regression.
void requestOSXNotificationPermission()
{
}
bool isOSXDevelopmentBuild()
{
  return true;
}
bool showOSXNotification(const QString &, const QString &)
{
  return false;
}
bool isOSXInterfaceStyleDark()
{
  return false;
}
void forceAppActive()
{
}

namespace {
class VetoWindow : public QWidget
{
public:
  bool allowClose = false;

protected:
  void closeEvent(QCloseEvent *event) override
  {
    allowClose ? event->accept() : event->ignore();
  }
};

int runScenario(int argc, char **argv)
{
  const auto scenario = QString::fromLocal8Bit(argv[2]);
  const auto reportPath = QString::fromLocal8Bit(argv[3]);
  QApplication app(argc, argv);
  const auto profile = qEnvironmentVariable("XDG_CONFIG_HOME");
  Settings::setSettingsFile(profile + "/profile.conf");
  Settings::setStateFile(profile + "/profile.state");
  Settings::setValue(Settings::Gui::AutoStartCore, false);
  Settings::setValue(Settings::Gui::AutoUpdateCheck, false);
  Settings::setValue(Settings::Gui::CloseReminder, false);
  Settings::setValue(Settings::Gui::CloseToTray, false);
  Settings::setValue(Settings::LanBridge::SetupComplete, true);

  MainWindow window;
  VetoWindow vetoWindow;
  QJsonObject report;
  QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [&] {
    report["aboutToQuit"] = true;
    report["autostart"] = Settings::value(Settings::Gui::AutoStartCore).toBool();
    QFile output(reportPath);
    if (output.open(QIODevice::WriteOnly))
      output.write(QJsonDocument(report).toJson());
  });
  QTimer::singleShot(3000, &app, [&] {
    report["timedOut"] = true;
    app.exit(72);
  });
  QTimer::singleShot(0, &window, [&] {
    window.show();
    if (scenario == "veto-quit") {
      vetoWindow.show();
      QCoreApplication::postEvent(&app, new QEvent(QEvent::Quit));
      QTimer::singleShot(50, &window, [&] {
        report["quitWasVetoed"] = !report["aboutToQuit"].toBool();
        window.show();
        report["normalCloseIgnored"] = !window.close();
        report["hiddenAfterVeto"] = !window.isVisible();
        vetoWindow.allowClose = true;
        vetoWindow.close();
        app.quit();
      });
      return;
    }
    if (scenario == "native-quit") {
      report["nativeQuitRequested"] = true;
      QCoreApplication::postEvent(&app, new QEvent(QEvent::Quit));
      return;
    }
    window.close();
    report["hidden"] = !window.isVisible();
    QTimer::singleShot(50, &window, [&] {
      report["backgroundTick"] = true;
      if (scenario == "close-restore") {
        auto *restore = window.findChild<QAction *>("actionRestore");
        if (!restore) {
          app.exit(73);
          return;
        }
        restore->trigger();
        report["restored"] = window.isVisible();
        window.close();
        report["hiddenAgain"] = !window.isVisible();
        app.quit();
      } else {
        auto *quit = window.findChild<QAction *>("actionTrayQuit");
        if (!quit) {
          app.exit(74);
          return;
        }
        report["trayQuitRequested"] = true;
        quit->trigger();
      }
    });
  });
  return app.exec();
}
} // namespace

class MainWindowLifecycleTests : public QObject
{
  Q_OBJECT
private:
  QJsonObject run(const QString &scenario)
  {
    QTemporaryDir directory(QStringLiteral("/tmp/lanbridge-window-XXXXXX"));
    if (!directory.isValid())
      return {};
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto &key : {"HOME", "TMPDIR", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_RUNTIME_DIR"})
      environment.insert(key, directory.path());
    environment.insert("QT_QPA_PLATFORM", "offscreen");
    const auto reportPath = directory.filePath("result.json");
    QProcess child;
    child.setProcessEnvironment(environment);
    child.start(QCoreApplication::applicationFilePath(), {"--scenario", scenario, reportPath});
    if (!child.waitForStarted() || !child.waitForFinished(10000)) {
      child.kill();
      child.waitForFinished();
      QTest::qFail("Isolated lifecycle scenario did not finish", __FILE__, __LINE__);
      return {};
    }
    if (child.exitStatus() != QProcess::NormalExit || child.exitCode() != 0) {
      qWarning().noquote() << child.readAllStandardError();
      QTest::qFail("Isolated lifecycle scenario exited unsuccessfully", __FILE__, __LINE__);
      return {};
    }
    QFile report(reportPath);
    if (!report.open(QIODevice::ReadOnly))
      return {};
    return QJsonDocument::fromJson(report.readAll()).object();
  }

private Q_SLOTS:
  void closingWindowKeepsApplicationAliveAndCanRestore()
  {
    const auto report = run("close-restore");
    QVERIFY(report["hidden"].toBool());
    QVERIFY(report["backgroundTick"].toBool());
    QVERIFY(report["restored"].toBool());
    QVERIFY(report["hiddenAgain"].toBool());
    QVERIFY(report["aboutToQuit"].toBool());
    QVERIFY(!report["autostart"].toBool());
    QVERIFY(!report["timedOut"].toBool());
  }

  void menuBarQuitExitsWhileWindowIsHidden()
  {
    const auto report = run("tray-quit");
    QVERIFY(report["hidden"].toBool());
    QVERIFY(report["backgroundTick"].toBool());
    QVERIFY(report["trayQuitRequested"].toBool());
    QVERIFY(report["aboutToQuit"].toBool());
    QVERIFY(!report["timedOut"].toBool());
  }

  void nativeApplicationQuitIsNotChangedIntoHiding()
  {
    const auto report = run("native-quit");
    QVERIFY(report["nativeQuitRequested"].toBool());
    QVERIFY(report["aboutToQuit"].toBool());
    QVERIFY(!report["timedOut"].toBool());
  }

  void vetoedQuitDoesNotTurnLaterWindowCloseIntoExit()
  {
    const auto report = run("veto-quit");
    QVERIFY(report["quitWasVetoed"].toBool());
    QVERIFY(report["normalCloseIgnored"].toBool());
    QVERIFY(report["hiddenAfterVeto"].toBool());
    QVERIFY(report["aboutToQuit"].toBool());
    QVERIFY(!report["timedOut"].toBool());
  }
};

int main(int argc, char **argv)
{
  if (argc == 4 && QByteArray(argv[1]) == "--scenario")
    return runScenario(argc, argv);
  QApplication app(argc, argv);
  MainWindowLifecycleTests tests;
  return QTest::qExec(&tests, argc, argv);
}

#include "MainWindowLifecycleTests.moc"
