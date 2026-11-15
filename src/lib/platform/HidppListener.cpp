/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/HidppListener.h"

#include "base/Log.h"
#include "base/String.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

namespace deskflow {

namespace {

constexpr std::uint8_t kShortReport = 0x10;
constexpr std::uint8_t kLongReport = 0x11;
constexpr std::size_t kLongLength = 20;
constexpr std::uint8_t kSoftwareId = 0x0B;
constexpr std::uint16_t kReprogControls = 0x1b04;
constexpr std::uint16_t kDeviceName = 0x0005;
// HID++ 1.0 notification from the receiver: a paired device (re)connected or dropped
constexpr std::uint8_t kConnectionNotice = 0x41;
// setCidReporting flags: divert, and "divert field is valid"
constexpr std::uint8_t kDivert = 0x01;
constexpr std::uint8_t kDivertValid = 0x02;
// getCidInfo flags
constexpr std::uint8_t kFlagDivertable = 0x20;
constexpr std::uint8_t kFlagVirtual = 0x80;

using Clock = std::chrono::steady_clock;

std::string readFile(const std::filesystem::path &path)
{
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool isPlainMouseButton(std::uint16_t cid)
{
  // left, right, middle, back, forward: these already arrive through evdev
  return cid == 0x50 || cid == 0x51 || cid == 0x52 || cid == 0x53 || cid == 0x56;
}

} // namespace

HidppListener::HidppListener(int wakeFd, std::string identifyPath)
    : m_wakeFd{wakeFd},
      m_identifyPath{std::move(identifyPath)}
{
  if (pipe2(m_stopPipe, O_NONBLOCK | O_CLOEXEC) != 0) {
    LOG_WARN("hidpp: cannot create pipe: %s", strerror(errno));
    return;
  }
  m_thread = std::thread([this] { run(); });
}

HidppListener::~HidppListener()
{
  if (m_thread.joinable()) {
    [[maybe_unused]] auto n = write(m_stopPipe[1], "x", 1);
    m_thread.join();
  }
  for (int fd : m_stopPipe) {
    if (fd >= 0) {
      close(fd);
    }
  }
}

void HidppListener::setBound(std::set<std::pair<std::string, std::uint32_t>> bound)
{
  std::scoped_lock lock{m_mutex};
  m_bound = std::move(bound);
  ++m_boundGeneration;
}

std::vector<HidppListener::Press> HidppListener::take()
{
  std::scoped_lock lock{m_mutex};
  return std::exchange(m_presses, {});
}

bool HidppListener::identifyActive() const
{
  struct stat st{};
  return !m_identifyPath.empty() && stat(m_identifyPath.c_str(), &st) == 0 &&
         std::abs(std::difftime(std::time(nullptr), st.st_mtime)) < 60;
}

bool HidppListener::wanted(const Device &dev, std::uint16_t cid) const
{
  if (m_identify) {
    return true;
  }
  const std::uint32_t code = kCodeBase + cid;
  std::scoped_lock lock{m_mutex};
  return m_bound.contains({dev.name, code}) || m_bound.contains({std::string(), code});
}

void HidppListener::emit(const std::string &device, std::uint16_t cid, bool pressed)
{
  {
    std::scoped_lock lock{m_mutex};
    m_presses.push_back({device, kCodeBase + cid, pressed});
  }
  const std::uint64_t one = 1;
  [[maybe_unused]] auto n = write(m_wakeFd, &one, sizeof(one));
}

bool HidppListener::request(
    Node &node, std::uint8_t dev, std::uint8_t feature, std::uint8_t function, const std::vector<std::uint8_t> &params,
    std::vector<std::uint8_t> &reply
)
{
  std::array<std::uint8_t, kLongLength> msg{};
  msg[0] = kLongReport;
  msg[1] = dev;
  msg[2] = feature;
  msg[3] = static_cast<std::uint8_t>((function << 4) | kSoftwareId);
  std::copy_n(params.begin(), std::min(params.size(), msg.size() - 4), msg.begin() + 4);
  if (write(node.fd, msg.data(), msg.size()) != static_cast<ssize_t>(msg.size())) {
    return false;
  }

  const auto deadline = Clock::now() + std::chrono::milliseconds(500);
  while (Clock::now() < deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    pollfd pfd{node.fd, POLLIN, 0};
    if (poll(&pfd, 1, static_cast<int>(std::max<long long>(left, 1))) <= 0) {
      break;
    }
    std::array<std::uint8_t, 64> buf{};
    const ssize_t len = read(node.fd, buf.data(), buf.size());
    if (len < 4) {
      if (len < 0 && errno != EAGAIN) {
        return false;
      }
      continue;
    }
    const auto *d = buf.data();
    if (d[1] == dev) {
      const bool ours = (d[3] & 0x0F) == kSoftwareId || (d[4] & 0x0F) == kSoftwareId;
      if (d[0] == kLongReport && d[2] == feature && (d[3] & 0x0F) == kSoftwareId) {
        reply.assign(d + 4, d + len);
        return true;
      }
      // HID++ 2.0 error (0xff) or HID++ 1.0 error (0x8f, e.g. no device in this slot)
      if ((d[2] == 0xFF || d[2] == 0x8F) && d[3] == feature && ours) {
        return false;
      }
    }
    handleFrame(node, d, static_cast<std::size_t>(len));
  }
  return false;
}

void HidppListener::probe(Node &node, std::uint8_t index)
{
  node.devices.erase(index);
  std::vector<std::uint8_t> r;
  if (!request(node, index, 0x00, 0, {kReprogControls >> 8, kReprogControls & 0xFF}, r) || r.empty() || r[0] == 0) {
    return;
  }
  Device dev;
  dev.feature = r[0];

  if (request(node, index, 0x00, 0, {kDeviceName >> 8, kDeviceName & 0xFF}, r) && !r.empty() && r[0] != 0) {
    const std::uint8_t nameFeature = r[0];
    if (request(node, index, nameFeature, 0, {}, r) && !r.empty()) {
      const std::size_t length = r[0];
      while (dev.name.size() < length &&
             request(node, index, nameFeature, 1, {static_cast<std::uint8_t>(dev.name.size())}, r)) {
        for (auto c : r) {
          if (dev.name.size() < length && c != 0) {
            dev.name.push_back(static_cast<char>(c));
          }
        }
        if (r.empty() || r[0] == 0) {
          break;
        }
      }
    }
  }

  if (!request(node, index, dev.feature, 0, {}, r) || r.empty()) {
    return;
  }
  const std::uint8_t count = r[0];
  std::string list;
  for (std::uint8_t i = 0; i < count; ++i) {
    if (!request(node, index, dev.feature, 1, {i}, r) || r.size() < 5) {
      continue;
    }
    const auto cid = static_cast<std::uint16_t>((r[0] << 8) | r[1]);
    const std::uint8_t flags = r[4];
    if ((flags & kFlagDivertable) && !(flags & kFlagVirtual) && !isPlainMouseButton(cid)) {
      dev.controls.push_back(cid);
      list += deskflow::string::sprintf(" 0x%x", cid);
    }
  }
  LOG_INFO("hidpp: \"%s\" (device %u), extra controls:%s", dev.name.c_str(), index, list.empty() ? " none" : list.c_str());

  // set every control explicitly: a diversion left over from an earlier run
  // would otherwise keep an unbound control from working
  for (auto cid : dev.controls) {
    const bool want = wanted(dev, cid);
    request(
        node, index, dev.feature, 3,
        {static_cast<std::uint8_t>(cid >> 8), static_cast<std::uint8_t>(cid & 0xFF),
         static_cast<std::uint8_t>(want ? (kDivert | kDivertValid) : kDivertValid), 0, 0},
        r
    );
    if (want) {
      dev.diverted.insert(cid);
    }
  }
  node.devices[index] = std::move(dev);
}

void HidppListener::applyDiversion(Node &node)
{
  std::vector<std::uint8_t> r;
  for (auto &[index, dev] : node.devices) {
    for (auto cid : dev.controls) {
      const bool want = wanted(dev, cid);
      if (want == dev.diverted.contains(cid)) {
        continue;
      }
      request(
          node, index, dev.feature, 3,
          {static_cast<std::uint8_t>(cid >> 8), static_cast<std::uint8_t>(cid & 0xFF),
           static_cast<std::uint8_t>(want ? (kDivert | kDivertValid) : kDivertValid), 0, 0},
          r
      );
      if (want) {
        dev.diverted.insert(cid);
      } else {
        dev.diverted.erase(cid);
        if (dev.pressed.erase(cid)) {
          emit(dev.name, cid, false);
        }
      }
    }
  }
}

void HidppListener::handleFrame(Node &node, const std::uint8_t *d, std::size_t len)
{
  if (len >= 5 && d[0] == kShortReport && d[2] == kConnectionNotice) {
    // bit 6 of the flags byte set means the link is down
    const bool connected = (d[4] & 0x40) == 0;
    if (connected) {
      node.reprobe.insert(d[1]);
    } else if (auto it = node.devices.find(d[1]); it != node.devices.end()) {
      for (auto cid : it->second.pressed) {
        emit(it->second.name, cid, false);
      }
      it->second.pressed.clear();
    }
    return;
  }

  auto it = node.devices.find(d[1]);
  if (len < 12 || d[0] != kLongReport || it == node.devices.end()) {
    return;
  }
  Device &dev = it->second;
  // divertedButtonsEvent: function 0, software id 0, up to four pressed CIDs
  if (d[2] != dev.feature || d[3] != 0x00) {
    return;
  }
  std::set<std::uint16_t> now;
  for (int k = 0; k < 4; ++k) {
    if (const auto cid = static_cast<std::uint16_t>((d[4 + 2 * k] << 8) | d[5 + 2 * k]); cid != 0) {
      now.insert(cid);
    }
  }
  for (auto cid : dev.pressed) {
    if (!now.contains(cid)) {
      emit(dev.name, cid, false);
    }
  }
  for (auto cid : now) {
    if (!dev.pressed.contains(cid)) {
      emit(dev.name, cid, true);
    }
  }
  dev.pressed = std::move(now);
}

void HidppListener::scan()
{
  static std::set<std::string> s_warned;
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator("/sys/class/hidraw", ec)) {
    const std::string path = "/dev/" + entry.path().filename().string();
    if (m_nodes.contains(path)) {
      continue;
    }
    const std::string uevent = readFile(entry.path() / "device/uevent");
    if (uevent.find(":0000046D:") == std::string::npos) {
      continue;
    }
    // only the receiver interface that carries HID++ long reports (report ID 0x11)
    const std::string descriptor = readFile(entry.path() / "device/report_descriptor");
    if (descriptor.find(std::string("\x85\x11", 2)) == std::string::npos) {
      continue;
    }
    const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
      if (s_warned.insert(path).second) {
        LOG_INFO(
            "hidpp: cannot open %s (%s); extra Logitech buttons need a udev rule, see HEADLESS.md", path.c_str(),
            strerror(errno)
        );
      }
      continue;
    }
    Node &node = m_nodes[path];
    node.fd = fd;
    for (std::uint8_t index = 1; index <= 6; ++index) {
      probe(node, index);
    }
  }
}

void HidppListener::run()
{
  scan();
  auto lastScan = Clock::now();
  auto lastCheck = Clock::now();
  std::uint32_t generation = m_boundGeneration;

  for (;;) {
    std::vector<pollfd> pfds{{m_stopPipe[0], POLLIN, 0}};
    std::vector<std::string> paths;
    for (const auto &[path, node] : m_nodes) {
      pfds.push_back({node.fd, POLLIN, 0});
      paths.push_back(path);
    }
    if (poll(pfds.data(), pfds.size(), 1000) < 0 && errno != EINTR) {
      break;
    }
    if (pfds[0].revents & POLLIN) {
      break;
    }

    for (std::size_t i = 1; i < pfds.size(); ++i) {
      Node &node = m_nodes[paths[i - 1]];
      bool gone = (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
      while (!gone && (pfds[i].revents & POLLIN)) {
        std::array<std::uint8_t, 64> buf{};
        const ssize_t len = read(node.fd, buf.data(), buf.size());
        if (len < 0) {
          gone = errno != EAGAIN;
          break;
        }
        handleFrame(node, buf.data(), static_cast<std::size_t>(len));
      }
      if (gone) {
        LOG_INFO("hidpp: %s went away", paths[i - 1].c_str());
        for (auto &[index, dev] : node.devices) {
          for (auto cid : dev.pressed) {
            emit(dev.name, cid, false);
          }
        }
        close(node.fd);
        m_nodes.erase(paths[i - 1]);
      }
    }

    for (auto &[path, node] : m_nodes) {
      if (!node.reprobe.empty()) {
        const auto indexes = std::exchange(node.reprobe, {});
        for (auto index : indexes) {
          probe(node, index);
        }
      }
    }

    const auto now = Clock::now();
    if (now - lastCheck >= std::chrono::seconds(1)) {
      lastCheck = now;
      const bool identify = identifyActive();
      if (identify != m_identify || generation != m_boundGeneration) {
        m_identify = identify;
        generation = m_boundGeneration;
        for (auto &[path, node] : m_nodes) {
          applyDiversion(node);
        }
      }
    }
    if (now - lastScan >= std::chrono::seconds(5)) {
      lastScan = now;
      scan();
    }
  }

  // give every control its built-in behaviour back
  m_identify = false;
  setBound({});
  for (auto &[path, node] : m_nodes) {
    applyDiversion(node);
    close(node.fd);
  }
  m_nodes.clear();
}

} // namespace deskflow
