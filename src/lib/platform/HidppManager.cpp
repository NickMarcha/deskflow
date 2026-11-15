/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/HidppManager.h"

#include "base/Log.h"
#include "common/HeadlessStatus.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <poll.h>
#include <set>
#include <unistd.h>

namespace deskflow {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint8_t kLongReport = 0x11;
constexpr std::size_t kLongLength = 20;
constexpr std::uint8_t kSoftwareId = 0x0C;
constexpr std::uint16_t kDeviceName = 0x0005;
constexpr std::uint16_t kBatteryStatus = 0x1000;
constexpr std::uint16_t kUnifiedBattery = 0x1004;
constexpr std::uint16_t kReprogControls = 0x1b04;
constexpr std::uint8_t kFlagReprogrammable = 0x10;
constexpr auto kBatteryWhileIdle = std::chrono::minutes(15);
constexpr auto kBatteryOnWake = std::chrono::minutes(5);
// a device that was just switched on answers only after it has reconnected
constexpr int kRetries = 5;

std::string readFile(const std::filesystem::path &path)
{
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

//! /dev/hidrawN of Logitech HID++ interfaces (descriptor has long report 0x11)
std::vector<std::string> hidppNodes()
{
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator("/sys/class/hidraw", ec)) {
    if (readFile(entry.path() / "device/uevent").find(":0000046D:") == std::string::npos) {
      continue;
    }
    if (readFile(entry.path() / "device/report_descriptor").find(std::string("\x85\x11", 2)) == std::string::npos) {
      continue;
    }
    out.push_back("/dev/" + entry.path().filename().string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

const char *unifiedBatteryState(std::uint8_t s)
{
  switch (s) {
  case 0:
    return "discharging";
  case 1:
    return "charging";
  case 2:
    return "charging slowly";
  case 3:
    return "full";
  default:
    return "error";
  }
}

const char *batteryStatusState(std::uint8_t s)
{
  switch (s) {
  case 0:
    return "discharging";
  case 1:
  case 2:
  case 4:
    return "charging";
  case 3:
    return "full";
  default:
    return "error";
  }
}

} // namespace

HidppManager::HidppManager()
{
  m_thread = std::thread([this] { run(); });
}

HidppManager::~HidppManager()
{
  {
    std::scoped_lock lock{m_mutex};
    m_stop = true;
  }
  m_cv.notify_all();
  if (m_thread.joinable()) {
    m_thread.join();
  }
}

void HidppManager::setRemaps(std::vector<Remap> remaps)
{
  {
    std::scoped_lock lock{m_mutex};
    m_remaps = std::move(remaps);
    m_wantCheck = true;
  }
  m_cv.notify_all();
}

void HidppManager::wake()
{
  {
    std::scoped_lock lock{m_mutex};
    m_wantCheck = true;
  }
  m_cv.notify_all();
}

void HidppManager::rescan()
{
  {
    std::scoped_lock lock{m_mutex};
    m_wantScan = true;
  }
  m_cv.notify_all();
}

bool HidppManager::idleFor(std::chrono::seconds s) const
{
  const auto last = Clock::time_point(Clock::duration(m_lastInput.load()));
  return Clock::now() - last >= s;
}

bool HidppManager::request(
    int fd, std::uint8_t dev, std::uint8_t feature, std::uint8_t function, const std::vector<std::uint8_t> &params,
    std::vector<std::uint8_t> &reply, int timeoutMs
) const
{
  std::array<std::uint8_t, kLongLength> msg{};
  msg[0] = kLongReport;
  msg[1] = dev;
  msg[2] = feature;
  msg[3] = static_cast<std::uint8_t>((function << 4) | kSoftwareId);
  std::copy_n(params.begin(), std::min(params.size(), msg.size() - 4), msg.begin() + 4);
  m_lastError = 0;
  if (write(fd, msg.data(), msg.size()) != static_cast<ssize_t>(msg.size())) {
    return false;
  }
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
  while (Clock::now() < deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    pollfd pfd{fd, POLLIN, 0};
    if (poll(&pfd, 1, static_cast<int>(std::max<long long>(left, 1))) <= 0) {
      return false;
    }
    std::array<std::uint8_t, 64> buf{};
    const ssize_t len = read(fd, buf.data(), buf.size());
    if (len < 5 || buf[1] != dev) {
      continue;
    }
    if (buf[0] == kLongReport && buf[2] == feature && (buf[3] & 0x0F) == kSoftwareId) {
      reply.assign(buf.begin() + 4, buf.begin() + len);
      return true;
    }
    // HID++ 2.0 error (0xff) or HID++ 1.0 error (0x8f: no device in the slot, asleep)
    if ((buf[2] == 0xFF || buf[2] == 0x8F) && buf[3] == feature && (buf[4] & 0x0F) == kSoftwareId) {
      m_lastError = buf[5];
      return false;
    }
  }
  return false;
}

std::uint8_t HidppManager::featureIndex(int fd, std::uint8_t dev, std::uint16_t feature) const
{
  std::vector<std::uint8_t> r;
  if (request(fd, dev, 0x00, 0, {static_cast<std::uint8_t>(feature >> 8), static_cast<std::uint8_t>(feature)}, r) &&
      !r.empty()) {
    return r[0];
  }
  return 0;
}

bool HidppManager::probe(int fd, const std::string &node, std::uint8_t index)
{
  Device dev;
  dev.node = node;
  dev.index = index;
  // the first request tells whether anything is in this slot; keep it short
  std::vector<std::uint8_t> r;
  if (!request(fd, index, 0x00, 0, {kDeviceName >> 8, kDeviceName & 0xFF}, r, 250) || r.empty() || r[0] == 0) {
    // 0x09 (unknown device) means the slot is empty; anything else, or no answer,
    // usually means a paired device that is asleep or switched off
    return m_lastError == 0x09 || (!r.empty() && r[0] == 0);
  }
  const std::uint8_t nameFeature = r[0];
  if (request(fd, index, nameFeature, 0, {}, r) && !r.empty()) {
    const std::size_t length = r[0];
    while (dev.name.size() < length &&
           request(fd, index, nameFeature, 1, {static_cast<std::uint8_t>(dev.name.size())}, r) && !r.empty()) {
      for (auto c : r) {
        if (dev.name.size() < length && c != 0) {
          dev.name.push_back(static_cast<char>(c));
        }
      }
      if (r[0] == 0) {
        break;
      }
    }
  }

  dev.reprog = featureIndex(fd, index, kReprogControls);
  if (dev.reprog && request(fd, index, dev.reprog, 0, {}, r) && !r.empty()) {
    const std::uint8_t count = r[0];
    for (std::uint8_t i = 0; i < count; ++i) {
      if (request(fd, index, dev.reprog, 1, {i}, r) && r.size() >= 8 && (r[4] & kFlagReprogrammable)) {
        dev.controls.push_back({static_cast<std::uint16_t>((r[0] << 8) | r[1]), r[6], r[7]});
      }
    }
  }
  if ((dev.battery = featureIndex(fd, index, kUnifiedBattery))) {
    dev.unifiedBattery = true;
  } else {
    dev.battery = featureIndex(fd, index, kBatteryStatus);
  }
  if (!dev.reprog && !dev.battery) {
    return true;
  }
  LOG_INFO(
      "hidpp: \"%s\" on %s slot %u: %zu reprogrammable control(s), battery %s", dev.name.c_str(), node.c_str(), index,
      dev.controls.size(), dev.battery ? (dev.unifiedBattery ? "0x1004" : "0x1000") : "none"
  );
  m_devices.push_back(std::move(dev));
  return true;
}

void HidppManager::readBattery(int fd, Device &dev)
{
  if (!dev.battery) {
    return;
  }
  std::vector<std::uint8_t> r;
  bool ok = false;
  if (dev.unifiedBattery) {
    if ((ok = request(fd, dev.index, dev.battery, 1, {}, r) && r.size() >= 3)) {
      dev.batteryPercent = r[0];
      dev.batteryState = unifiedBatteryState(r[2]);
    }
  } else if ((ok = request(fd, dev.index, dev.battery, 0, {}, r) && r.size() >= 3)) {
    dev.batteryPercent = r[0];
    dev.batteryState = batteryStatusState(r[2]);
  }
  if (ok) {
    dev.batteryRead = Clock::now();
    dev.batteryAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
  }
}

bool HidppManager::applyRemaps(int fd, Device &dev, const std::vector<Remap> &remaps)
{
  if (!dev.reprog) {
    return true;
  }
  bool ok = true;
  for (const auto &control : dev.controls) {
    std::uint16_t want = control.cid; // its own job
    for (const auto &remap : remaps) {
      if (remap.control == control.cid && (remap.device.empty() || remap.device == dev.name)) {
        want = remap.to;
      }
    }
    // only look at controls we remap or remapped before; a full check runs after a scan
    const bool known = dev.remapped.contains(control.cid);
    if (want == control.cid && known && dev.remapped[control.cid] == control.cid) {
      continue;
    }
    std::vector<std::uint8_t> r;
    const std::uint8_t hi = control.cid >> 8;
    const std::uint8_t lo = control.cid & 0xFF;
    if (!request(fd, dev.index, dev.reprog, 2, {hi, lo}, r) || r.size() < 5) {
      ok = ok && want == control.cid; // only a wanted remap is worth retrying
      continue;
    }
    const auto current = static_cast<std::uint16_t>((r[3] << 8) | r[4]);
    const std::uint16_t effective = current == 0 ? control.cid : current;
    if (effective != want) {
      request(
          fd, dev.index, dev.reprog, 3,
          {hi, lo, 0x00, static_cast<std::uint8_t>(want >> 8), static_cast<std::uint8_t>(want & 0xFF)}, r
      );
      LOG_INFO("hidpp: \"%s\" control 0x%x now acts as 0x%x", dev.name.c_str(), control.cid, want);
    }
    dev.remapped[control.cid] = want;
  }
  return ok;
}

void HidppManager::scan(int)
{
  m_devices.clear();
  m_unanswered.clear();
  for (const auto &node : hidppNodes()) {
    const int fd = open(node.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
      LOG_DEBUG("hidpp: cannot open %s: %s", node.c_str(), strerror(errno));
      continue;
    }
    for (std::uint8_t index = 1; index <= 6; ++index) {
      if (!probe(fd, node, index)) {
        m_unanswered.insert({node, index});
      }
    }
    close(fd);
  }
}

void HidppManager::reprobe()
{
  std::set<std::string> nodes;
  for (const auto &[node, index] : m_unanswered) {
    nodes.insert(node);
  }
  for (const auto &node : nodes) {
    const int fd = open(node.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    for (auto it = m_unanswered.begin(); it != m_unanswered.end();) {
      if (it->first == node && probe(fd, node, it->second)) {
        it = m_unanswered.erase(it);
      } else {
        ++it;
      }
    }
    close(fd);
  }
}

bool HidppManager::session(bool checkBattery)
{
  std::vector<Remap> remaps;
  {
    std::scoped_lock lock{m_mutex};
    remaps = m_remaps;
  }
  std::set<std::string> nodes;
  for (const auto &dev : m_devices) {
    nodes.insert(dev.node);
  }
  bool ok = true;
  for (const auto &node : nodes) {
    const int fd = open(node.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    for (auto &dev : m_devices) {
      if (dev.node != node) {
        continue;
      }
      ok = applyRemaps(fd, dev, remaps) && ok;
      if (checkBattery) {
        readBattery(fd, dev);
      }
    }
    close(fd);
  }
  publish();
  return ok;
}

void HidppManager::publish() const
{
  QJsonArray devices;
  for (const auto &dev : m_devices) {
    QJsonArray controls;
    for (const auto &control : dev.controls) {
      QJsonArray targets;
      for (const auto &other : dev.controls) {
        if (other.cid != control.cid && other.group > 0 && (control.groupMask & (1 << (other.group - 1)))) {
          targets.append(other.cid);
        }
      }
      QJsonObject c{{"cid", control.cid}, {"targets", targets}};
      if (auto it = dev.remapped.find(control.cid); it != dev.remapped.end()) {
        c.insert("actsAs", it->second);
      }
      controls.append(c);
    }
    QJsonObject d{{"device", QString::fromStdString(dev.name)}, {"controls", controls}};
    if (dev.batteryPercent >= 0) {
      d.insert(
          "battery", QJsonObject{
                         {"percent", dev.batteryPercent},
                         {"state", QString::fromStdString(dev.batteryState)},
                         {"at", QString::fromStdString(dev.batteryAt)},
                     }
      );
    }
    devices.append(d);
  }
  headless::updateStatus({{"hidpp", devices}});
}

void HidppManager::run()
{
  int retries = 0;
  for (;;) {
    bool doScan = false;
    bool doCheck = false;
    {
      std::unique_lock lock{m_mutex};
      const auto wait = retries > 0 ? std::chrono::seconds(1) : std::chrono::seconds(60);
      const bool woken = m_cv.wait_for(lock, wait, [this] { return m_stop || m_wantScan || m_wantCheck; });
      if (m_stop) {
        break;
      }
      doScan = std::exchange(m_wantScan, false);
      doCheck = std::exchange(m_wantCheck, false) || (!woken && retries > 0);
    }

    const auto now = Clock::now();
    if (doScan) {
      scan();
    } else if (doCheck && !m_unanswered.empty()) {
      reprobe();
    }
    if (doScan || doCheck) {
      const bool stale = std::any_of(m_devices.begin(), m_devices.end(), [&](const Device &d) {
        return d.battery && now - d.batteryRead >= kBatteryOnWake;
      });
      if (session(doScan || stale)) {
        retries = 0;
      } else if (++retries > kRetries) {
        LOG_DEBUG("hidpp: device not answering, waiting for its next input");
        retries = 0;
      }
      continue;
    }
    // every minute while nobody moves the mouse: re-check remaps (a device
    // switched off and on keeps nothing) and, now and then, the battery
    if (idleFor(std::chrono::seconds(2))) {
      const bool stale = std::any_of(m_devices.begin(), m_devices.end(), [&](const Device &d) {
        return d.battery && now - d.batteryRead >= kBatteryWhileIdle;
      });
      session(stale);
    }
  }
}

} // namespace deskflow
