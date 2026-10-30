// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <cstdint>
#include <deque>
#include <functional>

class QJsonObject;
class QString;

namespace deskflow {
// Metadata only: never stores input payloads or remote clock values.
class InputDiagnostics
{
public:
  enum class Stage
  {
    CaptureToSend,
    ReceiveToInject
  };
  static void configure(bool enabled);
  static bool enabled();
  static uint64_t now();
  static uint64_t sendOrigin();
  static uint64_t receiveOrigin();
  static void record(Stage stage, uint64_t start, uint64_t finish = 0);
  static void observeQueue(uint64_t bytes);
  static void discarded(uint64_t count);
  static void coalesced();
  static QJsonObject snapshot();
  static bool save(const QString &path);
  class SendScope
  {
  public:
    explicit SendScope(uint64_t origin);
    ~SendScope();
    SendScope(const SendScope &) = delete;
    SendScope &operator=(const SendScope &) = delete;

  private:
    uint64_t m_previous;
  };
  class ReceiveScope
  {
  public:
    explicit ReceiveScope(uint64_t origin);
    ~ReceiveScope();
    ReceiveScope(const ReceiveScope &) = delete;
    ReceiveScope &operator=(const ReceiveScope &) = delete;

  private:
    uint64_t m_previous;
  };
};

// Byte-offset metadata follows partial I/O under the data buffer's own lock.
class TimedByteQueue
{
public:
  void append(uint64_t bytes, uint64_t timestamp);
  uint64_t consume(uint64_t bytes, const std::function<void(uint64_t)> &completed = {});
  uint64_t clear();
  uint64_t size() const
  {
    return m_size;
  }
  uint64_t frontSize() const
  {
    return m_segments.empty() ? 0 : m_segments.front().bytes;
  }

private:
  struct Segment
  {
    uint64_t bytes;
    uint64_t timestamp;
  };
  std::deque<Segment> m_segments;
  uint64_t m_size = 0;
};
} // namespace deskflow
