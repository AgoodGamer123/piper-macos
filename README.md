# Piper for macOS

## TLDR

This is a vibecoded port of piper to macos

A native macOS app to configure gaming mice: DPI steps, button mappings,
macros, LEDs, report rate and onboard profiles. It is a SwiftUI front end to
[libratbag](https://github.com/libratbag/libratbag), the library behind the
Linux app [Piper](https://github.com/libratbag/piper), ported to macOS.

It supports the mice libratbag supports: most Logitech G-series mice
(wired, and wireless through Lightspeed and Unifying receivers), plus many
from SteelSeries, Roccat, ASUS, G.Skill, Glorious/Sinowealth and others. See
[the device list](libratbag/data/devices).

Changes are written to the mouse's onboard memory, so they keep working on
any computer, with nothing running in the background.

## Install

1. Download `Piper-<version>.dmg` from
   [Releases](../../releases), open it and drag Piper to Applications.
2. The app is not notarized by Apple, so macOS blocks it the first time.
   Open **System Settings → Privacy & Security**, scroll down and click
   **Open Anyway** next to the message about Piper. (Or, in Terminal:
   `xattr -dr com.apple.quarantine /Applications/Piper.app`.)

Requires macOS 13 Ventura or later, on Apple Silicon or Intel.

Logitech mice don't need any special permission. A few other mice can only
be reached through their keyboard interface; for those, macOS asks to allow
Piper under **Privacy & Security → Input Monitoring**.

## Using it

- The sidebar lists your mice and their onboard profiles. The check mark
  shows the profile the mouse is using now.
- Make changes in **Resolutions**, **Buttons**, **LEDs** and **Advanced**,
  then click **Apply** (⌘S) to write them to the mouse. Unsaved changes are
  marked with an orange dot.
- Wireless mice must be switched on and awake to show up; Piper notices
  them within a few seconds.

## Building

Needs Xcode (or the Command Line Tools) and nothing else.

```sh
scripts/build-app.sh                # build/Piper.app, universal
ARCHS=arm64 scripts/build-app.sh    # this Mac only, faster
scripts/make-dmg.sh                 # build/Piper-<version>.dmg
```

During development, `swift run` starts the app straight from the package,
but without the bundled device files; point it at them with
`LIBRATBAG_DATA_DIR=$PWD/libratbag/data/devices swift run`.

Every push is built by GitHub Actions; pushing a tag like `v1.0.0` publishes
a release with the DMG.

### Debugging

```sh
PIPER_RATBAG_DEBUG=1 /Applications/Piper.app/Contents/MacOS/Piper     # libratbag debug log
PIPER_RATBAG_DEBUG=raw /Applications/Piper.app/Contents/MacOS/Piper   # ... plus every HID report
```

`libratbag/tools/ratbag-probe` (built with meson, see below) lists every HID
device and dumps what libratbag finds, without the app.

### Testing without a mouse

`libratbag/tools/hidpp-emu.py` emulates a Logitech G703, wired and behind a
Lightspeed receiver, including its onboard profile memory. With
`RATBAG_EMU_SOCKET` set, libratbag talks to the emulator instead of real
devices:

```sh
python3 libratbag/tools/hidpp-emu.py --socket /tmp/emu.sock --mode both &
RATBAG_EMU_SOCKET=/tmp/emu.sock /Applications/Piper.app/Contents/MacOS/Piper
```

## How it works

```
Piper.app (SwiftUI)            Sources/Piper/
   │  Swift calls the C API directly
libratbag (C)                  libratbag/
   │  drivers: Logitech HID++, SteelSeries, Roccat, ...
   │  portable HID layer       libratbag/src/libratbag-hid-portable.c
IOKit IOHIDDevice              libratbag/src/libratbag-hid-iokit.c
```

libratbag on Linux depends on udev, the kernel's hidraw devices, systemd's
D-Bus library and GLib. The copy in `libratbag/` adds a portable build
(`meson -Dhid-backend=portable`, or the Swift package here) that needs none
of them:

- **HID access** goes through IOKit. Each open device is handed to the
  unchanged drivers as a file descriptor that reads and writes HID reports,
  backed by a socketpair and a small thread per device.
- **Wireless receivers**: Linux splits a Logitech receiver into one device
  per paired mouse; macOS doesn't, so libratbag asks the receiver what is
  paired and addresses the mouse by its slot.
- **No GLib**: the `.device` files are read by a small reader that behaves
  exactly like GLib's (`libratbag/test/keyfile-compat-test.c` checks it on
  every file).

The library-side changes are written to be upstreamable to libratbag; the
Linux build is unaffected and its tests run in CI.

### Building libratbag on its own

```sh
brew install meson ninja
cd libratbag
meson setup build -Dhid-backend=portable
ninja -C build
LIBRATBAG_DATA_DIR=$PWD/data/devices build/ratbag-probe -v
```

## License

Piper for macOS is GPL-2.0-or-later (see [COPYING](COPYING)), as it uses
Piper's device artwork (`Resources/svgs`). libratbag is MIT licensed (see
[libratbag/COPYING](libratbag/COPYING)).

Based on [libratbag](https://github.com/libratbag/libratbag) and
[Piper](https://github.com/libratbag/piper) by their contributors. Not
affiliated with Logitech or any other device maker.
