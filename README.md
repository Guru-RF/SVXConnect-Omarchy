# SVXConnect for Omarchy

A desktop client for **SvxLink reflectors** (protocol v3: mTLS, AES-GCM, Opus),
made for [Omarchy](https://omarchy.org). It is a port of
[SVXConnect-Qt](https://github.com/Guru-RF/SVXConnect-QT): the same window, the
same reflector core, the same configuration. Two things are new:

- **It looks like Omarchy and follows your theme.** It uses the same palette,
  font, corner rounding and control styling as the Omarchy shell. When you run
  `omarchy theme set`, `omarchy font set` or change the text size, the open
  window restyles itself without a restart.
- **Push-to-talk goes through Hyprland.** Hold a key anywhere on the desktop to
  transmit, and let go to stop. The key is a normal bind in
  `~/.config/hypr/bindings.lua`, and SVXConnect can write that bind for you.
- **It tells you when someone starts talking.** While the window is closed to
  the tray, a desktop notification names the station and its talkgroup; click it
  to bring the window back. Your own transmissions never notify, and you can
  turn it off in Preferences › General.

## How it fits together

- **It uses the CLI's core.** The reflector client, mTLS, AES-GCM, Opus, the
  jitter buffer, the talkgroup state machine, the control FIFO, the run lock and
  the status export are compiled from
  [SVXConnect-CLI](https://github.com/Guru-RF/SVXConnect-CLI) into a static
  library. The protocol is not reimplemented.
- **It shares one identity.** It uses the same `~/.config/svxconnect/svxconnect.conf`
  and the same `~/.config/svxconnect/pki/`, so you enrol once. You configure and
  enrol with the CLI; this app reads and writes the same file.
- **Only one client may be connected at a time.** A certificate and node id can't
  be used twice at once. So the CLI, the headless service and the desktop
  clients all take a shared run lock, and one that starts while another holds
  the lock refuses and names who has it.

## Following the Omarchy theme

The window reads the same files the Omarchy shell does:

| Source | Used for |
|---|---|
| `~/.local/state/omarchy/current/theme/colors.toml` | background, foreground, accent, and the red / green / yellow status colours |
| `~/.local/state/omarchy/current/theme/shell.toml` | control fill and border alphas, menu and tooltip borders, font base size, spacing scale |
| `~/.config/omarchy/shell.toml` | your machine-level overrides, e.g. from `omarchy display text size` |
| `hyprctl getoption decoration:rounding` | corner radius |
| `fc-match monospace` | the font, as set by `omarchy font set` |

Status colours keep their meaning in every theme: **red** means transmitting,
**green** means linked, **yellow** means reconnecting or locked. The actual shades
come from the theme, so they always read well on its background.

The window has no menu bar. Its actions are in the icon buttons at the top
right, and each one has a shortcut:

| Shortcut | Action |
|---|---|
| `Space` | toggle transmit (in the window) |
| `Esc` | stop transmitting |
| `Ctrl+,` | preferences |
| `Ctrl+E` | edit `svxconnect.conf` in your Omarchy editor |
| `Ctrl+L` | show the log |
| `Ctrl+R` | reconnect |
| `Ctrl+K` | lock the talkgroup |
| `Ctrl+Shift+S` | show or hide the sidebar |
| `Ctrl+Q` | quit |

## Push-to-talk on Hyprland

SVXConnect registers a global shortcut named **`SVXConnect:ptt`** through
`xdg-desktop-portal-hyprland`. Nothing fires until a Hyprland bind points a key
at it. You can add the bind yourself:

```lua
-- ~/.config/hypr/bindings.lua
hl.bind("SUPER + GRAVE", hl.dsp.global("SVXConnect:ptt"), { description = "SVXConnect push-to-talk" })
```

Or open **Preferences › Push-to-talk**, choose a key and press **Bind in
Hyprland**. SVXConnect then:

1. backs up `bindings.lua`
2. adds one marked block
3. reloads Hyprland
4. puts the file back if Hyprland reports a config error

If the key is already in use, the block unbinds the old binding first and notes
what it did. SVXConnect refuses keys you type, like a bare Enter or Space, so it
can never transmit while you type.

Hyprland sends SVXConnect both the key press and the key release, so holding the
key transmits and releasing it stops. The default suggestion, `SUPER + GRAVE`,
is free in a stock Omarchy. To use CapsLock, set `kb_options = "caps:super"` in
`~/.config/hypr/input.lua`; CapsLock + ` then works as the key.

As a safety net, a watchdog stops transmitting after `tx_timeout_sec` (120 s by
default). It also stops if the portal session is lost, because a missed key
release would otherwise leave the transmitter on.

**SVXConnect must be running for the key to work.** Closing the window hides it
to the tray in the Omarchy bar. Use **Quit** from the tray or `Ctrl+Q` to exit.

The control FIFO works without the portal. It is the better choice for a foot
switch or a script:

```lua
hl.bind("F12", hl.dsp.exec_cmd("echo 'ptt on' > ~/.local/state/svxconnect/ctl"))
hl.bind("F12", hl.dsp.exec_cmd("echo 'ptt off' > ~/.local/state/svxconnect/ctl"), { release = true })
```

## The enhanced reflector, and the map

A plain SvxLink reflector tells you who is talking on the talkgroups you
monitor, from the moment you connect. It does not know where anybody is, and it
remembers nothing — a station that keyed up two minutes before you connected
never happened as far as your client is concerned.

Some reflectors run a portal alongside them that publishes the same traffic as
JSON over a WebSocket, with node positions and a rolling 24 hours of history.
SVXConnect finds it by itself: there is nothing to configure and no extra host
to type. It derives the URL from the reflector you already gave it —
`be.svx.link` becomes `wss://reflector.be.svx.link/` — connects, and waits five
seconds for the portal's opening snapshot. If one arrives, this is an enhanced
reflector; if not, it is a plain one, and the client retries every fifteen
seconds in case the portal comes back.

When the feed is up:

- **Recent** is replaced by **Reflector · 24h**, the portal's own history. It
  covers *every* talkgroup the reflector saw, not only the ones you monitor,
  and it goes back before you connected. Rows still switch talkgroup when
  clicked.
- **the map** has something to draw: one marker per node the reflector knows
  the position of, the current talker highlighted and labelled, and your own
  station marked separately.

When it is not up, nothing changes: the local Recent list stays, and the map
stays folded. Preferences → Connection has a switch to turn the feed off
entirely, which is how you see exactly what a plain reflector gives you.

### The map pane

It lives under the push-to-talk controls and is folded away by default. In its
automatic mode it folds out when there is both something to show and room to
show it — room being measured against what the rest of the window needs, not a
number of pixels, so it holds up under a theme with a larger font. A tiled
half-screen window keeps all its height for the talkgroups and the activity
list. **Show map** (`Ctrl+M`) opens or closes it explicitly, and that choice
then sticks.

Drag to pan, scroll to zoom, hover a marker for its callsign and coordinates.
The tiles come from OpenStreetMap, cached on disk between runs; under a dark
Omarchy theme they are turned over into a dark basemap in the client, because
the ready-made dark tile services now want an API key and a map that stops
working when a key expires is worse than no map.

## Installing

Every way below installs two packages. `svxconnect-omarchy` is the desktop app,
with its launcher entry. `svxconnect` is the terminal client, which you use once
to create the configuration and enrol.

### From repo.rf.guru (recommended)

RF.Guru's own signed package repository. Once it's set up, updates arrive with
your normal system update.

Trust the repository's signing key once:

```sh
curl -fsSLO https://repo.rf.guru/repo-key.asc
sudo pacman-key --add repo-key.asc
sudo pacman-key --lsign-key 878DC4CB2070252EFC08E2AEAFD96E81AB70B5CA
```

Add the repository to the end of `/etc/pacman.conf`:

```ini
[guru]
SigLevel = Required
Server = https://repo.rf.guru/arch/$repo/os/$arch
```

Then install:

```sh
sudo pacman -Sy svxconnect-omarchy
```

### From the AUR

```sh
yay -S svxconnect-omarchy
```

### From a GitHub release

Each [release](https://github.com/Guru-RF/SVXConnect-Omarchy/releases) has
ready-built x86_64 packages. Download both `.pkg.tar.zst` files, then:

```sh
sudo pacman -U svxconnect-omarchy-*.pkg.tar.zst svxconnect-*.pkg.tar.zst
```

### Building the packages yourself

The PKGBUILDs published to the AUR are in [packaging/aur](packaging/aur). They
build from the tagged releases, CLI first:

```sh
git clone https://github.com/Guru-RF/SVXConnect-Omarchy
cd SVXConnect-Omarchy/packaging/aur/svxconnect && makepkg -si
cd ../svxconnect-omarchy && makepkg -si
```

## Building

```sh
sudo pacman -S --needed base-devel cmake qt6-base qt6-svg qt6-websockets qt6-wayland opus openssl

git clone --recurse-submodules https://github.com/Guru-RF/SVXConnect-Omarchy
cd SVXConnect-Omarchy
cmake -B build
cmake --build build
ctest --test-dir build
```

To build against a sibling CLI working tree instead of the pinned submodule:

```sh
cmake -B build -DCLI_DIR=../SVXConnect-CLI
```

You don't need any audio development packages: miniaudio loads `libpulse.so.0`
(served by PipeWire on Omarchy) or `libasound.so.2` at runtime.

`SVX_WINDOW_ONLY=1 ./build/svxconnect-omarchy` opens the window without starting
the core. This is useful for interface and theme work on a machine without a
certificate.

The global shortcut needs `SVXConnect.desktop` in an applications directory,
because the portal identifies the app by that file. The portal also ignores an
entry whose `Exec=` program it can't find on PATH. So when running from a build
tree, install the entry pointing at the build:

```sh
sed "s|^Exec=.*|Exec=$PWD/build/svxconnect-omarchy %u|" data/SVXConnect.desktop \
    > ~/.local/share/applications/SVXConnect.desktop
```

`SVX_SCREENSHOT=out.png` (with `SVX_WINDOW_ONLY=1` and `QT_QPA_PLATFORM=offscreen`)
renders the window to a file and quits. `SVX_SCREENSHOT_PREFS=<tab>` also
renders the preferences dialog on that tab. `SVX_OMARCHY_THEME_DIR` previews
any theme directory without switching the desktop:

```sh
QT_QPA_PLATFORM=offscreen SVX_WINDOW_ONLY=1 \
SVX_OMARCHY_THEME_DIR=/usr/share/omarchy/themes/gruvbox \
SVX_SCREENSHOT=gruvbox.png ./build/svxconnect-omarchy
```

## First run

Create the configuration and enrol with the CLI first:

```sh
svxconnect --init-config   # asks for your callsign, email and reflector
svxconnect --enroll        # sends a CSR; wait for the sysop to sign it
svxconnect-omarchy
```

`--init-config` writes a fully commented `~/.config/svxconnect/svxconnect.conf`.
The desktop app reads and writes the same file; its **Edit configuration**
(`Ctrl+E`) opens it in your Omarchy editor.

### Your station position

The reflector plots you from latitude and longitude, and the grid square in the
header is derived from them, so **Preferences → Connection → Station position**
offers two modes rather than expecting you to read coordinates off a map.

**Automatic** follows the location you already gave Omarchy for its weather
panel (`~/.local/state/omarchy/settings/weather.json`, written by
`omarchy-weather-location`). The three fields show what it resolved to and stop
being editable; change the location with `omarchy-weather-location` and
SVXConnect picks it up at its next start, writing it to `svxconnect.conf` so
the CLI agrees.

**Manual** is the default: type the fields yourself, or press
**Find my position…** and let the dialog fill them:

- **Look up address** — type street and number, postcode, city and country.
  The lookup goes to OpenStreetMap's [Nominatim][nominatim], the one free
  geocoder that resolves a house number, and offers every match with its
  coordinates so you can pick the right street.
- **Detect automatically** — takes the Omarchy weather location once, as a
  starting point, without following it afterwards. When Omarchy has no
  coordinates it asks ipapi.co, which places you in roughly the right town and
  says so.

Nothing leaves the machine until you press one of those buttons. Picking a
result fills the location, latitude and longitude fields, and the grid square
updates as you watch.

[nominatim]: https://nominatim.org/

## Licence

MIT. See [LICENSE](LICENSE) and [THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES).

Copyright (c) 2026 Diëlectricum BV. Written by Joeri Van Dooren, ON6URE.

SVXConnect for Omarchy is built with the **Qt toolkit**, © The Qt Company Ltd
and contributors, used under the **GNU Lesser General Public License version 3**.
Qt is linked dynamically and unmodified; you may replace the Qt libraries with
modified versions and relink. Only LGPL Qt modules are used. This is checked at
configure time by [cmake/LicenceGuard.cmake](cmake/LicenceGuard.cmake), and
again on the linked binary by [tools/licence_guard.sh](tools/licence_guard.sh).
