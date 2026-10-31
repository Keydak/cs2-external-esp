# Fortnite emotes for CS2

A standalone DLL: 79 Fortnite dances, 39 emotes, their music, CS2's own celebration clips and a Fortnite-style emote
wheel. Only you see them.

## Install

1. Build it (below) or take `build\bin\emotes.dll`.
2. Put the Emote Pack in your Documents folder as `Documents\Emotes`:
   `Emotes\csgo_addons\characters\kolka\fortnite_dance.vmdl_c`, `Emotes\music\*.wav`, `Emotes\icons\*.png`.
   Optional: `Emotes\fonts\` with Burbank Big Condensed Black for the wheel titles (or have it installed).
   Documents is found through Windows, so a moved or OneDrive Documents folder works too.
   The zip from the site can be extracted into `Documents\Emotes` as it is: the DLL looks up to three folders deep
   for the one holding `csgo_addons`. If it finds none, the game console says where to put it.
3. Inject `emotes.dll` into cs2.exe. The first run writes `emotes.ini` next to the DLL.

## Use

- Tap the wheel key (`B`) to open the wheel, or hold it and let go on an emote. Click to play, right-click or Esc to
  close, scroll for the next page.
- Tap the key again to stop. Moving, shooting, dying and a new round stop it too.
- The view goes to third person while you emote and back after.
- `End` unloads the DLL.

## emotes.ini

```
[emotes]
wheel_key=0x42      ; virtual key code
unload_key=0x23
music=1
volume=70           ; 0-100
slot1=0             ; Favourites page: 0-18 = CS2 clip, 1000+i = Fortnite dance i, -1 = empty
...
```

## Build

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

Dear ImGui and MinHook are fetched at configure time.

## How it works

- **Hooks** (`src/hooks`): Present/ResizeBuffers draw the wheel; the window procedure, SetCursorPos, ClipCursor,
  SetRelativeMouseMode and MouseInputEnabled free the mouse while it is open. FrameStageNotify stage 12 runs the
  emote each frame, LevelShutdown and OnRemoveEntity forget props the game destroys, and GenerateScenePrimitives
  hides the real player while its stand-in dances.
- **The dance** (`src/features/FortniteDances.cpp`): a client-only `prop_dynamic` plays it, and a copy of your agent
  is bonemerged onto it (`ClientProp.cpp`). The model loads through a resource manifest from the pack, added to the
  game's GAME search path.
- **CS2 clips and the wheel** (`src/features/EmoteWheel.cpp`): clips are sampled onto your pose in
  ApplyAnimGraphPose. Third person comes from a CAM_Think detour (`Thirdperson.cpp`).
- **Music** (`src/features/Music.cpp`): waveOut on a pool thread, never the game's sound system.
- **Messages** print to the game console with an `[emotes]` prefix.
