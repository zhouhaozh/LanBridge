// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "common/InputDiagnostics.h"
#include <QJsonObject>
#include <QTest>
#include <vector>

using deskflow::InputDiagnostics;
using deskflow::TimedByteQueue;

class InputDiagnosticsTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void partialWritesDoNotFinishEarly()
  {
    TimedByteQueue queue;
    queue.append(4, 0); // Protocol length prefix is not an input sample.
    queue.append(12, 100);
    QCOMPARE(queue.consume(8), uint64_t(100));
    QCOMPARE(queue.size(), uint64_t(8));
    std::vector<uint64_t> finished;
    queue.consume(7, [&](uint64_t start) { finished.push_back(start); });
    QVERIFY(finished.empty());
    queue.consume(1, [&](uint64_t start) { finished.push_back(start); });
    QCOMPARE(finished.size(), size_t(1));
    QCOMPARE(finished.front(), uint64_t(100));
  }
  void batchingKeepsOldestTimestamp()
  {
    TimedByteQueue queue;
    queue.append(3, 100);
    queue.append(5, 300);
    QCOMPARE(queue.consume(6), uint64_t(100));
    QCOMPARE(queue.consume(2), uint64_t(300));
    QCOMPARE(queue.size(), uint64_t(0));
  }
  void disconnectDiscardsOutstandingSamples()
  {
    TimedByteQueue queue;
    queue.append(4, 0);
    queue.append(12, 100);
    QCOMPARE(queue.clear(), uint64_t(1));
    QCOMPARE(queue.size(), uint64_t(0));
    QCOMPARE(queue.consume(12), uint64_t(0));
  }
  void scopedOriginsAreRestored()
  {
    InputDiagnostics::configure(true);
    {
      InputDiagnostics::SendScope outer(100);
      QCOMPARE(InputDiagnostics::sendOrigin(), uint64_t(100));
      {
        InputDiagnostics::SendScope inner(200);
        QCOMPARE(InputDiagnostics::sendOrigin(), uint64_t(200));
      }
      QCOMPARE(InputDiagnostics::sendOrigin(), uint64_t(100));
    }
    QCOMPARE(InputDiagnostics::sendOrigin(), uint64_t(0));
    InputDiagnostics::configure(false);
  }
  void percentilesIncludeLongQueueWaits()
  {
    InputDiagnostics::configure(true);
    for (int i = 1; i <= 100; ++i)
      InputDiagnostics::record(InputDiagnostics::Stage::CaptureToSend, 1, 1 + uint64_t(i) * 1000000);
    const auto snapshot = InputDiagnostics::snapshot();
    const auto sent = snapshot.value("capture_to_send").toObject();
    QCOMPARE(sent.value("samples").toInteger(), qint64(100));
    QCOMPARE(sent.value("p50_ms").toDouble(), 50.0);
    QCOMPARE(sent.value("p95_ms").toDouble(), 95.0);
    QCOMPARE(sent.value("p99_ms").toDouble(), 99.0);
    QVERIFY(!sent.contains("key"));
    InputDiagnostics::configure(false);
  }
  void disabledDoesNotCollect()
  {
    InputDiagnostics::configure(false);
    InputDiagnostics::record(InputDiagnostics::Stage::CaptureToSend, 1, 1000);
    QCOMPARE(InputDiagnostics::snapshot().value("capture_to_send").toObject().value("samples").toInteger(), qint64(0));
  }
};

QTEST_GUILESS_MAIN(InputDiagnosticsTests)
#include "InputDiagnosticsTests.moc"
