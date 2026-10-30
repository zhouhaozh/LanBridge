/*
 * SPDX-FileCopyrightText: (C) 2026 LanBridge Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "../deskflow/MockEventQueue.h"
#include "arch/Arch.h"
#include "base/EventQueueTimer.h"
#include "base/Log.h"
#include "client/Client.h"
#include "deskflow/AppUtil.h"
#include "deskflow/Clipboard.h"
#include "deskflow/Computer.h"
#include "deskflow/PlatformComputer.h"
#include "io/IStream.h"
#include "net/ISocketFactory.h"
#include "server/ClientProxy1_3.h"
#include "server/PrimaryClient.h"
#include "server/Server.h"

#include <QTemporaryDir>
#include <QTest>

#include <array>
#include <map>
#include <memory>

namespace {
class TestAppUtil : public AppUtil
{
public:
  int run() override
  {
    return 0;
  }
  std::vector<std::string> getKeyboardLayoutList() override
  {
    return {"en"};
  }
  std::string getCurrentLanguageCode() override
  {
    return "en";
  }
};

class DispatchQueue : public MockEventQueue
{
public:
  void addHandler(EventTypes type, void *target, const EventHandler &handler) override
  {
    handlers[{type, target}] = handler;
  }
  void removeHandler(EventTypes type, void *target) override
  {
    handlers.erase({type, target});
  }
  bool dispatchEvent(const Event &event) override
  {
    const auto found = handlers.find({event.getType(), event.getTarget()});
    if (found == handlers.end())
      return false;
    const auto handler = found->second;
    handler(event);
    return true;
  }
  void addEvent(Event &&event) override
  {
    dispatchEvent(event);
    Event::deleteData(event);
  }
  EventQueueTimer *newTimer(double duration, void *) override
  {
    recurring.append(duration);
    return new EventQueueTimer;
  }
  EventQueueTimer *newOneShotTimer(double duration, void *) override
  {
    oneShots.append(duration);
    return new EventQueueTimer;
  }
  void deleteTimer(EventQueueTimer *timer) override
  {
    delete timer;
  }

  QList<double> recurring;
  QList<double> oneShots;
  std::map<std::pair<EventTypes, void *>, EventHandler> handlers;
};

// No native platform API is called by this computer.
class TestComputer : public PlatformComputer
{
public:
  explicit TestComputer(IEventQueue *events) : PlatformComputer(events)
  {
  }
  bool held = false;
  bool entered = true;
  int emergencyRegistrations = 0;
  std::map<std::pair<KeyID, KeyModifierMask>, uint32_t> hotkeys;
  mutable int clipboardReads = 0;
  int clipboardWrites = 0;
  int clipboardGrabs = 0;
  std::array<Clipboard, kClipboardEnd> clipboards;
  void *getEventTarget() const override
  {
    return const_cast<TestComputer *>(this);
  }
  bool getClipboard(ClipboardID id, IClipboard *clipboard) const override
  {
    ++clipboardReads;
    return Clipboard::copy(clipboard, &clipboards.at(id));
  }
  void getShape(int32_t &x, int32_t &y, int32_t &w, int32_t &h) const override
  {
    x = y = 0;
    w = 800;
    h = 600;
  }
  void getCursorPos(int32_t &x, int32_t &y) const override
  {
    x = 400;
    y = 300;
  }
  void getCursorCenter(int32_t &x, int32_t &y) const override
  {
    getCursorPos(x, y);
  }
  void reconfigure(uint32_t) override
  {
  }
  uint32_t activeSides() override
  {
    return 0;
  }
  void warpCursor(int32_t, int32_t) override
  {
  }
  uint32_t registerHotKey(KeyID key, KeyModifierMask mask) override
  {
    if (key == kKeyEscape && mask == (KeyModifierControl | KeyModifierAlt))
      ++emergencyRegistrations;
    // Like X11, a later registration for this chord replaces its native ID.
    hotkeys[{key, mask}] = ++hotkey;
    return hotkey;
  }
  void unregisterHotKey(uint32_t id) override
  {
    std::erase_if(hotkeys, [id](const auto &entry) { return entry.second == id; });
  }
  void fakeInputBegin() override
  {
  }
  void fakeInputEnd() override
  {
  }
  int32_t getJumpZoneSize() const override
  {
    return 1;
  }
  bool isAnyMouseButtonDown(uint32_t &button) const override
  {
    button = held ? 1 : 0;
    return held;
  }
  void fakeMouseButton(ButtonID, bool) override
  {
  }
  void fakeMouseMove(int32_t, int32_t) override
  {
  }
  void fakeMouseRelativeMove(int32_t, int32_t) const override
  {
  }
  void fakeMouseWheel(ScrollDelta) const override
  {
  }
  void updateKeyMap() override
  {
  }
  void updateKeyState() override
  {
  }
  void setHalfDuplexMask(KeyModifierMask) override
  {
  }
  KeyModifierMask pollActiveModifiers() const override
  {
    return 0;
  }
  KeyModifierMask getActiveModifiers() const override
  {
    return 0;
  }
  void enable() override
  {
  }
  void disable() override
  {
  }
  void enter() override
  {
    entered = true;
  }
  bool canLeave() override
  {
    return true;
  }
  void leave() override
  {
    entered = false;
  }
  bool setClipboard(ClipboardID id, const IClipboard *clipboard) override
  {
    if (clipboard == nullptr) {
      ++clipboardGrabs;
      return true;
    }
    ++clipboardWrites;
    return Clipboard::copy(&clipboards.at(id), clipboard);
  }
  void checkClipboards() override
  {
  }
  void openScreensaver(bool) override
  {
  }
  void closeScreensaver() override
  {
  }
  void screensaver(bool) override
  {
  }
  void resetOptions() override
  {
  }
  void setOptions(const OptionsList &) override
  {
  }
  void setSequenceNumber(uint32_t) override
  {
  }
  bool isPrimary() const override
  {
    return true;
  }
  std::string getSecureInputApp() const override
  {
    return {};
  }

protected:
  void updateButtons() override
  {
  }
  IKeyState *getKeyState() const override
  {
    return nullptr;
  }
  void handleSystemEvent(const Event &) override
  {
  }

private:
  uint32_t hotkey = 0;
};

class SinkStream : public deskflow::IStream
{
public:
  void write(const void *, uint32_t) override
  {
  }
  void close() override
  {
  }
  uint32_t read(void *, uint32_t) override
  {
    return 0;
  }
  void flush() override
  {
  }
  void shutdownInput() override
  {
  }
  void shutdownOutput() override
  {
  }
  void *getEventTarget() const override
  {
    return const_cast<SinkStream *>(this);
  }
  bool isReady() const override
  {
    return false;
  }
  uint32_t getSize() const override
  {
    return 0;
  }
};

class TestRemote : public ClientProxy1_3
{
public:
  explicit TestRemote(IEventQueue *events) : ClientProxy1_3("mac", new SinkStream, events)
  {
  }
  void getShape(int32_t &x, int32_t &y, int32_t &w, int32_t &h) const override
  {
    x = y = 0;
    w = 800;
    h = 600;
  }
  void getCursorPos(int32_t &x, int32_t &y) const override
  {
    x = 400;
    y = 300;
  }
  void enter(int32_t, int32_t, uint32_t, KeyModifierMask, bool) override
  {
    ++entries;
  }
  bool leave() override
  {
    ++leaves;
    return acceptsLeave;
  }
  void setOptions(const OptionsList &options) override
  {
    receivedOptions = options;
    ClientProxy1_3::setOptions(options);
  }
  bool getClipboard(ClipboardID id, IClipboard *clipboard) const override
  {
    ++clipboardReads;
    return Clipboard::copy(clipboard, &providedClipboards.at(id));
  }
  void setClipboard(ClipboardID id, const IClipboard *clipboard) override
  {
    ++clipboardWrites;
    Clipboard::copy(&receivedClipboards.at(id), clipboard);
  }
  int entries = 0;
  int leaves = 0;
  bool acceptsLeave = true;
  OptionsList receivedOptions;
  mutable int clipboardReads = 0;
  int clipboardWrites = 0;
  std::array<Clipboard, kClipboardEnd> providedClipboards;
  std::array<Clipboard, kClipboardEnd> receivedClipboards;
};

class NoSocketFactory : public ISocketFactory
{
public:
  IDataSocket *create(IArchNetwork::AddressFamily, SecurityLevel) const override
  {
    return nullptr;
  }
  IListenSocket *createListen(IArchNetwork::AddressFamily, SecurityLevel) const override
  {
    return nullptr;
  }
};

bool writeClipboard(Clipboard &clipboard, const std::string &text, const std::string &html = {})
{
  if (!clipboard.open(1))
    return false;
  const bool cleared = clipboard.empty();
  if (cleared) {
    clipboard.add(IClipboard::Format::Text, text);
    if (!html.empty())
      clipboard.add(IClipboard::Format::HTML, html);
  }
  clipboard.close();
  return cleared;
}

std::string readClipboard(const Clipboard &clipboard, IClipboard::Format format = IClipboard::Format::Text)
{
  if (!clipboard.open(0))
    return {};
  const auto data = clipboard.get(format);
  clipboard.close();
  return data;
}

struct Fixture
{
  DispatchQueue events;
  deskflow::server::Config config{&events};
  TestComputer *platform = new TestComputer(&events);
  deskflow::Computer computer{platform, &events};
  PrimaryClient primary{"linux", &computer};
  std::unique_ptr<Server> server;
  TestRemote *remote = nullptr;

  explicit Fixture(bool conflictingEmergency = false, bool enableClipboard = false, int clipboardSize = 1024)
  {
    config.addComputer("linux");
    config.addComputer("mac");
    config.connect("linux", Direction::Right, 0, 1, "mac", 0, 1);
    config.connect("mac", Direction::Left, 0, 1, "linux", 0, 1);
    config.addOption("", kOptionDisableLockToComputer, 1);
    config.addOption("", kOptionClipboardSharing, enableClipboard);
    config.addOption("", kOptionClipboardSharingSize, clipboardSize);
    config.addOption("", kOptionHeartbeat, 0);
    if (conflictingEmergency)
      addConflictingEmergencyRule(*config.getInputFilter());
    server = std::make_unique<Server>(config, &primary, &computer, &events);
    remote = new TestRemote(&events);
    server->adoptClient(remote);
  }

  void switchTo(const std::string &name)
  {
    Server::SwitchToComputerInfo info(name);
    events.dispatchEvent(
        Event(EventTypes::ServerSwitchToComputer, config.getInputFilter(), &info, Event::EventFlags::DontFreeData)
    );
  }

  void remoteClipboardEvent(EventTypes type, ClipboardID id)
  {
    IComputer::ClipboardInfo info{id, 0};
    events.dispatchEvent(Event(type, remote->getEventTarget(), &info, Event::EventFlags::DontFreeData));
  }

  void addConflictingEmergencyRule(InputFilter &filter)
  {
    InputFilter::Rule rule(new InputFilter::KeystrokeCondition(&events, kKeyEscape, KeyModifierControl | KeyModifierAlt)
    );
    rule.adoptAction(new InputFilter::SwitchToComputerAction(&events, "mac"), true);
    rule.adoptAction(new InputFilter::SwitchToComputerAction(&events, "mac"), false);
    filter.addFilterRule(rule);
  }

  void nativeEmergency(EventTypes type)
  {
    const auto id = platform->hotkeys.at({kKeyEscape, KeyModifierControl | KeyModifierAlt});
    events.addEvent(Event(type, platform, IPrimaryComputer::HotKeyInfo::alloc(id)));
  }
};
} // namespace

class ServerSafetyTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase()
  {
    arch.init();
    Settings::setSettingsFile(directory.filePath("test.conf"));
  }

  void emergencyReturnRestoresLocalBeforeRemoteCanRefuse()
  {
    Fixture test;
    test.switchTo("mac");
    QVERIFY(!test.platform->entered);
    test.platform->held = true;
    test.remote->acceptsLeave = false;
    test.switchTo("linux");
    QVERIFY(test.platform->entered);
    QCOMPARE(test.remote->leaves, 1);
  }

  void rawEmergencyChordReturnsLocalDuringDrag()
  {
    Fixture test;
    test.switchTo("mac");
    test.platform->held = true;
    auto *info = IKeyState::KeyInfo::alloc(kKeyEscape, KeyModifierControl | KeyModifierAlt, 9, 1);
    test.events.addEvent(Event(EventTypes::KeyStateKeyDown, test.config.getInputFilter(), info));
    QVERIFY(test.platform->entered);
    QCOMPARE(test.remote->leaves, 1);
  }

  void nativeEmergencyOverridesConflictingConfigOnPressAndRelease()
  {
    Fixture test(true);
    QCOMPARE(test.platform->emergencyRegistrations, 1);
    test.switchTo("mac");
    test.platform->held = true;
    test.nativeEmergency(EventTypes::PrimaryComputerHotkeyDown);
    QVERIFY(test.platform->entered);
    QCOMPARE(test.remote->leaves, 1);
    test.nativeEmergency(EventTypes::PrimaryComputerHotkeyUp);
    QVERIFY(test.platform->entered);
    QCOMPARE(test.remote->entries, 1);
  }

  void nativeEmergencyRemainsReservedAfterConfigReloadAndRuleAddition()
  {
    Fixture test;
    InputFilter replacement(&test.events);
    test.addConflictingEmergencyRule(replacement);
    *test.config.getInputFilter() = replacement;
    test.addConflictingEmergencyRule(*test.config.getInputFilter());
    QCOMPARE(test.platform->emergencyRegistrations, 2);
    test.switchTo("mac");
    test.platform->held = true;
    test.nativeEmergency(EventTypes::PrimaryComputerHotkeyDown);
    QVERIFY(test.platform->entered);
    test.nativeEmergency(EventTypes::PrimaryComputerHotkeyUp);
    QVERIFY(test.platform->entered);
    QCOMPARE(test.remote->entries, 1);
  }

  void heldButtonBlocksEdgeEvenWhenScrollLockDisabled()
  {
    Fixture test;
    test.platform->held = true;
    auto *motion = IPrimaryComputer::MotionInfo::alloc(799, 300);
    test.events.addEvent(Event(EventTypes::PrimaryComputerMotionOnPrimary, test.platform, motion));
    QVERIFY(test.platform->entered);
    QCOMPARE(test.remote->entries, 0);
    test.platform->held = false;
    motion = IPrimaryComputer::MotionInfo::alloc(799, 300);
    test.events.addEvent(Event(EventTypes::PrimaryComputerMotionOnPrimary, test.platform, motion));
    QVERIFY(!test.platform->entered);
    QCOMPARE(test.remote->entries, 1);
  }

  void coreOptionsRespectClipboardAndOneSecondHeartbeat_data()
  {
    QTest::addColumn<bool>("enabled");
    QTest::addColumn<int>("sizeKiB");
    QTest::newRow("enabled") << true << 1024;
    QTest::newRow("disabled") << false << 1024;
    QTest::newRow("zero-limit") << true << 0;
  }

  void coreOptionsRespectClipboardAndOneSecondHeartbeat()
  {
    QFETCH(bool, enabled);
    QFETCH(int, sizeKiB);
    Fixture test(false, enabled, sizeKiB);
    std::map<OptionID, OptionValue> options;
    const auto &received = test.remote->receivedOptions;
    for (size_t i = 0; i < received.size(); i += 2)
      options[received[i]] = received[i + 1];
    QCOMPARE(options[kOptionClipboardSharing], int(enabled));
    QCOMPARE(options[kOptionClipboardSharingSize], sizeKiB);
    QCOMPARE(options[kOptionHeartbeat], 1000);
    QCOMPARE(test.server->getMaximumClipboardSizeBytes(), size_t(sizeKiB) * 1024);
    QCOMPARE(test.events.recurring.last(), 1.0);
    QCOMPARE(test.events.oneShots.last(), 3.0);
    test.switchTo("mac");
    QCOMPARE(test.platform->clipboardReads, enabled && sizeKiB > 0 ? int(kClipboardEnd) : 0);
    if (!enabled || sizeKiB == 0) {
      test.remoteClipboardEvent(EventTypes::ClipboardGrabbed, kClipboardClipboard);
      test.remoteClipboardEvent(EventTypes::ClipboardChanged, kClipboardClipboard);
      QCOMPARE(test.remote->clipboardReads, 0);
      QCOMPARE(test.remote->clipboardWrites, 0);
      QCOMPARE(test.platform->clipboardWrites, 0);
      QCOMPARE(test.platform->clipboardGrabs, 0);
    }
  }

  void clipboardFromPrimaryReachesRemoteOnCrossing()
  {
    Fixture test(false, true);
    QVERIFY(writeClipboard(test.platform->clipboards[kClipboardClipboard], "Linux 中文", "<b>Linux 中文</b>"));
    QVERIFY(writeClipboard(test.platform->clipboards[kClipboardSelection], "X11 selection"));

    test.switchTo("mac");

    QCOMPARE(readClipboard(test.remote->receivedClipboards[kClipboardClipboard]), std::string("Linux 中文"));
    QCOMPARE(
        readClipboard(test.remote->receivedClipboards[kClipboardClipboard], IClipboard::Format::HTML),
        std::string("<b>Linux 中文</b>")
    );
    QCOMPARE(readClipboard(test.remote->receivedClipboards[kClipboardSelection]), std::string("X11 selection"));
    QCOMPARE(test.remote->clipboardWrites, int(kClipboardEnd));
    QCOMPARE(test.platform->clipboardWrites, 0);
  }

  void clipboardFromRemoteReachesPrimary()
  {
    Fixture test(false, true);
    QVERIFY(writeClipboard(test.remote->providedClipboards[kClipboardClipboard], "Mac 中文", "<b>Mac 中文</b>"));

    test.remoteClipboardEvent(EventTypes::ClipboardGrabbed, kClipboardClipboard);
    test.remoteClipboardEvent(EventTypes::ClipboardChanged, kClipboardClipboard);

    QCOMPARE(readClipboard(test.platform->clipboards[kClipboardClipboard]), std::string("Mac 中文"));
    QCOMPARE(
        readClipboard(test.platform->clipboards[kClipboardClipboard], IClipboard::Format::HTML),
        std::string("<b>Mac 中文</b>")
    );
    QCOMPARE(test.remote->clipboardReads, 1);
    QCOMPARE(test.platform->clipboardWrites, 1);
  }

  void oversizedClipboardDoesNotReplacePrimaryAndNextUpdateWorks()
  {
    Fixture test(false, true, 1);
    QVERIFY(writeClipboard(test.remote->providedClipboards[kClipboardClipboard], std::string(1024, 'x')));
    test.remoteClipboardEvent(EventTypes::ClipboardGrabbed, kClipboardClipboard);
    test.remoteClipboardEvent(EventTypes::ClipboardChanged, kClipboardClipboard);
    QCOMPARE(test.platform->clipboardWrites, 0);

    QVERIFY(writeClipboard(test.remote->providedClipboards[kClipboardClipboard], "within limit"));
    test.remoteClipboardEvent(EventTypes::ClipboardChanged, kClipboardClipboard);
    QCOMPARE(test.platform->clipboardWrites, 1);
    QCOMPARE(readClipboard(test.platform->clipboards[kClipboardClipboard]), std::string("within limit"));
  }

  void clientRespectsClipboardOptions_data()
  {
    QTest::addColumn<bool>("enabled");
    QTest::addColumn<int>("sizeKiB");
    QTest::newRow("enabled") << true << 1024;
    QTest::newRow("disabled") << false << 1024;
    QTest::newRow("zero-limit") << true << 0;
  }

  void clientRespectsClipboardOptions()
  {
    QFETCH(bool, enabled);
    QFETCH(int, sizeKiB);
    DispatchQueue events;
    auto *platform = new TestComputer(&events);
    deskflow::Computer computer(platform, &events);
    Client client(&events, "mac", NetworkAddress{}, new NoSocketFactory, &computer);
    Clipboard clipboard;
    QVERIFY(writeClipboard(clipboard, "Remote 中文", "<b>Remote 中文</b>"));
    client.setOptions({kOptionClipboardSharing, uint32_t(enabled), kOptionClipboardSharingSize, uint32_t(sizeKiB)});

    client.setClipboard(kClipboardClipboard, &clipboard);
    client.grabClipboard(kClipboardClipboard);

    const int expectedUpdates = enabled && sizeKiB > 0 ? 1 : 0;
    QCOMPARE(platform->clipboardWrites, expectedUpdates);
    QCOMPARE(platform->clipboardGrabs, expectedUpdates);
    QCOMPARE(client.getMaximumClipboardReceiveSizeBytes(), size_t(sizeKiB) * 1024);
    if (expectedUpdates) {
      QCOMPARE(readClipboard(platform->clipboards[kClipboardClipboard]), std::string("Remote 中文"));
      QCOMPARE(
          readClipboard(platform->clipboards[kClipboardClipboard], IClipboard::Format::HTML),
          std::string("<b>Remote 中文</b>")
      );
    }

    client.setOptions({kOptionClipboardSharing, 0});
    client.setClipboard(kClipboardClipboard, &clipboard);
    client.grabClipboard(kClipboardClipboard);
    QCOMPARE(platform->clipboardWrites, expectedUpdates);
    QCOMPARE(platform->clipboardGrabs, expectedUpdates);
  }

  void reconnectStaysOnLocalUntilAnotherCrossing()
  {
    Fixture test;
    test.switchTo("mac");
    test.events.addEvent(Event(EventTypes::ClientProxyDisconnected, test.remote));
    QVERIFY(test.platform->entered);
    test.remote = new TestRemote(&test.events);
    test.server->adoptClient(test.remote);
    QVERIFY(test.platform->entered);
    QCOMPARE(test.remote->entries, 0);
  }

  void disconnectedPendingDestinationCancelsQueuedSwitchTimer()
  {
    Fixture test;
    test.config.addOption("", kOptionComputerSwitchDelay, 100);
    QVERIFY(test.server->setConfig(test.config));
    test.events.addEvent(
        Event(EventTypes::PrimaryComputerMotionOnPrimary, test.platform, IPrimaryComputer::MotionInfo::alloc(799, 300))
    );
    QVERIFY(test.platform->entered);
    QCOMPARE(test.remote->entries, 0);
    test.events.addEvent(Event(EventTypes::ClientProxyDisconnected, test.remote));
    // Timer events already delivered to the queue can outlive timer deletion.
    QVERIFY(test.events.dispatchEvent(Event(EventTypes::Timer, test.server.get())));
    QVERIFY(test.platform->entered);
    test.remote = new TestRemote(&test.events);
    test.server->adoptClient(test.remote);
    QVERIFY(test.events.dispatchEvent(Event(EventTypes::Timer, test.server.get())));
    QVERIFY(test.platform->entered);
    QCOMPARE(test.remote->entries, 0);
  }

private:
  Arch arch;
  Log log;
  TestAppUtil appUtil;
  QTemporaryDir directory;
};

QTEST_GUILESS_MAIN(ServerSafetyTests)
#include "ServerSafetyTests.moc"
