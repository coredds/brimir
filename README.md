# Brimir

<p align="center">
  <strong>Sega Saturn Emulation for libretro</strong><br>
  <em>A high-performance libretro core based on the Ymir emulator</em>
</p>

---

## Overview

**Brimir** is a libretro core wrapper for the [Ymir Sega Saturn emulator](https://github.com/StrikerX3/Ymir) by [StrikerX3](https://github.com/StrikerX3), bringing Ymir's accurate and feature-rich Sega Saturn emulation to RetroArch and other libretro frontends.

**All emulation work is done by Ymir.** Brimir provides the libretro API integration layer.

## Demo

[![Sega Rally on Brimir](https://img.youtube.com/vi/akkdVZk8GUY/0.jpg)](https://www.youtube.com/watch?v=akkdVZk8GUY)

Watch [Sega Rally Championship running on Brimir](https://www.youtube.com/watch?v=akkdVZk8GUY) - smooth 60 FPS gameplay.

## Features

### Core Emulation
- **Accurate hardware emulation** via Ymir's cycle-accurate core
- **Multiple disc formats:** CHD, BIN/CUE, ISO, CCD, MDS
- **7 BIOS versions supported:** US, EU, JP variants with auto-detection
- **Backup RAM (SRAM)** with persistent game saves
- **Real-Time Clock (RTC)** with system-wide persistence
- **Save state support** with complete serialization
- **Expansion cartridge support:**
  - 1MB/4MB/6MB DRAM cartridges with auto-detection
  - 24+ games with automatic cartridge support
  - Auto-insertion based on game database

### libretro Integration
- **Full libretro API compliance** (v1)
- **Controller support** for Player 1 & 2 (6-button digital pads)
- **RGB565 video output** with deinterlacing support
- **44.1 kHz stereo audio**
- **Core options** for video, audio, and system configuration

### Platform Support
- Windows 10+ (x86-64) ✅
- Linux (x86-64) ✅
- macOS - Not yet tested

## Building

### Prerequisites

**Windows:**
- CMake 3.20+
- Visual Studio Build Tools 2022
- Git with submodules

**Linux:**
- CMake 3.20+
- Clang 14+ or GCC 11+
- Git with submodules

### Quick Build

**Windows:**
```powershell
git clone --recursive https://github.com/coredds/brimir.git
cd Brimir
.\build.ps1
```

**Linux:**
```bash
git clone --recursive https://github.com/coredds/brimir.git
cd Brimir
chmod +x build.sh
./build.sh
```

**Build Outputs:**
- Windows: `build\bin\Release\brimir_libretro.dll`
- Linux: `build-linux/lib/brimir_libretro.so`

## Installation

### RetroArch (Windows)

1. Copy the core:
   ```
   C:\RetroArch-Win64\cores\brimir_libretro.dll
   ```

2. Copy core info:
   ```
   C:\RetroArch-Win64\info\brimir_libretro.info
   ```

3. Add BIOS files to `C:\RetroArch-Win64\system\`

### RetroArch (Linux)

```bash
# Copy core
cp build-linux/lib/brimir_libretro.so ~/.config/retroarch/cores/

# Copy core info
cp brimir_libretro.info ~/.config/retroarch/info/
```

Then add BIOS files to your RetroArch system directory.

### BIOS Requirements

Place Sega Saturn BIOS files in RetroArch's `system/` directory:

| File | Region | Size |
|------|--------|------|
| `sega_101.bin` | USA | 512 KB |
| `Sega Saturn BIOS v1.01 (JAP).bin` | Japan | 512 KB |
| `sega_100.bin` | Europe | 512 KB |

The core auto-detects available BIOS files. You must obtain BIOS files legally from your own Saturn console.

**⚠️ Note:** Japanese BIOS v1.003 (`sega1003.bin`) is NOT supported.

## Tested Games

| Game | Status | Notes |
|------|--------|-------|
| Sega Rally Championship | ✅ Working | 60 FPS, saves working |
| Panzer Dragoon Zwei | ✅ Working | Hi-res menus supported |
| Saturn Bomberman | ✅ Working | Full gameplay tested |
| King of Fighters '96 | ✅ Working | 1MB DRAM auto-detected |
| Street Fighter Zero 3 | ✅ Working | 4MB DRAM auto-detected |

## Known Limitations

- **Cartridge RAM persistence:** Not yet implemented - progress lost between sessions
- **ROM cartridges:** Not implemented (KOF'95, Ultraman not playable)
- **Controllers:** Digital pads only (no analog/mouse/light gun)
- **Some Capcom fighters:** May freeze (upstream Ymir issue)

## Contributing

Contributions are welcome! Please:
- Report bugs with detailed reproduction steps
- Test games and report compatibility
- Submit pull requests for improvements

See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

## License

GPL-3.0 - See [LICENSE](LICENSE) for details.

## Acknowledgments

- **[Ymir](https://github.com/StrikerX3/Ymir)** by [StrikerX3](https://github.com/StrikerX3) - All emulation credit goes to Ymir
- **[libretro](https://github.com/libretro)** - The platform making this integration possible
- **Sega Saturn community** - For preservation and documentation efforts

## Support

**Support the upstream emulator:**
- **Support StrikerX3:** [Patreon](https://www.patreon.com/StrikerX3)
- **Star Ymir:** [https://github.com/StrikerX3/Ymir](https://github.com/StrikerX3/Ymir)

## Links

- **Ymir Repository:** [https://github.com/StrikerX3/Ymir](https://github.com/StrikerX3/Ymir)
- **RetroArch:** [https://www.retroarch.com](https://www.retroarch.com)
- **libretro:** [https://github.com/libretro](https://github.com/libretro)

---

<p align="center">
  <strong>Powered by <a href="https://github.com/StrikerX3/Ymir">Ymir</a></strong><br>
  Made for the Sega Saturn community
</p>
