# Black - XBE Recomp (Linux)
An unofficial native Linux (`x86_64`) port of the original Xbox version of Black, powered by a dedicated Vulkan renderer and SDL3 host, with extended graphics options, keyboard and mouse support, and other enhancements.

This port is based on [BLACK-XBERecomp](https://github.com/TGP482/BLACK-XBERecomp) and created using the [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool.

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own legally acquired copy of the game to install or build.

## Installation
- Put your game files in the `assets` folder. This can be an ISO, or the extracted data files, of the USA or Europe release.
- Run `./BLACK`.
- Recomp settings can be adjusted in the in-game options menu while the game is running, or through the Settings overlay, which is opened with F10 by default.
<img width="1920" height="1080" alt="{5852D359-24A0-48C8-9319-07C3A619E355}" src="https://github.com/user-attachments/assets/e0156eb8-c1b5-49d9-ab93-3abb46bf5f47" />


## Features
### Display
- **Monitor** - Picks the monitor the game shows on, for Fullscreen and Borderless (and where Windowed opens). Lists every connected monitor by its desktop resolution.
- **Display Mode** - Switch between Fullscreen, Borderless and Windowed (supports both Wayland and X11).
- **Resolution** - Lists every mode your monitor reports and defaults to your desktop resolution, replacing the original 640x480 resolution.
- **Render Resolution** - Renders the game at 20% to 200% of the output resolution, allowing supersampling or downscaling independent of the window size.
- **Field of View** - Sets the horizontal field of view during gameplay from 45 to 140 degrees, with 70 being the game's default.
- **Viewmodel Field of View** - Adjusts the first-person weapon and arms field of view.
- **Max Frame Rate** - Caps the frame rate to your monitor's refresh rate or a custom value, or removes the cap entirely, replacing the Xbox's 30 FPS limiter.
- **V-Sync** - Reduces screen tearing.
- **FPS Counter** - Displays the frame rate on screen.
- **FMV Scale** - Choose between Fill, which zooms the 4:3 FMVs to fill widescreen displays, and 4:3, which keeps them at their original aspect ratio.

### Graphics
- **Multisample Antialiasing (MSAA)** - Enables 2x, 4x or 8x MSAA to smooth jagged edges while preserving a sharp image.
- **Subpixel Morphological Antialiasing (SMAA)** - Enables SMAA to smooth jagged edges across the whole frame with a softer image. SMAA also smooths alpha-tested edges such as fences and foliage, and can be combined with MSAA.
- **Anisotropic Filtering** - Forces the selected anisotropic filtering level on all textures.
- **Draw Distance** - Increases the level of detail and draw distances past the game's values: 2x, 4x or 8x.
- **Ambient Occlusion** - Adds screen space ambient occlusion, creating soft contact shadows where surfaces meet. Choose between SSAO and GTAO.
- **Reload Blur** - Toggles the blur effect while reloading.

### Input
- **Mouse and Keyboard** - Full keyboard and mouse support with relative mouse mode, adjustable sensitivity, inverted look and rebindable controls.
- **Controller** - Controller with SDL3 support.

### General
- **Skip Intro** - Skips the intro logos.
- **Pause On Focus Loss** - Pauses the game and its sound while the window is in the background or minimized.

### Miscellaneous
- **Custom Menu Cursor** - Place a `cursor.png` (or `cursor.dds`) beside the executable to replace the arrow cursor in the game's menus.

## System Requirements
### Minimum
- **OS**
  - 64-bit Linux distribution (Ubuntu 22.04+, Fedora 39+, Arch Linux, etc.)
- **CPU** with 2 cores and support for SSE4.2 and POPCNT (`x86-64-v2`)
  - Intel Nehalem (Intel Core 1st Generation)
  - AMD Bulldozer (AMD FX series)
- **GPU** with support for Vulkan 1.2+
  - NVIDIA GeForce GTX 900 series or newer (or proprietary/Mesa drivers with Vulkan 1.2+)
  - AMD Radeon GCN 1.2+ (RADV / Mesa)
  - Intel HD Graphics 500 series (ANV / Mesa)
- **Memory**
  - 4 GB
- **Storage**
  - 2 GB for the game files and saves

## Building
### 1. Clone the Repository
Clone this repository using [Git](https://git-scm.com/):
```bash
git clone -b linux https://github.com/Peterson047/BLACK-XBERecomp-Linux.git
cd BLACK-XBERecomp-Linux
```

### 2. Add the Required Game Files
Place your game files in the `assets` folder. `default.xbe` from the USA release must be there for static recompilation, along with either the rest of the extracted game files or the `.iso` disc image beside it.

### 3. Install Dependencies
Install the required compiler toolchain (`clang`, `lld`, `cmake`, `ninja`) and development headers for Vulkan, `shaderc`, X11/Wayland, and audio (ALSA/PulseAudio):

- **Ubuntu / Debian / Linux Mint / Pop!\_OS:**
  ```bash
  sudo apt update && sudo apt install -y \
      clang lld cmake ninja-build git pkg-config \
      libvulkan-dev libshaderc-dev \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev \
      libwayland-dev wayland-protocols libxkbcommon-dev \
      libasound2-dev libpulse-dev
  ```

- **Fedora:**
  ```bash
  sudo dnf install -y \
      clang lld cmake ninja-build git pkgconf-pkg-config \
      vulkan-loader-devel libshaderc-devel \
      libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXi-devel libXfixes-devel \
      wayland-devel wayland-protocols-devel libxkbcommon-devel \
      alsa-lib-devel pulseaudio-libs-devel
  ```

- **Arch Linux / CachyOS / Manjaro:**
  ```bash
  sudo pacman -S --needed \
      clang lld cmake ninja git pkgconf \
      vulkan-headers vulkan-icd-loader shaderc \
      libx11 libxext libxrandr libxcursor libxi libxfixes \
      wayland wayland-protocols libxkbcommon \
      alsa-lib libpulse
  ```

> [!TIP]
> `./build.sh` automatically checks for all required tools and libraries and offers to install any missing packages for you on Ubuntu/Debian (`apt`), Fedora (`dnf`), and Arch Linux (`pacman`).

### 4. Build the Project
- Run `./build.sh`. The first build recompiles the entire game and can take 30 to 90 minutes on weaker systems.
- When it finishes, run `./out/build/release/BLACK`.
