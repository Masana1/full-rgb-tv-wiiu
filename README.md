# full-rgb-tv-wiiu
A WUPS plugin for Wii U to force Full RGB and change resolution on the fly.
# Full RGB TV (Wii U) 🎮

[English version](#-english-version) | [Version française](#-version-française)

---

## 🇬🇧 English Version

A WUPS plugin for **Wii U** (Aroma environment) to force **Full RGB** (11 master patches) and change the console's video resolution on the fly directly from the Aroma configuration menu, while avoiding conflicts with system settings.

> **Note:** This project and its source code were developed in collaboration with an artificial intelligence (Gemini).

### ✨ Features
* **Full RGB Force:** Applies 11 patches in RAM (AVM, TVE, and GPU Latte CSC) for optimal color reproduction.  
* **Interactive Resolution Selector:** Choose between **1080p**, **720p**, **1080i**, and **480p** right from the Aroma menu.  
* **Toggle Button (True/False):** Enable or disable Full RGB on the fly without rebooting.  
* **Automatic Storage:** Your preferences are saved using the WUPS storage API.  
* **Safe & Secure:** Includes built-in protection to prevent system freezes when exiting Café OS System Settings.  

### 📦 Installation
1. Download the latest `.wps` file from the [Releases](../../releases) page.
2. Place the `.wps` file on your SD card into the Aroma plugins directory:
   ```text
   sd:/wiiu/environments/aroma/plugins/

> **Compatibility Note:** Tested exclusively on European (PAL) Wii U consoles.
