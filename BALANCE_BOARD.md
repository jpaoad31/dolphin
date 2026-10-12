# Emulated Balance Board (branch `balance-board-emu`)

This branch adds an **Emulated Balance Board** option to the Balance Board slot in
Dolphin's Wii Remote settings, so Balance Board games (Wii Fit / Wii Fit Plus) can be played
or tested without a real board.

## Where the code comes from

- Upstream PR [dolphin-emu/dolphin#13337](https://github.com/dolphin-emu/dolphin/pull/13337)
  ("Core/DolphinQt: Implement Emulated Balance Board", Jordan Woyak, building on Pokechu22's
  #8268). It is still open upstream. Its two commits are cherry-picked unchanged onto
  `upstream/master` (8d2ad1a662); they applied cleanly. That PR has the device itself: its own
  Bluetooth slot, extension ID `00 00 a4 20 04 02`, calibration block with CRC, temperature,
  battery, the four load cells in the extension bytes, the mapping UI and TAS input.
- On top of it, in this branch:
  - The register read/write and report mode debug logs name the device (`BalanceBoard: ...`)
    and show the bytes written.
  - **Hotkeys** group on the board with **Step Off Toggle** (default key `O`) and
    **Step Off Hold**: removes all weight from the board.
  - Battery defaults to 100%; a per-frame INFO log was removed.
  - Unit tests: `Source/UnitTests/Core/HW/WiimoteEmu/BalanceBoardTest.cpp`.

## Building (macOS, Apple Silicon)

```sh
cd ~/github/dolphin-bb
git submodule update --init --recursive
brew install cmake ninja qt molten-vk ccache   # molten-vk: see note below
mkdir -p build && cd build
cmake .. -GNinja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$(brew --prefix qt);/opt/homebrew" \
  -DUSE_BUNDLED_MOLTENVK=OFF -DENABLE_NOGUI=ON -DENABLE_AUTOUPDATE=OFF \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
ninja > build.log 2>&1; echo "exit=$?" >> build.log
```

- The app is `build/Binaries/Dolphin.app`; the headless frontend is
  `build/Binaries/dolphin-emu-nogui`.
- `-DUSE_BUNDLED_MOLTENVK=OFF` is needed with the current Xcode: the bundled MoltenVK's
  dependencies project targets macOS 10.15, which Xcode no longer accepts, so the build fails.
  Homebrew's `molten-vk` is used and copied into the app bundle. This has nothing to do with
  the board.
- The app links Homebrew's dylibs, so it only runs on a Mac with those Homebrew packages.
- Unit tests: `ninja tests && ./Binaries/Tests/tests --gtest_filter='BalanceBoard*'`.
- `build-fastlog/` is a second build with `-DFASTLOG=ON` (DEBUG logs in a release build),
  `dolphin-emu-nogui` target only. It was used for the log checks below.

## Using it

1. **Options > Controller Settings > Wii Remotes**: keep "Emulate the Wii's Bluetooth
   adapter". Set **Wii Remote 1** to *Emulated Wii Remote* (Wii Fit needs a remote for the
   menus). Set **Balance Board** to *Emulated Balance Board* and click **Configure**.
2. Mappings (keyboard defaults in brackets):
   - **Buttons > Power** [P]: the board's front button (used to sync / power).
   - **Balance** stick [I/K/J/L = front/back/left/right]: centre of balance. Centred spreads the
     weight evenly over the four sensors; moving it shifts weight to the front/back and
     left/right sensors. Map it to a gamepad stick for analog control.
   - **Options > Weight** (default 60 kg, 0-250): total weight on the board.
     Options also has Battery (default 100%), Foot Separation and Foot Length (how far a full
     stick deflection moves the centre of balance).
   - **Hotkeys > Step Off Toggle** [O]: press once to step off (0 kg), press again to step
     back on. **Step Off Hold**: off the board while held.
   - **Left Foot / Right Foot > Step Off** (upstream's, unmapped by default): analog "lift a
     foot". Half pressed, that foot carries nothing and the other foot carries all the weight;
     fully pressed, the foot and its share of the weight leave the board (half the weight).
     Both fully pressed = off the board. For simple step on/off use the hotkey instead.
3. In Wii Fit Plus: the board connects by itself when the game starts (the board slot is
   connected like the remotes). When the game asks you to **step off** the board so it can
   measure the zero point, press **O** (Step Off Toggle) and wait; when it asks you to
   **step on**, press **O** again and keep the Balance stick centred and still until the
   weight is measured. After that, use the Balance stick for centre-of-balance exercises
   and the weight setting for body tests.
   If the game says the board is disconnected, check that the Balance Board slot is set to
   *Emulated Balance Board* before booting the game.

## What was verified

- Unmodified upstream builds and runs on this Mac (with the MoltenVK note above); the branch
  builds with no compiler warnings in the board/Wii Remote files; all 1035 unit tests pass, including the 4 new board tests
  (extension ID, calibration values and CRC-32 checked against an independent CRC, 0x34 data
  report converts back to 60 kg with valid temperature and full battery, conversion and
  centre-of-balance maths). The new tests fail when the ID or the CRC is broken.
- Headless boot of the original Wii Fit Plus (RFPE01, `main.orig.dol` in a copy of the
  extracted disc layout) with `dolphin-emu-nogui -p headless -v Null` and the FASTLOG build.
  The log shows the game talking to `BalanceBoard` the same way the SDK does with a real board:
  status/EEPROM reads, report mode 0x34, the `aa aa aa 55 aa aa aa` key to register 0xF1,
  extension ID read at 0xFA, encryption key set, then the calibration read chain 0x24 (16),
  0x34 (8), 0x3C (4), 0x60 (16), 0x21, 0x20.
- Game memory read through Dolphin's MemoryWatcher (addresses from the decomp's
  `config/RFPE01/symbols.txt`): `WBCCalRaw` holds the board's 10000/11700/13400 calibration,
  **`WBCCalibrated` = 1** (the SDK accepted the calibration CRC), and `WBCTotalLoad` (raw load
  over the zero points) is 6000 = 60 kg by default, 8000 with Weight = 80, 0 while Step Off
  Hold is active, and back to 6000 after.

## Not verified

- The Qt mapping window and Wii Remote settings UI: it builds, but no GUI was opened (the
  Hotkeys box sits under Options in the Balance Board mapping window).
- Playing through the Wii Fit Plus board screens (step off / step on / weigh-in / balance
  games): that needs the menus, so it has to be tried by hand.
- The Balance stick's effect inside the game (only the total load was read from game memory;
  the per-sensor split is covered by the unit test).
- Netplay, movies and save states with the board (the upstream PR has code for these;
  not tried here).

## Headless test recipe

The user dir used is `build/test-user` (short path: the MemoryWatcher socket path must be
under 104 characters). `Config/WiimoteNew.ini`:

```ini
[Wiimote1]
Source = 1
[BalanceBoard]
Source = 1
Hotkeys/Step Off Hold = timer(30) > 0.5
```

`Config/Logger.ini` with `[Options] Verbosity = 5, WriteToConsole = True` and
`[Logs] Wiimote = True`, `MemoryWatcher/Locations.txt` with `80816C08`, `80816C20`, `80810830`.
Listen on the datagram socket `build/test-user/MemoryWatcher/MemoryWatcher`, then run
`build-fastlog/Binaries/dolphin-emu-nogui -u build/test-user -p headless -v Null -e <copy>/sys/main.dol`
and grep its output for `BalanceBoard:`.
