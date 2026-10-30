/*
 * SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "arch/Arch.h"
#include "base/EventQueue.h"
#include "base/Log.h"
#include "common/InputDiagnostics.h"
#include "deskflow/PacketStreamFilter.h"
#include "io/IStream.h"

#include <QTest>

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

using deskflow::InputDiagnostics;

namespace {

QByteArray frame(const QByteArray &payload)
{
  const auto size = static_cast<uint32_t>(payload.size());
  QByteArray bytes;
  for (const auto shift : {24, 16, 8, 0})
    bytes.append(static_cast<char>((size >> shift) & 0xff));
  return bytes + payload;
}

class FakeStream : public deskflow::IStream
{
public:
  struct Chunk
  {
    QByteArray bytes;
    uint64_t timestamp;
  };

  void push(QByteArray bytes, uint64_t timestamp)
  {
    m_chunks.push_back({std::move(bytes), timestamp});
  }
  void close() override
  {
    m_chunks.clear();
    m_closed = true;
  }
  uint32_t read(void *buffer, uint32_t count) override
  {
    if (m_closed || m_chunks.empty() || count == 0)
      return 0;
    auto &chunk = m_chunks.front();
    const auto size = std::min(count, static_cast<uint32_t>(chunk.bytes.size()));
    if (buffer)
      std::memcpy(buffer, chunk.bytes.constData(), size);
    m_lastRead = chunk.timestamp;
    chunk.bytes.remove(0, size);
    if (chunk.bytes.isEmpty())
      m_chunks.pop_front();
    return size;
  }
  uint64_t lastReadTimestamp() const override
  {
    return m_lastRead;
  }
  void write(const void *buffer, uint32_t count) override
  {
    writes.push_back({QByteArray(static_cast<const char *>(buffer), count), InputDiagnostics::sendOrigin()});
  }
  void flush() override
  {
  }
  void shutdownInput() override
  {
    close();
  }
  void shutdownOutput() override
  {
  }
  void *getEventTarget() const override
  {
    return const_cast<FakeStream *>(this);
  }
  bool isReady() const override
  {
    return !m_closed && !m_chunks.empty();
  }
  uint32_t getSize() const override
  {
    uint32_t size = 0;
    for (const auto &chunk : m_chunks)
      size += static_cast<uint32_t>(chunk.bytes.size());
    return size;
  }

  std::vector<Chunk> writes;

private:
  std::deque<Chunk> m_chunks;
  uint64_t m_lastRead = 0;
  bool m_closed = false;
};

class RecordingEventQueue : public IEventQueue
{
public:
  ~RecordingEventQueue() override
  {
    for (const auto &event : m_events)
      Event::deleteData(event);
  }
  int loop() override
  {
    return 0;
  }
  void adoptBuffer(IEventQueueBuffer *) override
  {
  }
  bool getEvent(Event &, double = -1.0) override
  {
    return false;
  }
  bool dispatchEvent(const Event &event) override
  {
    auto handler = m_handlers.find({event.getType(), event.getTarget()});
    if (handler == m_handlers.end())
      handler = m_handlers.find({EventTypes::Unknown, event.getTarget()});
    if (handler == m_handlers.end())
      return false;
    const auto callback = handler->second;
    callback(event);
    return true;
  }
  void addEvent(Event &&event) override
  {
    m_events.emplace_back(std::move(event));
  }
  EventQueueTimer *newTimer(double, void *) override
  {
    return nullptr;
  }
  EventQueueTimer *newOneShotTimer(double, void *) override
  {
    return nullptr;
  }
  void deleteTimer(EventQueueTimer *) override
  {
  }
  void addHandler(EventTypes type, void *target, const EventHandler &handler) override
  {
    m_handlers[{type, target}] = handler;
  }
  void removeHandler(EventTypes type, void *target) override
  {
    m_handlers.erase({type, target});
  }
  void removeHandlers(void *target) override
  {
    std::erase_if(m_handlers, [target](const auto &entry) { return entry.first.target == target; });
  }
  void waitForReady() const override
  {
  }
  void *getSystemTarget() override
  {
    return this;
  }

private:
  struct Key
  {
    EventTypes type;
    void *target;
    bool operator<(const Key &other) const
    {
      if (type != other.type)
        return static_cast<uint32_t>(type) < static_cast<uint32_t>(other.type);
      return std::less<void *>{}(target, other.target);
    }
  };
  std::map<Key, EventHandler> m_handlers;
  std::vector<Event> m_events;
};

} // namespace

class PacketStreamFilterTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase()
  {
    m_arch.init();
  }
  void init()
  {
    InputDiagnostics::configure(true);
  }
  void cleanup()
  {
    InputDiagnostics::configure(false);
  }

  void fragmentedHeaderAndPayloadKeepFirstByteTimestamp()
  {
    RecordingEventQueue events;
    FakeStream stream;
    PacketStreamFilter filter(&events, &stream, false);
    const auto bytes = frame("DMOV");

    stream.push(bytes.first(2), 100);
    QVERIFY(events.dispatchEvent(Event(EventTypes::StreamInputReady, stream.getEventTarget())));
    QVERIFY(!filter.isReady());
    QCOMPARE(filter.getSize(), uint32_t{0});

    stream.push(bytes.mid(2, 4), 200);
    QVERIFY(events.dispatchEvent(Event(EventTypes::StreamInputReady, stream.getEventTarget())));
    QVERIFY(!filter.isReady());
    char buffer[8]{};
    QCOMPARE(filter.read(buffer, sizeof(buffer)), uint32_t{0});

    stream.push(bytes.mid(6), 300);
    QVERIFY(events.dispatchEvent(Event(EventTypes::StreamInputReady, stream.getEventTarget())));
    QVERIFY(filter.isReady());
    QCOMPARE(filter.read(buffer, 2), uint32_t{2});
    QCOMPARE(QByteArray(buffer, 2), QByteArray("DM"));
    QCOMPARE(filter.lastReadTimestamp(), uint64_t{100});
    QCOMPARE(filter.read(buffer, sizeof(buffer)), uint32_t{2});
    QCOMPARE(QByteArray(buffer, 2), QByteArray("OV"));
    QCOMPARE(filter.lastReadTimestamp(), uint64_t{100});
    QVERIFY(!filter.isReady());
  }

  void batchedPacketsAndSplitNextHeaderKeepIndependentTimestamps()
  {
    RecordingEventQueue events;
    FakeStream stream;
    PacketStreamFilter filter(&events, &stream, false);
    const auto first = frame("DKDN");
    const auto second = frame("DKUP");
    const auto third = frame("DMMV");
    // One transport read completes packet one and begins packet two's header.
    stream.push(first + second.first(2), 100);
    stream.push(second.mid(2) + third, 200);
    QVERIFY(events.dispatchEvent(Event(EventTypes::StreamInputReady, stream.getEventTarget())));

    char buffer[64]{};
    QCOMPARE(filter.read(buffer, sizeof(buffer)), uint32_t{4});
    QCOMPARE(QByteArray(buffer, 4), QByteArray("DKDN"));
    QCOMPARE(filter.lastReadTimestamp(), uint64_t{100});
    QCOMPARE(filter.read(buffer, sizeof(buffer)), uint32_t{4});
    QCOMPARE(QByteArray(buffer, 4), QByteArray("DKUP"));
    QCOMPARE(filter.lastReadTimestamp(), uint64_t{100});
    QCOMPARE(filter.read(buffer, sizeof(buffer)), uint32_t{4});
    QCOMPARE(QByteArray(buffer, 4), QByteArray("DMMV"));
    QCOMPARE(filter.lastReadTimestamp(), uint64_t{200});
    QVERIFY(!filter.isReady());
  }

  void discardedPayloadDoesNotShiftNextPacketTimestamp()
  {
    RecordingEventQueue events;
    FakeStream stream;
    PacketStreamFilter filter(&events, &stream, false);
    stream.push(frame("DISCARD"), 100);
    stream.push(frame("KEEP"), 200);
    QVERIFY(events.dispatchEvent(Event(EventTypes::StreamInputReady, stream.getEventTarget())));
    QCOMPARE(filter.read(nullptr, 3), uint32_t{3});
    QCOMPARE(filter.lastReadTimestamp(), uint64_t{100});
    QCOMPARE(filter.read(nullptr, 64), uint32_t{4});
    char buffer[8]{};
    QCOMPARE(filter.read(buffer, sizeof(buffer)), uint32_t{4});
    QCOMPARE(QByteArray(buffer, 4), QByteArray("KEEP"));
    QCOMPARE(filter.lastReadTimestamp(), uint64_t{200});
  }

  void closingAndShuttingDownInputDiscardBufferedBytesAndTiming_data()
  {
    QTest::addColumn<bool>("shutdown");
    QTest::newRow("close") << false;
    QTest::newRow("shutdownInput") << true;
  }

  void closingAndShuttingDownInputDiscardBufferedBytesAndTiming()
  {
    QFETCH(bool, shutdown);
    RecordingEventQueue events;
    FakeStream stream;
    PacketStreamFilter filter(&events, &stream, false);
    stream.push(frame("FIRST") + frame("SECOND"), 100);
    QVERIFY(events.dispatchEvent(Event(EventTypes::StreamInputReady, stream.getEventTarget())));
    QCOMPARE(filter.read(nullptr, 2), uint32_t{2});
    QCOMPARE(filter.lastReadTimestamp(), uint64_t{100});
    if (shutdown)
      filter.shutdownInput();
    else
      filter.close();
    QVERIFY(!filter.isReady());
    QCOMPARE(filter.getSize(), uint32_t{0});
    QCOMPARE(filter.lastReadTimestamp(), uint64_t{0});
    QCOMPARE(filter.read(nullptr, 64), uint32_t{0});
  }

  void outgoingPrefixHasNoTimingAndWireBytesAreUnchanged()
  {
    RecordingEventQueue events;
    FakeStream stream;
    PacketStreamFilter filter(&events, &stream, false);
    const auto payload = QByteArray::fromHex("444d4f56000000ff");
    {
      InputDiagnostics::SendScope input(123456);
      filter.write(payload.constData(), static_cast<uint32_t>(payload.size()));
      QCOMPARE(InputDiagnostics::sendOrigin(), uint64_t{123456});
    }
    QCOMPARE(stream.writes.size(), size_t{2});
    QCOMPARE(stream.writes[0].timestamp, uint64_t{0});
    QCOMPARE(stream.writes[1].timestamp, uint64_t{123456});
    QCOMPARE(stream.writes[0].bytes, QByteArray::fromHex("00000008"));
    QCOMPARE(stream.writes[1].bytes, payload);
    QCOMPARE(stream.writes[0].bytes + stream.writes[1].bytes, frame(payload));
    QCOMPARE(InputDiagnostics::sendOrigin(), uint64_t{0});
  }

  void diagnosticsDisabledDoesNotChangePacketBytes()
  {
    InputDiagnostics::configure(false);
    RecordingEventQueue events;
    FakeStream stream;
    PacketStreamFilter filter(&events, &stream, false);
    const auto payload = QByteArray::fromHex("444b444eff00");
    stream.push(frame(payload), 100);
    QVERIFY(events.dispatchEvent(Event(EventTypes::StreamInputReady, stream.getEventTarget())));
    char buffer[8]{};
    QCOMPARE(filter.read(buffer, sizeof(buffer)), uint32_t{6});
    QCOMPARE(QByteArray(buffer, 6), payload);
    QCOMPARE(filter.lastReadTimestamp(), uint64_t{0});
    filter.write(payload.constData(), static_cast<uint32_t>(payload.size()));
    QCOMPARE(stream.writes[0].bytes + stream.writes[1].bytes, frame(payload));
    QCOMPARE(stream.writes[0].timestamp, uint64_t{0});
    QCOMPARE(stream.writes[1].timestamp, uint64_t{0});
  }

  void transportFloodYieldsWithoutDroppingPackets()
  {
    RecordingEventQueue events;
    FakeStream stream;
    PacketStreamFilter filter(&events, &stream, false);
    const auto packet = frame("DMOV");
    stream.push(packet.repeated(10000), 100);
    QVERIFY(events.dispatchEvent(Event(EventTypes::StreamInputReady, stream.getEventTarget())));
    QVERIFY(stream.isReady()); // One dispatch must leave work for a later turn.
    int packets = 0;
    while (filter.isReady()) {
      QCOMPARE(filter.read(nullptr, 4), uint32_t(4));
      ++packets;
    }
    QVERIFY(events.dispatchEvent(Event(EventTypes::StreamInputReady, stream.getEventTarget())));
    while (filter.isReady()) {
      QCOMPARE(filter.read(nullptr, 4), uint32_t(4));
      ++packets;
    }
    QCOMPARE(packets, 10000);
    QVERIFY(!stream.isReady());
  }

  void eventQueuePreservesCaptureThroughPendingQueueAndDerivedEvent()
  {
    EventQueue events;
    std::vector<uint64_t> observed;
    bool timedOut = false;
    auto *timeout = events.newOneShotTimer(1.0, this);
    events.addHandler(EventTypes::Timer, this, [&](const Event &) {
      timedOut = true;
      events.addEvent(Event(EventTypes::Quit));
    });
    events.addHandler(EventTypes::KeyStateKeyDown, this, [&](const Event &event) {
      observed.push_back(event.captureTimestamp());
      observed.push_back(InputDiagnostics::sendOrigin());
      // This derived event enters the running queue under the original event's scope.
      events.addEvent(Event(EventTypes::KeyStateKeyUp, this));
    });
    events.addHandler(EventTypes::KeyStateKeyUp, this, [&](const Event &event) {
      observed.push_back(event.captureTimestamp());
      observed.push_back(InputDiagnostics::sendOrigin());
      events.addEvent(Event(EventTypes::Quit));
    });
    {
      InputDiagnostics::SendScope capture(987654321);
      // This event waits in the pre-loop pending queue before it reaches a handler.
      events.addEvent(Event(EventTypes::KeyStateKeyDown, this));
    }
    QCOMPARE(events.loop(), 0);
    events.deleteTimer(timeout);
    QVERIFY(!timedOut);
    QCOMPARE(observed.size(), size_t{4});
    for (const auto timestamp : observed)
      QCOMPARE(timestamp, uint64_t{987654321});
    QCOMPARE(InputDiagnostics::sendOrigin(), uint64_t{0});
  }

private:
  Arch m_arch;
  Log m_log;
};

QTEST_GUILESS_MAIN(PacketStreamFilterTests)
#include "PacketStreamFilterTests.moc"
