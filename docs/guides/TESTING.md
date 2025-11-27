# RetroArch Testing Guide for Brimir Core

## Overview

This guide will help you test the Brimir libretro core with RetroArch, the most popular libretro frontend.

## Prerequisites

### Required
- [OK] **RetroArch installed** (download from [https://www.retroarch.com/](https://www.retroarch.com/))
- [OK] **Brimir core built** (you have this!)
- [OK] **Saturn BIOS files** (in `tests/fixtures/` - you have these!)
- [OK] **Saturn disc image** (CUE/BIN, ISO, CHD, etc.)

### Optional
- USB controller or gamepad
- Headphones/speakers for audio testing

## Step 1: Build the Core for RetroArch

The core DLL is already built! It's located at:
```
build\bin\Release\brimir_libretro.dll
```

### Verify the Build
```powershell
# Check the file exists and size
Get-Item build\bin\Release\brimir_libretro.dll | Select-Object Name, Length

# Should show:
# Name                    Length
# ----                    ------
# brimir_libretro.dll     ~XX MB
```

## Step 2: Install Core in RetroArch

### Option A: Manual Installation (Recommended for Testing)

1. **Find RetroArch cores directory:**
   ```powershell
   # Common locations:
   # %APPDATA%\RetroArch\cores
   # C:\RetroArch\cores
   # C:\Program Files\RetroArch\cores
   ```

2. **Copy the core:**
   ```powershell
   # Example (adjust path to your RetroArch installation)
   Copy-Item build\bin\Release\brimir_libretro.dll "$env:APPDATA\RetroArch\cores\"
   ```

3. **Copy the core info file:**
   ```powershell
   # Copy to RetroArch info directory
   Copy-Item brimir_libretro.info "$env:APPDATA\RetroArch\info\"
   ```

### Option B: Direct Testing (Development Mode)

You can also point RetroArch directly to your build directory:
- Load Core → Load Core from File → Navigate to `build\bin\Release\brimir_libretro.dll`

## Step 3: Set Up BIOS Files

RetroArch needs BIOS files in its system directory.

### Locate RetroArch System Directory
1. Open RetroArch
2. Go to **Settings → Directory → System/BIOS**
3. Note the path (typically `%APPDATA%\RetroArch\system`)

### Copy BIOS Files
```powershell
# Copy your BIOS files to RetroArch system directory
Copy-Item tests\fixtures\sega_101.bin "$env:APPDATA\RetroArch\system\"
Copy-Item tests\fixtures\sega_100.bin "$env:APPDATA\RetroArch\system\"
Copy-Item tests\fixtures\sega1003.bin "$env:APPDATA\RetroArch\system\"
```

### Verify BIOS Files
In RetroArch system directory, you should have:
```
system/
├── sega_101.bin                       (US BIOS v1.01 - 512 KB) ✅ Recommended
├── sega_100.bin                       (EU BIOS v1.00 - 512 KB)
└── Sega Saturn BIOS v1.01 (JAP).bin   (JP BIOS v1.01 - 512 KB)
```

**⚠️ IMPORTANT:** Do NOT use `sega1003.bin` (JP BIOS v1.003) - it is not supported.

## Step 4: Prepare a Test Game

### Get a Saturn Game Disc Image

**Legal Options:**
1. **Dump your own games** from physical discs you own
2. **Homebrew games** (free, legal Saturn homebrew)
3. **Demo discs** from magazine coverdiscs you own

### Supported Formats
- `.cue` + `.bin` (CD image + binary)
- `.iso` (single file ISO)
- `.chd` (compressed)
- `.ccd` (CloneCD)
- `.mds` (Media Descriptor)

### Place Game in ROMs Directory
```powershell
# Example: Copy to RetroArch Saturn ROMs folder
$romsPath = "$env:APPDATA\RetroArch\roms\Sega - Saturn"
New-Item -ItemType Directory -Force -Path $romsPath
Copy-Item "path\to\your\game.cue" $romsPath
Copy-Item "path\to\your\game.bin" $romsPath
```

## Step 5: Load and Test the Core

### Initial Core Test (No Game)

1. **Launch RetroArch**
2. Go to **Main Menu → Load Core**
3. Select **Brimir**
4. You should see "Core loaded: Sega - Saturn (Brimir)"

### Load a Game

1. **With core loaded**, go to **Load Content**
2. Navigate to your Saturn game (`.cue` file)
3. Select the game
4. RetroArch should:
   - Load the BIOS
   - Load the game
   - Start emulation

### Expected First Run

On first load, you should see:
- **BIOS boot screen** (Sega Saturn logo)
- **Loading screen** (if game loads)
- **Audio** (from BIOS or game)
- **Controller** responsive

## Step 6: Testing Checklist

### [OK] Basic Functionality

**BIOS Loading:**
- [ ] Core loads without errors
- [ ] BIOS boot screen appears
- [ ] No crash on startup

**Video Output:**
- [ ] Screen displays correctly
- [ ] Colors look correct
- [ ] No visual glitches
- [ ] Proper aspect ratio

**Audio Output:**
- [ ] Sound plays
- [ ] Audio is synchronized
- [ ] No crackling/popping
- [ ] Stereo sound working

**Controller Input:**
- [ ] D-pad responds (Up/Down/Left/Right)
- [ ] Face buttons work (A/B/C/X/Y/Z)
- [ ] Shoulder buttons work (L/R)
- [ ] Start button works
- [ ] No input lag

### [OK] Advanced Features

**Save States:**
- [ ] Save state (F2 or Quick Save)
- [ ] Load state (F4 or Quick Load)
- [ ] State saves successfully
- [ ] State loads correctly
- [ ] Game state preserved

**Reset:**
- [ ] Core reset works (Ctrl+R)
- [ ] Soft reset functional
- [ ] No crashes on reset

**Performance:**
- [ ] Runs at correct speed (~60 FPS NTSC / 50 FPS PAL)
- [ ] No major slowdowns
- [ ] Frame timing consistent

## Step 7: RetroArch Quick Menu

While game is running, press **F1** or **Escape** to open Quick Menu:

### Useful Options
- **Resume** - Continue game
- **Reset** - Reset emulator
- **Save State** - Quick save
- **Load State** - Quick load
- **Take Screenshot** - Capture frame
- **Close Content** - Exit game

### Configuration
- **Controls** - Remap buttons
- **Options** - Core-specific settings (if any)
- **Information** - Core info

## Step 8: Troubleshooting

### Core Won't Load
```
Error: "Failed to load core"
```
**Solutions:**
- Verify `brimir_libretro.dll` is in cores directory
- Check for missing dependencies (fmt.dll should be in same folder)
- Try loading core manually (Load Core from File)

### BIOS Not Found
```
Error: "Cannot find BIOS"
```
**Solutions:**
- Verify BIOS files are in RetroArch system directory
- Check filenames match exactly: `sega_101.bin`, `sega_100.bin`, `sega1003.bin`
- Verify file sizes (exactly 524,288 bytes each)

### Game Won't Load
```
Error: "Failed to load content"
```
**Solutions:**
- Check file format is supported (.cue, .iso, .chd)
- For .cue files, ensure .bin file is in same directory
- Verify disc image is valid (not corrupted)

### No Video Output
```
Black screen or no display
```
**Solutions:**
- Check video driver in RetroArch settings (try 'gl' or 'd3d11')
- Verify display resolution settings
- Try fullscreen mode (F11)

### No Audio Output
```
Silent or no audio
```
**Solutions:**
- Check audio driver in RetroArch settings
- Verify Windows audio is not muted
- Check RetroArch audio settings (Settings → Audio)

### Controller Not Working
```
Input not responsive
```
**Solutions:**
- Configure controls in RetroArch (Settings → Input)
- Use Input → Port 1 Controls to bind buttons
- Check controller is detected by Windows
- Try keyboard input first (arrow keys, Z/X/A/S/Q/W/Enter)

### Poor Performance
```
Slow, stuttering, or laggy
```
**Solutions:**
- Check Task Manager for CPU usage
- Close unnecessary applications
- Try different video driver
- Disable shaders/filters

### Core Crashes
```
RetroArch crashes or freezes
```
**Debug Steps:**
1. Check RetroArch log: `%APPDATA%\RetroArch\retroarch.log`
2. Look for error messages
3. Try with different game
4. Verify BIOS files are correct

## Step 9: Performance Testing

### Frame Rate Check
1. Enable FPS display: **Settings → On-Screen Display → FPS Counter**
2. Target: **~60 FPS** (NTSC) or **~50 FPS** (PAL)

### Audio Sync Check
1. Listen for audio crackling/popping
2. Check audio doesn't drift out of sync
3. Settings → Audio → Audio Sync (should be ON)

### Input Latency Check
1. Test button responsiveness
2. Should feel immediate
3. Settings → Input → Input Latency (lower is better)

## Step 10: Advanced Testing

### Multi-Controller Test
1. Connect second controller
2. Configure Port 2 in Controls settings
3. Test 2-player games

### Save State Slots
1. Use State Slot +/- (F6/F7) to change slots
2. Test multiple save slots (0-9)
3. Verify each slot saves/loads correctly

### Screenshot Test
1. Take screenshot (F8)
2. Check `%APPDATA%\RetroArch\screenshots`
3. Verify image quality

## RetroArch Keyboard Controls (Default)

| Function | Key |
|----------|-----|
| **Menu** | F1 or Escape |
| **Fast Forward** | Space |
| **Save State** | F2 |
| **Load State** | F4 |
| **State Slot +** | F6 |
| **State Slot -** | F7 |
| **Screenshot** | F8 |
| **Reset** | H |
| **Fullscreen** | F |
| **Pause** | P |

### Default Controller Mapping

| RetroArch | Saturn | Keyboard |
|-----------|--------|----------|
| D-pad Up | Up | ↑ |
| D-pad Down | Down | ↓ |
| D-pad Left | Left | ← |
| D-pad Right | Right | → |
| B | A | Z |
| A | B | X |
| Y | C | A |
| X | X | S |
| L | L | Q |
| R | R | W |
| Start | Start | Enter |

## Expected Results

### Successful Test Run
```
[OK] Core loads in RetroArch
[OK] BIOS screen appears
[OK] Game loads and runs
[OK] Video displays correctly (320x224 or 640x448)
[OK] Audio plays without issues
[OK] Controller input works
[OK] Save states function
[OK] Performance is acceptable (near 60 FPS)
```

### First Boot Experience
1. **Load core** → "Sega - Saturn (Brimir)" appears
2. **Load game** → BIOS logo appears
3. **BIOS animation** → Saturn boot sequence
4. **Game loads** → Title screen or intro
5. **Gameplay** → Fully interactive!

## Common Test Games

### Recommended for Testing
1. **Homebrew games** - Free, legal, good for testing
2. **Simple 2D games** - Less demanding, good first test
3. **Your own dumps** - Games you own physically

### What to Test
- **Menu navigation** - D-pad and button response
- **Gameplay** - All controls functional
- **Audio** - Music and sound effects
- **Save states** - During gameplay
- **Performance** - Frame rate stability

## Development Testing Workflow

### Quick Test Cycle
```powershell
# 1. Rebuild core
cmake --build build --config Release

# 2. Copy to RetroArch
Copy-Item build\bin\Release\brimir_libretro.dll "$env:APPDATA\RetroArch\cores\" -Force

# 3. Launch RetroArch and test
# (RetroArch may need restart to reload core)
```

### Automated Build & Deploy Script
```powershell
# Create deploy-retroarch.ps1
$retroarchCores = "$env:APPDATA\RetroArch\cores"
cmake --build build --config Release
if ($LASTEXITCODE -eq 0) {
    Copy-Item build\bin\Release\brimir_libretro.dll $retroarchCores -Force
    Copy-Item build\bin\Release\fmt.dll $retroarchCores -Force
    Write-Host "[OK] Core deployed to RetroArch" -ForegroundColor Green
}
```

## Debugging Tips

### Enable Verbose Logging
1. Settings → Logging → Logging Verbosity → Debug
2. Check log: `%APPDATA%\RetroArch\retroarch.log`

### Test with Known Good Content
1. Try BIOS only (no game) - should boot to BIOS menu
2. Try different game formats
3. Compare with other Saturn cores (if available)

### Performance Profiling
1. Enable FPS counter
2. Monitor CPU usage in Task Manager
3. Check for frame drops
4. Test different video drivers

## Success Criteria

### Minimum Viable Test
- [OK] Core loads without crashing
- [OK] BIOS boots successfully
- [OK] Video output appears
- [OK] Audio is functional

### Full Feature Test
- [OK] Game loads and runs
- [OK] All controls responsive
- [OK] Save states work
- [OK] Performance acceptable
- [OK] No major bugs

### Production Ready
- [OK] Multiple games tested
- [OK] Extended play sessions
- [OK] No crashes or hangs
- [OK] Performance optimized
- [OK] User-friendly experience

## Next Steps After Testing

### If Everything Works [OK]
1. **Test more games** - Build compatibility list
2. **Performance tuning** - Optimize if needed
3. **Documentation** - User guide
4. **Release preparation** - Binary packaging
5. **Community feedback** - Share with users

### If Issues Found 
1. **Document issues** - Note specific problems
2. **Collect logs** - RetroArch and system logs
3. **Reproduce bugs** - Consistent test cases
4. **Fix issues** - Iterate on code
5. **Retest** - Verify fixes

## Additional Resources

### RetroArch Documentation
- **Website**: https://www.retroarch.com/
- **Docs**: https://docs.libretro.com/
- **Forums**: https://forums.libretro.com/

### Brimir Documentation
- [README.md](../../README.md) - Project overview
- [ARCHITECTURE.md](../development/ARCHITECTURE.md) - Technical details
- [TESTING_STRATEGY.md](../development/TESTING_STRATEGY.md) - Testing approach
- [ROADMAP.md](../ROADMAP.md) - Development roadmap

### Support
- Check `retroarch.log` for errors
- Review core implementation for issues
- Test with simpler games first
- Compare with reference cores

## Conclusion

You now have everything you need to test the Brimir core with RetroArch!

**Quick Start:**
1. Copy `brimir_libretro.dll` to RetroArch cores folder
2. Copy BIOS files to RetroArch system folder
3. Load core in RetroArch
4. Load a Saturn game
5. Play! 

---

**Good luck with your testing!** 

If you encounter any issues, refer to the troubleshooting section or check the log files for detailed error messages.



