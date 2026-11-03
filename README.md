# omarchy-edges

Turn the edges of your laptop touchpad into sliders and hot corners on
[Omarchy](https://omarchy.org).

```
        ┌───────────────────────────────┐ ← double-tap: mute
        │                               │
  bright│                               │volume
  ness  │                               │
        │ ← ─ ─ swipe: prev / next ─ ─ →│
        └───────────────────────────────┘
```

| Gesture | Result |
|---|---|
| Slide up/down on the left edge | Brightness, with the Omarchy OSD |
| Slide up/down on the right edge | Volume, with the Omarchy OSD |
| Double-tap the top-right corner | Mute / unmute |
| Swipe along the bottom edge | Next / previous track |
| Double-tap the bottom edge | Play / pause (off by default) |

- A slide starts from the current level and is absolute: move your finger back to where you started and you get the old level back.
- The pointer does not move while you slide.
- Two-finger scrolling, clicks and gestures that start on an edge still work. A second finger, a physical click, or moving inward hands the touch back to Hyprland.

> This is a community project. It is not part of Omarchy and has no
> affiliation with the EdgeBar or SlideSense macOS apps that inspired it.

## Install

```bash
git clone https://github.com/m1q/omarchy-edges.git
cd omarchy-edges
./install.sh
```

The installer builds the daemon and runs the tests, then installs:

| What | Where |
|---|---|
| Daemon | `~/.local/bin/omarchy-edgesd` |
| Config | `~/.config/omarchy-edges/edges.conf` |
| User service | `~/.config/systemd/user/omarchy-edges.service`, started with your graphical session |
| udev rule (the only step that asks for `sudo`) | `/etc/udev/rules.d/70-omarchy-edges.rules` |

The udev rule tags touchpads and `/dev/uinput` with `uaccess`. That gives the
user logged in at the seat access to those devices, and to nothing else. It
never touches keyboards.

The installer leaves `~/.config/hypr` and `/usr/share/omarchy` alone.

To build it you need `gcc`, `make`, `pkg-config` and `libevdev`. Omarchy
already ships `libevdev`. If the compiler is missing, run
`sudo pacman -S --needed base-devel`.

## Use

```bash
systemctl --user status omarchy-edges   # running?
journalctl --user -u omarchy-edges -f   # logs
omarchy-edgesd --check                  # your pad's size and the band widths in mm
systemctl --user reload omarchy-edges   # apply config changes
systemctl --user stop omarchy-edges     # give the touchpad back immediately
```

To watch gesture decisions live, stop the service and run
`omarchy-edgesd --debug`.

## Configure

Every option is listed and commented in `~/.config/omarchy-edges/edges.conf`.
The common ones:

| Option | Meaning |
|---|---|
| `left`, `right` | `volume`, `brightness` or `none`. Swap the sides or turn one off. |
| `edge_width` | Band width as a fraction of the pad width (default `0.06`). |
| `active_area` | `0.25` lets slides start only in the bottom quarter. |
| `travel` | How much of the pad height a full 0–100% sweep takes. |
| `step` | Level granularity in percent. |
| `quick_mute`, `media_strip`, `media_reverse`, `media_double_tap` | Turn the extras on or off. |

## How it works

`omarchy-edgesd` grabs the physical touchpad with `EVIOCGRAB`. It then creates
a copy with uinput called **Omarchy Edges Touchpad**, with the same
properties, resolution and bus as the real pad.

Every touch that is not an edge gesture goes to the copy unchanged. Hyprland
and libinput treat the copy as your built-in touchpad, so these keep working:
- tap-to-click
- disable-while-typing
- three-finger gestures
- your `input.touchpad` settings

Touches that start on an edge are held back until their direction is clear.
A slide along the band is a gesture. Anything else goes to Hyprland.

It uses Omarchy's own tools, so it behaves like the keyboard keys:

| Feature | Omarchy tool |
|---|---|
| Volume | `pactl`, on the sink picked by `omarchy-audio-output-sink` |
| Brightness | `brightnessctl`, on the device picked by `omarchy-hw-display`. It never goes below 1%. |
| On-screen display | `omarchy-osd` |
| Mute | `omarchy-audio-output-volume mute-toggle` |
| Media | `omarchy-shell media next / previous / playPause` |

Commands run on worker threads, and slider updates are coalesced. A slow
command never stalls the touchpad.

The virtual device's name contains "Touchpad", so Omarchy's touchpad toggle
(`XF86TouchpadToggle`) finds it. On some laptops the touchpad's own name
doesn't contain the word, and this fixes the toggle for them.

## Troubleshooting

- **The pointer feels wrong:** run `systemctl --user stop omarchy-edges`. The real touchpad comes back instantly.
- **"permission denied" in the logs:** the udev rule is missing. Run `./install.sh` again.
- **Something else already grabbed the touchpad:** another remapper (for example edgepad) owns it. Stop one of them.
- **Accidental slides:** lower `edge_width`, or set `active_area = 0.5`.

## Tests

```bash
make test   # gesture engine: 72 checks under ASan + UBSan
make e2e    # through the real kernel with a fake uinput clickpad (needs the udev rule)
```

## Roadmap

- Workspace scrubber: slide along the bottom edge to move through workspaces.
- Per-app edges, based on `hyprctl activewindow`: tabs in the browser, scrollback in the terminal, windows elsewhere.
- Corners as hot buttons: switch keyboard layout, screenshot, lock, Omarchy menu.
- Text-caret scrubbing with the arrow keys.

## Limitations

- No haptics. Most Linux laptop touchpads have no haptic motor.
- Quick taps on an edge are swallowed, so tap a little further in to click.
- There is no "hold a key for fine steps" mode. Reading the keyboard would need keyboard access, which this deliberately does not ask for. Lower `step` instead.

## Uninstall

```bash
./install.sh --uninstall   # keeps ~/.config/omarchy-edges
```

## License

MIT
