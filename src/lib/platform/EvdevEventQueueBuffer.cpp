/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2024 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2022 Red Hat, Inc.
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/EvdevEventQueueBuffer.h"

#include "base/Event.h"
#include "base/EventTypes.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "mt/Thread.h"

#include <cassert>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace deskflow {

EvdevEventQueueBuffer::EvdevEventQueueBuffer(int fd, IEventQueue *events) : m_fd{fd}, m_events{events}
{
  // We need a pipe to signal ourselves when addEvent() is called
  int pipefd[2];
  int result = pipe2(pipefd, O_NONBLOCK);
  assert(result == 0);

  m_pipeRead = pipefd[0];
  m_pipeWrite = pipefd[1];
}

EvdevEventQueueBuffer::~EvdevEventQueueBuffer()
{
  close(m_pipeRead);
  close(m_pipeWrite);
}

void EvdevEventQueueBuffer::waitForEvent(double msTimeout)
{
  Thread::testCancel();

  static const auto s_inputFd = 0;
  static const auto s_pipeFd = 1;
  static const auto s_pollFdCount = 2;

  struct pollfd pfds[s_pollFdCount];
  pfds[s_inputFd].fd = m_fd;
  pfds[s_inputFd].events = POLLIN;
  pfds[s_pipeFd].fd = m_pipeRead;
  pfds[s_pipeFd].events = POLLIN;

  int timeout = (msTimeout < 0.0) ? -1 : static_cast<int>(1000.0 * msTimeout);

  if (int retval = poll(pfds, s_pollFdCount, timeout); retval > 0) {
    if (pfds[s_inputFd].revents & POLLIN) {
      std::scoped_lock lock{m_mutex};

      // Push a data-less system event; EvdevComputer::handleSystemEvent()
      // then calls libinput_dispatch() and drains every pending event.
      m_queue.push({true, 0U});
    }
    // the pipefd data doesn't matter, it only exists to wake up the thread
    // and potentially testCancel
    if (pfds[s_pipeFd].revents & POLLIN) {
      char buf[64];
      ssize_t total = 0;
      ssize_t result;
      while ((result = read(m_pipeRead, buf, sizeof(buf)) > 0)) {
        total += result;
      }
      LOG_VERBOSE("event queue read result: %d (total drained: %zd)", result, total);
    }
  }
  Thread::testCancel();
}

IEventQueueBuffer::Type EvdevEventQueueBuffer::getEvent(Event &event, uint32_t &dataID)
{
  // libinput events are not queued here: a system event only means "the fd
  // is readable" and EvdevComputer drains libinput itself.
  std::scoped_lock lock{m_mutex};
  auto pair = m_queue.front();
  m_queue.pop();

  // if this an injected special event, just return the data and exit
  if (pair.first == false) {
    dataID = pair.second;
    return IEventQueueBuffer::Type::User;
  }

  event = Event(EventTypes::System, m_events->getSystemTarget());

  return IEventQueueBuffer::Type::System;
}

bool EvdevEventQueueBuffer::addEvent(uint32_t dataID)
{
  std::scoped_lock lock{m_mutex};
  m_queue.push({false, dataID});

  // tickle the pipe so our read thread wakes up
  auto result = write(m_pipeWrite, "!", 1);
  LOG_VERBOSE("event queue write result: %d", result);

  return true;
}

bool EvdevEventQueueBuffer::isEmpty() const
{
  std::scoped_lock lock{m_mutex};

  return m_queue.empty();
}

} // namespace deskflow
