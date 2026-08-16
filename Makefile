Markdown
# 📺 Full RGB TV & Video Mode Switcher for Wii U (Aroma)

[![WUPS](https://img.shields.io/badge/Platform-Wii%20U%20%28Aroma%29-blue.svg)](https://aroma.foryour.cafe/)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Release](https://img.shields.io/badge/Release-v1.0.0--beta.3-green.svg)](https://github.com/Masana1/full-rgb-tv-wiiu/releases)

A native **WUPS plugin** for Nintendo Wii U (Aroma) that forces **Full Range RGB (0–255)** color output over HDMI and provides a **dynamic on-the-fly video mode switcher** with extended display resolutions.

---

## 🌟 Overview & Why This Plugin?

* **Native Full RGB (0–255):** By default, the Wii U outputs HDMI video strictly in **Limited Range RGB (16–235)**. On modern PC monitors, OLED screens, HDMI capture cards, and TVs expecting Full RGB, this causes washed-out blacks, grey tinting, and inaccurate color tracking. This plugin injects custom display buffers directly into memory to force true **Full RGB (0–255)** output.
* **On-the-Fly Video Switching:** Switch video output resolutions and refresh rates instantly from the Aroma configuration menu without exiting to the Wii U System Settings.
* **Extended Display Modes:** Unlocks native access to extended modes, including **720p 3D Frame Packing**, 50Hz/60Hz interlaced and progressive modes, as well as legacy analog modes (Composite & SCART PAL60).

---

## ✨ Features

* 🎨 **Full RGB Color Range (0–255):** Enjoy deep blacks and accurate color reproduction.
* ⚡ **Instant Switch:** Change resolutions on-the-fly directly inside games, homebrew, or the Wii U Menu.
* 💾 **Apply on Boot (Persistence):** Save your Full RGB configuration to persist across coldboots via the WUPS Storage API.
* 📺 **Extended Video Modes Supported:**
  * **Composite / Analog:** 480i (NTSC 60Hz), 576i (PAL 50Hz), 480i PAL60 (60Hz)
  * **HDMI Standard:** 480p (60Hz), 576p (50Hz), 720p (50Hz & 60Hz), 1080i (50Hz & 60Hz), 1080p (50Hz & 60Hz)
  * **HDMI 3D:** 720p 3D Frame Packing
* 🔒 **Direct Hardware Driver Control:** Patches `tve.rpl` and `avm.rpl` at low level to prevent HDMI clock/PLL drops (*No Signal* black screens).
* 🌍 **Multi-Region Architecture:** Built-in auto-detection for EUR consoles with dedicated static offset tables ready for USA and JPN regions.

---

## 📥 Installation

1. Download the latest `full_rgb_TV.wps` from the [Releases](https://github.com/Masana1/full-rgb-tv-wiiu/releases) section.
2. Copy `full_rgb_TV.wps` to your SD card at:
   ```text
   sd:/wiiu/environments/aroma/plugins/
Make sure WiiUPluginLoaderBackend is present in:

Plaintext
sd:/wiiu/environments/aroma/modules/
Insert the SD card into your Wii U and boot into Aroma.

🎮 Usage
Open the WUPS Config Menu at any time by pressing:

L + D-Pad Down + Minus (-) on the GamePad, Wii U Pro Controller, or Classic Controller.

Select Full RGB TV:

Full RGB Mode:

Disabled: Standard limited range RGB (16–235).

Enabled (Session only): Forces Full RGB (0–255) for the current session.

Enabled (Apply on Boot): Forces Full RGB (0–255) and saves it permanently for future coldboots.

Video Mode / Resolution: Choose your target resolution and refresh rate.

🛠️ Building from Source
Prerequisites
devkitPPC

wut

wups

Compilation
Bash
# Build standard release
make

# Build debug release (with UDP logging on port 4405)
make DEBUG=1

# Clean build directory
make clean
Building with Docker
Bash
# 1. Build the docker container
docker build . -t full-rgb-tv-builder

# 2. Compile
docker run -it --rm -v ${PWD}:/project full-rgb-tv-builder make

# 3. Clean
docker run -it --rm -v ${PWD}:/project full-rgb-tv-builder make clean
🙏 Special Thanks & Acknowledgments
Masana: Plugin author and reverse-engineering of AVM/TVE video registers.

Aroma Team: For the Aroma custom firmware environment.

Maschell & wiiu-env: For the Wii U Plugin System (WUPS) framework and devkit tools.

devkitPro: For maintaining the devkitPPC and WUT toolchains.

The Wii U Homebrew Community: For continuous support, testing, and feedback.

📜 License
This project is licensed under the GNU General Public License v3.0 (GPLv3). See the LICENSE file for details.
