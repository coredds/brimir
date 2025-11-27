# Test Fixtures Directory

This directory is for test assets that cannot be distributed with the repository due to copyright restrictions.

## Required Files for Full Testing

### Saturn BIOS ROMs (Required for integration tests)

Place Saturn BIOS files here for local testing. These files are **NOT included** in the repository and must be obtained legally.

#### US BIOS
- **Filename**: `sega_101.bin`
- **Size**: 524,288 bytes (512 KB)
- **MD5**: `f273555d7d91e8a5a6bfd9bcf066331c`
- **Region**: North America / US

#### European BIOS
- **Filename**: `sega_100.bin`
- **Size**: 524,288 bytes (512 KB)
- **MD5**: `2aba43c2f1526c5e898dfe1cafbfc53a`
- **Region**: Europe / PAL

#### Japanese BIOS (v1.01 - Recommended)
- **Filename**: `Sega Saturn BIOS v1.01 (JAP).bin`
- **Size**: 524,288 bytes (512 KB)
- **Region**: Japan
- **Note**: Preferred Japanese BIOS for best compatibility

#### Japanese BIOS (v1.00 - Alternative)
- **Filename**: `Sega Saturn BIOS v1.00 (JAP).bin`
- **Size**: 524,288 bytes (512 KB)
- **Region**: Japan

**⚠️ WARNING**: Do NOT use `sega1003.bin` (Japanese v1.003) - this version is not supported due to compatibility issues with the emulator core.

### Test Disc Images (Optional for game loading tests)

You can also place test disc images here:
- **Supported formats**: `.iso`, `.cue/.bin`, `.chd`, `.ccd`, `.mds`
- **Naming**: Use descriptive names (e.g., `test_game_us.cue`)

## Directory Structure

```
tests/fixtures/
├── README.md                              (this file)
├── sega_101.bin                           (US BIOS - provide yourself)
├── sega_100.bin                           (EU BIOS - provide yourself)
├── Sega Saturn BIOS v1.01 (JAP).bin      (JP BIOS - provide yourself)
├── Sega Saturn BIOS v1.00 (JAP).bin      (JP BIOS alt - provide yourself)
└── games/                                 (optional test games)
    ├── test_game.cue
    └── test_game.bin
```

## Legal Notice

**IMPORTANT**: 
- Saturn BIOS files are copyrighted by Sega Corporation
- You must own a Sega Saturn console to legally use BIOS files
- This project does NOT provide or distribute BIOS files
- You are responsible for obtaining files legally

## How to Obtain BIOS Files Legally

1. **Own a physical Sega Saturn console**
2. **Extract BIOS from your own console** using appropriate hardware/software
3. Place the BIOS file(s) in this directory

## Testing Behavior

### Without BIOS Files
- Unit tests will run normally
- Integration tests requiring BIOS will be **skipped** with a message
- All other tests will pass

### With BIOS Files
- All unit tests will run
- Integration tests will run with real BIOS
- Full end-to-end testing enabled

## Verifying Your BIOS Files

After placing BIOS files here, you can verify them with MD5 checksums:

**Windows (PowerShell)**:
```powershell
Get-FileHash -Algorithm MD5 sega_101.bin
```

**Linux/Mac**:
```bash
md5sum sega_101.bin
```

Compare the output with the MD5 hashes listed above.

## .gitignore

This directory is configured in `.gitignore` to prevent accidental commits of copyrighted material:
```
tests/fixtures/*.bin
tests/fixtures/*.iso
tests/fixtures/*.cue
tests/fixtures/*.chd
tests/fixtures/games/
```

Only this README file is tracked in git.






