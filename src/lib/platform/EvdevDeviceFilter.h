/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

// Device and binding selection for EvdevComputer, kept free of libinput and
// udev so it can be tested on its own.

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace deskflow::evdev {

//! Splits "a, b,c" into {"a", "b", "c"}; empty items are dropped.
inline std::vector<std::string> splitList(const char *value)
{
  std::vector<std::string> out;
  if (!value)
    return out;
  std::stringstream ss(value);
  std::string item;
  while (std::getline(ss, item, ',')) {
    item.erase(0, item.find_first_not_of(' '));
    item.erase(item.find_last_not_of(' ') + 1);
    if (!item.empty())
      out.push_back(item);
  }
  return out;
}

inline bool containsAny(const std::string &name, const std::vector<std::string> &needles)
{
  return std::any_of(needles.begin(), needles.end(), [&](const auto &n) { return name.find(n) != std::string::npos; });
}

//! What udev says about one input node.
struct NodeInfo
{
  std::string name;
  bool keyboard = false;      // ID_INPUT_KEYBOARD
  bool pointer = false;       // ID_INPUT_MOUSE or ID_INPUT_TOUCHPAD
  bool keys = false;          // ID_INPUT_KEY
  bool pointingStick = false; // ID_INPUT_POINTINGSTICK
  bool hasSibling = false;    // same USB device has a keyboard or mouse node
};

//! DESKFLOW_EVDEV_MATCH, _IGNORE and _KEYS.
struct DeviceFilter
{
  std::vector<std::string> match;
  std::vector<std::string> ignore;
  std::vector<std::string> keys;
};

struct DeviceChoice
{
  const char *kind;  // keyboard, pointer, keyboard+pointer, keys, other
  const char *state; // used, filtered (left out by MATCH/IGNORE), skipped
  bool companion;    // a keys-only node that is taken
};

/*!
A keys-only node (media, power or volume keys) is taken when the same USB
device also has a keyboard or mouse, e.g. a receiver's Consumer Control node,
or when its name contains one of DESKFLOW_EVDEV_KEYS. Without either, e.g. a
headset's buttons, it stays untouched. ID_INPUT_POINTINGSTICK only counts with
a sibling: hwdb sets it on every node of some TrackPoint keyboards.
MATCH and IGNORE then apply to every candidate alike.
*/
inline DeviceChoice chooseDevice(const NodeInfo &node, const DeviceFilter &filter)
{
  const bool keysOnly = !node.keyboard && !node.pointer && (node.keys || node.pointingStick);
  const bool companion = keysOnly && (node.hasSibling || (node.keys && containsAny(node.name, filter.keys)));

  const char *kind = node.keyboard && node.pointer ? "keyboard+pointer"
                     : node.keyboard               ? "keyboard"
                     : node.pointer                ? "pointer"
                     : node.keys || companion      ? "keys"
                                                   : "other";
  if (!node.keyboard && !node.pointer && !companion) {
    return {kind, "skipped", false};
  }
  if (containsAny(node.name, filter.ignore) || (!filter.match.empty() && !containsAny(node.name, filter.match))) {
    return {kind, "filtered", companion};
  }
  return {kind, "used", companion};
}

/*!
Picks the binding for \p code pressed on \p device, or -1. A binding's device is
the exact device name, a part of it, or empty for any device; the exact name
wins over a part, and a part over any device. Among equals the first one wins.
*/
template <typename Binding>
int findBinding(const std::vector<Binding> &bindings, const std::string &device, std::uint32_t code)
{
  int best = -1;
  int bestRank = 0;
  for (int i = 0; i < static_cast<int>(bindings.size()); ++i) {
    const auto &b = bindings[i];
    if (b.code != code)
      continue;
    int rank = 0;
    if (b.device.empty()) {
      rank = 1;
    } else if (b.device == device) {
      rank = 3;
    } else if (device.find(b.device) != std::string::npos) {
      rank = 2;
    }
    if (rank > bestRank) {
      best = i;
      bestRank = rank;
    }
  }
  return best;
}

} // namespace deskflow::evdev
