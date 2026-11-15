/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace deskflow {

//! Reads Logitech controls that never reach evdev, over HID++ 2.0.
/*!
Buttons such as the MX Master's thumb (gesture) button and wheel-mode button
are only reported to software that asks for them: the host has to "divert"
them through the REPROG_CONTROLS_V4 feature (0x1b04) on the receiver's
hidraw node, and then reads press notifications from it.

This class does that on its own thread. It diverts a control only while a
binding needs it, or while the web UI waits for a press to identify, so an
unbound control keeps its built-in behaviour. Diversion is volatile, so it is
redone when the receiver reports that the device reconnected.

Presses are reported as code kCodeBase + CID on the device's HID++ name
(e.g. "MX Master 3S"), collected with take() on the event loop thread.
*/
class HidppListener
{
public:
  static constexpr std::uint32_t kCodeBase = 0x1000;

  struct Press
  {
    std::string device;
    std::uint32_t code;
    bool pressed;
  };

  //! \p wakeFd is an eventfd that gets written when presses are waiting.
  //! \p identifyPath is the web UI's identify file (empty to disable).
  HidppListener(int wakeFd, std::string identifyPath);
  ~HidppListener();
  HidppListener(const HidppListener &) = delete;
  HidppListener &operator=(const HidppListener &) = delete;

  //! (device name, code) pairs that have bindings; device "" means any.
  void setBound(std::set<std::pair<std::string, std::uint32_t>> bound);

  std::vector<Press> take();

private:
  struct Device
  {
    std::string name;
    std::uint8_t feature = 0; // index of 0x1b04
    std::vector<std::uint16_t> controls; // divertable, not plain mouse buttons
    std::set<std::uint16_t> diverted;
    std::set<std::uint16_t> pressed;
  };
  struct Node
  {
    int fd = -1;
    std::map<std::uint8_t, Device> devices; // by device index 1..6
    std::set<std::uint8_t> reprobe;
    std::set<std::uint8_t> unanswered; // slots that didn't answer (device asleep or off)
  };

  void run();
  void scan();
  //! false if the slot didn't answer, so it is probed again later
  bool probe(Node &node, std::uint8_t index);
  void enableNotifications(Node &node);
  void applyDiversion(Node &node);
  bool wanted(const Device &dev, std::uint16_t cid) const;
  bool request(
      Node &node, std::uint8_t dev, std::uint8_t feature, std::uint8_t function, const std::vector<std::uint8_t> &params,
      std::vector<std::uint8_t> &reply
  );
  void handleFrame(Node &node, const std::uint8_t *data, std::size_t len);
  void emit(const std::string &device, std::uint16_t cid, bool pressed);
  bool identifyActive() const;

  int m_wakeFd;
  std::string m_identifyPath;
  int m_stopPipe[2] = {-1, -1};
  std::thread m_thread;

  std::map<std::string, Node> m_nodes; // by /dev/hidrawN, thread only
  bool m_identify = false;             // thread only
  std::uint8_t m_lastError = 0;        // HID++ error of the last failed request, 0 = no answer

  mutable std::mutex m_mutex; // guards the members below
  std::set<std::pair<std::string, std::uint32_t>> m_bound;
  std::atomic<std::uint32_t> m_boundGeneration{0};
  std::vector<Press> m_presses;
};

} // namespace deskflow
