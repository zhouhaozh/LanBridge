/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2024 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "Diagnostic.h"

#include "common/RemoteInputSession.h"
#include "common/Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QProcess>

namespace deskflow::gui::diagnostic {

void restart()
{
  QString program = QCoreApplication::applicationFilePath();
  QStringList arguments = QCoreApplication::arguments();

  // look for and remove --reset option if found
  if (int resetIndex = arguments.indexOf("--reset"); resetIndex != -1)
    arguments.remove(resetIndex);

  qInfo("launching new process: %s", qPrintable(program));
  QProcess::startDetached(program, arguments);

  qDebug("exiting current process");
  QCoreApplication::exit();
}

bool clearSettings(bool enableRestart)
{
  // A reset must never erase recovery evidence or the lock of an orphan receiver.
  const auto marker = deskflow::RemoteInputSession::markerPath();
  if (deskflow::RemoteInputSession::recoveryRequired() || !QDir().mkpath(Settings::settingsPath())) {
    qWarning("refusing settings reset: local input recovery is required or profile is unavailable");
    return false;
  }
  QLockFile resetLock(marker + QStringLiteral(".lock"));
  resetLock.setStaleLockTime(0);
  if (!resetLock.tryLock() || deskflow::RemoteInputSession::recoveryRequired()) {
    qWarning("refusing settings reset: a receiver is active or local input recovery is required");
    return false;
  }

  qDebug("clearing settings");
  Settings::proxy().clear();
  Settings::clearState();

  // save but do not emit saving signal which will prevent the current state of
  // the app config and server configs from being applied.
  Settings::save(false);

  auto profileDir = QDir(Settings::settingsPath());
  // Hold the lock while clearing files; deleting the directory would unlink the
  // lock and allow another receiver to create a new session during the reset.
  const auto entries = profileDir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
  for (const auto &entry : entries) {
    if (entry.absoluteFilePath() == marker + QStringLiteral(".lock"))
      continue;
    const bool removed = entry.isDir() && !entry.isSymLink() ? QDir(entry.absoluteFilePath()).removeRecursively()
                                                             : QFile::remove(entry.absoluteFilePath());
    if (!removed) {
      qWarning().noquote() << "could not remove profile entry:" << entry.absoluteFilePath();
      return false;
    }
  }

#ifdef Q_OS_WIN
  if (Settings::isPortableMode()) {
    // make a new empty portable settings file
    if (profileDir.mkpath(Settings::settingsPath())) {
      QFile file(Settings::settingsFile());
      std::ignore = file.open(QIODevice::WriteOnly);
      file.write(" ", 1);
      file.close();
    }
  }
#endif

  if (enableRestart) {
    qDebug("restarting");
    restart();
  } else {
    qDebug("skipping restart");
  }
  return true;
}

} // namespace deskflow::gui::diagnostic
