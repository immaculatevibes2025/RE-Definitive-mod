# Resident Evil 1 - Definitive Mod


> **About this fork**
>
> This is a modded fork of [ecruells/resident-evil-pc-decomp](https://github.com/ecruells/resident-evil-pc-decomp),
> the Resident Evil (1997 PC) decompilation by **ecruells** and contributors. All of the
> decompilation, engine and port work is theirs; this fork only adds the changes listed
> below on top of it, under the same GNU GPL v3 license.
>



Resident Evil: Definitive Mod

Resident Evil: Definitive Mod aims to bring together the exclusive content, features, and gameplay changes from the various versions of the original 1996 Resident Evil into one package, while introducing modern quality-of-life improvements for the PC version.

The goal is to create the most complete and feature-rich way to experience the original Resident Evil without losing the classic charm of the original game.

🆚 Before and After

See how the original PC version compares to the Resident Evil: Definitive Mod

Original                                                            Definitive Mod 
<img width="3788" height="1411" alt="OG vs DE Title Screen" src="https://github.com/user-attachments/assets/e6008f6e-ccfb-412e-8c49-3461cb4c1581" />
<img width="3807" height="1433" alt="OG version Options menu" src="https://github.com/user-attachments/assets/07497b15-8159-49d8-b2fb-8bdff5ffa91b" />












✨ Features

The mod combines content from different releases of Resident Evil and adds several improvements designed to make the PC version more convenient and enjoyable to play.

🕹️ Version-Exclusive Content

Battle Game — An exclusive game mode originally found in the Sega Saturn version of Resident Evil.

Ticks — An exclusive enemy originally found in the Sega Saturn version.

Saturn-Exclusive Costumes — Additional costumes originally exclusive to the Sega Saturn version of Resident Evil.

⚙️ Modern Quality-of-Life Improvements

Director's Cut Support — This mod supports the Director's Cut version of Resident Evil with the Sega Saturn content included.

Overhauled Options Menu — A completely updated in-game options menu with video, sound, control and gameplay settings.

CRT Shader — A classic CRT-style presentation directly through the in-game options.

Anti-Aliasing — Smooth jagged edges with SMAA anti-aliasing, switched on or off in the in-game video options.

Widescreen — Play the game in widescreen, with an option to preserve the original 4:3 aspect ratio.

60 FPS — An interpolated 60 FPS option that preserves the original 30 FPS game logic while providing smoother visual motion.

Modernized Controls — The default keyboard/mouse and gamepad controls have been adjusted to feel more accessible.

(Optional) Knife Button — A dedicated knife button based on the controls introduced in the Nintendo DS version, Resident Evil: Deadly Silence.

(Optional) Quick Turn — A dedicated quick-turn button, also based on Deadly Silence.

(Optional) Reload Button — A dedicated reload button, also based on the Nintendo DS version.

Mouse Button Support — Mouse buttons can now be assigned and used as in-game controls.

Expanded Keyboard Support — Additional keyboard keys, including Shift and Tab, can now be used for in-game controls.

Optional Gameplay Changes: The new gameplay features are completely optional. They can be disabled if you want to preserve the original gameplay experience of the 1996 release. The goal is to simply provide modern conveniences. 
They are disabled by default. 

The aim is to make these features available directly within the game, eliminating the need for a separate configuration program or manual editing of config.ini.

📦 Requirements

Only **Windows** (For now)

A legitimate PC copy of Resident Evil (Steam, GOG etc) to extract the PC assets from. 

(Optional) A legitimate Sega Saturn copy of Resident Evil to extract the Saturn assets.

(Optional) A legitimate PlayStation copy of Resident Evil: Director's Cut.

🔧 How to Install

1. Download the latest build

2. Extract the Assets using the "RE Asset Migrator.exe"

Note: FFmpeg is not included. Download it separately if you want to convert PS1 video files or extract the Director's cut. Place it in the folder alongside the RE Asset Migrator.exe.

4. Play

Important: This project does not provide copyrighted game data or game assets. You must supply your own copies of the PC, Sega Saturn and PlayStation versions of Resident Evil.

❤️ Credits

This project would not be possible without the work of the Resident Evil community and the developers of the Resident Evil 1 PC Decompilation project.

SMAA — The anti-aliasing uses SMAA (Enhanced Subpixel Morphological Antialiasing) by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez, used under the MIT license ([iryoku/smaa](https://github.com/iryoku/smaa)).

Claude — This project was also used as a test to see how well Anthropic's new Opus 5.5 model could assist with bug testing, coding, debugging, and general development tasks.

⚠️ Known Issues

Battle Game Enemy Health — The exact health values of some Battle Game bosses may differ from the original Sega Saturn version.

Saturn-Exclusive Costumes — The Saturn-exclusive costumes may exhibit minor graphical glitches due to differences between the original Saturn hardware and the PC version.

Battle Game Ending Sequence — The ending shot, music, and rank screen are not a 1:1 recreation of the Sega Saturn version.

Ticks — The Tick may exhibit slightly different behavior compared to its implementation in the original Sega Saturn version. Most notably his decapitation animation is different. 

60 FPS — The interpolated 60 FPS mode may introduce some unintended visual or gameplay bugs.

Widescreen — Some camera angles may be tricky to view correctly when playing in widescreen due to the game's original fixed-camera design.

📝 TODO

Battle Game Accuracy — Improve the Battle Game to more accurately match the original Sega Saturn version, including enemy health and other gameplay details.

Tick Accuracy — Improve the Tick's behavior to more accurately match the original Sega Saturn version. 

Bug Fixes — Investigate and squash any remaining bugs and unintended issues found within the mod.

More Platforms - Support for Linux and Steamdeck. 

Resident Evil 1.5 Content - The decompilation should allow for more ambitious modding opportunities. 


## License

This project is licensed under the **GNU General Public License v3.0** — see
[`LICENSE`](LICENSE) for the full text.

In practice: fork it, port it, mod it — but derivative works have to ship
their source under the same terms, so improvements stay available to
everyone.

The license covers **only the source in this repository** (`src/`, `tools/`,
`tests/`, `docs/`). It does not and cannot grant any rights over the original
game.

### Legal notice

- **No game assets are distributed here.** The repository contains source code
  only; `assets\` is git-ignored. Running the build requires your own legally
  obtained copy of *Resident Evil* for PC (the GOG or 1997 retail USA release,
  or the Japanese *Biohazard* PC release) to supply the data files.
- *Resident Evil*, its code, assets, characters and trademarks are the
  property of **CAPCOM CO., LTD.** This project is an independent
  reverse-engineering and preservation effort, **not affiliated with,
  authorized, endorsed or sponsored by Capcom** in any way.
- The decompiled logic in `src\` is derived from the original executable for
  interoperability, documentation and preservation purposes. It is published
  in the belief that this constitutes fair use / lawful reverse engineering in
  the contributors' jurisdictions; no rights over Capcom's copyrighted work
  are claimed or granted.
