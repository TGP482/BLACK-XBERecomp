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
The output is a Windows x64 executable on either build host. Linux builds use
Clang and the Microsoft headers/libraries supplied by xwin; they can be run
through Proton. Wine/Proton is not needed to compile.

#### Windows
Install [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/) and select the following **Workloads** and **Individual components**:
- Desktop development with C++
- C++ Clang tools for Windows
- C++ CMake tools for Windows

> [!TIP]
> `build.bat` offers to install the Clang and CMake components for you if they're missing.

#### Linux
Install Git, CMake 3.25 or newer, Make, a native C/C++ compiler, and LLVM with
`clang-cl`, `lld-link`, `llvm-lib`, `llvm-rc`, and `llvm-mt` on `PATH`.
Install [xwin](https://github.com/Jake-Shadle/xwin) using Cargo or a release binary,
then download the Windows x64 SDK and CRT (the command accepts Microsoft's license):

```sh
cargo install xwin --locked
xwin --accept-license --cache-dir "$HOME/.cache/xwin" splat --output "$HOME/.local/share/xwin"
export XWIN_ROOT="$HOME/.local/share/xwin"
```

### 4. Build the Project
#### Windows
- Run `build.bat`. The first build recompiles the entire game and can take 30 to 90 minutes on weaker systems.
- When it finishes, run `out\build\release\BLACK.exe`.

#### Linux
With the USA game files extracted into `assets` as described above:

```sh
cmake --preset linux-release
cmake --build --preset linux-release --parallel 2
```

Increase `--parallel` if memory permits. The build compiles a native Linux
`xbe_recomp` to generate C++, then uses `clang-cl` and `lld-link` to produce
`out/build/linux-release/BLACK.exe` and `Black.dll`. Add `BLACK.exe` to Steam as
a non-Steam game and select Proton in its Compatibility settings. Keep `Black.dll`
beside the executable; the runtime finds the repository's `assets` directory.
Proton's built-in `d3dcompiler_47` is supported: the renderer converts the SDK's
FXC-specific infinity literals to bit-exact HLSL `asfloat` expressions before
compilation. No Microsoft shader compiler DLL is needed under Proton. If your
prefix already overrides that DLL, select the built-in compiler in Steam's launch options:

```sh
WINEDLLOVERRIDES="d3dcompiler_47=b" %command%
```

For a standalone Proton folder, copy both binaries and the full `assets` directory.
A Windows distribution still uses the runtime DLLs listed in `release.bat`,
including the x64 Microsoft shader compiler from the Windows SDK's `Redist/D3D/x64`
directory. xwin supplies build headers and libraries, not that runtime DLL.

The Linux preset does not bundle Windows LLVM executables: the game DLL is already
compiled. To make a distribution that can recompile on first launch, configure with
`-DBLACK_BUNDLE_COMPILER=ON -DBLACK_WINDOWS_LLVM_BIN=/path/to/windows/llvm/bin`
and include the compiler folder and runtime DLLs described in `release.bat`.
Do not substitute Linux LLVM binaries in that folder.

Both builds use the pinned XboxXBE-Recomp SDK revision. A local `sdk` directory
or sibling `XboxXBE-Recomp` checkout overrides the download; use revision
`91146ff6e2a218b19c1109dc665d06020472ab57` for those checkouts too. Build orchestration
lives in `cmake/XbeRecomp.cmake` so host tools and target tools stay separate without
patching the SDK. `BLACK_HOST_C_COMPILER` and `BLACK_HOST_CXX_COMPILER` can override
the native compilers used during a cross-build.
