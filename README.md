# 🃏 Balatro in Grand Theft Auto V (PC)

Play **Balatro** game directly inside your character's cell phone in **Grand Theft Auto V**!

Balatro is streamed seamlessly in real-time onto the in-game phones, directly inside GTA V.

---

## 🌟 Features

1. **📱 Real In-Game Phone Integration:**
   - **First-Person View:** 3D phone model directly in their hand.
   - **Third-Person View:** Displays on the horizontal in-engine HUD phone at the bottom right.
2. **🔄 3 Dynamic Display Modes (Press `F10` to cycle):**
   - **(Standard In-Engine Phone):** Physical 3D phone in first-person; HUD phone in third-person.
   - **(Large Centered Phone):** Enlarges and centers the phone UI on screen with high-resolution direct compositing.
   - **(Fullscreen Mode):** Full native display filling the entire monitor.
3. **⚡ High-Performance Shared-Memory IPC:**
   - 60 FPS uncompressed video streaming with ultra-low latency (< 0.2ms GPU frame upload).
   - Custom 8-neighbor integer Laplacian sharpening pass on the in-game phone texture, keeping card descriptions, chip values, and font stems tack-sharp.
4. **🖱️ Smart Mouse & Input Passthrough:**
   - Full drag-and-drop and click support for cards, jokers, vouchers, and shop items.
   - Automatically hides the default GTA square cursor while hovering over the phone display and restores it when moving away.
   - Tab-out protection: keystrokes while alt-tabbed are never sent to the game.
5. **🚀 One-Key Auto-Launch (`F9`):**
   - Pressing **F9** in-game automatically boots Balatro with `--gta-passthrough` if it isn't already running.
   - Pressing **F9** again cleanly closes the phone and mutes Balatro's audio.


## 🚀 Quick Start (Automated Installer)

The easiest way to install the mod is using the standalone installer:

1. Download **[`Installer.exe`](releases/Installer.exe)** from the [Releases](releases/) folder (or the Releases tab on GitHub).
2. Run `Installer.exe`.
3. It will auto-detect your GTA V and Balatro directories (or click **Browse...** to select them).
4. Click **Install Mod**.
5. Launch GTA V and press **F9** to play!

---

## 📦 Manual Installation (Loose Files)

If you prefer installing manually from the [`bin/`](bin/) folder:

### 1. Prerequisites
- [ScriptHookV](http://www.dev-c.com/gtav/scripthookv/) (`ScriptHookV.dll` & `dinput8.dll` in your GTA V folder).
- [ReShade](https://reshade.me/) (v5.0+ installed with full add-on support).
- [Lovely Mod Loader](https://github.com/ethangreen-dev/lovely-injector/releases) (`version.dll` in your Balatro folder).

### 2. Copy GTA V Files
Copy the contents of [`bin/gta/`](bin/gta/) into your main GTA V folder:
- `BalatroPassthrough.asi` -> `<GTA V Directory>\`
- `BalatroPassthrough.ini` -> `<GTA V Directory>\`
- `reshade-shaders\Shaders\BalatroPhone.fx` -> `<GTA V Directory>\reshade-shaders\Shaders\`

### 3. Copy Balatro Mod Files
Copy the folder [`bin/balatro/BalatroPassthrough/`](bin/balatro/BalatroPassthrough/) into your Balatro mods directory:
```
%APPDATA%\Balatro\Mods\BalatroPassthrough\
├── balatro_bridge.lua
└── lovely.toml
```

### 4. Set Balatro Path
Open `BalatroPassthrough.ini` in your GTA V folder and verify `ExecutablePath` points to your `Balatro.exe`:
```ini
[Balatro]
ExecutablePath=C:\Program Files (x86)\Steam\steamapps\common\Balatro\Balatro.exe
Arguments=--gta-passthrough
AutoLaunchOnOpen=1
```

---

## 🎮 Controls & Keybindings

| Key / Input | Action | Description |
| :--- | :--- | :--- |
| **`F9`** (Configurable) | Toggle Phone | Opens / closes the phone in GTA V (auto-boots Balatro). |
| **`F10`** (Configurable) | Cycle Display Mode | Cycles **Mode 0** (In-engine phone) -> **Mode 1** (Large phone) -> **Mode 2** (Fullscreen). |
| **`Mouse Left-Click`** | Select / Play Card | Full interaction with cards, buttons, shop, and jokers. |
| **`Arrow Keys`** | Move Phone | *(Large Phone Mode)* Move large phone position on screen. |
| **`[ & ]`** | Scale Phone | *(Large Phone Mode* Adjust large phone display scale. |

> [!TIP]
> **Custom Keybinds:** You can change `F9` and `F10` to any key you like by editing the `[Controls]` section in `BalatroPassthrough.ini`:
> ```ini
> [Controls]
> ToggleKey=F9
> ModeKey=F10
> ```
> Supports `F1`–`F12`, `A`–`Z`, `0`–`9`, `TAB`, `INSERT`, `DELETE`, `HOME`, `END`, `PAGEUP`, `PAGEDOWN`, `TILDE`, or virtual-key hex codes (e.g., `0x78`).


---

## 🛠️ Building From Source

### GTA V Plugin (`BalatroPassthrough.asi`)
1. Open a command prompt in `src/gta-mod/`.
2. Run `build.bat`.
3. Output is generated at `src/gta-mod/build/BalatroPassthrough.asi`.
*(Requires Visual Studio 2022 with C++ desktop tools).*

### Installer (`Installer.exe`)
1. Open a command prompt in `src/installer/`.
2. Run `build_installer.bat`.
3. Output is generated at `releases/Installer.exe`.
*(Uses Windows' built-in .NET Framework compiler `csc.exe`—no SDK required).*

---

## 📄 License & Credits

- Distributed under the [MIT License](LICENSE).
- Built with [ScriptHookV](http://www.dev-c.com/gtav/scripthookv/) by Alexander Blade and [ReShade](https://reshade.me/) by crosire.
- Balatro modding enabled by [Lovely](https://github.com/ethangreen-dev/lovely-injector) by ethangreen-dev.
- Contains 100% original passthrough bridge code. No proprietary game assets or copyrighted binaries from Grand Theft Auto V or Balatro are included.
