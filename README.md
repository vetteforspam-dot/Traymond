# Traymond

Minimize any window to the system tray — including Chromium browsers and Electron apps.

Right-click a window's minimize button to send it to the tray. Click the tray icon to bring it back. That's it.

## The problem

Windows doesn't let you minimize windows to the system tray. The [original Traymond](https://github.com/fcFn/traymond) solved this with a global hotkey, but it didn't work well with modern apps:

- **Chromium and Electron apps** (Chrome, Edge, Brave, Helium, VS Code, Discord, Slack) use custom title bars that don't respond to standard Windows hit-testing. There was no way to right-click their minimize button.
- **Tray icons could be blank.** Chromium windows return icon handles that look valid but render as invisible squares in the tray — your window disappears with no way to get it back.
- **Restoring required a double-click**, which felt sluggish for something you do constantly.
- **Traymond added its own icon to the tray**, wasting space for no reason — you never need to interact with Traymond itself.

## How this version works

**Right-click any minimize button** to send the window to the tray. This works on every app — native Win32, Chromium, Electron, CEF, anything with a minimize button. For apps with custom title bars, Traymond uses DPI-aware geometric detection based on DWM's known caption button dimensions (46 DIP wide) to identify the minimize button region.

**Tray icons always show up.** For Chromium/Electron windows, the icon is extracted directly from the process executable instead of relying on unreliable window icon handles.

**If Traymond crashes**, all hidden windows are automatically restored on restart. Window handles are saved to disk continuously.

## Usage

| Action | How |
|---|---|
| Minimize to tray | **Right-click** the minimize button, or **Win+Shift+Z** |
| Restore | **Click** the tray icon |
| Restore all and exit | **Win+Shift+X** |

Traymond runs silently with no tray icon of its own — only icons for minimized windows appear.

## Download

Grab `Traymond.exe` from the [latest release](https://github.com/vetteforspam-dot/Traymond/releases/latest). No installer needed — just run it.

Place it in a writable directory (it creates `traymond.dat` for crash recovery). To run at startup, add a shortcut to `shell:startup` with the working directory set to the exe's folder.

## Building from source

Requires Visual Studio (MSVC). Open a **Developer Command Prompt** and run:

```
nmake
```
