/*
 * SPDX-FileCopyrightText: (C) 2026 LanBridge Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "common/Settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QUuid>

#include <bitset>
#include <utility>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

namespace deskflow {

// A receiver must own a durable marker before injecting any input. Destruction
// deliberately does not remove it: only ordered input cleanup, or a local user
// acknowledgement after the receiver has stopped, may do so.
class RemoteInputSession
{
public:
  static constexpr int recoveryExitCode = 6;

  explicit RemoteInputSession(QString path = markerPath())
      : m_path(std::move(path)),
        m_lock(m_path + QStringLiteral(".lock"))
  {
    // Do not declare a live receiver stale just because it has run for hours.
    // QLockFile still detects terminated owners using PID/process identity.
    m_lock.setStaleLockTime(0);
  }

  RemoteInputSession(const RemoteInputSession &) = delete;
  RemoteInputSession &operator=(const RemoteInputSession &) = delete;

  static QString markerPath()
  {
    // Do not use QStandardPaths::AppConfigLocation: the GUI and core have
    // different application names. They share the selected settings directory.
    return QDir(Settings::settingsPath()).filePath(QStringLiteral("uncleanRemoteSession"));
  }

  static bool recoveryRequired(const QString &path = markerPath())
  {
    const QFileInfo marker(path);
    return marker.exists() || marker.isSymLink();
  }

  // Caller must stop the core and ask the user to restore local keyboard/mouse
  // input first. This does not synthesize releases after a crashed process.
  static bool acknowledgeRecovery(const QString &path = markerPath())
  {
    const auto directory = QFileInfo(path).absolutePath();
    if (!QDir(directory).exists())
      return !recoveryRequired(path);
    QLockFile lock(path + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock())
      return false;
    if (!recoveryRequired(path))
      return true;
    if (!QFile::remove(path))
      return false;
    return synchronizeDirectory(directory);
  }

  bool begin()
  {
    if (m_blocked)
      return false;
    if (m_ownsMarker)
      return true;
    const auto directory = QFileInfo(m_path).absolutePath();
    if (!QDir().mkpath(directory))
      return denyInput();
    if (!m_lock.isLocked() && !m_lock.tryLock())
      return denyInput();
    if (recoveryRequired(m_path))
      return denyInput();

    QFile marker(m_path);
    if (!marker.open(QIODevice::WriteOnly | QIODevice::NewOnly, QFileDevice::ReadOwner | QFileDevice::WriteOwner))
      return denyInput();

    m_token = QUuid::createUuid().toByteArray(QUuid::WithoutBraces) + '\n';
    // Failed/partial writes are retained as an unclean marker. Never grant
    // ownership unless all persistence steps completed successfully.
    if (marker.write(m_token) != m_token.size() || !marker.flush())
      return denyInput();
#ifdef Q_OS_UNIX
    if (::fsync(marker.handle()) != 0)
      return denyInput();
#endif
    marker.close();
    if (!synchronizeDirectory(directory))
      return denyInput();

    m_ownsMarker = true;
    return true;
  }

  bool ownsMarker() const
  {
    return m_ownsMarker;
  }

  template <typename Cleanup, typename DirectorySync = bool (*)(const QString &)>
  bool finish(Cleanup releaseInput, DirectorySync syncDirectory = synchronizeDirectory)
  {
    // Once cleanup was uncertain, a later reconnect/destructor cannot certify
    // that releases attempted without permission actually reached macOS.
    if (m_blocked)
      return false;
    if (!m_ownsMarker)
      return recoveryRequired(m_path) ? denyInput() : true;

    if (!releaseInput())
      return denyInput();

    QFile marker(m_path);
    if (!marker.open(QIODevice::ReadOnly) || marker.readAll() != m_token)
      return denyInput();
    marker.close();
    if (!QFile::remove(m_path))
      return denyInput();

    const auto directory = QFileInfo(m_path).absolutePath();
    if (!syncDirectory(directory)) {
      // Removal was not durable. Restore a conservative marker before
      // reporting failure so a new process cannot treat this as a clean exit.
      QFile restore(m_path);
      if (restore.open(QIODevice::WriteOnly | QIODevice::NewOnly, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        restore.write(m_token);
        restore.flush();
#ifdef Q_OS_UNIX
        ::fsync(restore.handle());
#endif
        restore.close();
        synchronizeDirectory(directory);
      }
      return denyInput();
    }

    m_ownsMarker = false;
    m_token.clear();
    return true;
  }

private:
  bool denyInput()
  {
    m_blocked = true;
    return false;
  }

  static bool synchronizeDirectory(const QString &path)
  {
#ifdef Q_OS_UNIX
    const int directory = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY);
    if (directory < 0)
      return false;
    const bool synchronized = ::fsync(directory) == 0;
    ::close(directory);
    return synchronized;
#else
    Q_UNUSED(path);
    return true;
#endif
  }

  QString m_path;
  QLockFile m_lock;
  QByteArray m_token;
  bool m_ownsMarker = false;
  bool m_blocked = false;
};

// Separate remote ownership from the OS's physical-button state. Event taps
// can overwrite physical state and must not hide a remotely injected press.
template <size_t ButtonCount> class RemoteInputButtons
{
public:
  void record(size_t index, bool pressed)
  {
    if (index < ButtonCount)
      m_held.set(index, pressed);
  }

  template <typename Release> bool releaseAll(Release release)
  {
    for (size_t index = 0; index < ButtonCount; ++index) {
      if (m_held.test(index) && release(index))
        m_held.reset(index);
    }
    return m_held.none();
  }

private:
  std::bitset<ButtonCount> m_held;
};

} // namespace deskflow
