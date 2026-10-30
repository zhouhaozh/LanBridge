/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "common/InputDiagnostics.h"
#include "io/StreamBuffer.h"
#include "io/StreamFilter.h"

#include <mutex>

class IEventQueue;

//! Packetizing stream filter
/*!
Filters a stream to read and write packets.
*/
class PacketStreamFilter : public StreamFilter
{
public:
  PacketStreamFilter(IEventQueue *events, deskflow::IStream *stream, bool adoptStream = true);
  ~PacketStreamFilter() override = default;

  // IStream overrides
  void close() override;
  uint32_t read(void *buffer, uint32_t n) override;
  uint64_t lastReadTimestamp() const override;
  void write(const void *buffer, uint32_t n) override;
  void shutdownInput() override;
  bool isReady() const override;
  uint32_t getSize() const override;

protected:
  // StreamFilter overrides
  void filterEvent(const Event &) override;

private:
  bool isReadyNoLock() const;
  bool readPacketSize();
  bool readMore();

private:
  mutable std::mutex m_mutex;
  uint32_t m_size = 0;
  StreamBuffer m_buffer;
  deskflow::TimedByteQueue m_bufferTimes;
  uint64_t m_packetTimestamp = 0;
  uint64_t m_lastReadTimestamp = 0;
  bool m_inputShutdown = false;
  IEventQueue *m_events = nullptr;
};
