/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/PlatformComputer.h"
#include "platform/EvdevDeviceFilter.h"

#include <array>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct libinput;
struct libinput_device;
struct libinput_event;
struct udev;

class EventQueueTimer;

namespace deskflow {

class EiKeyState;

//! Headless primary computer reading input devices directly through libinput.
/*!
Meant for a server with no display at all (e.g. a Raspberry Pi with the shared
keyboard and mouse plugged in). Devices are opened through udev and grabbed
exclusively, so the local console never sees the input. The computer has no real
screen: it keeps a virtual cursor inside a virtual rectangle so the server's
edge switching still works, and hotkeys work as usual.

Configuration (environment, read once at startup):
  DESKFLOW_EVDEV_MATCH   comma-separated name substrings; only matching devices
                         are used (default: every udev keyboard or mouse)
  DESKFLOW_EVDEV_IGNORE  comma-separated name substrings to skip
  DESKFLOW_EVDEV_KEYS    comma-separated name substrings; keys-only nodes with
                         no keyboard or mouse beside them (a headset's media
                         buttons) are used only when they match
  DESKFLOW_EVDEV_SHAPE   virtual screen size WxH (default 1920x1080)
  DESKFLOW_EVDEV_ACCEL   libinput pointer speed, -1.0 .. 1.0 (default 0)
  DESKFLOW_EVDEV_REPEAT  key repeat delay_ms,rate_hz (default 500,30)
  XKB_DEFAULT_LAYOUT etc. select the keyboard layout (libxkbcommon)
*/
class EvdevComputer : public PlatformComputer
{
public:
  explicit EvdevComputer(IEventQueue *events);
  ~EvdevComputer() override;

  // IComputer overrides
  void *getEventTarget() const final;
  bool getClipboard(ClipboardID id, IClipboard *) const override;
  void getShape(std::int32_t &x, std::int32_t &y, std::int32_t &width, std::int32_t &height) const override;
  void getCursorPos(std::int32_t &x, std::int32_t &y) const override;

  // IPrimaryComputer overrides
  void reconfigure(std::uint32_t activeSides) override;
  std::uint32_t activeSides() override;
  void warpCursor(std::int32_t x, std::int32_t y) override;
  std::uint32_t registerHotKey(KeyID key, KeyModifierMask mask) override;
  void unregisterHotKey(std::uint32_t id) override;
  void fakeInputBegin() override;
  void fakeInputEnd() override;
  std::int32_t getJumpZoneSize() const override;
  bool isAnyMouseButtonDown(std::uint32_t &buttonID) const override;
  void getCursorCenter(std::int32_t &x, std::int32_t &y) const override;

  // ISecondaryComputer overrides (primary only, so these are no-ops)
  void fakeMouseButton(ButtonID id, bool press) override;
  void fakeMouseMove(std::int32_t x, std::int32_t y) override;
  void fakeMouseRelativeMove(std::int32_t dx, std::int32_t dy) const override;
  void fakeMouseWheel(ScrollDelta delta) const override;

  // IPlatformComputer overrides
  void enable() override;
  void disable() override;
  void enter() override;
  bool canLeave() override;
  void leave() override;
  bool setClipboard(ClipboardID, const IClipboard *) override;
  void checkClipboards() override;
  void openScreensaver(bool notify) override;
  void closeScreensaver() override;
  void screensaver(bool activate) override;
  void resetOptions() override;
  void setOptions(const OptionsList &options) override;
  void setSequenceNumber(std::uint32_t) override;
  bool isPrimary() const override;

  // libinput open/close callbacks, public so the C trampolines can reach them
  int openRestricted(const char *path, int flags);
  void closeRestricted(int fd);

protected:
  // IPlatformComputer overrides
  void handleSystemEvent(const Event &event) override;
  void updateButtons() override;
  IKeyState *getKeyState() const override;
  std::string getSecureInputApp() const override;

private:
  void sendEvent(EventTypes type, void *data);
  void onDeviceAdded(libinput_device *device);
  void onKeyEvent(libinput_event *event);
  void onButtonEvent(libinput_event *event);
  void onMotionEvent(libinput_event *event);
  void onWheelEvent(libinput_event *event);
  void onFingerScrollEvent(libinput_event *event);
  bool onHotkey(KeyID key, bool isPressed, KeyModifierMask mask);
  void updateLeds(KeyModifierMask mask);
  void startRepeat(KeyID key, KeyButton button);
  void stopRepeat();
  void onRepeatTimer();
  void setDeviceStatus(const std::string &path, const std::string &name, const char *kind, const char *state);
  void handleKey(std::uint32_t keycode, bool pressed);
  void handleButton(std::uint32_t code, bool pressed);
  bool applyBinding(const std::string &device, std::uint32_t code, bool pressed);
  void pollControlFiles();
  void loadBindings();
  void noteLogitechInput(libinput_device *device);

  // one physical input on one device, remapped to other evdev key/button codes
  struct Binding
  {
    std::string device; // device name or part of it, empty for any device
    std::uint32_t code = 0;
    std::vector<std::uint32_t> send;
  };

  IEventQueue *m_events = nullptr;
  EiKeyState *m_keyState = nullptr;
  udev *m_udev = nullptr;
  libinput *m_libinput = nullptr;
  std::vector<libinput_device *> m_keyboards;
  // every node libinput offered, for the web UI: path -> {name, kind, state}
  std::map<std::string, std::array<std::string, 3>> m_deviceStatus;

  // bindings file (DESKFLOW_EVDEV_BINDINGS) and identify mode for the web UI;
  // both are checked at most once a second, from the input path
  std::vector<Binding> m_bindings;
  std::string m_bindingsPath;
  std::int64_t m_bindingsMtime = -1;
  std::string m_identifyPath;
  bool m_identify = false;
  std::uint32_t m_inputSeq = 0;
  std::chrono::steady_clock::time_point m_lastPoll{};

  // extra Logitech controls over HID++ (thumb button etc.), see HidppListener
  std::unique_ptr<class HidppListener> m_hidpp;
  int m_hidppWake = -1;
  // short HID++ sessions: button remaps and battery, see HidppManager
  std::unique_ptr<class HidppManager> m_hidppManager;
  std::chrono::steady_clock::time_point m_lastLogitechInput{};

  evdev::DeviceFilter m_filter;
  double m_accelSpeed = 0.0;
  double m_repeatDelay = 0.5;
  double m_repeatInterval = 1.0 / 30.0;

  std::bitset<NumButtonIDs> m_buttons;
  std::uint32_t m_activeSides = 0;
  std::int32_t m_w = 1920;
  std::int32_t m_h = 1080;
  std::int32_t m_cursorX = 0;
  std::int32_t m_cursorY = 0;
  bool m_isOnComputer = true;
  double m_bufferDX = 0;
  double m_bufferDY = 0;
  double m_scrollX = 0;
  double m_scrollY = 0;

  // software key repeat: libinput drops the kernel's repeat events
  EventQueueTimer *m_repeatTimer = nullptr;
  bool m_repeatStarted = false;
  KeyID m_repeatKey = kKeyNone;
  KeyButton m_repeatButton = 0;

  // hotkeys: id -> (key, mask)
  std::map<std::uint32_t, std::pair<KeyID, KeyModifierMask>> m_hotkeys;
  std::uint32_t m_nextHotkeyId = 0;
};

} // namespace deskflow
