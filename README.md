# I-5 debug menu

OpenRAC mod that restores the trigger for Ratchet & Clank's leftover debug menu.

## Installation

The easiest way to play this mod is with the [OpenRAC Launcher](https://github.com/OpenRAC/OpenRAC). Open the launcher, click the **Mods** tab at the top, and paste the link to this repository into the **Add a mod** field. Hit **Find**, switch on the mod and start playing.

## Usage

During gameplay, press **R3** (right stick click, or **V** on the keyboard) to open the debug menu.
Press **R3** again or **Select** to close it.

You can refer to this [TCRF page](https://tcrf.net/Ratchet_%26_Clank_(PlayStation_2)/Debug_Mode) for more information on the debug menu's specific features.

## How it works

When pressing **R3** in-game, the mod sets the level's game-mode to `-1` and saves the previous mode where the menu already expects it *(this trigger replaces the map's **R3** shortcut, which can already be accessed by pressing **Select** instead)*. While the debug menu is open, the game's full-screen clear is drawn as short black bands so each frame replaces the last instead of stacking on it. The original clear is restored when the menu closes.

On PAL copies of the game, some planets (such as Kerwan and Eudora) upload a texture onto the debug font's graphics block. To mitigate this, after each level loads the mod reuploads that font again at the block the game is using so the debug menu displays correctly.

Additionally, since the code behind the mod menu's **Invinc** option was removed from later prototypes and retail, it was reimplemented in this mod by hooking its **on** state to `Hero_TakeDamage` so it actually skips hit damage.

## Known issues

Currently, opening the ***Weapons***, ***Gadgets*** or ***Items*** pages on the pause menu will overwrite the debug font in memory with the weapon or gadget's preview moby texture, causing the debug menu to become visually corrupted. Reloading your save or changing planets should fix this.

Code for several of the debug menu's options was stripped from the retail game, so options like ***Profile***, all options in the ***Draw*** section, ***Bookmark***, ***Coll***, ***BattleCam*** and ***Actuator*** will not work as the underlying code is missing *(the state of the switches is not read by any function)*. If one of the earlier prototypes is ever decompiled, I should be able to reimplement most of these options into the mod.

Additionally, due to OpenRAC using its own renderer for the game world, options like ***Occl***, ***Drawdist***, and ***Screen*** will have no noticable effect in-game.

## Building

Building the mod requires Python 3 and some common build tools: CMake, Ninja, and a compiler (Xcode's command-line tools on macOS, `build-essential` on Linux, Visual Studio with "Desktop development with C++" on Windows).

Once you've installed the required dependencies, clone this repository to get started:
```sh
git clone https://github.com/ParadoxEpoch/rac1-debug-menu.git
cd rac1-debug-menu
```

Then, download the pinned OpenRAC SDK:
```sh
python3 tools/fetch_sdk.py
```

Finally, build and package the mod:
```sh
cmake -S . -B build -G Ninja
cmake --build build
python3 .sdk/openrac-mod-sdk-*/tools/package_mod.py build/package/debug_menu
```

The compiled mod will be in `dist/debug-menu-<version>-<platform>.zip`.

You can then install the packaged mod in the OpenRAC Launcher by dragging the zip file onto the **Add a mod** field in the **Mods** tab.
