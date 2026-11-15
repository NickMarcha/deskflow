/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "EvdevDeviceFilterTests.h"

#include "platform/EvdevDeviceFilter.h"

#include <cstring>

using namespace deskflow::evdev;

namespace {

const char *const Headset = "HP, Inc HyperX Cloud III S Wireless Consumer Control";

NodeInfo headsetKeys()
{
  NodeInfo n;
  n.name = Headset;
  n.keys = true;
  return n;
}

bool is(const char *a, const char *b)
{
  return std::strcmp(a, b) == 0;
}

struct TestBinding
{
  std::string device;
  std::uint32_t code = 0;
};

} // namespace

void EvdevDeviceFilterTests::splitList_spacesAndEmptyItems_trimmedAndDropped()
{
  const auto list = splitList(" HyperX , ,Consumer Control,");
  QCOMPARE(list.size(), std::size_t{2});
  QCOMPARE(list[0], std::string("HyperX"));
  QCOMPARE(list[1], std::string("Consumer Control"));
  QVERIFY(splitList(nullptr).empty());
}

void EvdevDeviceFilterTests::keyboardAndMouse_noFilter_used()
{
  NodeInfo kb;
  kb.name = "Lenovo TrackPoint Keyboard II";
  kb.keyboard = true;
  kb.keys = true;
  auto c = chooseDevice(kb, {});
  QVERIFY(is(c.kind, "keyboard"));
  QVERIFY(is(c.state, "used"));

  NodeInfo mouse;
  mouse.name = "Logitech USB Receiver Mouse";
  mouse.pointer = true;
  c = chooseDevice(mouse, {});
  QVERIFY(is(c.kind, "pointer"));
  QVERIFY(is(c.state, "used"));
}

void EvdevDeviceFilterTests::keysWithSibling_noFilter_used()
{
  NodeInfo n;
  n.name = "Logitech USB Receiver Consumer Control";
  n.keys = true;
  n.hasSibling = true;
  const auto c = chooseDevice(n, {});
  QVERIFY(is(c.kind, "keys"));
  QVERIFY(is(c.state, "used"));
  QVERIFY(c.companion);
}

void EvdevDeviceFilterTests::headsetKeys_noKeysList_skipped()
{
  const auto c = chooseDevice(headsetKeys(), {});
  QVERIFY(is(c.kind, "keys"));
  QVERIFY(is(c.state, "skipped"));
  QVERIFY(!c.companion);
}

void EvdevDeviceFilterTests::headsetKeys_inKeysList_used()
{
  DeviceFilter f;
  f.keys = {"HyperX Cloud III S Wireless Consumer Control"};
  const auto c = chooseDevice(headsetKeys(), f);
  QVERIFY(is(c.kind, "keys"));
  QVERIFY(is(c.state, "used"));
  QVERIFY(c.companion);

  // a keys-only node of the same headset without that name stays untouched
  NodeInfo other;
  other.name = "HP, Inc HyperX Cloud III S Wireless";
  other.keys = true;
  QVERIFY(is(chooseDevice(other, f).state, "skipped"));
}

void EvdevDeviceFilterTests::headsetKeys_inKeysAndIgnore_filtered()
{
  DeviceFilter f;
  f.keys = {"HyperX"};
  f.ignore = {"Consumer Control"};
  QVERIFY(is(chooseDevice(headsetKeys(), f).state, "filtered"));
}

void EvdevDeviceFilterTests::headsetKeys_inKeysNotInMatch_filtered()
{
  DeviceFilter f;
  f.keys = {"HyperX"};
  f.match = {"Lenovo"};
  QVERIFY(is(chooseDevice(headsetKeys(), f).state, "filtered"));
  f.match = {"Lenovo", "HyperX"};
  QVERIFY(is(chooseDevice(headsetKeys(), f).state, "used"));
}

void EvdevDeviceFilterTests::pointingStickOnly_inKeysList_skipped()
{
  NodeInfo n;
  n.name = "HyperX thing";
  n.pointingStick = true;
  DeviceFilter f;
  f.keys = {"HyperX"};
  const auto c = chooseDevice(n, f);
  QVERIFY(is(c.kind, "other"));
  QVERIFY(is(c.state, "skipped"));
}

void EvdevDeviceFilterTests::match_unchangedForKeyboards()
{
  NodeInfo kb;
  kb.name = "Lenovo TrackPoint Keyboard II";
  kb.keyboard = true;
  DeviceFilter f;
  f.match = {"Logitech"};
  f.keys = {"Lenovo"}; // KEYS only concerns keys-only nodes
  QVERIFY(is(chooseDevice(kb, f).state, "filtered"));
}

void EvdevDeviceFilterTests::findBinding_exactBeatsPartBeatsAny()
{
  const std::vector<TestBinding> bindings = {
      {"", 163},
      {"HyperX", 163},
      {Headset, 163},
      {"Logitech USB Receiver", 163},
  };
  QCOMPARE(findBinding(bindings, Headset, 163), 2);
  QCOMPARE(findBinding(std::vector<TestBinding>(bindings.begin(), bindings.begin() + 2), Headset, 163), 1);
  QCOMPARE(findBinding(bindings, "Lenovo TrackPoint Keyboard II", 163), 0);
  QCOMPARE(findBinding(bindings, "Logitech USB Receiver", 163), 3);
  // a name is also a part of longer names: the receiver's mouse node matches too
  QCOMPARE(findBinding(bindings, "Logitech USB Receiver Mouse", 163), 3);
}

void EvdevDeviceFilterTests::findBinding_noMatch_minusOne()
{
  const std::vector<TestBinding> bindings = {{"HyperX", 163}};
  QCOMPARE(findBinding(bindings, Headset, 164), -1);
  QCOMPARE(findBinding(bindings, "Lenovo TrackPoint Keyboard II", 163), -1);
}

QTEST_MAIN(EvdevDeviceFilterTests)
