/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace deskflow {

//! Short HID++ 2.0 sessions with Logitech devices: button remaps and battery.
/*!
Unlike HidppListener this never keeps a receiver's HID++ node open. On a
Raspberry Pi 4 with a Logi Bolt receiver, an open node stops the mouse from
moving (#125), so each session opens the node, does a few requests and closes
it again. A session while the mouse is idle costs nothing noticeable.

- Remaps: a control (e.g. the MX Master's thumb button, CID 0xc3) is told to
  act as another one (e.g. forward, 0x56) in the device's own firmware, so it
  arrives through evdev like a normal button and bindings can use it. The
  device forgets this when it is switched off, so wake() should be called
  when its input resumes after a quiet spell; the remap is then re-checked.
- Battery: read on wake() and every 15 minutes while idle.

Everything known is published in the status file under "hidpp".
*/
class HidppManager
{
public:
  struct Remap
  {
    std::string device; // HID++ device name, e.g. "MX Master 3S"
    std::uint16_t control = 0;
    std::uint16_t to = 0;
  };

  HidppManager();
  ~HidppManager();
  HidppManager(const HidppManager &) = delete;
  HidppManager &operator=(const HidppManager &) = delete;

  void setRemaps(std::vector<Remap> remaps);
  //! Input from a Logitech device resumed after a quiet spell.
  void wake();
  //! A Logitech input device was plugged in.
  void rescan();
  //! Any input from a Logitech device (cheap; used to find idle moments).
  void noteInput()
  {
    m_lastInput = std::chrono::steady_clock::now().time_since_epoch().count();
  }

private:
  struct Control
  {
    std::uint16_t cid = 0;
    std::uint8_t group = 0;
    std::uint8_t groupMask = 0;
  };
  struct Device
  {
    std::string node;
    std::uint8_t index = 0;
    std::string name;
    std::uint8_t reprog = 0;   // feature index of 0x1b04
    std::uint8_t battery = 0;  // feature index of 0x1004 or 0x1000
    bool unifiedBattery = false;
    std::vector<Control> controls;
    std::map<std::uint16_t, std::uint16_t> remapped; // cid -> target, as set by us
    int batteryPercent = -1;
    std::string batteryState;
    std::string batteryAt;
    std::chrono::steady_clock::time_point batteryRead{};
  };

  void run();
  void scan(int fdHint = -1);
  //! false if a wanted remap could not be checked (device asleep or reconnecting)
  bool session(bool checkBattery);
  bool request(
      int fd, std::uint8_t dev, std::uint8_t feature, std::uint8_t function, const std::vector<std::uint8_t> &params,
      std::vector<std::uint8_t> &reply, int timeoutMs = 400
  ) const;
  std::uint8_t featureIndex(int fd, std::uint8_t dev, std::uint16_t feature) const;
  //! false if the slot didn't answer (device asleep or off), so it is probed again later
  bool probe(int fd, const std::string &node, std::uint8_t index);
  void reprobe();
  void readBattery(int fd, Device &dev);
  bool applyRemaps(int fd, Device &dev, const std::vector<Remap> &remaps);
  void publish() const;
  bool idleFor(std::chrono::seconds s) const;

  std::thread m_thread;
  std::mutex m_mutex;
  std::condition_variable m_cv;
  bool m_stop = false;
  bool m_wantScan = true;
  bool m_wantCheck = false;
  std::vector<Remap> m_remaps;
  std::atomic<std::int64_t> m_lastInput{0};

  std::vector<Device> m_devices; // thread only
  std::set<std::pair<std::string, std::uint8_t>> m_unanswered; // (node, slot), thread only
  mutable std::uint8_t m_lastError = 0; // HID++ error code of the last failed request, 0 = no answer
};

} // namespace deskflow
