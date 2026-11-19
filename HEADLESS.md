# Deskflow, headless server fork

This is a downstream fork of [Deskflow](https://github.com/deskflow/deskflow) that adds one thing: a server backend that needs no display. The shared keyboard and mouse plug into a small always-on box (here a Raspberry Pi 4), and every desktop becomes a normal Deskflow client.

## Why

Deskflow's server has to run on the machine the keyboard and mouse are plugged into, and it needs that machine's desktop session to capture input. In practice that means one of your real computers is the server, and when it's off, the others lose the shared keyboard.

With this fork the server runs on the Pi instead. Any desktop can be shut down and the rest keep working. The Pi has no monitor, no desktop and no login session. Nobody controls it directly.

It sits next to [Bragi](https://github.com/NickMarcha/bragi), which already runs on the same Pi and does the same job for audio: one USB headset shared across the network. Bragi is the audio half of the KVM; this fork is the keyboard and mouse half.

## What the fork changes

The `headless` branch carries a short stack of commits on top of the latest upstream release tag:

- `src/lib/platform/EvdevComputer.{h,cpp}`: a primary-only backend. It opens input devices through libinput's udev backend (pointer acceleration, hi-res scrolling, hotplug) and grabs them exclusively, so the Pi's text console never sees the input. It keeps a virtual cursor inside a virtual screen so edge switching and hotkeys work as usual. libinput drops the kernel's key repeat, so repeat is done in software.
- `src/lib/platform/EvdevEventQueueBuffer.{h,cpp}`: wakes Deskflow's event loop on the libinput fd.
- `ServerApp::createComputer()` picks the backend when `DESKFLOW_BACKEND=evdev` is set. Without it, Deskflow behaves exactly like upstream.
- `EiKeyState::keyRepeats()`: a small helper the backend uses.
- `CoreIpcServer` takes a `keystroke` command, and `Config::parseKeystrokeAction()` parses it (see "Sending keystrokes" below).
- `src/lib/common/HeadlessStatus.{h,cpp}`: when `DESKFLOW_STATUS_DIR` is set, the server writes `status.json` there: connected clients and their screen sizes, which computer has the cursor, every input device and whether it's used, and the result of the last config reload. Nothing is written when the variable is unset.
- `deskflow-core` blocks SIGHUP, SIGINT, SIGTERM and SIGUSR2 before Qt starts its threads, so they reach Arch's signal thread: SIGHUP reloads the server config and SIGTERM shuts down cleanly. In 1.27 they can land on a Qt thread and kill the process instead. This is upstream PR deskflow/deskflow#10382 (issue #10335), carried here until it is merged.
- `headless/systemd/`: the server's user unit, installed by the `.deb`. `headless/udev/`: rules for Logitech receivers, copied to `/etc/udev/rules.d/` by hand. Settings are edited from [Bragi](https://github.com/NickMarcha/bragi) (see "Settings" below); the fork's own web UI is gone.
- `.github/workflows/headless-*.yml`, this file and a banner on the README.

Clients are unchanged. Use stock Deskflow on Windows, macOS and Linux.

## Running it

The `.deb` installs systemd user units. Put the settings in `~/.config/deskflow-headless/server.ini` (computers, port, TLS) and the layout in the server config file it points to, then:

```sh
systemctl --user enable --now deskflow-headless
loginctl enable-linger "$USER"   # start at boot, without a login
```

The user needs read access to `/dev/input/event*` (the `input` group on Debian). By hand, without the units:

```sh
DESKFLOW_BACKEND=evdev QT_QPA_PLATFORM=offscreen \
  deskflow-core server -s ~/.config/deskflow-headless/server.ini
```

Options, read once at startup. The unit loads them from `~/.config/deskflow-headless/env`, which Bragi's KVM setup writes:

| Variable | Default | Meaning |
|---|---|---|
| `DESKFLOW_EVDEV_MATCH` | (all) | Comma-separated name substrings. Only matching devices are used. |
| `DESKFLOW_EVDEV_IGNORE` | (none) | Comma-separated name substrings to skip. |
| `DESKFLOW_EVDEV_SHAPE` | `1920x1080` | Size of the virtual screen. |
| `DESKFLOW_EVDEV_ACCEL` | `0` | libinput pointer speed, -1.0 to 1.0. |
| `DESKFLOW_EVDEV_REPEAT` | `500,30` | Key repeat delay in ms and rate in Hz. |
| `XKB_DEFAULT_LAYOUT` | `us` | Keyboard layout of the physical keyboard (libxkbcommon). |
| `DESKFLOW_EVDEV_HIDPP` | `0` | `1` reads extra Logitech controls (MX Master thumb and wheel-mode buttons) over HID++ so they can be bound. Needs read/write access to the receiver's hidraw node. On a Raspberry Pi 4 with a Bolt receiver this currently stops the mouse from moving while enabled. |
| `XKB_DEFAULT_OPTIONS` | `srvrkeys:none` | xkb options. The default keeps Ctrl+Alt+F1..F12 as F-keys instead of console switching, so hotkeys like `keystroke(Control+Alt+F12)` work. |

Only devices udev tags as a keyboard or mouse are considered. Headset volume buttons and HDMI-CEC remotes show up as "keys" but not as keyboards, so they're left alone.

In the server config, don't link anything into the Pi's own screen if you never want the cursor to land there, or use hotkeys such as `keystroke(control+alt+right) = switchToScreen(desk)`.

## Settings

The fork used to ship its own web UI for these files. [Bragi](https://github.com/NickMarcha/bragi), the desk hub on the same box, edits them now (Settings, KVM setup), behind its own login:

- **Layout and hotkeys** in the server config. Saving sends SIGHUP, so clients stay connected; `status.json` reports whether the reload worked, and a rejected file is put back. Grid positions are kept in `webui-layout.json` (the name is kept for existing setups).
- **Computers** (names, aliases) in `server.ini`, and **input options** in `env`. The server reads these at startup, so saving restarts it.
- **Bindings** in `bindings.json`: a key or button sends other key codes instead. The server reads the file again on the next press after it changes. While the file `identify` in the status directory is less than a minute old, the server reports the last key or button pressed, with its device, as `lastInput` in `status.json`.

Every save keeps the previous file in `~/.config/deskflow-headless/backups/`.

## Sending keystrokes and switching

Other programs on the box can make the server press and release a key combination on some computers, the same way a hotkey with a `keystroke(Control+Shift+Alt+M,sage:sage-dev)` action does, or make a computer the active one. They write one line to `deskflow-core`'s IPC socket, `$DESKFLOW_STATUS_DIR/core.sock` (with the unit: `$XDG_RUNTIME_DIR/deskflow-headless/core.sock`), and get back `ok` or `error=<message>`:

```sh
printf 'keystroke=Control+Shift+Alt+M,sage:sage-dev:sagedeck\n' | socat - UNIX-CONNECT:/run/user/1000/deskflow-headless/core.sock
printf 'switch=sage\n' | socat - UNIX-CONNECT:/run/user/1000/deskflow-headless/core.sock
```

`keystroke` uses the config file's key syntax; the computers take names or aliases, `*` means every connected client, and leaving them out sends the keys to the computer with the cursor. Upstream makes its IPC socket world-writable for its GUI, so the server only accepts these commands from processes of its own user (checked with `SO_PEERCRED`). The key runs through `InputFilter::KeystrokeAction`, the code that runs a `keystroke()` hotkey action.

A hotkey can also tell such a program that it was pressed, instead of doing something inside Deskflow: `keystroke(Control+Alt+F8) = notify(mute)` sends `action=mute` to every client connected to the socket at that moment (none connected: nothing happens, it isn't queued). Bragi keeps a connection open for these and runs its own action for the name.

Stock Linux clients that use libei (KDE and GNOME on Wayland) drop these keys while the cursor is on another computer, reported upstream as deskflow/deskflow#10393. Windows clients get them either way.

## Logitech devices (HID++)

The server talks to Logitech receivers and devices over HID++ 2.0 for two things, in short sessions that open the receiver's hidraw node for a few milliseconds and close it again:

- **Remaps.** Extra controls like the MX Master's thumb button send nothing a normal driver can see. A remap tells the device's own firmware to make the control act as another one (for example thumb button as forward), so it arrives as a normal button and bindings can use it. Devices forget this when switched off, so the server checks again whenever a Logitech device's input resumes after two seconds of quiet, retries while the device reconnects, and checks once a minute while it's idle. Stored as `remaps` in `bindings.json`.
- **Battery.** Read on those checks and every 15 minutes while idle, and written to `status.json`.

Both need read/write access to the receiver's hidraw node, for example with this udev rule:

```
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", ATTRS{idVendor}=="046d", MODE="0660", GROUP="input"
```

`DESKFLOW_EVDEV_HIDPP_MANAGER=0` turns this off.

`DESKFLOW_EVDEV_HIDPP=1` goes further: it keeps the node open and diverts bound controls, so a control like the thumb button becomes an input of its own (bind it in `bindings.json`) instead of borrowing another button's identity through a remap. It's off by default because of a Raspberry Pi 4 limitation: its USB controller services only three of a Logi Bolt receiver's four 1 ms interrupt endpoints at once, and with all four active the mouse stops moving. `headless/udev/71-logitech-bolt-no-touchpad.rules` disables the receiver's interface 3 (only used by Logitech touchpads), which leaves room. `headless/udev/70-logitech-hidraw.rules` gives the `input` group access to the hidraw nodes.

## Staying in sync with upstream

- `master` is a plain mirror of upstream. Nothing is committed to it.
- `headless` is rebased onto each new upstream release tag. The patch is almost entirely new files, so upstream changes usually break the compile (which CI catches) rather than the rebase.
- `headless-upstream-check.yml` runs weekly and opens an issue when upstream has a release newer than the one `headless` is based on.
- `headless-build.yml` builds the Debian trixie `.deb` for arm64 and amd64 on every push to `headless`, and publishes both as one GitHub release when run manually with `release: true`.

Rebasing by hand:

```sh
git fetch upstream --tags
git rebase --onto vNEW vOLD headless
git push --force-with-lease origin headless
```

## Status

In daily use on a Raspberry Pi 4 (Debian 13, Deskflow 1.27.0) since November 2026, with three stock clients: one Windows, two KDE Wayland. Edge switching, typing with modifiers, key repeat, buttons, wheel, hotkeys and hotplug all work. Bragi on the same box edits the settings, switches computers and handles the MX Master's extra buttons itself (over HID++, with `DESKFLOW_EVDEV_HIDPP` off). Still to come: settings keys instead of environment variables, and then asking upstream whether they want the backend.
