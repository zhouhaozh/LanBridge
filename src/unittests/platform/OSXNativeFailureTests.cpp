// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "arch/Arch.h"
#include "base/EventQueue.h"
#include "base/Log.h"
#include "common/RemoteInputSession.h"
#include "platform/OSXKeyState.h"
#include <QTemporaryDir>
#include <QTest>

class FailingKeyboard : public OSXKeyState
{
public:
  explicit FailingKeyboard(IEventQueue *events) : OSXKeyState(events, {"en"}, false)
  {
  }
  using OSXKeyState::postKeyboardKey;

protected:
  CGEventRef createKeyboardEvent(CGKeyCode, bool) override
  {
    return nullptr;
  }
};

class OSXNativeFailureTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase()
  {
    m_arch.init();
  }
  void failedNativeReleaseKeepsRecoveryMarker()
  {
    EventQueue events;
    FailingKeyboard keyboard(&events);
    QTemporaryDir dir;
    const auto path = dir.filePath("session");
    deskflow::RemoteInputSession session(path);
    QVERIFY(session.begin());
    QVERIFY(!keyboard.nativeInjectionFailed());
    // Inject only allocation failure: no keyboard event is posted to macOS.
    keyboard.postKeyboardKey(0, false);
    QVERIFY(keyboard.nativeInjectionFailed());
    QVERIFY(!session.finish([&] { return !keyboard.nativeInjectionFailed(); }));
    QVERIFY(deskflow::RemoteInputSession::recoveryRequired(path));
    // Even later cleanup/reconnection cannot certify the uncertain release.
    QVERIFY(!session.begin());
    QVERIFY(!session.finish([] { return true; }));
    QVERIFY(deskflow::RemoteInputSession::recoveryRequired(path));
  }

private:
  Arch m_arch;
  Log m_log;
};
QTEST_GUILESS_MAIN(OSXNativeFailureTests)
#include "OSXNativeFailureTests.moc"
