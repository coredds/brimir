# Testing Brimir on the Samsung Galaxy A56

A step-by-step guide for the first test of the Android core on real hardware. You do not need to know Android
tools. Allow about one hour. At the end you fill in the report in [step 22](#22-the-report).

The result of this test decides whether the first Android release goes out. So write down everything odd, also
small things.

## How to read this guide

Each step says what you do and what you should **see**. Two tags mark how sure we are:

- **[emulator]**: this was run in the Android emulator on a PC (an x86_64 virtual phone, Android 14 and 15) and
  worked as written. The A56 is a different machine, so please confirm it.
- **[not tested]**: nobody has run this step. It is normal RetroArch or Android behaviour, but menu words can differ
  a little. If something looks different, write down what you see.

What the emulator did **not** prove: the arm64 build itself running (the emulator runs an x86_64 build of the same
source), real phone GPU drivers, Android 8 to 13 (the build supports Android 8.0 / API 26 and newer, but the emulator
could not run API 26; the oldest it ran was Android 14), and game play through RetroArch's menu (the emulator runs
checked the Saturn BIOS menu in RetroArch and a game only in a test tool).

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
download. GitHub asks you to log in first. **[not tested]** (the workflow has not run yet when this was written).

### 2. Unpack it

You get `brimir-android-arm64.zip`. Inside are three files: `brimir_libretro_android.so`, `brimir_libretro.info`
and `ANDROID-INSTALL.txt`. On some browsers the download is a zip inside a zip. If you only see another `.zip`,
unpack that one too. **[not tested]**

- **Phone only**: on the phone's browser, open the Actions page (if the Artifacts list does not show, choose
  "Desktop site" in the browser menu). Download the zip. Open the **My Files** app > Downloads, tap the zip, tap
  **Extract**. You should **see** the three files in a new folder in Download.
- **PC route**: download and unzip on the PC. Copy the files later (step 5).

Do not rename `brimir_libretro_android.so`. If your browser added something like ` (1)` to the name, rename it back.
RetroArch matches the core to the info file by this name.

## Part B: RetroArch

### 3. Install the right RetroArch

Install the **64-bit ARM ("AArch64") RetroArch 1.22.2** from the libretro site:
https://buildbot.libretro.com/stable/1.22.2/android/RetroArch_aarch64.apk

The Play Store RetroArch also works in principle, but it was not tested with Brimir. Prefer the APK.

- Open the downloaded APK on the phone. Android asks you to allow installs from this source (browser or My Files):
  allow it.
- **Samsung tip [not tested]**: if the install is blocked, Samsung's **Auto Blocker** (Settings > Security and
  privacy > Auto Blocker) may be on. Turn it off for the install, then on again.
- PC route: `adb install RetroArch_aarch64.apk`. **[emulator]** for `adb install` with the universal APK
  (`RetroArch.apk`); the aarch64 APK was checked only by reading its manifest, it was not installed in the emulator.

**How to tell which RetroArch you have.** There are two packages:

| Where from | App name on the phone | Package name |
|---|---|---|
| `RetroArch_aarch64.apk` | **RetroArch (AArch64)** | `com.retroarch.aarch64` |
| `RetroArch.apk` (universal) and the Play Store | **RetroArch** | `com.retroarch` |

(App names and package names come from the APK files themselves; the Play Store app was not checked.) You find the
name under Settings > Apps. Use **one** of them only. Both use the same `RetroArch` folder in the phone's storage, so
two installs get confusing. Write which one you used in the report.

### 4. Start RetroArch

Open it. If it asks for permission to access files or photos/media, **allow** it. RetroArch needs this to find its
folders (without it the folder names in its log are garbage) **[emulator]**.

You should **see** RetroArch's menu: a dark-grey list (Main Menu) with a bar of icons at the bottom: back, home,
list, gear (Settings) and a play button (Quick Menu). **[emulator]** (this is the "MaterialUI" menu of the phone
build). After the first start a `RetroArch` folder exists in the phone's internal storage
(`/storage/emulated/0/RetroArch`) **[emulator]**.

**If RetroArch closes by itself right after it opens**, your A56 may use 16 KB memory pages (newer Android on newer
phones). RetroArch 1.22.2 itself crashes there, before Brimir is involved **[emulator]** (Android 15 image with 16 KB
pages: the nightly build runs, 1.22.2 crashes at start). Fix: install the **nightly** build from the nightly page,
https://buildbot.libretro.com/nightly/android/ , and carry on. The file `RetroArch.apk` there is the one tested
**[emulator]**; whether an aarch64 file sits there too was not checked. The core itself is built for 16 KB pages
and ran on the 16 KB emulator.

To check the page size with a PC: `adb shell getconf PAGE_SIZE` prints `4096` or `16384` **[emulator]**. Without a
PC the page size is not shown in Settings that we know of; 4 KB is likely on the A56 (Android 15 Samsung phones)
**[not tested]**. If RetroArch starts, you can ignore all this. Write the number in the report if you have it.

## Part C: BIOS and game

### 5. Put the BIOS and the game on the phone

RetroArch looks for the BIOS in its **System** folder. Default on the phone:
`/storage/emulated/0/RetroArch/system` **[emulator]**.

1. In RetroArch: gear icon (Settings) > **Directory** > **System/BIOS**. You should **see** the folder path. Note it.
2. Copy the BIOS file into that folder. The names Brimir knows: `mpr-17933.bin` (US/EU), `Sega Saturn BIOS v1.01
   (JAP).bin`, `Sega Saturn BIOS (EUR).bin` (and a few more, listed in `brimir_libretro.info`). One BIOS is enough.
   With one Japan and one US/EU BIOS, Brimir picks the one that fits the disc.
3. Copy the game(s) into a folder RetroArch can browse. For example create `RetroArch/roms` (or use Download). For
   a `.cue` file keep its `.bin` files next to it.

- **Phone only**: use **My Files** > Internal storage > `RetroArch` > `system`. Move or copy the files there.
- **PC route**: connect the phone with the USB cable. Pull down the notification shade, tap the USB notification and
  choose **File transfer**; the phone appears in the PC's file explorer. Or with adb:
  ```
  adb push mpr-17933.bin /storage/emulated/0/RetroArch/system/
  adb push brimir_libretro_android.so /storage/emulated/0/Download/
  ```
  (`adb push` into `/storage/emulated/0/Download` **[emulator]**.)

The core file must also be in the phone's **Download** folder (or any folder you can browse in RetroArch) before the
next part. In the phone-only route it is already there from step 2.

## Part D: install the core

### 6. Install the core in RetroArch **[emulator]**

1. Main Menu > **Load Core** > **Install or Restore a Core**. You should **see** a file list with the title
   `Install or Restore a Core /storage/emulated/0/RetroArch/downloads` and only "Parent Directory" and "No Items".
2. Tap **Parent Directory** twice. The title now shows `/storage/emulated/0/`.
3. Open **Download**. You should **see** `brimir_libretro_android.so` in the list.
4. Tap `brimir_libretro_android.so`. RetroArch copies it and returns to the Main Menu.

On the emulator RetroArch copied the file unchanged to its own cores folder. It needs no root and no special
permission. If you used the Play Store RetroArch, this step may look different **[not tested]**.

### 7. Load the core

Main Menu > **Load Core**. You should **see** `brimir_libretro_android.so` in the list (with "License: N/A") **[emulator]**.
Tap it. Back at the Main Menu the title bar now reads `1.22.2 - Brimir (0.8.1)` (the RetroArch version, then Brimir
and its version) and the list has **Start Core** and **Load Content** **[emulator]**.

If the core is not in the list: the file name changed (see step 2), or it is the wrong file.

### 8. Optional: the info file

The `.info` file makes RetroArch show the name "Sega - Saturn (Brimir)" and a list of BIOS files with
Present/Missing. That list is a handy BIOS check. It works without root, but it has a **trade-off**:

RetroArch has its own info files for all its cores. If you point the **Core Info** folder to your own folder, RetroArch
reads info files **only** from that folder. Its own ones (292 of them in the emulator test) are then not seen
**[emulator]**. On a phone you only use to test Brimir this does not matter. If you use other cores in this RetroArch,
skip this step. (To get the other info files back there: Online Updater > "Update Core Info Files" may download them
into the chosen folder; **[not tested]**. Setting the folder back to the default also works.)

1. Copy `brimir_libretro.info` to `/storage/emulated/0/RetroArch/info` (create the folder).
2. Settings > **Directory** > **Core Info** > choose that folder. **[emulator]** (the setting was written to the
   configuration file there; the menu wording is from RetroArch 1.22.)
3. Quit RetroArch fully and open it again **[not tested]**.
4. Main Menu > **Load Core**. You should **see** "Sega - Saturn (Brimir)", "License: GPLv3". Load it, then
   Main Menu > **Information** > **Core Information**. It shows version, extensions, and the firmware (BIOS) list
   with "Present, Optional: ..." next to your BIOS and "Missing, Optional" next to the others **[emulator]**.

The notes in that info file still say "Optional Vulkan renderer (64-bit desktop builds)". Ignore that for now.

## Part E: turn on the log

### 9. Switch on logging

The Brimir core writes its messages into RetroArch's log. You need the log file for the report.

1. Settings > **Logging**: switch on **Logging Verbosity** and **Log to File**. If there is a "Core Logging Level",
   set it to the lowest number (Debug, 0) like in the tests **[emulator]**: the test runs used logging verbosity on,
   log to file on, frontend and core logging level 0. The menu words are from RetroArch 1.22 and were not clicked
   through on the emulator. **[not tested]**
2. Settings > **Directory** > **Logs**: note the folder. It is probably `/storage/emulated/0/RetroArch/logs`
   **[not tested]**. If it shows `<default>` or nothing, set it to that folder.
3. Quit RetroArch fully (Main Menu > **Quit RetroArch**) and open it again, so the log starts clean **[not tested]**.

The log file is probably called `retroarch.log` (or has a date in its name if you switch on "Timestamp Log Files").
The core's messages are in this file, **not** in Android's logcat **[emulator]** (the `Device report:` line was
absent from logcat in every emulator run). So the file is the thing to collect.

Tip: after each run below, copy the log file under a new name (for example `log-software.txt`), because later runs
may be added to the same file. Whichever way it works, the line `Device report:` appears once per game load.

## Part F: first run, Software renderer

Brimir's default is the Software renderer. It does not need a GPU. This is the baseline.

### 10. Check the video driver and the renderer

- Settings > **Drivers** > **Video**: write down the value. The emulator default was `gl` **[emulator]**.
  Leave it.
- Load Brimir if it is not loaded yet (the title bar shows `Brimir`; if it does not, Main Menu > **Load Core** >
  `brimir_libretro_android.so`, or "Sega - Saturn (Brimir)"). Once a core is loaded, the Main Menu has no "Load Core"
  entry; use **Unload Core** first to change it **[emulator]**.
  Then open the core options: **Quick Menu** (play icon at the bottom right) > **Core Options** (called just
  **Options** in some versions) **[not tested]**. In the **Video** group you should **see**
  **Renderer (restart required)** = **Software** **[not tested]** (the option exists in the core; its place in the
  menu was not clicked through).

### 11. Run the BIOS menu **[emulator]**

Main Menu > **Start Core**. You should **see** the Saturn BIOS screen. On a first start it is the language choice
screen. A moving picture, not black, and no crash. The core ran well over 600 frames in 20 seconds on the emulator.

If the picture stays black or RetroArch closes: write down what you did and go to step 21.

### 12. Run a game

Main Menu > **Load Content** > browse to your game (`.chd` or `.cue`) > select it. If RetroArch asks which core,
choose Brimir. You should **see** the Sega Saturn logo and then the game. **[not tested]** (in the emulator only the
BIOS menu ran through RetroArch; games ran only in a test tool with identical frames to the Windows build).

Play for two or three minutes, with a scene that moves a lot. Is the picture right? Is the sound clean? Does it stutter?
RetroArch's touch buttons appear on the screen; the A56 can also take a Bluetooth gamepad **[not tested]**.

### 13. Read the device report

Brimir logs one line each time a game (or the BIOS menu) loads. Quit RetroArch (so the log file is complete), open
the log file (My Files > RetroArch > logs; open it with a text viewer, or share it to yourself) and search for
`Device report`. Format:

```
[Brimir] Device report: renderer software; GPU "none"; Vulkan API none (renderer needs 1.2); host threads 4; fallback "none"
```

That is the line from the emulator with the Software renderer and the `gl` driver **[emulator]**. On the A56 expect
`renderer software`, and `host threads` = a number (the emulator had 4). The GPU part may still say `"none"` while
RetroArch uses its `gl` driver. Copy the whole line into the report.

### 14. Read the frame rate **[not tested]**

A Saturn game runs at about 60 frames per second (59.94; 50 for European PAL games). RetroArch can show the number:

1. Settings > **On-Screen Display** > **On-Screen Notifications** (or in the same group): switch on **Display
   Frame Rate** (you may find it as "Notification Visibility > Display Framerate", or "Display Statistics" for more
   numbers). The menu words differ a little between RetroArch versions, so look for "frame rate".
2. Start the game again. A small number (frames per second) appears in a screen corner.

Write down for each game: the number while playing a calm scene and while playing a heavy scene. "60" is full speed.
A lower number than 59 means the phone cannot keep up.

Optional, a second measure from the log: Quick Menu > Core Options > **Performance Profiling** = ON (it is in the
System group **[not tested]**). Brimir then writes a table into the log every 300 frames
(`=== Performance Profile (300 frames) ===`) **[emulator]**. Look for `RunFrame_Total: avg=...ms`: under about
16.7 ms per frame (60 frames per second) is fast enough. Switch the option off again afterwards; it makes the log
noisy.

## Part G: second run, Vulkan renderer

The Vulkan renderer draws on the GPU. It needs three things (all or it falls back to software, with a reason in the
log): RetroArch's video driver set to `vulkan`, the core option Renderer = Vulkan, and a GPU with Vulkan 1.2.
The A56 is believed to have an Exynos 1580 with an **Xclipse 540** GPU (AMD RDNA-based, not Mali or Adreno). That is
what we expect; please **confirm it in the report**.

### 15. Set RetroArch's video driver to vulkan **[not tested]**

Settings > **Drivers** > **Video** > choose **vulkan**. RetroArch tells you to restart. Quit RetroArch fully and
open it again. In the emulator RetroArch's Vulkan driver ran when set in its configuration file; the menu switch
itself was not clicked there.

If RetroArch crashes or shows nothing with `vulkan`, put the driver back to `gl` and write it into the report: that
is a finding on its own (it is RetroArch's driver, before Brimir is involved).

### 16. Set the core option

With Brimir loaded (step 7), open **Quick Menu** > **Core Options** > **Renderer (restart required)** >
**Vulkan**. **[not tested]** (the option and values `Software` / `Vulkan` are in the core).

"Restart required" means the new value is read when a game is loaded. So close the content (Quick Menu > **Close
Content**) and load the BIOS menu or the game again.

### 17. Run and read the report again

Start the BIOS menu (Start Core) and then the game. You should **see** the same picture as before, not black.

Open the log and find the new `Device report:` line. Expect one of two results:

- **It ran with Vulkan** (what the emulator showed with a PC graphics card: **[emulator]**):
  ```
  [Brimir] Device report: renderer vulkan; GPU "AMD Radeon RX 9060 XT"; Vulkan API 1.3.0 (renderer needs 1.2); host threads 4; fallback "none"
  ```
  On the A56 the GPU name should be an Xclipse name and the Vulkan API 1.2 or newer. If the name is something else,
  say so in the report.
- **It fell back to software**: the line says `renderer software` and the `fallback "..."` part says why (for example
  the Vulkan version is below 1.2, or the video driver is not vulkan). The picture must **still show** (software
  frames), never a black screen. A black screen with a fallback is a bug: report it. **[not tested]** on hardware
  (the fallback path was tested with a fake old Vulkan version on a PC, not on a phone).

With Vulkan, one more core option works at once: **Internal Resolution (Vulkan)** (1x native, 2x, 3x, 4x) and
**VDP1 Upscaling (Vulkan)**. Optional: try 2x and note whether the picture is sharper and whether the frame rate
drops. **[not tested]**

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

**Phone only.** In **My Files** open `Internal storage` > `RetroArch` > `logs` (path **[not tested]**; use the
folder you saw in step 9). Long-press the log file(s) > **Share** > email or cloud drive, or copy the files to a
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

`adb pull` was not run in the emulator (`adb push` was). The `logs` path is the one from step 9. Do not rely on
`adb logcat`: the core's lines are not in it **[emulator]**.

Also useful with adb: `adb shell getconf PAGE_SIZE` **[emulator]**.

### 20. Turn off what you switched on

Optionally set Performance Profiling back to OFF, run-ahead OFF, and USB debugging off.

### 21. If something goes wrong

| What you see | What to do |
|---|---|
| RetroArch closes right at the start | See step 4 (nightly build for 16 KB pages). Write the RetroArch version. |
| The core is not in the Load Core list | File name changed, or you did not tap it in step 6. |
| Black screen with the BIOS menu or a game | Note whether software or Vulkan. Copy the log. |
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
RetroArch: package (RetroArch or RetroArch (AArch64)), version, which APK:
Test build: GitHub run / commit:
Info file used (step 8): yes / no

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
  Internal Resolution 2x tried: yes / no, result:

RUN-AHEAD, second instance (step 18, optional)
  Software: ok / problem:
  Vulkan: message seen once (expected) / problem:

Anything wrong or strange (with the step number):

Log file(s) attached: yes / no
```

## Before the first Android release

`ANDROID-INSTALL.txt` says the core was tested on the Galaxy A56 and the x86_64 emulator, and that Mali and Adreno are
untested. That sentence is only true once this report is in. Do not release before that, and change the sentence if
the A56 result is different (for example if the Vulkan run fell back, or the GPU is not an Xclipse 540).
