# Black - XBE Recomp
An unofficial native PC port of the original Xbox version of Black, with extended graphics options, keyboard and mouse support, and other enhancements.

This port was created using the [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool.

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own legally acquired copy of the game to install or build.

## Installation
- Put your game files in the `assets` folder. This can be an ISO, or the extracted data files, of the USA or Europe release.
- Run `BLACK.exe`. The first start compiles the game's code for your PC, this only happens once.
- Recomp settings can be adjusted in the in-game options menu while the game is running, or through the Settings overlay, which is opened with F10 by default.
<img width="1920" height="1080" alt="{5852D359-24A0-48C8-9319-07C3A619E355}" src="https://github.com/user-attachments/assets/e0156eb8-c1b5-49d9-ab93-3abb46bf5f47" />


## Running on Linux

The built game is a Windows binary, but it runs on Linux through Proton, which translates D3D12 to Vulkan via vkd3d-proton. The `linux` folder contains a launcher that sets this up. No game code is changed.

### Requirements
- 64-bit Linux with a Vulkan-capable GPU driver (`vulkaninfo --summary` reports a device)
- Steam installed, with **Proton Hotfix** downloaded (Steam > Library > enable the `Tools` filter > search `Proton Hotfix` > Install)
- Your legally acquired game files (USA or Europe release)

### Setup
1. Get a prebuilt `BLACK.exe` with `d3dcompiler_47.dll` from the [Releases](https://github.com/TGP482/BLACK-XBERecomp/releases) page and put them in a new game folder beside an empty `assets` folder, for example `~/Games/BLACK/`.
2. Put your game files in `assets`, either the disc ISO, or the extracted `default.xbe`, `GlobData.bin` and the game's data folders.
3. Run:
```
./linux/run-proton.sh
```
The launcher looks for `BLACK.exe` beside the repository, in `~/Games/BLACK`, in `~/Games/BLACK-XBERecomp`, in `~/BLACK`, then shallowly under `~/Games`. Point at it explicitly with `GAME_DIR=/path/to/game ./linux/run-proton.sh` if it lives somewhere else.

On first start Proton creates a Wine prefix next to the game in `compatdata`. Press F10 in-game to open the Recomp Settings overlay.

### Troubleshooting
- If the game does not start, run `./linux/debug-proton.sh`. It runs the game for 30 seconds with Proton logging enabled and prints the `vkd3d` / `d3d12` / error lines, followed by the path of the full log.
- `nvidia-smi dmon` while the game runs (or `radeontop` on AMD) shows whether the GPU is actually used.
- Wayland users: uncomment `PROTON_ENABLE_WAYLAND=1` in `linux/run-proton.sh` when the window misbehaves.
- Saves from other consoles show as damaged. They are signed with that console's EEPROM key, so start a new save instead.

Settings are stored in `black.toml` beside `BLACK.exe` (see `linux/black.toml.default` for an example). `./linux/install-menu-entry.sh` adds the game to your app menu with its cover art, and `./linux/install-menu-entry.sh --remove` undoes it.

## Features
### Display
- **Monitor** - Picks the monitor the game shows on, for Fullscreen and Borderless (and where Windowed opens). Lists every connected monitor by its Windows number and desktop resolution.
- **Display Mode** - Switch between Fullscreen, Borderless and Windowed.
- **Resolution** - Lists every mode your monitor reports and defaults to your desktop resolution, replacing the original 640x480 resolution.
- **Render Resolution** - Renders the game at 20% to 200% of the output resolution, allowing supersampling or downscaling independent of the window size.
- **Field of View** - Sets the horizontal field of view during gameplay from 45 to 140 degrees, with 70 being the game's default.
- **Viewmodel Field of View** - Adjusts the first-person weapon and arms field of view.
- **Max Frame Rate** - Caps the frame rate to your monitor's refresh rate or a custom value, or removes the cap entirely, replacing the Xbox's 30 FPS limiter.
- **V-Sync** - Reduces screen tearing.
- **HDR Output** - Enables HDR10 output on displays with Windows HDR turned on, detected automatically. Highlights extend up to the display's peak brightness.
- **FPS Counter** - Displays the frame rate on screen.
- **FMV Scale** - Choose between Fill, which zooms the 4:3 FMVs to fill widescreen displays, and 4:3, which keeps them at their original aspect ratio.

### Graphics
- **Multisample Antialiasing (MSAA)** - Enables 2x, 4x or 8x MSAA to smooth jagged edges while preserving a sharp image.
- **Subpixel Morphological Antialiasing (SMAA)** - Enables SMAA to smooth jagged edges across the whole frame with a softer image. SMAA also smooths alpha-tested edges such as fences and foliage, and can be combined with MSAA.
- **Anisotropic Filtering** - Forces the selected anisotropic filtering level on all textures.
- **Draw Distance** - Increases the level of detail and draw distances past the game's values: 2x, 4x, 8x, 12x or 16x.
- **Ambient Occlusion** - Adds screen space ambient occlusion, creating soft contact shadows where surfaces meet. Choose between SSAO and GTAO.
- **Reload Blur** - Toggles the blur effect while reloading.

### Input
- **Mouse and Keyboard** - Full keyboard and mouse support with raw input, adjustable sensitivity, inverted look and rebindable controls.
- **Controller** - Controller with SDL3 support and additional features.
- **Gyro Aiming** - Adds gyro aiming for controllers with motion sensors, such as the DualSense, DualShock 4 and Switch Pro Controller, with adjustable sensitivity. Works together with the right stick.

### General
- **Skip Intro** - Skips the intro logos.
- **Pause On Focus Loss** - Pauses the game and its sound while the window is in the background or minimized.

### Miscellaneous
- **Custom Menu Cursor** - Place a `cursor.png` (or `cursor.dds`) beside the exe to replace the Windows arrow in the game's menus.

## System Requirements
### Minimum
- **OS**
  - Windows 10 64-bit
- **CPU** with 2 cores and support for SSE4.2 and POPCNT
  - Intel Nehalem (Intel Core 1st Generation)
  - AMD Bulldozer (AMD FX series)
- **GPU** with support for Direct3D 12
  - NVIDIA GeForce GT 630 (Kepler)
  - AMD Radeon HD 7750
  - Intel HD Graphics 510 (Skylake)
- **Memory**
  - 4 GB
- **Storage**
  - 3 GB for the game files, the compiled game code and saves

## Building
### 1. Clone the Repository
Clone this repository using [Git](https://git-scm.com/).
```
git clone https://github.com/TGP482/BLACK-XBERecomp.git
```

### 2. Add the Required Game Files
Place your extracted game data files in the `assets` folder. `default.xbe` from the USA release must be there, with the rest of the game's files beside it.


### 3. Install Dependencies
Install [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/) and select the following **Workloads** and **Individual components**:
- Desktop development with C++
- C++ Clang tools for Windows
- C++ CMake tools for Windows

> [!TIP]
> `build.bat` offers to install the Clang and CMake components for you if they're missing.

### 4. Build the Project
- Run `build.bat`. The first build recompiles the entire game and can take 30 to 90 minutes on weaker systems.
- When it finishes, run `out\build\release\BLACK.exe`.
