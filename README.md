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
- **Colour Blind Mode** - Adjusts the colours for Protanopia, Deuteranopia or Tritanopia.

### Graphics
- **Multisample Antialiasing (MSAA)** - Enables 2x, 4x or 8x MSAA to smooth jagged edges while preserving a sharp image.
- **Subpixel Morphological Antialiasing (SMAA)** - Enables SMAA to smooth jagged edges across the whole frame with a softer image. SMAA also smooths alpha-tested edges such as fences and foliage, and can be combined with MSAA.
- **Anisotropic Filtering** - Forces the selected anisotropic filtering level on all textures.
- **Draw Distance** - Increases the level of detail and draw distances past the game's values: 2x, 4x, 8x, 12x or 16x.
- **Ambient Occlusion** - Adds screen space ambient occlusion, creating soft contact shadows where surfaces meet. Choose between SSAO and GTAO.
- **Reload Blur** - Toggles the blur effect while reloading.

### Textures
- **Texture Replacement** - Replaces the game's textures with the `.dds` or `.png` images in the `textures\replacement` folder beside the exe.
- **Texture Dumping** - Saves every texture the game uses into the `textures\dumps` folder beside the exe.
- **Dump Format** - Choose between PNG or DDS, which keeps the original compression and mipmaps.

### Input
- **Mouse and Keyboard** - Full keyboard and mouse support with raw input, adjustable horizontal and vertical sensitivity, inverted look and rebindable controls.
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

## macOS
### Requirements
- Apple Silicon, macOS 14 or later.
- `brew install cmake ninja sdl3 vulkan-loader molten-vk vulkan-tools glslang`. Xcode command line tools provide clang.

### Build
- Clone the MIT SDK into `sdk/` and apply the patch: `git clone https://github.com/TGP482/XboxXBE-Recomp sdk`, then `git apply tools/macos/sdk-apple.patch`. The patch is cut against `cc9edda`; a later upstream commit may need a rebase.
- Put the game in `assets` at the repository root. Without `assets/default.xbe` at configure time, only the tools and host objects build.
- `cmake --preset mac-release`, then `cmake --build out/build/mac-release --target Black Black_code -- -k 0`. Five minutes on an M1, most of it the game's recompile.
- `tools/macos/unpack-iso.sh` extracts a disc image. `tools/macos/check-assets.sh` says whether a folder can boot.
- A release ships `BLACK`, `compiler/` (with `libhooks.a`) and an empty `assets/`. `compiler/build-code.sh` writes `BLACK.dylib` from the player's own xbe on first start; neither the dylib nor `generated/` is distributed.

### Run
```
VK_ICD_FILENAMES=$(brew --prefix molten-vk)/etc/vulkan/icd.d/MoltenVK_icd.json ./out/build/mac-release/BLACK
```
- `glslangValidator` on `PATH` for shaders not yet in `~/Library/Caches/black/shaders`.

### Settings
- `black.toml` beside the executable, same keys as Windows. `logs/black.log` beside it.
- `XBE_VERBOSE=1` keeps the info lines a release build drops. `XBE_PERF=1` adds frame times, `XBE_DIAG=1` the temporary diagnostics.

### Limits
- HDR and MSAA are unverified. The window is borderless or windowed; Cocoa has no exclusive fullscreen.
- `BLACK.dylib` is a build product. Do not ship it or `generated/`.

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

## macOS
### Requirements
- Apple Silicon and macOS 14 or later. MoltenVK and D3DMetal both need 14; this port was developed on macOS 26.6 on an M1.
- Install `cmake`, `ninja`, `sdl3`, `vulkan-loader`, `molten-vk`, `vulkan-tools` and `glslang` with Homebrew. Xcode command line tools provide clang.
- `git clone https://github.com/TGP482/XboxXBE-Recomp sdk` puts the MIT SDK in `sdk/`. The patch is cut against `cc9edda`; a later upstream commit may need a rebase.
- `git apply tools/macos/sdk-apple.patch` from the repository root adds the Apple backend the build uses.

### Build
- Place your game files in an `assets` folder at the repository root first; without `assets/default.xbe` at configure time, only the tools and host objects build.
- Run `cmake --preset mac-release`, then `cmake --build out/build/mac-release --target Black -- -k 0`. Configure and build take about five minutes on an M1, of which the game's recompile is most of it.
- `tools/macos/unpack-iso.sh` extracts a disc image into `assets`, and `tools/macos/check-assets.sh` reports whether a folder has what the game needs to boot.
- A release ships `BLACK`, `compiler/` (including `libhooks.a`) and an empty `assets/`; `compiler/build-code.sh` builds `BLACK.dylib` from the player's own xbe on the first start. Neither that dylib nor `generated/` is distributed.

### Run
```
VK_ICD_FILENAMES=$(brew --prefix molten-vk)/etc/vulkan/icd.d/MoltenVK_icd.json ./out/build/mac-release/BLACK
```
- `glslangValidator` must be on `PATH` for any shader that is not already compiled in `~/Library/Caches/black/shaders`.

### Settings
- `black.toml` beside the executable takes the same keys as the Windows port, and `logs/black.log` is written beside it.
- `XBE_VERBOSE=1` keeps the info lines a release build drops, `XBE_PERF=1` adds the frame time block, and `XBE_DIAG=1` adds the temporary diagnostics.

### Limits
- There is no first run compile yet: the binary already contains the recompiled game code and must not be distributed.
- HDR and MSAA are unverified. The window is borderless or windowed, since Cocoa has no exclusive fullscreen mode.
