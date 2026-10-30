/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "EventQueueTests.h"

#include "base/EventQueue.h"
#include "base/IEventQueueBuffer.h"

#include <QTest>

#include <memory>

namespace {
class BusyNativeBuffer : public IEventQueueBuffer
{
public:
  void init() override
  {
  }
  void waitForEvent(double) override
  {
  }
  Type getEvent(Event &event, uint32_t &) override
  {
    event = Event(EventTypes::System, this);
    return Type::System;
  }
  bool addEvent(uint32_t) override
  {
    return true;
  }
  bool isEmpty() const override
  {
    return false;
  }
};
} // namespace

void EventQueueTests::expiredTimerRunsBeforeContinuousNativeInput()
{
  EventQueue events;
  events.adoptBuffer(new BusyNativeBuffer);
  auto *deadline = events.newOneShotTimer(0.001, this);
  QTest::qSleep(5);
  Event event;
  QVERIFY(events.getEvent(event, 0));
  QCOMPARE(event.getType(), EventTypes::Timer);
  QCOMPARE(event.getTarget(), this);
  events.deleteTimer(deadline);
}

void EventQueueTests::initTestCase()
{
  m_arch.init();
}

void EventQueueTests::dispatchEvent_noHandler_returnsFalse()
{
  EventQueue events;

  QVERIFY(!events.dispatchEvent(Event(EventTypes::ClientDisconnected, this)));
}

void EventQueueTests::dispatchEvent_noTypeHandler_dispatchesUnknownHandler()
{
  EventQueue events;
  bool fallbackCalled = false;
  events.addHandler(EventTypes::Unknown, this, [&fallbackCalled](const Event &) { fallbackCalled = true; });

  QVERIFY(events.dispatchEvent(Event(EventTypes::ClientDisconnected, this)));
  QVERIFY(fallbackCalled);
}

void EventQueueTests::dispatchEvent_handlerRemovesItself_keepsHandlerAliveUntilReturn()
{
  EventQueue events;
  auto handlerLifetime = std::make_shared<int>(1);
  std::weak_ptr<int> handlerLifetimeObserver = handlerLifetime;
  bool handlerAliveAfterRemoval = false;

  events.addHandler(
      EventTypes::ClientDisconnected, this,
      [this, &events, &handlerLifetimeObserver, &handlerAliveAfterRemoval, handlerLifetime](const Event &) {
        events.removeHandler(EventTypes::ClientDisconnected, this);
        handlerAliveAfterRemoval = handlerLifetime != nullptr && !handlerLifetimeObserver.expired();
      }
  );
  handlerLifetime.reset();

  QVERIFY(events.dispatchEvent(Event(EventTypes::ClientDisconnected, this)));
  QVERIFY(handlerAliveAfterRemoval);
  QVERIFY(handlerLifetimeObserver.expired());
}

QTEST_MAIN(EventQueueTests)
