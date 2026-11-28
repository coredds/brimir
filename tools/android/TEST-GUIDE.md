# Testing Brimir on the Samsung Galaxy A56

A step-by-step guide for testing the Android core on a real phone. You do not need to know Android tools. Allow
about one hour. At the end you fill in the report in [step 22](#22-the-report).

The first test was done on the developer's Samsung Galaxy A56 on 16 November 2026. This guide now holds what that
test verified and corrected. It is also the guide for the next phones: Mali and Adreno phones have not been tried, so
write down everything odd, also small things.

## How to read this guide

Each step says what you do and what you should **see**. Three tags mark how sure we are:

- **[A56]**: this was done on the Galaxy A56 (Exynos 1580, Xclipse 540 GPU, Android 17, RetroArch 1.22.2) and
  worked as written, or the text was corrected to what the phone showed.
- **[emulator]**: this was run in the Android emulator on a PC (an x86_64 virtual phone, Android 14 and 15) and
  worked as written. Your phone is a different machine, so please confirm it.
- **[not tested]**: nobody has run this step. It is normal RetroArch or Android behaviour, but menu words can differ
  a little. If something looks different, write down what you see.

What is still **not** proved: Mali and Adreno GPUs (the A56 has an Xclipse GPU), Android 8 to 16 on a phone (the
build supports Android 8.0 / API 26 and newer, but the emulator could not run API 26; the oldest it ran was Android
14; the A56 runs Android 17), and phones with 16 KB memory pages (see step 4).

There are two routes. Most steps are the same.

- **Phone only**: everything on the A56. This is enough to test and to report.
- **Phone + PC with adb**: optional. adb is a Windows/Linux tool that talks to the phone over USB. It only makes
  copying files and collecting the log easier. The adb steps are marked **PC route**. Skip them if you go phone only.

## What you need

- The Galaxy A56, charged, on Wi-Fi.
- A Saturn BIOS file (for example `mpr-17933.bin`). Not included, on purpose.
- At least one Saturn game as `.chd`, or `.cue` with its `.bin` files. Two or three different games are better: one
  light, one heavy (for example a 3D game).
- A GitHub account (to download the test build).
- PC route only: a PC, a USB cable, and adb from Google's "platform-tools".

## Part A: get the test build

### 1. Find the test build on GitHub

The test build is not a release. It is a file ("artifact") attached to a run of the release workflow without a tag
(a "dry run"). The person who started the run gives you the link. Otherwise: the repository
(https://github.com/coredds/brimir) > **Actions** > the newest run of the release workflow, started by hand.

Scroll to the bottom of the run page, to **Artifacts**. You should **see** `brimir-android-arm64`. Tap it to
download. GitHub asks you to log in first. The dry run exists (run 43828670633); the person testing with you sends
you the link to that run. **[not tested]** on a phone.

### 2. Unpack it

What you download from the Actions page is always a zip inside a zip: GitHub wraps every artifact in a zip, and the
artifact is itself `brimir-android-arm64.zip`. So you get `brimir-android-arm64.zip` (the outer one), which holds
another `brimir-android-arm64.zip` (the inner one). Unpack both: the inner zip holds the five files
`brimir_libretro_android.so`, `brimir_libretro.info`, `ANDROID-INSTALL.txt`, `LICENSE` and `THIRD_PARTY_NOTICES.txt`.

- **Phone only**: on the phone's browser, open the Actions page (if the Artifacts list does not show, choose
  "Desktop site" in the browser menu). Download the zip. Open the **My Files** app > Downloads, tap the zip, tap
  **Extract**. You should **see** a new folder `brimir-android-arm64` inside Download, holding the inner
  `brimir-android-arm64.zip`. Open that folder, tap the inner zip, tap **Extract** again. You should **see** a second
  folder `brimir-android-arm64` inside the first one, with the five files. So the core is probably at
  `Download/brimir-android-arm64/brimir-android-arm64/brimir_libretro_android.so`; note the real folder names, you open
  them in step 6. If you prefer, move `brimir_libretro_android.so` into Download itself. **[not tested]**
- **PC route**: download and unzip on the PC. Copy the files later (step 5): the core goes to Download itself.

Do not rename `brimir_libretro_android.so`. If your browser added something like ` (1)` to the name, rename it back.
RetroArch matches the core to the info file by this name.

## Part B: RetroArch

### 3. Install the right RetroArch

Install **RetroArch 1.22.2 or newer, 64-bit**. Two builds ran Brimir on the A56 **[A56]**:

- **The Play Store RetroArch** (1.22.2, 2026-10-30).
- **The libretro site's universal APK**, `RetroArch.apk` (package `com.retroarch`, 1.22.2):
  https://buildbot.libretro.com/stable/1.22.2/android/RetroArch.apk

The two keep their files in **different folders** (step 5 and 9 use them), and both need the `.info` file (step 8).
Pick one. The "AArch64" APK (`RetroArch_aarch64.apk`) was not run on the phone, only checked in the emulator and by
its manifest, so use one of the two above.

- Open the downloaded APK on the phone. Android asks you to allow installs from this source (browser or My Files):
  allow it. The Play Store needs nothing.
- **Samsung tip [not tested]**: if the install is blocked, Samsung's **Auto Blocker** (Settings > Security and
  privacy > Auto Blocker) may be on. Turn it off for the install, then on again.
- PC route: `adb install RetroArch.apk`. **[emulator]** for `adb install` with the universal APK. If the phone also
  has a Work profile (the A56 has one), use `adb install --user 0 ...` so that it goes to the main user
  **[not tested]**.

**How to tell which RetroArch you have.** Settings > Apps > RetroArch. Both builds are named **RetroArch** with the
package `com.retroarch`, so **only one of them can be installed**: to change, uninstall the first. Uninstalling also
removes that RetroArch's own files; on the A56 the saves of the Play Store build were deleted with it. So copy your
`saves` folder away first. The "AArch64" APK has its own package, `com.retroarch.aarch64`, and its own folders. Write
which build you used in the report.

| Build | RetroArch folder on the phone **[A56]** |
|---|---|
| libretro site APK | `/storage/emulated/0/RetroArch/` with `system`, `saves`, `logs`, ... |
| Play Store | `/storage/emulated/0/Android/media/com.retroarch/RetroArch/` with `downloads`, `system`, `logs`, ... |

The Play Store build asks for no storage permission and works in its own folder. The APK is built for an older Android
(`targetSdk` 28): on Android 17 it does not offer "Allow management of all files", but it still reads and writes
`/storage/emulated/0/RetroArch` **[A56]**. These are Android storage limits, not faults of Brimir.

### 4. Start RetroArch

Open it. If it asks for permission to access files or photos/media, **allow** it; the Play Store build asks for none
**[A56]** (see step 3 for the APK on Android 17). RetroArch needs its folder to be reachable (without it the folder
names in its log are garbage) **[emulator]**.

You should **see** RetroArch's menu: a dark-grey list (Main Menu) with a bar of icons at the bottom: back, home,
list, gear (Settings) and a play button (Quick Menu). **[emulator]** (this is the "MaterialUI" menu of the phone
build). After the first start the RetroArch folder of your build exists (the table in step 3) **[A56]**.

**If RetroArch closes by itself right after it opens**, your phone may use 16 KB memory pages (newer Android on newer
phones). What was tested **[emulator]**: on an Android 15 image with 16 KB pages, the **universal** RetroArch 1.22.2
(`RetroArch.apk`, package `com.retroarch`) crashes at start, before Brimir is involved, and the **universal nightly**
build (`RetroArch.apk` from the nightly page, package `com.retroarch`) runs. **[not tested]** on a phone. Fix: install
the nightly `RetroArch.apk` from https://buildbot.libretro.com/nightly/android/ and carry on (uninstall the other
RetroArch first, and back up its saves). The core itself is built for 16 KB pages and ran on the 16 KB emulator.

To check the page size with a PC: `adb shell getconf PAGE_SIZE` prints `4096` or `16384`. The A56 prints `4096`
**[A56]**, and RetroArch 1.22.2 starts there. Without a PC the page size is not shown in Settings that we know of. If
RetroArch starts, you can ignore all this. Write the number in the report if you have it.

## Part C: BIOS and game

### 5. Put the BIOS and the game on the phone

RetroArch looks for the BIOS in its **System** folder: `system` inside the RetroArch folder of your build (table in
step 3) **[A56]**: `/storage/emulated/0/RetroArch/system` for the APK,
`/storage/emulated/0/Android/media/com.retroarch/RetroArch/system` for the Play Store build. Confirm it on your phone
in the menu:

1. In RetroArch: gear icon (Settings) > **Directory** > **System/BIOS**. You should **see** the folder path. Note it
   and use that folder, whatever it is.
2. Copy the BIOS file into that folder. The names Brimir knows: `mpr-17933.bin` (US/EU), `Sega Saturn BIOS v1.01
   (JAP).bin`, `Sega Saturn BIOS (EUR).bin` (and a few more, listed in `brimir_libretro.info`). One BIOS is enough.
   With one Japan and one US/EU BIOS, Brimir picks the one that fits the disc **[A56]** (a Japanese disc got the
   Japanese BIOS).
3. Copy the game(s) into a folder RetroArch can browse. For example create a `roms` folder **inside the RetroArch
   folder of your build** (the Play Store build sees only its own folder, or what its file picker offers). For a
   `.cue` file keep its `.bin` files next to it.

- **Phone only**: use **My Files** and the RetroArch folder of your build. Move or copy the files there. Whether
  My Files can open the Play Store build's `Android/media` folder was not checked **[not tested]**.
- **PC route**: connect the phone with the USB cable. Pull down the notification shade, tap the USB notification and
  choose **File transfer**; the phone appears in the PC's file explorer **[not tested]** (the A56 files went over
  Windows' Phone Link). Or with adb:
  ```
  adb push mpr-17933.bin /storage/emulated/0/RetroArch/system/
  adb push brimir_libretro_android.so /storage/emulated/0/Download/
  ```
  (`adb push` into `/storage/emulated/0/Download` **[emulator]**.)

**Check the file sizes after you copy big files** **[A56]**. On the A56, `adb push` of large files stalled with
"device offline", and games copied over Windows' Phone Link were **incomplete on the phone** (8 MB and 23 MB
although the PC showed the full size). Compare the size on the phone with the size on the PC, or the checksum
(My Files > long-press the file > Details shows the size **[not tested]**). The core and the BIOS were checked after
copying and were complete; the games that were complete ran. An incomplete game file can give a game that does not
start or breaks later. If a copy is wrong, copy it again (another cable or route, for example USB "File transfer"
mode **[not tested]**) and check again.

The core file must be in a folder you can open in the next part, the one RetroArch shows in "Install or Restore a
Core". With the **Play Store build** that is its own `downloads` folder: put the core in
`/storage/emulated/0/Android/media/com.retroarch/RetroArch/downloads` **[A56]**. With the **APK** it is the Download
folder (step 6). Phone only (APK): the core is already there, in the folder from step 2 (for example
`Download/brimir-android-arm64`). PC route (APK): the `adb push` above, or the file transfer, puts it in Download
itself.

## Part D: install the core

### 6. Install the core in RetroArch **[A56]** **[emulator]**

This is the route that worked on the A56 (Load Core > Install or Restore a Core, from the folder RetroArch shows)
and on the emulator.

1. Main Menu > **Load Core** > **Install or Restore a Core**. You should **see** a file list with the title
   `Install or Restore a Core` and the folder RetroArch opens: `/storage/emulated/0/RetroArch/downloads` with the
   APK (only "Parent Directory" and "No Items" on the emulator), the `downloads` folder of the Play Store build's
   RetroArch folder with the Play Store build.
2. **Play Store build**: if the core is in that `downloads` folder (step 5), you should **see** it: go to step 4.
   **APK**: tap **Parent Directory** twice. The title now shows `/storage/emulated/0/`.
3. **APK**: open **Download**. If you pushed the file (PC route) or moved it there, you should **see**
   `brimir_libretro_android.so` in the list. If you extracted the zip on the phone (step 2), open the extracted folder
   first, and the folder inside it (probably `brimir-android-arm64` twice: the outer zip's folder, then the inner
   zip's folder; **[not tested]**). In the emulator test the file lay directly in Download.
4. Tap `brimir_libretro_android.so`. RetroArch copies it and returns to the Main Menu.

On the emulator RetroArch copied the file unchanged to its own cores folder. It needs no root and no special
permission. On the A56 the core file copied to the phone was checked against the checksum of the download (SHA-256
`FE3D0F8F...` for the test build) **[A56]**.

### 7. Load the core

Main Menu > **Load Core**. You should **see** `brimir_libretro_android.so` in the list (with "License: N/A") **[emulator]**.
Tap it. Back at the Main Menu the title bar now reads `1.22.2 - Brimir (0.11.1)` (the RetroArch version, then Brimir
and its version) and the list has **Start Core** and **Load Content** **[emulator]**.

If the core is not in the list: the file name changed (see step 2), or it is the wrong file.

### 8. The info file (needed to load games) **[A56]**

RetroArch learns which core can open a game from the core's `.info` file. **Without the `.info` file and the file
browser change below, on the A56, RetroArch hid the games or said that no core is available for `.chd` files**
(Play Store build; the APK build had both set). **Do both steps (the Core Info folder and the filter); they were
set together on the A56, and whether either alone is enough is unknown.** The
`.info` file also makes RetroArch show the name "Sega - Saturn/ST-V (Brimir)" and a list of BIOS files with
Present/Missing, a handy BIOS check. It works without root, but it has a **trade-off**:

RetroArch has its own info files for all its cores. If you point the **Core Info** folder to your own folder, RetroArch
reads info files **only** from that folder. Its own ones (292 of them in the emulator test) are then not seen
**[emulator]**. On a phone you only use to test Brimir this does not matter. If you use other cores in this RetroArch,
copy their info files into the same folder, or set the folder back to the default when you use them. (Online Updater >
"Update Core Info Files" may download them into the chosen folder; **[not tested]**.)

An official fix is planned: Brimir's `.info` is to be added to libretro's own set of core info files, so that RetroArch
ships it and "Update Core Info Files" installs it. Until then, use these steps.

1. Copy `brimir_libretro.info` into an `info` folder inside the RetroArch folder of your build (create it): for the
   APK `/storage/emulated/0/RetroArch/info`, for the Play Store build
   `/storage/emulated/0/Android/media/com.retroarch/RetroArch/info` **[A56]**.
2. Settings > **Directory** > **Core Info** > choose that folder. **[A56 via config file]**: on the A56 the setting
   (`libretro_info_path`) was written in RetroArch's configuration file, not through the menu; the menu wording is
   from RetroArch 1.22 and was not clicked on the phone **[not tested]**.
3. Quit RetroArch fully and open it again **[not tested]**.
4. **Also turn the file browser's filter OFF**: Settings > **User Interface** > **File Browser** > **Filter Unknown
   Extensions** (the setting is `menu_navigation_browser_filter_supported_extensions_enable`). On the A56 both the
   Core Info folder and the filter change were set together when the games showed and ran, and in the Play Store
   build the filter had to be off; we do not know if either alone is enough. That filter was changed in the
   configuration file; the label and menu path come from RetroArch 1.22.2's source and were not checked on a phone
   **[A56 via config file]** **[not tested]** (menu).
5. Main Menu > **Load Core**. You should **see** "Sega - Saturn/ST-V (Brimir)", "License: GPLv3". Load it, then
   Main Menu > **Information** > **Core Information**. It shows version, extensions, and the firmware (BIOS) list
   with "Present, Optional: ..." next to your BIOS and "Missing, Optional" next to the others **[emulator]**.

## Part E: turn on the log

### 9. Switch on logging

The Brimir core writes its messages into RetroArch's log. You need the log file for the report.

1. Settings > **Logging**: switch on **Logging Verbosity** and **Log to File**. The menu words are from RetroArch 1.22
   and were not clicked through on the emulator. **[not tested]** The emulator runs used these two settings on
   **[emulator]**. A "Core Logging Level" can stay at its default: the `Device report:` line is an info message and
   the default level shows it. (The emulator runs set the level to 0, "Debug". That is optional and makes the log
   longer.)
2. Settings > **Directory** > **Logs**: look at the folder it shows and write it down. It should be the `logs` folder
   inside the RetroArch folder of your build: `/storage/emulated/0/RetroArch/logs` for the APK **[A56]**,
   `/storage/emulated/0/Android/media/com.retroarch/RetroArch/logs` for the Play Store build **[A56]**. If it shows
   something else, set it to the `logs` folder of your build. (Do not point the Play Store build to
   `/storage/emulated/0/RetroArch/logs`: it has no permission there **[not tested]**. The AArch64 package keeps its
   own files under `Android/data/com.retroarch.aarch64/...`; Samsung's My Files may not open that folder.
   **[not tested]**)
3. Quit RetroArch fully (Main Menu > **Quit RetroArch**) and open it again, so the log starts clean **[not tested]**.
   Then check: Main Menu > **Load Core** and back out once. Open the `logs` folder with My Files. You should **see** a
   log file (probably `retroarch.log`, or with a date in its name if you switch on "Timestamp Log Files").
   **[not tested]**

**RetroArch starts a new log file every time it is launched** **[A56]**: the log of a session is gone after the next
start (one crash log was lost like this). Copy the log before you open RetroArch again.

The core's messages are in this file, **not** in Android's logcat **[emulator]** (the `Device report:` line was
absent from logcat in every emulator run). So the file is the thing to collect.

**If no log file appears** (empty `logs` folder, or no such folder): do not lose time on it. Instead (a) run the game
and take **screenshots** of any message RetroArch shows on the screen; (b) take a screenshot of the Core Options
screen and of the title bar; (c) use the PC route with adb (step 19), which can also look for the log file under
`/storage/emulated/0/RetroArch` and the app's own folder; and (d) write the exact text of what you see into the
report. Say in the report that no log file was produced, and with which RetroArch (package and version).

Tip: after each run below, copy the log file under a new name (for example `log-software.txt`), because later runs
may be added to the same file. Whichever way it works, the line `Device report:` appears once per game load.

## Part F: first run, Software renderer

Brimir's default is the Software renderer. It does not need a GPU. This is the baseline.

### 10. Check the video driver and the renderer

- Settings > **Drivers** > **Video**: write down the value. The A56 runs with the `gl` video driver were done with `gl` **[A56]**, and `gl` was the default
  on the emulator **[emulator]**. Leave it for this run.
- Load Brimir if it is not loaded yet (the title bar shows `Brimir`; if it does not, Main Menu > **Load Core** >
  `brimir_libretro_android.so`, or "Sega - Saturn/ST-V (Brimir)"). Once a core is loaded, the Main Menu has no "Load Core"
  entry; use **Unload Core** first to change it **[emulator]**.
  Then open the core options: **Quick Menu** (play icon at the bottom right) > **Core Options** (called just
  **Options** in some versions) **[not tested]**. In the **Video** group you should **see**
  **Renderer (restart required)** = **Software** **[not tested]** (the option exists in the core; its place in the
  menu was not clicked through).

### 11. Run the BIOS menu **[A56]** **[emulator]**

Main Menu > **Start Core**. You should **see** the Saturn BIOS screen. On a first start it is the language choice
screen. A moving picture, not black, and no crash. The BIOS menu ran on the A56 **[A56]**, and the core ran well over
600 frames in 20 seconds on the emulator.

If the picture stays black or RetroArch closes: write down what you did and go to step 21.

### 12. Run a game

Main Menu > **Load Content** > browse to your game (`.chd` or `.cue`) > select it. If RetroArch asks which core,
choose Brimir. You should **see** the Sega Saturn logo and then the game. **[A56]** Radiant Silvergun (Japan) ran
(`Game loaded successfully` in the log, the Japanese BIOS chosen for the Japanese disc), and the other games tried
were listed and ran once the info file was in place (step 8). If the game is not listed, or RetroArch says no core
is available for `.chd`, go back to step 8. (In the emulator only the BIOS menu ran through RetroArch; games ran
only in a test tool with identical frames to the Windows build.)

Play for two or three minutes, with a scene that moves a lot. Is the picture right? Is the sound clean? Does it stutter?
RetroArch's touch buttons appear on the screen; the A56 can also take a Bluetooth gamepad **[not tested]**.

**Saves.** The Saturn's save memory is the file `<game>.srm` in the `saves` folder (a cartridge save is `.bcr`).
Android can close RetroArch at any time, and RetroArch 1.22.2 does not write the save when you only leave the app
(its source saves on a normal quit, not when the app goes to the background):
- Use **Quick Menu > Close Content** (or Quit RetroArch) before you leave RetroArch.
- Swiping RetroArch away, or Android closing it for memory, keeps only what RetroArch's periodic autosave already
  wrote (Settings > **Saving** > **SaveRAM Autosave Interval**, 10 seconds by default on Android; the name is from
  RetroArch 1.22.2's source). Brimir copies in-game saves to that autosave about once a second while the game runs.
- After an in-game save keep playing 10 to 15 seconds before you switch to another app: the game is paused in the
  background, so a save made just before may still be waiting.

Earlier Brimir versions wrote in-game saves only when the content was closed, and a session without a game could write
an empty `Brimir.srm`; both are fixed in this build. These save rules come from RetroArch's source and Brimir's tests;
no app kill was tried on the phone **[not tested]**.

### 13. Read the device report

Brimir logs one line each time a game (or the BIOS menu) loads. Quit RetroArch (so the log file is complete), open
the log file (My Files, the `logs` folder from step 9; open it with a text viewer, or
share it to yourself) and search for
`Device report`. Format:

```
[Brimir] Device report: renderer software; GPU "none"; Vulkan API none (renderer needs 1.2); host threads 4; fallback "none"
```

That is the line from the emulator with the Software renderer and the `gl` driver **[emulator]**. The A56 with the
`gl` driver gave `renderer software`, `GPU "none"` and `host threads 8` **[A56]**: the GPU part says `"none"` while
RetroArch uses its `gl` driver. Copy the whole line into the report.

### 14. Read the frame rate **[not tested]**

A Saturn game runs at about 60 frames per second (59.94; 50 for European PAL games). RetroArch can show the number:

1. Settings > **On-Screen Display** > **On-Screen Notifications** (or in the same group): switch on **Display
   Frame Rate** (you may find it as "Notification Visibility > Display Framerate", or "Display Statistics" for more
   numbers). The menu words differ a little between RetroArch versions, so look for "frame rate".
2. Start the game again. A small number (frames per second) appears in a screen corner.

Write down for each game: the number while playing a calm scene and while playing a heavy scene. "60" is full speed.
A lower number than 59 means the phone cannot keep up. On the A56 the software renderer performed poorly and the
Vulkan renderer (step 15 to 17) ran at full speed at 1x (the developer's observation) **[A56]**; no frame rate numbers
were recorded.

Optional, a second measure from the log: Quick Menu > Core Options > **Performance Profiling** = ON (it is in the
System group **[not tested]**). Brimir then writes a table into the log every 300 frames
(`=== Performance Profile (300 frames) ===`) **[emulator]**. Look for `RunFrame_Total: avg=...ms`: under about
16.7 ms per frame (60 frames per second) is fast enough. Switch the option off again afterwards; it makes the log
noisy.

## Part G: second run, Vulkan renderer

The Vulkan renderer draws on the GPU. It needs three things (all or it falls back to software, with a reason in the
log): RetroArch's video driver set to `vulkan`, the core option Renderer = Vulkan, and a GPU with Vulkan 1.2.
The A56 has an Exynos 1580 (s5e8855) with an **Xclipse 540** GPU (AMD RDNA-based, not Mali or Adreno); its Vulkan
instance version is 1.4 and its OpenGL ES runs on Vulkan 1.3 (ANGLE) **[A56]**. On another phone, write down what
you have in the report.

### 15. Set RetroArch's video driver to vulkan **[not tested]** (menu)

Settings > **Drivers** > **Video** > choose **vulkan**. RetroArch tells you to restart. Quit RetroArch fully and
open it again. The Vulkan renderer ran at full speed at 1x on the A56 (the developer's observation) **[A56]**, but how its video driver was
switched was not recorded, and in the emulator RetroArch's Vulkan driver ran when set in its configuration file: the
menu switch itself was not clicked in either test.

If RetroArch crashes or shows nothing with `vulkan`, put the driver back to `gl` and write it into the report: that
is a finding on its own (it is RetroArch's driver, before Brimir is involved).

### 16. Set the core option

With Brimir loaded (step 7), open **Quick Menu** > **Core Options** > **Renderer (restart required)** >
**Vulkan**. **[A56]** (the option and values `Software` / `Vulkan` are in the core).

"Restart required" means the new value is read when a game is loaded. **After you switch the renderer, close the
content (Quick Menu > Close Content) and load the BIOS menu or the game again**: the renderer applies on load, and
a game that is already running keeps the old one **[A56]**.

### 17. Run and read the report again

Start the BIOS menu (Start Core) and then the game. You should **see** the same picture as before, not black.

Open the log and find the new `Device report:` line. Expect one of two results:

- **It ran with Vulkan** (what the emulator showed with a PC graphics card: **[emulator]**):
  ```
  [Brimir] Device report: renderer vulkan; GPU "AMD Radeon RX 9060 XT"; Vulkan API 1.3.0 (renderer needs 1.2); host threads 4; fallback "none"
  ```
  On the A56 the GPU name should be an Xclipse name and the Vulkan API 1.2 or newer. If the name is something else,
  say so in the report. (On the A56 Vulkan ran at full speed at 1x, the developer's observation **[A56]**, but nobody has read its `Device
  report:` line from the log yet: **[not tested]**. Please copy it into your report.)
- **It fell back to software**: the line says `renderer software` and the `fallback "..."` part says why (for example
  the Vulkan version is below 1.2, or the video driver is not vulkan). The picture must **still show** (software
  frames), never a black screen. A black screen with a fallback is a bug: report it. **[not tested]** on hardware
  (the fallback path was tested with a fake old Vulkan version on a PC, not on a phone).

**Internal resolution above 1x is not available on Android yet.** On the Android build the option **Internal
Resolution (Vulkan)** lists only "1x (Native)", and RetroArch itself resets an older setting of 2x or more to 1x (it
is not in the option list on Android). The core's own cap, with one log line
(`Internal resolution 2x is not available on Android (it can freeze some phone GPUs); using 1x`), only applies to
frontends that still pass such a value and to hand-edited options. Why: on the A56,
2x froze the phone **[A56]**: RetroArch stopped answering, the phone's display composer aborted on its watchdog
and Android restarted. The same freeze happened with Brimir's test tool alone, without RetroArch, at 2x from the
start of a game (Sega Rally's attract demo): a GPU job of the VDP1 upscaling never finished on the Xclipse 540. It is
not that the work is too heavy (heavier frames finished), and it also happens without switching the option while
playing.
Whether the fault is in Brimir's shader or in the phone's GPU driver is still open; it is being investigated.
**VDP1 Upscaling (Vulkan)** does nothing at 1x.

Read the frame rate for each game again (step 14) and write it down.

### 18. Optional: run-ahead (RetroArch's "Use Second Instance")

Run-ahead reduces input lag. "Use Second Instance" makes RetroArch load a **second copy** of the core into the same
app. The core was built so that two copies can live together. This is worth one check on a phone.

Settings > **Latency**: **Run-Ahead to Reduce Latency** ON, number of frames 1 or 2, **Use Second Instance** ON.
**[not tested]** (menu words).

- With the **Software** renderer: the game runs as before. The log shows `Brimir ... initializing...` and
  `Device report:` twice **[emulator]**, and no "Failed to create second instance".
- With the **Vulkan** renderer: RetroArch shows (and logs) once per game load:
  `Failed to create second instance. Run-Ahead will now use only one instance.` This is **expected, not a fault**.
  The game keeps running with run-ahead on one copy **[emulator]**.

A crash, a black screen or a frozen picture in either case: report it.

After you have tried this, switch run-ahead off again if you want clean frame rate numbers.

## Part H: send me the results

### 19. Collect the log

**Phone only.** In **My Files** open `Internal storage` > `RetroArch` > `logs`: the folder you set in step 9
(`/storage/emulated/0/RetroArch/logs`). **[not tested]** If it is empty, see "If no log file appears" in step 9.
Long-press the log file(s) > **Share** > email or cloud drive, or copy the files to a
folder you can attach to a GitHub issue. The log contains the folder paths and the names of your game files, but
nothing else personal. If the file is huge, the lines with `Device report:` and everything after `Loading game` may
be enough.

**PC route.** First turn on USB debugging on the phone (only needed for this route):

1. Settings > **About phone** > **Software information** > tap **Build number** seven times. Enter your PIN if
   asked. The phone says "Developer mode has been turned on". **[not tested]**
2. Settings > **Developer options** > switch on **USB debugging**. **[not tested]**
3. Connect the USB cable. The phone asks "Allow USB debugging?". Tap **Allow**.
4. On the PC: `adb devices` should list your phone as `device` (not `unauthorized`).

Then pull the logs:

```
adb pull /storage/emulated/0/RetroArch/logs ./a56-logs
```

`adb pull` was not run in the emulator (`adb push` was); on the A56 the log was read over adb **[A56]**. The `logs`
path is the one you set or saw in step 9: for the Play Store build it is
`/storage/emulated/0/Android/media/com.retroarch/RetroArch/logs` **[not tested]** with adb. If the folder is empty,
look for the file: `adb shell find /storage/emulated/0 -name "*.log"` (it may be under
`/storage/emulated/0/Android/data/com.retroarch.aarch64/files` or `.../com.retroarch/files`; adb may refuse that
folder on newer Android). **[not tested]** Do not rely on `adb logcat`: the core's lines are not in it
**[emulator]**.

Also useful with adb: `adb shell getconf PAGE_SIZE` **[A56]**.

**Problems with adb on the A56 [A56]:**
- **The connection dropped after a while** (`adb devices` showed the phone as `offline`, or not at all). Samsung's
  **Auto Blocker** can cut USB debugging after about 30 minutes: Settings > Security and privacy > **Auto Blocker**
  (the part that blocks commands over the USB cable, "Block commands by USB cable"): switch it off for the test, then
  on again. Also switch on Settings > **Developer options** > **Stay awake** so the phone does not lock while it is
  connected, and plug the cable in again if the phone asks "Allow USB debugging?". The Auto Blocker cause is the
  developer's observation; the menu words were not checked on the phone **[not tested]**.
- **Large files stall.** `adb push` of a big game stopped with "device offline" part way. Copy big files with the
  phone's own file manager (or USB "File transfer" mode **[not tested]**), and check the size afterwards (step 5).

### 20. Turn off what you switched on

Optionally set Performance Profiling back to OFF, run-ahead OFF, and USB debugging off (and Auto Blocker back on).

### 21. If something goes wrong

| What you see | What to do |
|---|---|
| RetroArch closes right at the start | See step 4 (nightly build for 16 KB pages). Write the RetroArch version. |
| The core is not in the Load Core list | File name changed, or you did not tap it in step 6. |
| Load Content does not list your games, or RetroArch says no core is available for `.chd` | The info file (step 8): Core Info folder set and RetroArch restarted; the file browser's "Filter Unknown Extensions" off. Also check that the game is in a folder RetroArch can open (step 5). |
| A game does not start, or breaks part way, and was copied over USB or Phone Link | Compare the size with the PC (step 5): incomplete copies happened. Copy it again. |
| The game says the save memory is not initialised or unformatted | Should not happen with this build (an empty save file is repaired when the game starts). If it does, copy the log and the `saves` folder's `.srm` file names and sizes. |
| The phone froze or restarted with the Vulkan renderer | Write what you did and the time. Do not look for 2x: the Android build has no internal resolution above 1x. |
| Black screen with the BIOS menu or a game | Note whether software or Vulkan. Copy the log. |
| No log file in `RetroArch/logs` | See "If no log file appears" in step 9. |
| No picture or no boot, and the log talks about a missing BIOS | The BIOS is not in the System folder from step 5. With the info file (step 8), Core Information shows "Missing". |
| A folder `retroarch_temp` shows up in the Load Core list | Left over from run-ahead when RetroArch was force-stopped **[emulator]**. Ignore it. |
| Phone gets hot, frame rate falls over time | Write the time it took to drop. Let it cool before the next run. |

Whatever happens, copy the log before you start the next run, and write down the step number.

## 22. The report

Copy this into a message or a GitHub issue. Fill in every line. Write "not tried" for what you skipped.

```
Brimir Android test report

Device model (Settings > About phone):
Android version / One UI version:
SoC and GPU (expected: Exynos 1580 / Xclipse 540; write what you see):
Memory page size (4096 or 16384, or "not checked"):
RetroArch: Play Store or APK, package (com.retroarch or com.retroarch.aarch64), version:
Test build: GitHub run / commit:
Brimir version that ran (title bar in the Main Menu, the Load Core list, or the log line
  "Brimir x.y.z initializing..."):
Info file used (step 8): yes / no; Core Info folder:
Filter Unknown Extensions (File Browser) turned off: yes / no

Core installed through "Install or Restore a Core": worked / did not work (what happened):
BIOS file(s) used:

SOFTWARE RENDERER (step 10-14)
  BIOS menu: ok / wrong / crash
  Device report line:
  Game 1 name: frame rate calm / heavy:      picture and sound:
  Game 2 name: frame rate calm / heavy:      picture and sound:

VULKAN RENDERER (step 15-17)
  RetroArch video driver vulkan started: yes / no
  Renderer that ran (from the device report): vulkan / software
  Vulkan version (from the device report):
  Device report line:
  Fallback reason (if any):
  Picture right (not black): yes / no
  Game 1 name: frame rate calm / heavy:      picture and sound:
  Game 2 name: frame rate calm / heavy:      picture and sound:
  (Internal resolution is 1x only on Android: nothing to report.)

RUN-AHEAD, second instance (step 18, optional)
  Software: ok / problem:
  Vulkan: message seen once (expected) / problem:

Anything wrong or strange (with the step number):

Log file(s) attached (from the logs folder of step 9): yes / no
  If no log file was produced: what you did instead (screenshots, exact text):
```

## Before the first Android release (and for later hardware results)

`ANDROID-INSTALL.txt` ships in the download with a "TESTED ON" block. The A56 report is in: the block now says what
the test showed (Galaxy A56, Exynos 1580 / Xclipse 540, Android 17: BIOS menu and games run; Vulkan renderer full
speed at 1x (the developer's observation), software renderer poor performance; the x86_64 emulator). It also names what was **not** tested:
Mali and Adreno phones, phones with 16 KB memory pages, app kill or swipe-away (the saves advice comes from
RetroArch's source and Brimir's tests), and that the A56's Vulkan `Device report:` line was not read.

**The rule stays for future hardware results.** The block claims only what a report shows. A phone that is still
waiting for its result is listed with the words "in testing" and no result; the release job
(`tools/android/check_release_text.sh`) refuses a tag build while the install text says that, and a dry run (no tag)
is allowed to say it. When a new report arrives, the controller replaces the line with the real result (device, GPU,
Android version, what ran) before the release tag.