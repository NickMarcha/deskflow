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
- `.github/workflows/headless-*.yml`, this file and a banner on the README.

Clients are unchanged. Use stock Deskflow on Windows, macOS and Linux.

## Running it

```sh
DESKFLOW_BACKEND=evdev QT_QPA_PLATFORM=offscreen \
  deskflow-core server -s ~/.config/Deskflow/headless.conf
```

The user needs read access to `/dev/input/event*` (the `input` group on Debian).

Options, read once at startup (environment variables for now):

| Variable | Default | Meaning |
|---|---|---|
| `DESKFLOW_EVDEV_MATCH` | (all) | Comma-separated name substrings. Only matching devices are used. |
| `DESKFLOW_EVDEV_IGNORE` | (none) | Comma-separated name substrings to skip. |
| `DESKFLOW_EVDEV_SHAPE` | `1920x1080` | Size of the virtual screen. |
| `DESKFLOW_EVDEV_ACCEL` | `0` | libinput pointer speed, -1.0 to 1.0. |
| `DESKFLOW_EVDEV_REPEAT` | `500,30` | Key repeat delay in ms and rate in Hz. |
| `XKB_DEFAULT_LAYOUT` | `us` | Keyboard layout of the physical keyboard (libxkbcommon). |

Only devices udev tags as a keyboard or mouse are considered. Headset volume buttons and HDMI-CEC remotes show up as "keys" but not as keyboards, so they're left alone.

In the server config, don't link anything into the Pi's own screen if you never want the cursor to land there, or use hotkeys such as `keystroke(control+alt+right) = switchToScreen(desk)`.

## Staying in sync with upstream

- `master` is a plain mirror of upstream. Nothing is committed to it.
- `headless` is rebased onto each new upstream release tag. The patch is almost entirely new files, so upstream changes usually break the compile (which CI catches) rather than the rebase.
- `headless-upstream-check.yml` runs weekly and opens an issue when upstream has a release newer than the one `headless` is based on.
- `headless-build.yml` builds the Debian trixie arm64 `.deb` on every push to `headless`, and publishes it as a GitHub release when run manually with `release: true`.

Rebasing by hand:

```sh
git fetch upstream --tags
git rebase --onto vNEW vOLD headless
git push --force-with-lease origin headless
```

## Status

Working in a spike on a Raspberry Pi 4 (Debian 13, Deskflow 1.27.0): edge switching, typing with modifiers, key repeat, buttons, wheel, hotkeys and hotplug all verified against a stock client. Still to come: real-hardware stability testing, settings keys instead of environment variables, and then asking upstream whether they want the backend.
