# Brimir

<p align="center">
  <strong>Sega Saturn Emulation for libretro</strong><br>
  <em>A libretro core based on the Ymir emulator</em>
</p>

---

## Overview

**Brimir** is a libretro core adaptation of the [Ymir Sega Saturn emulator](https://github.com/StrikerX3/Ymir), bringing accurate and feature-rich Sega Saturn emulation to RetroArch and other libretro frontends.

Named after the primordial giant in Norse mythology (complementing "Ymir"), Brimir aims to make Ymir's modern C++20 emulation core accessible to the broader libretro ecosystem while maintaining upstream compatibility and feature parity.

## Status

🚧 **Project Status:** Planning Phase  
📅 **Target Release:** TBD  
📄 **PRD Version:** 1.0

This project is currently in the planning and requirements gathering phase. See [PRD.md](PRD.md) for the complete Product Requirements Document.

## Features (Planned)

### Core Emulation
- ✅ Accurate Sega Saturn hardware emulation
- ✅ Multiple disc format support (CHD, BIN/CUE, ISO, CCD, MDS)
- ✅ Automatic BIOS detection and region switching
- ✅ High and low-level CD block emulation

### libretro Integration
- ✅ Full libretro API compliance
- ✅ Save state support with forward compatibility
- ✅ Rewind functionality
- ✅ Multiple controller types (pad, analog, mouse, wheel, etc.)
- ✅ Backup RAM and cartridge support
- ✅ Achievement support (planned)

### Enhancements
- ✅ Graphics enhancements (deinterlacing, progressive rendering)
- ✅ Internal resolution scaling
- ✅ Fast-forward and frame stepping
- ✅ Comprehensive core options

### Platform Support
- Windows 10+ (x86-64, ARM64)
- Linux (modern distributions)
- macOS 13+ (Intel, Apple Silicon)
- FreeBSD

## Building

> **Note:** Build instructions will be added once the project enters active development.

The project will use CMake as the primary build system with Makefile compatibility for libretro build bots.

### Prerequisites
- CMake 3.20+
- C++20-capable compiler (GCC 11+, Clang 14+, MSVC 2022+)
- vcpkg (for dependency management)
- Git (for submodules)

## Usage

Once released, Brimir will be available through:
- RetroArch's built-in core downloader
- libretro buildbot
- Manual installation from releases

### BIOS Requirements

Brimir requires Sega Saturn IPL/BIOS ROMs to function. Place them in RetroArch's `system/` directory:

```
system/
├── saturn_bios.bin    (any supported region)
├── mpr-17933.bin      (Japan)
├── mpr-18811.bin      (USA)
├── mpr-17951.bin      (Europe)
└── ...
```

## Roadmap

See the [PRD.md](PRD.md) for the detailed roadmap. High-level phases:

1. **Phase 1 (Weeks 1-4):** Foundation - Basic playable core
2. **Phase 2 (Weeks 5-8):** Core Features - Complete emulation features  
3. **Phase 3 (Weeks 9-12):** Polish - Production-ready release
4. **Phase 4 (Weeks 13-16):** Advanced Features - Full parity with Ymir

## Documentation

- [PRD.md](PRD.md) - Complete Product Requirements Document
- COMPILING.md - Build instructions (coming soon)
- CONTRIBUTING.md - Contribution guidelines (coming soon)

## Contributing

We welcome contributions! More details will be provided as the project progresses.

In the meantime, you can:
- ⭐ Star this repository to show interest
- 💬 Join discussions in the Issues section
- 📖 Review the PRD and provide feedback
- 🧪 Help identify test cases and games

## License

Brimir is licensed under the **GPL-3.0** license, inherited from the Ymir project.

See [LICENSE](LICENSE) for details.

## Acknowledgments

- **[Ymir](https://github.com/StrikerX3/Ymir)** by [@StrikerX3](https://github.com/StrikerX3) - The foundation of this project
- **[libretro](https://github.com/libretro)** - The platform that makes this possible
- **Sega Saturn emulation community** - For preservation efforts and technical documentation

## Support

If you find this project useful and want to support its development, consider:

- Supporting the upstream [Ymir project](https://github.com/StrikerX3/Ymir) via [Patreon](https://www.patreon.com/StrikerX3)
- Contributing code, testing, or documentation
- Sharing the project with others

## Links

- **Upstream Project:** [Ymir Sega Saturn Emulator](https://github.com/StrikerX3/Ymir)
- **libretro:** [https://github.com/libretro](https://github.com/libretro)
- **RetroArch:** [https://www.retroarch.com](https://www.retroarch.com)
- **Libretro Docs:** [https://docs.libretro.com](https://docs.libretro.com)

## Contact

- **Issues:** Use GitHub Issues for bug reports and feature requests
- **Discussions:** Use GitHub Discussions for general questions
- **Discord:** Join the Ymir Discord community (link TBD)

---

<p align="center">
  Made with ❤️ for the emulation community
</p>

