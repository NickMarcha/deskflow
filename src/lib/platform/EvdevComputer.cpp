/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/EvdevComputer.h"

#include "base/IEventQueue.h"
#include "base/Log.h"
#include "common/HeadlessStatus.h"
#include "deskflow/IPrimaryComputer.h"
#include "platform/EiKeyState.h"
#include "platform/EvdevEventQueueBuffer.h"
#include "platform/HidppListener.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <libinput.h>
#include <libudev.h>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <linux/input.h>
#include <sstream>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace deskflow {

namespace {

std::vector<std::string> splitList(const char *value)
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

bool containsAny(const std::string &name, const std::vector<std::string> &needles)
{
  return std::any_of(needles.begin(), needles.end(), [&](const auto &n) { return name.find(n) != std::string::npos; });
}

std::int64_t mtimeNs(const struct stat &st)
{
  return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
}

int openRestrictedTrampoline(const char *path, int flags, void *userData)
{
  return static_cast<EvdevComputer *>(userData)->openRestricted(path, flags);
}

void closeRestrictedTrampoline(int fd, void *userData)
{
  static_cast<EvdevComputer *>(userData)->closeRestricted(fd);
}

const libinput_interface s_interface = {
    .open_restricted = openRestrictedTrampoline,
    .close_restricted = closeRestrictedTrampoline,
};

bool hasProperty(udev_device *dev, const char *key)
{
  const char *v = udev_device_get_property_value(dev, key);
  return v && std::strcmp(v, "1") == 0;
}

//! True if another input node under the same USB device is a keyboard or mouse.
bool hasInputSibling(udev_device *dev)
{
  udev_device *usb = udev_device_get_parent_with_subsystem_devtype(dev, "usb", "usb_device");
  if (!usb) {
    return false;
  }
  bool found = false;
  udev_enumerate *e = udev_enumerate_new(udev_device_get_udev(dev));
  udev_enumerate_add_match_subsystem(e, "input");
  udev_enumerate_add_match_parent(e, usb);
  udev_enumerate_scan_devices(e);
  udev_list_entry *entry;
  udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e))
  {
    udev_device *other = udev_device_new_from_syspath(udev_device_get_udev(dev), udev_list_entry_get_name(entry));
    if (other) {
      found = udev_device_get_devnode(other) && (hasProperty(other, "ID_INPUT_KEYBOARD") ||
                                                 hasProperty(other, "ID_INPUT_MOUSE"));
      udev_device_unref(other);
    }
    if (found) {
      break;
    }
  }
  udev_enumerate_unref(e);
  return found;
}

ButtonID mapButton(std::uint32_t code)
{
  switch (code) {
  case BTN_LEFT:
    return kButtonLeft;
  case BTN_RIGHT:
    return kButtonRight;
  case BTN_MIDDLE:
    return kButtonMiddle;
  case BTN_SIDE:
  case BTN_BACK:
    return kButtonExtra0;
  case BTN_EXTRA:
  case BTN_FORWARD:
    return kButtonExtra1;
  default:
    return kButtonNone;
  }
}

} // namespace

EvdevComputer::EvdevComputer(IEventQueue *events)
    : PlatformComputer{events},
      m_events{events},
      m_match{splitList(std::getenv("DESKFLOW_EVDEV_MATCH"))},
      m_ignore{splitList(std::getenv("DESKFLOW_EVDEV_IGNORE"))}
{
  if (const char *shape = std::getenv("DESKFLOW_EVDEV_SHAPE")) {
    int w = 0;
    int h = 0;
    if (std::sscanf(shape, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
      m_w = w;
      m_h = h;
    } else {
      LOG_WARN("evdev: ignoring bad DESKFLOW_EVDEV_SHAPE '%s'", shape);
    }
  }
  if (const char *accel = std::getenv("DESKFLOW_EVDEV_ACCEL")) {
    m_accelSpeed = std::clamp(std::atof(accel), -1.0, 1.0);
  }
  if (const char *repeat = std::getenv("DESKFLOW_EVDEV_REPEAT")) {
    int delayMs = 0;
    int rateHz = 0;
    if (std::sscanf(repeat, "%d,%d", &delayMs, &rateHz) == 2 && delayMs > 0 && rateHz > 0) {
      m_repeatDelay = delayMs / 1000.0;
      m_repeatInterval = 1.0 / rateHz;
    } else {
      LOG_WARN("evdev: ignoring bad DESKFLOW_EVDEV_REPEAT '%s'", repeat);
    }
  }
  m_cursorX = m_w / 2;
  m_cursorY = m_h / 2;
  if (const char *bindings = std::getenv("DESKFLOW_EVDEV_BINDINGS")) {
    m_bindingsPath = bindings;
  }
  if (const char *statusDir = std::getenv("DESKFLOW_STATUS_DIR")) {
    m_identifyPath = std::string(statusDir) + "/identify";
  }
  loadBindings();

  // xkb's default "srvrkeys" turn Ctrl+Alt+F1..F12 into console switching
  // (XF86Switch_VT_n), which no hotkey matches and no client can type. A
  // headless server never switches consoles, so make them plain F-keys.
  setenv("XKB_DEFAULT_OPTIONS", "srvrkeys:none", 0);

  // EiKeyState only needs its computer to fake keys, which a primary never does
  m_keyState = new EiKeyState(nullptr, events);

  m_udev = udev_new();
  if (!m_udev) {
    throw std::runtime_error("evdev: udev_new failed");
  }
  m_libinput = libinput_udev_create_context(&s_interface, this, m_udev);
  if (!m_libinput) {
    throw std::runtime_error("evdev: libinput_udev_create_context failed");
  }
  if (libinput_udev_assign_seat(m_libinput, "seat0") != 0) {
    throw std::runtime_error("evdev: libinput_udev_assign_seat failed");
  }

  m_events->addHandler(EventTypes::System, m_events->getSystemTarget(), [this](const auto &e) {
    handleSystemEvent(e);
  });
  // opt-in: on a Raspberry Pi 4 with a Logi Bolt receiver, holding the HID++
  // node open stops the mouse from moving (#125), so it stays off by default
  if (qEnvironmentVariable("DESKFLOW_EVDEV_HIDPP") == QStringLiteral("1")) {
    m_hidppWake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_hidppWake >= 0) {
      m_hidpp = std::make_unique<HidppListener>(m_hidppWake, m_identifyPath);
      loadBindings();
    }
  }
  m_events->adoptBuffer(new EvdevEventQueueBuffer(libinput_get_fd(m_libinput), m_events, m_hidppWake));

  LOG_INFO("evdev: headless primary, virtual screen %dx%d", m_w, m_h);
  auto env = [](const char *name) { return QString::fromLocal8Bit(std::getenv(name)); };
  headless::updateStatus(
      {{"options",
        QJsonObject{
            {"match", env("DESKFLOW_EVDEV_MATCH")},
            {"ignore", env("DESKFLOW_EVDEV_IGNORE")},
            {"shape", QStringLiteral("%1x%2").arg(m_w).arg(m_h)},
            {"accel", m_accelSpeed},
            {"repeatDelayMs", static_cast<int>(std::lround(m_repeatDelay * 1000))},
            {"repeatRateHz", static_cast<int>(std::lround(1.0 / m_repeatInterval))},
            {"layout", env("XKB_DEFAULT_LAYOUT")},
        }}}
  );
}

void EvdevComputer::setDeviceStatus(const std::string &path, const std::string &name, const char *kind, const char *state)
{
  if (state == nullptr) {
    m_deviceStatus.erase(path);
  } else {
    m_deviceStatus[path] = {name, kind, state};
  }
  QJsonArray devices;
  for (const auto &[devPath, info] : m_deviceStatus) {
    devices.append(QJsonObject{
        {"path", QString::fromStdString(devPath)},
        {"name", QString::fromStdString(info[0])},
        {"kind", QString::fromStdString(info[1])},
        {"state", QString::fromStdString(info[2])},
    });
  }
  headless::updateStatus({{"devices", devices}});
}

EvdevComputer::~EvdevComputer()
{
  stopRepeat();
  m_hidpp.reset();
  m_events->adoptBuffer(nullptr);
  if (m_hidppWake >= 0) {
    close(m_hidppWake);
  }
  m_events->removeHandler(EventTypes::System, m_events->getSystemTarget());
  for (auto *device : m_keyboards) {
    libinput_device_unref(device);
  }
  if (m_libinput) {
    libinput_unref(m_libinput);
  }
  if (m_udev) {
    udev_unref(m_udev);
  }
  delete m_keyState;
}

bool EvdevComputer::wantDevice(const std::string &name, bool isKeyboard, bool isPointer) const
{
  if (!isKeyboard && !isPointer)
    return false;
  if (containsAny(name, m_ignore))
    return false;
  if (!m_match.empty() && !containsAny(name, m_match))
    return false;
  return true;
}

int EvdevComputer::openRestricted(const char *path, int flags)
{
  // Decide here whether to take the device at all: refusing the open makes
  // libinput skip it, which keeps e.g. headset volume buttons untouched.
  std::string name;
  bool isKeyboard = false;
  bool isPointer = false;
  bool isCompanion = false;
  struct stat st{};
  if (stat(path, &st) == 0) {
    if (udev_device *dev = udev_device_new_from_devnum(m_udev, 'c', st.st_rdev); dev) {
      isKeyboard = hasProperty(dev, "ID_INPUT_KEYBOARD");
      isPointer = hasProperty(dev, "ID_INPUT_MOUSE") || hasProperty(dev, "ID_INPUT_TOUCHPAD");
      // A keys-only node (media, power or volume keys) is taken only when the same
      // USB device also has a keyboard or mouse, e.g. a receiver's Consumer Control
      // node. A headset's volume buttons have no such sibling and stay untouched.
      // ID_INPUT_POINTINGSTICK is treated the same way rather than trusted on its
      // own: hwdb sets it on every node of some TrackPoint keyboards.
      if (!isKeyboard && !isPointer && (hasProperty(dev, "ID_INPUT_KEY") || hasProperty(dev, "ID_INPUT_POINTINGSTICK"))) {
        isCompanion = hasInputSibling(dev);
      }
      if (udev_device *parent = udev_device_get_parent(dev)) {
        if (const char *n = udev_device_get_sysattr_value(parent, "name")) {
          name = n;
        }
      }
      udev_device_unref(dev);
    }
  }

  const char *kind = isKeyboard && isPointer ? "keyboard+pointer"
                     : isKeyboard                ? "keyboard"
                     : isPointer                 ? "pointer"
                     : isCompanion               ? "keys"
                                                 : "other";
  if (!wantDevice(name, isKeyboard || isCompanion, isPointer)) {
    LOG_DEBUG("evdev: skipping %s (%s)", path, name.c_str());
    // "filtered" is a keyboard or mouse left out by DESKFLOW_EVDEV_MATCH/IGNORE
    setDeviceStatus(path, name, kind, isKeyboard || isCompanion || isPointer ? "filtered" : "skipped");
    return -EACCES;
  }

  int fd = open(path, flags | O_CLOEXEC);
  if (fd < 0) {
    const int err = errno;
    LOG_WARN("evdev: cannot open %s: %s", path, strerror(err));
    setDeviceStatus(path, name, kind, "error");
    return -err;
  }
  // exclusive grab: nothing else on this machine (the text console) sees the input
  if (ioctl(fd, EVIOCGRAB, 1) != 0) {
    LOG_WARN("evdev: cannot grab %s: %s", path, strerror(errno));
  }
  LOG_INFO(
      "evdev: using %s (%s)%s%s%s", path, name.c_str(), isKeyboard ? " keyboard" : "", isPointer ? " pointer" : "",
      isCompanion ? " keys" : ""
  );
  setDeviceStatus(path, name, kind, "used");
  return fd;
}

void EvdevComputer::closeRestricted(int fd)
{
  ioctl(fd, EVIOCGRAB, 0);
  close(fd);
}

void *EvdevComputer::getEventTarget() const
{
  return const_cast<void *>(static_cast<const void *>(this));
}

bool EvdevComputer::getClipboard(ClipboardID, IClipboard *) const
{
  // no local clipboard; the server relays clipboards between clients itself
  return false;
}

void EvdevComputer::getShape(std::int32_t &x, std::int32_t &y, std::int32_t &w, std::int32_t &h) const
{
  x = 0;
  y = 0;
  w = m_w;
  h = m_h;
}

void EvdevComputer::getCursorPos(std::int32_t &x, std::int32_t &y) const
{
  x = m_cursorX;
  y = m_cursorY;
}

void EvdevComputer::reconfigure(std::uint32_t activeSides)
{
  LOG_DEBUG("evdev: active sides: %s", sidesMaskToString(activeSides).c_str());
  m_activeSides = activeSides;
}

std::uint32_t EvdevComputer::activeSides()
{
  return m_activeSides;
}

void EvdevComputer::warpCursor(std::int32_t x, std::int32_t y)
{
  m_cursorX = x;
  m_cursorY = y;
}

std::uint32_t EvdevComputer::registerHotKey(KeyID key, KeyModifierMask mask)
{
  std::uint32_t id = ++m_nextHotkeyId;
  m_hotkeys[id] = {key, mask};
  LOG_DEBUG("evdev: registered hotkey %u key=0x%x mask=0x%x", id, key, mask);
  return id;
}

void EvdevComputer::unregisterHotKey(std::uint32_t id)
{
  LOG_DEBUG("evdev: unregistered hotkey %u", id);
  m_hotkeys.erase(id);
}

void EvdevComputer::fakeInputBegin()
{
  // nothing to do
}

void EvdevComputer::fakeInputEnd()
{
  // nothing to do
}

std::int32_t EvdevComputer::getJumpZoneSize() const
{
  return 1;
}

bool EvdevComputer::isAnyMouseButtonDown(std::uint32_t &buttonID) const
{
  if (m_buttons.none())
    return false;
  buttonID = std::countr_zero(m_buttons.to_ulong());
  return true;
}

void EvdevComputer::getCursorCenter(std::int32_t &x, std::int32_t &y) const
{
  x = m_w / 2;
  y = m_h / 2;
}

void EvdevComputer::fakeMouseButton(ButtonID, bool)
{
  // primary only
}

void EvdevComputer::fakeMouseMove(std::int32_t, std::int32_t)
{
  // primary only
}

void EvdevComputer::fakeMouseRelativeMove(std::int32_t, std::int32_t) const
{
  // primary only
}

void EvdevComputer::fakeMouseWheel(ScrollDelta) const
{
  // primary only
}

void EvdevComputer::enable()
{
  // devices are grabbed as soon as libinput opens them
}

void EvdevComputer::disable()
{
  stopRepeat();
}

void EvdevComputer::enter()
{
  m_isOnComputer = true;
}

bool EvdevComputer::canLeave()
{
  return true;
}

void EvdevComputer::leave()
{
  m_isOnComputer = false;
}

bool EvdevComputer::setClipboard(ClipboardID, const IClipboard *)
{
  return true;
}

void EvdevComputer::checkClipboards()
{
  // no local clipboard
}

void EvdevComputer::openScreensaver(bool)
{
  // no screen
}

void EvdevComputer::closeScreensaver()
{
  // no screen
}

void EvdevComputer::screensaver(bool)
{
  // no screen
}

void EvdevComputer::resetOptions()
{
  // no options
}

void EvdevComputer::setOptions(const OptionsList &)
{
  // no options
}

void EvdevComputer::setSequenceNumber(std::uint32_t)
{
  // no local clipboard, so sequence numbers are unused
}

bool EvdevComputer::isPrimary() const
{
  return true;
}

void EvdevComputer::updateButtons()
{
  m_buttons.reset();
}

IKeyState *EvdevComputer::getKeyState() const
{
  return m_keyState;
}

std::string EvdevComputer::getSecureInputApp() const
{
  return {};
}

void EvdevComputer::sendEvent(EventTypes type, void *data)
{
  m_events->addEvent(Event(type, getEventTarget(), data));
}

void EvdevComputer::handleSystemEvent(const Event &)
{
  if (m_hidpp) {
    std::uint64_t count = 0;
    [[maybe_unused]] auto n = read(m_hidppWake, &count, sizeof(count));
    for (const auto &press : m_hidpp->take()) {
      // no fallback: these controls have no evdev meaning of their own
      applyBinding(press.device, press.code, press.pressed);
    }
  }
  libinput_dispatch(m_libinput);
  libinput_event *event;
  while ((event = libinput_get_event(m_libinput)) != nullptr) {
    switch (libinput_event_get_type(event)) {
    case LIBINPUT_EVENT_DEVICE_ADDED:
      onDeviceAdded(libinput_event_get_device(event));
      break;
    case LIBINPUT_EVENT_DEVICE_REMOVED: {
      auto *device = libinput_event_get_device(event);
      LOG_INFO("evdev: device removed: %s", libinput_device_get_name(device));
      setDeviceStatus(std::string("/dev/input/") + libinput_device_get_sysname(device), {}, "", nullptr);
      if (auto it = std::find(m_keyboards.begin(), m_keyboards.end(), device); it != m_keyboards.end()) {
        libinput_device_unref(*it);
        m_keyboards.erase(it);
      }
      // a held button or key on the unplugged device never gets its release
      updateButtons();
      stopRepeat();
      break;
    }
    case LIBINPUT_EVENT_KEYBOARD_KEY:
      onKeyEvent(event);
      break;
    case LIBINPUT_EVENT_POINTER_BUTTON:
      onButtonEvent(event);
      break;
    case LIBINPUT_EVENT_POINTER_MOTION:
      onMotionEvent(event);
      break;
    case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
      onWheelEvent(event);
      break;
    case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
    case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS:
      onFingerScrollEvent(event);
      break;
    default:
      break;
    }
    libinput_event_destroy(event);
  }
}

void EvdevComputer::onDeviceAdded(libinput_device *device)
{
  if (libinput_device_config_accel_is_available(device)) {
    libinput_device_config_accel_set_speed(device, m_accelSpeed);
  }
  if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_KEYBOARD)) {
    m_keyboards.push_back(libinput_device_ref(device));
    updateLeds(m_keyState->pollActiveModifiers());
  }
}

void EvdevComputer::updateLeds(KeyModifierMask mask)
{
  int leds = 0;
  if (mask & KeyModifierCapsLock)
    leds |= LIBINPUT_LED_CAPS_LOCK;
  if (mask & KeyModifierNumLock)
    leds |= LIBINPUT_LED_NUM_LOCK;
  if (mask & KeyModifierScrollLock)
    leds |= LIBINPUT_LED_SCROLL_LOCK;
  for (auto *device : m_keyboards) {
    libinput_device_led_update(device, static_cast<libinput_led>(leds));
  }
}

bool EvdevComputer::onHotkey(KeyID key, bool isPressed, KeyModifierMask mask)
{
  // compare ignoring lock modifiers, so Caps Lock doesn't disable hotkeys
  const KeyModifierMask locks = KeyModifierCapsLock | KeyModifierNumLock | KeyModifierScrollLock;
  for (const auto &[id, hotkey] : m_hotkeys) {
    if (hotkey.first == key && (hotkey.second & ~locks) == (mask & ~locks)) {
      LOG_DEBUG("evdev: hotkey %u %s (key=0x%x mask=0x%x)", id, isPressed ? "down" : "up", key, mask);
      sendEvent(
          isPressed ? EventTypes::PrimaryComputerHotkeyDown : EventTypes::PrimaryComputerHotkeyUp, HotKeyInfo::alloc(id)
      );
      return true;
    }
  }
  return false;
}

void EvdevComputer::onKeyEvent(libinput_event *event)
{
  auto *kb = libinput_event_get_keyboard_event(event);
  std::uint32_t keycode = libinput_event_keyboard_get_key(kb);
  bool pressed = libinput_event_keyboard_get_key_state(kb) == LIBINPUT_KEY_STATE_PRESSED;
  if (!applyBinding(libinput_device_get_name(libinput_event_get_device(event)), keycode, pressed)) {
    handleKey(keycode, pressed);
  }
}

void EvdevComputer::handleKey(std::uint32_t keycode, bool pressed)
{
  // X keycodes are evdev keycodes + 8
  std::uint32_t keyval = keycode + 8;
  KeyID keyid = m_keyState->mapKeyFromKeyval(keyval);
  auto button = static_cast<KeyButton>(keyval);

  // same order as EiComputer: update xkb first, then report the resulting mask
  KeyModifierMask before = m_keyState->pollActiveModifiers();
  m_keyState->updateXkbState(keyval, pressed);
  KeyModifierMask mask = m_keyState->pollActiveModifiers();
  if ((before ^ mask) & (KeyModifierCapsLock | KeyModifierNumLock | KeyModifierScrollLock)) {
    updateLeds(mask);
  }

  LOG_VERBOSE("evdev: key %s keycode=%u keyid=0x%x mask=0x%x", pressed ? "press" : "release", keycode, keyid, mask);

  if (onHotkey(keyid, pressed, mask)) {
    return;
  }
  if (keyid == kKeyNone) {
    return;
  }

  m_keyState->sendKeyEvent(getEventTarget(), pressed, false, keyid, mask, 1, button);

  if (pressed && m_keyState->keyRepeats(keyval)) {
    startRepeat(keyid, button);
  } else if (!pressed && button == m_repeatButton) {
    stopRepeat();
  }
}

void EvdevComputer::startRepeat(KeyID key, KeyButton button)
{
  stopRepeat();
  m_repeatKey = key;
  m_repeatButton = button;
  m_repeatStarted = false;
  m_repeatTimer = m_events->newOneShotTimer(m_repeatDelay, nullptr);
  m_events->addHandler(EventTypes::Timer, m_repeatTimer, [this](const auto &) { onRepeatTimer(); });
}

void EvdevComputer::onRepeatTimer()
{
  if (!m_repeatStarted) {
    // switch from the initial delay to the repeat rate
    auto *oneShot = m_repeatTimer;
    m_events->removeHandler(EventTypes::Timer, oneShot);
    m_events->deleteTimer(oneShot);
    m_repeatStarted = true;
    m_repeatTimer = m_events->newTimer(m_repeatInterval, nullptr);
    m_events->addHandler(EventTypes::Timer, m_repeatTimer, [this](const auto &) { onRepeatTimer(); });
  }
  m_keyState->sendKeyEvent(
      getEventTarget(), true, true, m_repeatKey, m_keyState->pollActiveModifiers(), 1, m_repeatButton
  );
}

void EvdevComputer::stopRepeat()
{
  if (m_repeatTimer) {
    m_events->removeHandler(EventTypes::Timer, m_repeatTimer);
    m_events->deleteTimer(m_repeatTimer);
    m_repeatTimer = nullptr;
  }
  m_repeatKey = kKeyNone;
  m_repeatButton = 0;
}

void EvdevComputer::onButtonEvent(libinput_event *event)
{
  auto *p = libinput_event_get_pointer_event(event);
  std::uint32_t code = libinput_event_pointer_get_button(p);
  bool pressed = libinput_event_pointer_get_button_state(p) == LIBINPUT_BUTTON_STATE_PRESSED;
  if (!applyBinding(libinput_device_get_name(libinput_event_get_device(event)), code, pressed)) {
    handleButton(code, pressed);
  }
}

void EvdevComputer::handleButton(std::uint32_t code, bool pressed)
{
  ButtonID id = mapButton(code);
  if (id == kButtonNone) {
    LOG_DEBUG("evdev: unmapped button 0x%x", code);
    return;
  }
  m_buttons.set(id, pressed);
  sendEvent(
      pressed ? EventTypes::PrimaryComputerButtonDown : EventTypes::PrimaryComputerButtonUp,
      ButtonInfo::alloc(id, m_keyState->pollActiveModifiers())
  );
}

void EvdevComputer::pollControlFiles()
{
  const auto now = std::chrono::steady_clock::now();
  if (now - m_lastPoll < std::chrono::seconds(1)) {
    return;
  }
  m_lastPoll = now;

  struct stat st{};
  if (!m_bindingsPath.empty()) {
    const std::int64_t mtime = stat(m_bindingsPath.c_str(), &st) == 0 ? mtimeNs(st) : 0;
    if (mtime != m_bindingsMtime) {
      loadBindings();
    }
  }
  // the web UI touches this file while it waits for a press to identify;
  // only then is the last pressed input reported in the status file
  m_identify = !m_identifyPath.empty() && stat(m_identifyPath.c_str(), &st) == 0 &&
               std::abs(std::difftime(std::time(nullptr), st.st_mtime)) < 60;
}

void EvdevComputer::loadBindings()
{
  m_bindings.clear();
  if (m_bindingsPath.empty()) {
    return;
  }
  struct stat st{};
  m_bindingsMtime = stat(m_bindingsPath.c_str(), &st) == 0 ? mtimeNs(st) : 0;
  QFile file(QString::fromStdString(m_bindingsPath));
  if (!file.open(QIODevice::ReadOnly)) {
    return;
  }
  const auto doc = QJsonDocument::fromJson(file.readAll());
  for (const auto &value : doc.object().value("bindings").toArray()) {
    const auto obj = value.toObject();
    Binding b;
    b.device = obj.value("device").toString().toStdString();
    b.code = static_cast<std::uint32_t>(obj.value("code").toInt());
    for (const auto &code : obj.value("send").toArray()) {
      if (code.toInt() > 0 && code.toInt() < KEY_MAX) {
        b.send.push_back(static_cast<std::uint32_t>(code.toInt()));
      }
    }
    if (b.code > 0) {
      m_bindings.push_back(std::move(b));
    }
  }
  LOG_INFO("evdev: %zu input binding(s) from %s", m_bindings.size(), m_bindingsPath.c_str());
  if (m_hidpp) {
    std::set<std::pair<std::string, std::uint32_t>> bound;
    for (const auto &b : m_bindings) {
      if (b.code >= HidppListener::kCodeBase) {
        bound.insert({b.device, b.code});
      }
    }
    m_hidpp->setBound(std::move(bound));
  }
}

bool EvdevComputer::applyBinding(const std::string &name, std::uint32_t code, bool pressed)
{
  pollControlFiles();
  if (m_identify && pressed) {
    headless::updateStatus(
        {{"lastInput",
          QJsonObject{
              {"seq", static_cast<qint64>(++m_inputSeq)},
              {"device", QString::fromStdString(name)},
              {"code", static_cast<int>(code)},
              {"at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
          }}}
    );
  }

  const auto it = std::find_if(m_bindings.begin(), m_bindings.end(), [&](const Binding &b) {
    return b.code == code && (b.device.empty() || b.device == name);
  });
  if (it == m_bindings.end()) {
    return false;
  }
  // press in order and release in reverse, so modifiers wrap the key
  auto emit = [this, pressed](std::uint32_t c) {
    if (c >= BTN_MISC && c < KEY_OK) {
      handleButton(c, pressed);
    } else {
      handleKey(c, pressed);
    }
  };
  if (pressed) {
    std::for_each(it->send.begin(), it->send.end(), emit);
  } else {
    std::for_each(it->send.rbegin(), it->send.rend(), emit);
  }
  return true;
}

void EvdevComputer::onMotionEvent(libinput_event *event)
{
  auto *p = libinput_event_get_pointer_event(event);
  double dx = libinput_event_pointer_get_dx(p);
  double dy = libinput_event_pointer_get_dy(p);
  if (!std::isfinite(dx) || !std::isfinite(dy)) {
    return;
  }

  m_bufferDX += dx;
  m_bufferDY += dy;
  auto pixelDx = static_cast<std::int32_t>(m_bufferDX);
  auto pixelDy = static_cast<std::int32_t>(m_bufferDY);
  if (!pixelDx && !pixelDy) {
    return;
  }
  m_bufferDX -= pixelDx;
  m_bufferDY -= pixelDy;

  if (m_isOnComputer) {
    // move the virtual cursor; the server switches screens when it hits a linked edge
    m_cursorX = std::clamp(m_cursorX + pixelDx, 0, m_w - 1);
    m_cursorY = std::clamp(m_cursorY + pixelDy, 0, m_h - 1);
    sendEvent(EventTypes::PrimaryComputerMotionOnPrimary, MotionInfo::alloc(m_cursorX, m_cursorY));
  } else {
    sendEvent(EventTypes::PrimaryComputerMotionOnSecondary, MotionInfo::alloc(pixelDx, pixelDy));
  }
}

void EvdevComputer::onWheelEvent(libinput_event *event)
{
  // libinput and deskflow both use 120 per wheel click, with opposite signs
  auto *p = libinput_event_get_pointer_event(event);
  double v = 0;
  double h = 0;
  if (libinput_event_pointer_has_axis(p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
    v = libinput_event_pointer_get_scroll_value_v120(p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
  if (libinput_event_pointer_has_axis(p, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL))
    h = libinput_event_pointer_get_scroll_value_v120(p, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
  if (v != 0 || h != 0) {
    sendEvent(
        EventTypes::PrimaryComputerWheel, WheelInfo::alloc(-static_cast<int32_t>(h), -static_cast<int32_t>(v))
    );
  }
}

void EvdevComputer::onFingerScrollEvent(libinput_event *event)
{
  // touchpad scroll is in pixel-like units; 10 units == one wheel click, as in EiComputer
  static const double s_clicksPerUnit = 0.1;
  auto *p = libinput_event_get_pointer_event(event);
  if (libinput_event_pointer_has_axis(p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
    m_scrollY += libinput_event_pointer_get_scroll_value(p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL) * s_clicksPerUnit;
  if (libinput_event_pointer_has_axis(p, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL))
    m_scrollX +=
        libinput_event_pointer_get_scroll_value(p, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL) * s_clicksPerUnit;
  double cx = std::trunc(m_scrollX);
  double cy = std::trunc(m_scrollY);
  if (cx != 0 || cy != 0) {
    sendEvent(
        EventTypes::PrimaryComputerWheel,
        WheelInfo::alloc(-static_cast<int32_t>(cx) * s_scrollDelta, -static_cast<int32_t>(cy) * s_scrollDelta)
    );
    m_scrollX -= cx;
    m_scrollY -= cy;
  }
}

} // namespace deskflow
