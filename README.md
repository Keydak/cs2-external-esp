# 🕹️ CS2 External ESP | Recode

Simple external ESP for Counter-Strike 2. After years of development the codebase has been modernized for clarity and ease of use, featuring a redesigned interface, noticeable performance improvements, several quality-of-life additions, and automatic offset scanning to help maintain compatibility through game updates.

## Showcase

> Click the picture below to go to the showcase video

[![cs2esp](.github/showcase.png)](https://youtu.be/3WHHLUyHyzA)

## ✨ Added Features

> This fork builds on top of the original project. Everything below is new.

**Read only (ESP & overlays)**
* **Grenade ESP**: smoke & molotov areas that follow the map collision (blocked by walls, doors & props), trails, timers, landing spots & a prediction for the grenade in hand.
* **Item ESP**: dropped weapons & other items in the world.
* **Visibility check**: players behind walls or inside smokes are drawn differently.
* **Game crosshair**: the crosshair overlay now follows your own CS2 crosshair settings (style, color, size, dot).
* **Configs tab**: export / import configs & skin loadouts as separate files in `export/`, rename, delete & set one as **Default** (loaded on start).
* **Auto accept**: clicks ACCEPT when a match is found, brings the game to the front if needed. It only looks at the game window, nothing in the game is read or written for it.
* **Performance**: frame time breakdown in the watermark, lighter ESP & cache for low-end PCs.

**Memory writing (only with `-insecure`, see the warning below)**
* **Skin changer**: weapon skins, knives, gloves, agents & music kits (round music & MVP anthem), with player model previews.
* **View**: custom FOV, viewmodel override (FOV 40 – 120, offsets ±20) & third person.
* **Game radar**: enemies shown on the radar of the game.
* **Visuals**: no flash (with a strength slider), no smoke. Chams (players colored by the game itself) & the outline glow of the game for players, the bomb, dropped items, dropped grenades & thrown grenades (a color per type), in their ESP tabs.
* **Movement**: bunny hop, auto strafe (follows the mouse), quick stop & null binds.
* **Clan tag & name**: custom tag with 12 animations (blink, scroll, typing, bruteforce, wave, fade, decrypt, glitch, expand, pulse, slide…), several texts taking turns, shown in the clan slot, before / after the name or as the whole name, and a custom name. Set by the game itself on its main thread, only you see them.

> [!WARNING]
> **The features under "Memory writing" are dangerous.** They write into the memory of the game (and patch some of its code), which is far easier for an anti-cheat to notice than only reading it.
>
> * They are **only enabled when CS2 is launched with `-insecure`**, which disables VAC so you cannot join secured servers. Never try to use them in matchmaking or on VAC / third party anti-cheat servers, **you will get banned**.
> * Use them **offline** (practice / bots) only. You are the only one responsible for what happens to your account.
> * Changes are put back when the feature is turned off or the program is closed. Close the program **before** the game, so CS2 does not save the overridden values (e.g. `viewmodel_*`) into its config.

## 🌳 Simple Use

- You can download it from [**Releases**](https://github.com/IMXNOOBX/cs2-external-esp/releases) tab or **build it yourself** by following [developers instructions](#-developer-instructions).
- Open the game & the `cs2-external-esp.exe`, and thats it!
- **Star** the repository **if** you **like the project**! ⭐⭐⭐

> If the app crashes on startup, you may be missing the latest [Windows Visual C++ redistributables](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170#latest-supported-redistributable-version). Install the appropriate package for your system:
>
> - [64-bit Windows](https://aka.ms/vc14/vc_redist.x64.exe) *for most modern systems*
> - [32-bit](https://aka.ms/vc14/vc_redist.x86.exe) *for older devices*


> [!IMPORTANT]
> Make sure your game is in full screen windowed❗

## 💡 Important

> This project is provided *'as is'* for learning purposes with no warranties or responsibility from the developers/contributors. Use it at your own risk; you are the only one accountable for your actions

* **Detection Status:** This project is intended solely for single-player use. That said, no ban reports have been raised for other modes.
* **Anti-Virus Alerts:** This software may resemble malware in behavior because it accesses other processes memory, so it is commonly flagged by anti‑virus programs. I strongly encourage you to read the source code and build it yourself by following the [developers instructions](#-developer-instructions). All provided binaries are compiled via the [GitHub workflow](.github/workflows/auto_build.yml) from the repository source.

## 🕹️ Previous Versions

> This project has been reworked **3 times**, the current one been the third!

* [Discord Overlay (2023)](https://github.com/IMXNOOBX/cs2-external-esp/tree/discord-overlay) is the first and the **simplest** version of all, great to start learning.
* [Gdi Overlay (2023-2025)](https://github.com/IMXNOOBX/cs2-external-esp/tree/gdi-overlay) is an **improved version**, featuring automatic offset updating & configurations
* [Modern Version (Today)](https://github.com/IMXNOOBX/cs2-external-esp/tree/main) is the current and the **latest version**, with a click ui, automatic offset scanning and more!

## 📘 Developer Instructions

> - This project is mirrored in the following locations.
>	 - **GitHub**: [*github.com/IMXNOOBX/cs2-external-esp*](https://github.com/IMXNOOBX/cs2-external-esp) (main)
> 	 - **GitLab**: [*gitlab.com/IMXNOOBX/cs2-external-esp*](https://gitlab.com/IMXNOOBX/cs2-external-esp) (mirror)
>	 - **CodeBerg**: [*codeberg.org/IMXNOOBX/cs2-external-esp*](https://codeberg.org/IMXNOOBX/cs2-external-esp) (mirror)

1. Clone repository. Make sure you copy the command below to clone dependencies too

```sh
git clone --recursive https://github.com/IMXNOOBX/cs2-external-esp
```

* If you cloned the repository before submodules were added, run this command `git submodule update --init --recursive`

2. Build the app using **Visual Studio 2022** (or later)
	- Build: **`x64 - Release`**

3. Locate your binary file in the folder `<arch>/<configuration>`, e.g., `x64/Release`.

## 💫 Credits

* [**IMXNOOBX**](https://github.com/IMXNOOBX) for the original [cs2-external-esp](https://github.com/IMXNOOBX/cs2-external-esp), which this fork is built on.
* [**keydak**](https://github.com/keydak) for this fork and the features added to it.
* All [contributors](https://github.com/IMXNOOBX/cs2-external-esp/graphs/contributors) who have helped improve the project!
* [**a2x**](https://github.com/a2x) for his [offset dumper](https://github.com/a2x/cs2-dumper) and constant updates to it!

## 🔖 License & Copyright

This project is licensed under [**CC BY-NC 4.0**](https://creativecommons.org/licenses/by-nc/4.0/).

```diff
+ You are free to:
	• Share: Copy and redistribute the material in any medium or format.
	• Adapt: Remix, transform, and build upon the material.
+ Under the following terms:
	• Attribution: You must give appropriate credit, provide a link to the original source repository, and indicate if changes were made.
	• Non-Commercial: You may not use the material for commercial purposes.
- You are not allowed to:
	• Sell: This license forbids selling original or modified material for commercial purposes.
	• Sublicense: This license forbids sublicensing original or modified material.
```

### ©️ Copyright
The content of this project is ©️ by [IMXNOOBX](https://github.com/IMXNOOBX) and the respective contributors. See the [LICENSE.md](LICENSE) file for details.
