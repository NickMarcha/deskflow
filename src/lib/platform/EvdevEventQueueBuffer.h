/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2024 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2022 Red Hat, Inc.
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "base/IEventQueueBuffer.h"
#include "platform/EvdevComputer.h"

#include <mutex>
#include <queue>

namespace deskflow {

//! Event queue buffer that wakes on a single fd (the libinput fd)
class EvdevEventQueueBuffer : public IEventQueueBuffer
{
public:
  EvdevEventQueueBuffer(int fd, IEventQueue *events);
  ~EvdevEventQueueBuffer() override;

  // IEventQueueBuffer overrides
  void init() override
  {
    // do nothing
  }
  void waitForEvent(double msTimeout) override;
  Type getEvent(Event &event, uint32_t &dataID) override;
  bool addEvent(uint32_t dataID) override;
  bool isEmpty() const override;

private:
  int m_fd;
  IEventQueue *m_events;
  std::queue<std::pair<bool, uint32_t>> m_queue;
  int m_pipeWrite;
  int m_pipeRead;

  mutable std::mutex m_mutex;
};

} // namespace deskflow
