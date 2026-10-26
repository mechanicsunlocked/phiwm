# phiwm

Dwindle tiling for Windows 11, configured like `hyprland.conf`. Every new window splits the
focused one — by default at the golden ratio, so the existing window keeps 61.8% and windows
spiral in toward the bottom-right. One mode, no layout zoo.

- Plain C, one ~160 KB exe, no services, no helper daemons, no runtime.
- Hyprland config syntax: `$vars`, `section { }`, `bind`/`binde`/`bindd`, `windowrule`,
  `exec-once`. Most Omarchy/Hyprland lines can be pasted in; unknown options are ignored.
- Saves reload instantly (the file is watched).
- Workspaces 1–10, bound to monitors the way Hyprland does it.
- Native Windows 11 border colors and square/rounded corners — no overlay windows.
- Tray icon shows the focused workspace number.

## Install

```
winget install phiwm
phiwm
```

Tray menu → **Start with Windows** to autostart. The config is created on first run at
`%USERPROFILE%\.config\phiwm\phiwm.conf` (override with `PHIWM_CONFIG`).

## Default keys

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
| Win + Ctrl + R | Reload config |
| Win + Ctrl + Shift + Q | Quit (shows every hidden window) |

Mouse: drag a tile's border to resize the split, drag its title bar onto another tile to swap,
or onto another monitor to move it there.

## Config

```ini
$mainMod = SUPER

general {
    gaps_in  = 5
    gaps_out = 10
    col.active_border   = rgba(33ccffee)
    col.inactive_border = rgba(595959aa)
}
decoration { rounding = 10 }        # 0 = square corners
dwindle {
    default_split_ratio = 1.236     # Hyprland scale: 1.0 = 50/50, 1.236 = golden
    force_split = 2                 # 0 = side of the mouse, 1 = left/top, 2 = right/bottom
    preserve_split = true
}
input { follow_mouse = 0 }          # 1 = focus follows mouse (this session only)

bind  = $mainMod, RETURN, exec, wt.exe
binde = $mainMod, minus, resizeactive, -100 0
bind  = $mainMod, code:10, workspace, 1       # code:N = key position, any layout

windowrule = float, title:^(Picture-in-Picture)$
windowrule = workspace 3 silent, exe:^(Spotify\.exe)$
exec-once  = C:\Tools\something.exe
```

**Dispatchers:** `exec`, `killactive`, `movefocus`, `movewindow`, `swapwindow`,
`togglesplit`, `togglefloating`, `pseudo`, `fullscreen 0|1`, `workspace`, `movetoworkspace`,
`movetoworkspacesilent`, `focusmonitor`, `splitratio`, `resizeactive`, `cyclenext`,
`layoutmsg togglesplit`, `reload`, `exit`.
Workspace arguments: `3`, `+1`, `-1`, `e+1`, `e-1`, `previous`, `empty`.

**Window rules:** `float`, `tile`, `pseudo`, `workspace N [silent]`. Match on `class:`,
`title:` or `exe:`. `class:` also matches the exe name without `.exe`, so `class:^(firefox)$`
works. Patterns accept the usual Hyprland regex shapes (`^(a|b)$`, `.*`, `\.`), matched
case-insensitively against the whole string.

**Modifiers:** `SUPER`, `SHIFT`, `CTRL`, `ALT`. **Keys:** letters, digits, `F1`–`F24`,
`RETURN`, `SPACE`, `TAB`, `ESCAPE`, arrows, `minus`, `equal`, `comma`, `period`, `slash`,
`semicolon`, `apostrophe`, `grave`, `bracketleft/right`, `backslash`, `Home`, `End`,
`Prior`, `Next`, `Delete`, `Print`, `XF86Audio*`, `code:N`.

From scripts: `phiwm dispatch workspace 3`, `phiwm reload`, `phiwm quit`.

## How it behaves

- Windows on hidden workspaces are hidden, so they leave the taskbar — like Hyprland.
  If phiwm is killed, the next start brings them back; a normal quit does it right away.
- Dialogs, tool windows and fixed-size windows float on their own.
- An app that maximizes itself is left maximized until it restores.
- Apps that insist on a minimum size win after a few corrections instead of jittering.
- Logs: `%LOCALAPPDATA%\phiwm\phiwm.log` (every managed window is logged with class and exe,
  which is what you match in rules).

## Limits (Windows, not phiwm)

- **Win + L** always locks; Windows does not let anyone rebind it.
- **Admin windows** (Task Manager, elevated terminals) can't be moved by a normal process and
  are left alone. Run phiwm elevated (e.g. a Task Scheduler entry with "highest privileges")
  to tile them too. `phiwm dispatch …` then has to run elevated as well — on purpose, so
  normal programs can't make an elevated phiwm `exec` things.
- Don't mix with **Windows virtual desktops**; phiwm's workspaces replace them.
- No animations: Windows does not let a tiler draw transitions.

## Build

```
make            # cross-compile on Linux (mingw-w64)
make test       # layout + config tests, native
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
