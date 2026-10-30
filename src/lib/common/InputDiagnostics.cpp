// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "InputDiagnostics.h"
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

namespace deskflow {
namespace {
std::atomic_bool s_enabled = false;
thread_local uint64_t s_sendOrigin = 0;
thread_local uint64_t s_receiveOrigin = 0;
std::mutex s_mutex;
constexpr uint64_t bucketNs = 100000;
constexpr size_t bucketCount = 10001;
struct Histogram
{
  std::array<uint64_t, bucketCount> buckets{};
  uint64_t samples = 0;
  uint64_t maximum = 0;
  void add(uint64_t elapsed)
  {
    ++buckets[std::min<uint64_t>((elapsed + bucketNs - 1) / bucketNs, bucketCount - 1)];
    ++samples;
    maximum = std::max(maximum, elapsed);
  }
  double percentile(double quantile) const
  {
    if (!samples)
      return 0;
    const auto target = static_cast<uint64_t>(std::ceil(samples * quantile));
    uint64_t count = 0;
    for (size_t i = 0; i < bucketCount; ++i) {
      count += buckets[i];
      if (count >= target)
        return (i == bucketCount - 1 ? maximum : i * bucketNs) / 1000000.0;
    }
    return maximum / 1000000.0;
  }
  QJsonObject json() const
  {
    return {
        {"samples", static_cast<double>(samples)},
        {"p50_ms", percentile(.50)},
        {"p95_ms", percentile(.95)},
        {"p99_ms", percentile(.99)},
        {"max_ms", maximum / 1000000.0},
        {"percentile_resolution_ms", .1},
        {"overflow_bucket_samples", static_cast<double>(buckets.back())}
    };
  }
};
Histogram s_send;
Histogram s_receive;
uint64_t s_queuePeak = 0;
uint64_t s_discarded = 0;
uint64_t s_coalesced = 0;
QDateTime s_startedUtc;
std::chrono::steady_clock::time_point s_started;
} // namespace

void InputDiagnostics::configure(bool enabled)
{
  s_enabled.store(false, std::memory_order_relaxed);
  std::scoped_lock lock(s_mutex);
  s_send = {};
  s_receive = {};
  s_queuePeak = s_discarded = s_coalesced = 0;
  s_startedUtc = QDateTime::currentDateTimeUtc();
  s_started = std::chrono::steady_clock::now();
  s_enabled.store(enabled, std::memory_order_relaxed);
}
bool InputDiagnostics::enabled()
{
  return s_enabled.load(std::memory_order_relaxed);
}
uint64_t InputDiagnostics::now()
{
  return enabled()
             ? std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                   .count()
             : 0;
}
uint64_t InputDiagnostics::sendOrigin()
{
  return s_sendOrigin;
}
uint64_t InputDiagnostics::receiveOrigin()
{
  return s_receiveOrigin;
}
InputDiagnostics::SendScope::SendScope(uint64_t origin) : m_previous(s_sendOrigin)
{
  s_sendOrigin = origin;
}
InputDiagnostics::SendScope::~SendScope()
{
  s_sendOrigin = m_previous;
}
InputDiagnostics::ReceiveScope::ReceiveScope(uint64_t origin) : m_previous(s_receiveOrigin)
{
  s_receiveOrigin = origin;
}
InputDiagnostics::ReceiveScope::~ReceiveScope()
{
  s_receiveOrigin = m_previous;
}
void InputDiagnostics::record(Stage stage, uint64_t start, uint64_t finish)
{
  if (!enabled() || !start)
    return;
  if (!finish)
    finish = now();
  if (finish < start)
    return;
  std::scoped_lock lock(s_mutex);
  (stage == Stage::CaptureToSend ? s_send : s_receive).add(finish - start);
}
void InputDiagnostics::observeQueue(uint64_t bytes)
{
  if (!enabled())
    return;
  std::scoped_lock lock(s_mutex);
  s_queuePeak = std::max(s_queuePeak, bytes);
}
void InputDiagnostics::discarded(uint64_t count)
{
  if (!enabled() || !count)
    return;
  std::scoped_lock lock(s_mutex);
  s_discarded += count;
}
void InputDiagnostics::coalesced()
{
  if (!enabled())
    return;
  std::scoped_lock lock(s_mutex);
  ++s_coalesced;
}
QJsonObject InputDiagnostics::snapshot()
{
  std::scoped_lock lock(s_mutex);
  return {
      {"format_version", 1},
      {"enabled", enabled()},
      {"updated_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
      {"session_started_utc", s_startedUtc.toString(Qt::ISODateWithMs)},
      {"duration_ms",
       static_cast<double>(
           std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s_started).count()
       )},
      {"capture_to_send", s_send.json()},
      {"receive_to_inject", s_receive.json()},
      {"socket_queue_peak_bytes", static_cast<double>(s_queuePeak)},
      {"discarded_timed_writes", static_cast<double>(s_discarded)},
      {"coalesced_mouse_messages", static_cast<double>(s_coalesced)},
      {"clock", "host-local monotonic"},
      {"scope", "local application queues; not network RTT or input-to-photon latency"}
  };
}
bool InputDiagnostics::save(const QString &path)
{
  if (!enabled())
    return false;
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return false;
  const auto bytes = QJsonDocument(snapshot()).toJson();
  return file.write(bytes) == bytes.size() && file.commit();
}
void TimedByteQueue::append(uint64_t bytes, uint64_t timestamp)
{
  if (!bytes)
    return;
  if (!m_segments.empty() && m_segments.back().timestamp == timestamp)
    m_segments.back().bytes += bytes;
  else
    m_segments.push_back({bytes, timestamp});
  m_size += bytes;
}
uint64_t TimedByteQueue::consume(uint64_t bytes, const std::function<void(uint64_t)> &completed)
{
  uint64_t oldest = 0;
  while (bytes && !m_segments.empty()) {
    auto &segment = m_segments.front();
    if (segment.timestamp && (!oldest || segment.timestamp < oldest))
      oldest = segment.timestamp;
    const auto used = std::min(bytes, segment.bytes);
    segment.bytes -= used;
    bytes -= used;
    m_size -= used;
    if (!segment.bytes) {
      if (completed && segment.timestamp)
        completed(segment.timestamp);
      m_segments.pop_front();
    }
  }
  return oldest;
}
uint64_t TimedByteQueue::clear()
{
  const auto timed =
      std::count_if(m_segments.begin(), m_segments.end(), [](const auto &s) { return s.timestamp != 0; });
  m_segments.clear();
  m_size = 0;
  return static_cast<uint64_t>(timed);
}
} // namespace deskflow
