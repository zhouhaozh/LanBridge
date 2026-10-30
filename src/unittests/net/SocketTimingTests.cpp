// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "arch/Arch.h"
#include "base/EventQueue.h"
#include "base/Log.h"
#include "common/InputDiagnostics.h"
#include "mt/Lock.h"
#include "net/SocketMultiplexer.h"
#include "net/TCPSocket.h"
#include <QJsonObject>
#include <QTest>

class BufferedSocket : public TCPSocket
{
public:
  BufferedSocket(IEventQueue *events, SocketMultiplexer *mux) : TCPSocket(events, mux)
  {
    setReadable(true);
    setWritable(true);
  }
  ISocketMultiplexerJob *newJob() override
  {
    return nullptr;
  }
  void receive(const QByteArray &bytes, uint64_t timestamp)
  {
    Lock lock(&getMutex());
    m_inputBuffer.write(bytes.constData(), bytes.size());
    m_inputTimes.append(bytes.size(), timestamp);
  }
  void acceptBytes(int bytes)
  {
    Lock lock(&getMutex());
    discardWrittenData(bytes);
  }
};

class SocketTimingTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase()
  {
    m_arch.init();
  }
  void init()
  {
    deskflow::InputDiagnostics::configure(true);
  }
  void cleanup()
  {
    deskflow::InputDiagnostics::configure(false);
  }
  void aggregatedReceiveRetainsArrivalBoundaries()
  {
    EventQueue events;
    SocketMultiplexer mux;
    BufferedSocket socket(&events, &mux);
    socket.receive("first", 100);
    socket.receive("second", 200);
    char buffer[4096]{};
    QCOMPARE(socket.read(buffer, sizeof(buffer)), uint32_t(5));
    QCOMPARE(QByteArray(buffer, 5), QByteArray("first"));
    QCOMPARE(socket.lastReadTimestamp(), uint64_t(100));
    QCOMPARE(socket.read(buffer, sizeof(buffer)), uint32_t(6));
    QCOMPARE(QByteArray(buffer, 6), QByteArray("second"));
    QCOMPARE(socket.lastReadTimestamp(), uint64_t(200));
    socket.close();
    QCOMPARE(socket.read(buffer, sizeof(buffer)), uint32_t(0));
    QCOMPARE(socket.lastReadTimestamp(), uint64_t(0));
  }
  void queuedAndPartiallyAcceptedOutputDoesNotCountAsSent()
  {
    using D = deskflow::InputDiagnostics;
    EventQueue events;
    SocketMultiplexer mux;
    BufferedSocket socket(&events, &mux);
    {
      D::SendScope origin(D::now());
      socket.write("payload", 7);
    }
    auto samples = [] { return D::snapshot().value("capture_to_send").toObject().value("samples").toInteger(); };
    QCOMPARE(samples(), qint64(0));
    socket.acceptBytes(6);
    QCOMPARE(samples(), qint64(0));
    socket.acceptBytes(1);
    QCOMPARE(samples(), qint64(1));
    {
      D::SendScope origin(D::now());
      socket.write("discard", 7);
    }
    socket.close();
    QCOMPARE(samples(), qint64(1));
    QCOMPARE(D::snapshot().value("discarded_timed_writes").toInteger(), qint64(1));
  }

private:
  Arch m_arch;
  Log m_log;
};
QTEST_GUILESS_MAIN(SocketTimingTests)
#include "SocketTimingTests.moc"
