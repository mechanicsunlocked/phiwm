# phiwm

Dwindle tiling for Windows 11. Every new window splits the focused one at the golden ratio,
so the existing window keeps 61.8% and windows spiral in toward the bottom-right. One layout,
one set of keys, nothing to configure: install it and it works.

- Plain C, one ~100 KB exe, no services, no helper daemons, no runtime, no config file,
  no system setting touched.
- Workspaces 1–10, bound to monitors the way Hyprland does it.
- Native Windows 11 border colors and rounded corners — no overlay windows.
- Tray icon shows the focused workspace number.

## Install

```
winget install phiwm
phiwm
```

Tray menu → **Start with Windows** to autostart.

## Keys

| Keys | Action |
|---|---|
| Win + Enter / B / E | Terminal (`wt`) / browser / Explorer |
| Win + W | Close window |
| Win + arrows | Move focus (crosses monitors) |
| Win + Shift + arrows | Swap window (moves to the next monitor at the edge) |
| Win + 1…0 | Workspace 1–10 |
| Win + Shift + 1…0 | Move window to workspace and follow |
| Win + Tab / Shift+Tab | Next / previous used workspace |
| Win + J | Toggle split direction |
| Win + V | Toggle floating (replaces clipboard history) |
| Win + P | Pseudotile |
| Win + F / Shift+F | Fullscreen / maximize |
| Win + − / = (+Shift) | Shrink / grow width (height) |
| Win + Alt + Tab | Cycle windows on the workspace |
| Win + Ctrl + R | Refresh: rescan monitors, retile, reinstall the keyboard hook |
| Win + Ctrl + Shift + Q | Quit (shows every hidden window) |

The digit keys are bound by position, so Win+1…0 is the top row on any keyboard layout.

Mouse: drag a tile's border to resize the split, drag its title bar onto another tile to swap,
or onto another monitor to move it there.

## Built in, not configured

Everything that used to be a config file is one table in `src/defaults.c`: the keys above,
the layout constants (gaps 5/10 px, golden ratio, new windows go right/bottom, splits keep
their direction) and the window rules. Change it there and rebuild.

Window rules: Picture-in-Picture windows, Calculator, Explorer's copy dialog and PowerToys
windows float. Dialogs, tool windows and fixed-size windows float on their own.

From scripts: `phiwm dispatch workspace 3`, `phiwm quit`. Dispatchers: `exec`, `killactive`,
`movefocus`, `movewindow`, `swapwindow`, `togglesplit`, `togglefloating`, `pseudo`,
`fullscreen 0|1`, `workspace`, `movetoworkspace`, `movetoworkspacesilent`, `focusmonitor`,
`splitratio`, `resizeactive`, `cyclenext`, `layoutmsg togglesplit`, `refresh`, `exit`.
Workspace arguments: `3`, `+1`, `-1`, `e+1`, `e-1`, `previous`, `empty`.

## How it behaves

- Windows on hidden workspaces are hidden, so they leave the taskbar — like Hyprland.
  If phiwm is killed, the next start brings them back; a normal quit does it right away.
- Workspace switches are instant: DWM's fade-in/fade-out is switched off per managed window
  (nothing system-wide) and a revealed window is moved and shown in one step.
- Switching to an empty workspace shows the bare desktop. The tray number says where you are.
- A new window gets the focus, like in Hyprland, so the next one splits it: the spiral.
- Focus follows the mouse, like in Hyprland: hovering a window focuses it. Menus, popups,
  dialogs and Start keep the focus, nothing happens while a button is held or a window is
  being dragged, and the desktop between the gaps is ignored. When phiwm moves the focus
  by keyboard, the cursor goes along, so the focus does not jump straight back.
- Win+F covers the whole monitor, taskbar included, with the title bar gone. Leaving
  fullscreen, switching workspace, or quitting phiwm puts the frame back.
- An app that maximizes itself is left maximized until it restores.
- A split's ratio is the golden ratio, or what you dragged it to, and never bends to what
  sits inside: the first window keeps its 61.8% however many windows follow. The layout
  does know each app's minimum size (Windows Terminal wants about 100 columns): two windows
  that cannot sit side by side are stacked instead. A window that still cannot shrink to
  its tile keeps its minimum, slid back inside the monitor, so it overlaps a neighbour
  rather than leaving the screen; the log says so. An app that only announces its minimum
  after it is up (Steam) is learned the first time it refuses a tile.
- Log: `%LOCALAPPDATA%\phiwm\phiwm.log` (previous run: `phiwm.old.log`). Every managed window
  is logged with its class and exe, and so is every keybind and workspace switch.

## Source

- `src/layout.c` — the dwindle tree, minimum-size aware. Platform independent, unit tested.
- `src/defaults.c` — keys, window rules, constants. Platform independent, unit tested.
- `src/wm.c` — the window manager: WinEvent hooks, workspaces, monitors, dispatchers, IPC.
  Everything runs on the main thread; calls into other processes' windows are asynchronous,
  so a hung app can never freeze the tiler.
- `src/keys.c` — low-level keyboard hook on its own thread; it only matches keys and posts
  the bind index to the main thread.
- `src/tray.c`, `src/util.c` — tray icon and menu, logging, paths, string helpers.

## Limits (Windows, not phiwm)

- **Win + L** always locks; Windows does not let anyone rebind it.
- **Admin windows** (Task Manager, elevated terminals) can't be moved by a normal process and
  are left alone. Run phiwm elevated (e.g. a Task Scheduler entry with "highest privileges")
  to tile them too. `phiwm dispatch …` then has to run elevated as well — on purpose, so
  normal programs can't make an elevated phiwm `exec` things.
- Don't mix with **Windows virtual desktops**; phiwm's workspaces replace them.
- No animations of its own: Windows does not let a tiler draw transitions.

## Build

```
make            # cross-compile on Linux (mingw-w64)
make test       # layout + defaults tests, native
make dist       # build/phiwm-<version>-x64.zip
```

On Windows: MSYS2 UCRT64 shell, `pacman -S mingw-w64-ucrt-x86_64-gcc make zip`, then
`make CROSS=`.

## Release and winget

Push a tag `vX.Y.Z`. The workflow builds, tests, attaches the zip to a GitHub release and
generates the three winget manifests as a release artifact. First submission:

```
wingetcreate submit --token <github-pat> build/winget/manifests/<x>/<Publisher>/phiwm/<version>
```

(or open a PR to `microsoft/winget-pkgs` with that folder). After it's accepted, later
releases are `wingetcreate update <Id> --version X.Y.Z --urls <zip-url> --submit`.

## License

MIT
