# Full RGB TV (Wii U) 🎮

[English version](#-english-version) | [Version française](#-version-française)

---

## 🇬🇧 English Version

A WUPS plugin for **Wii U** (Aroma environment) to force **Full RGB** (11 master patches) and change the console's video resolution on the fly directly from the Aroma configuration menu, while avoiding conflicts with system settings.

> **Note:** This project and its source code were developed in collaboration with an artificial intelligence (Gemini).  
> **Compatibility Note:** Tested exclusively on European (PAL) Wii U consoles.

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
Make sure you have an up-to-date Aroma environment running on your Wii U.

⚙️ Usage
Turn on your Wii U and open the Aroma configuration menu (usually by pressing L + Down + Select or your custom shortcut).

Go to the Full RGB TV menu.

Toggle Full RGB on/off and select your target resolution.

Close the menu: your changes are applied instantly!

🛠️ Building from Source
To build this plugin yourself, you need:

wups

wut

Install them according to their instructions, then compile using:

make

### 🙏 Credits & Acknowledgments
* **Maschell & Aroma Team:** For the [Wii U Plugin System (WUPS)](https://github.com/Maschell/WiiUPluginSystem) and the Aroma environment.
* **devkitPro & WUT Team:** For the toolchains and libraries powering Wii U homebrew development.
* **Gemini (Google):** For collaborative support with code analysis, patch structure, and documentation.
* **The Wii U Homebrew Community:** For continuous reverse-engineering efforts, documentation, and tools.

📜 License
This project is licensed under the GPLv3 license. See the LICENSE file for details.

🇫🇷 Version française
Un plugin WUPS pour Wii U (environnement Aroma) permettant de forcer le Full RGB (11 patchs maîtres) et de changer la résolution vidéo de la console à la volée directement depuis le menu de configuration Aroma, tout en évitant les conflits avec les paramètres système.

Note : Ce projet et son code source ont été rédigés et mis au point en collaboration avec une intelligence artificielle (Gemini).

Note de compatibilité : Testé exclusivement sur les consoles Wii U européennes (PAL).

✨ Fonctionnalités

Forçage Full RGB : Applique 11 patchs en RAM (AVM, TVE, et GPU Latte CSC) pour un rendu des couleurs optimal.

Sélecteur de résolution interactif : Choisissez entre le 1080p, 720p, 1080i et 480p depuis le menu Aroma.

Bouton d'activation (True/False) : Activez ou désactivez le Full RGB à la volée sans redémarrer.

Sauvegarde automatique : Vos préférences sont enregistrées via l'API de stockage WUPS.

Sécurisé : Intègre une protection pour éviter les gels lors de la sortie des Paramètres de la console de Café OS.

📦 Installation

Téléchargez la dernière version du fichier .wps dans les Releases.

Placez le fichier .wps sur votre carte SD dans le dossier des plugins Aroma :

sd:/wiiu/environments/aroma/plugins/

Assurez-vous d'avoir un environnement Aroma à jour sur votre console Wii U.

⚙️ Utilisation
Allumez votre Wii U et ouvrez le menu de configuration Aroma (généralement en appuyant sur L + Down + Select ou selon votre configuration de touches).

Rendez-vous dans le menu Full RGB TV.

Activez ou désactivez l'option Full RGB et choisissez votre résolution cible.

Fermez le menu : vos modifications sont appliquées instantanément !

🛠️ Compilation depuis les sources
Pour compiler ce plugin vous-même, vous avez besoin de :

wups

wut

Installez-les selon leurs instructions, puis compilez via :

make

### 🙏 Remerciements & Crédits
* **Maschell & l'équipe Aroma :** Pour le [Wii U Plugin System (WUPS)](https://github.com/Maschell/WiiUPluginSystem) et l'environnement Aroma.
* **devkitPro & l'équipe WUT :** Pour les chaînes de compilation et bibliothèques indispensables au développement sur Wii U.
* **Gemini (Google) :** Pour la collaboration sur l'analyse de code, l'intégration des patchs et la documentation.
* **La communauté Homebrew Wii U :** Pour l'ensemble des recherches, des outils et du partage de connaissances.

📜 Licence
Ce projet est sous licence GPLv3. Voir le fichier LICENSE pour plus de détails.
