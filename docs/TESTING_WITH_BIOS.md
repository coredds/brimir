# Testing with Real BIOS Files

This guide explains how to set up real Saturn BIOS files for comprehensive testing of the Brimir libretro core.

## Why Use Real BIOS Files for Testing?

While unit tests work with synthetic data, integration tests with real BIOS files allow you to:

- [OK] Verify actual boot sequences
- [OK] Test with real Saturn hardware behavior
- [OK] Validate region-specific functionality
- [OK] Measure realistic performance
- [OK] Test compatibility with actual games

## Legal Requirements

**IMPORTANT**: Saturn BIOS files are copyrighted by Sega Corporation.

To legally use BIOS files:
1. You must **own a physical Sega Saturn console**
2. You must **extract the BIOS from your own console**
3. You are responsible for legal compliance in your jurisdiction

**This project does NOT provide or distribute BIOS files.**

## Setting Up BIOS Files

### Step 1: Create the Fixtures Directory

The test fixtures directory should already exist at:
```
tests/fixtures/
```

If not, create it:
```bash
mkdir -p tests/fixtures
```

### Step 2: Obtain BIOS Files Legally

Extract BIOS from your own Saturn console using appropriate hardware/software.

### Step 3: Place BIOS Files

Copy your legally obtained BIOS files to `tests/fixtures/`:

```
tests/fixtures/
├── sega_101.bin                         (US BIOS - optional but recommended)
├── sega_100.bin                         (EU BIOS - optional)
├── Sega Saturn BIOS v1.01 (JAP).bin    (JP BIOS v1.01 - optional)
└── Sega Saturn BIOS v1.00 (JAP).bin    (JP BIOS v1.00 - optional)
```

**⚠️ IMPORTANT:** Do NOT use `sega1003.bin` (JP BIOS v1.003) - it is not supported by the core.

**At least ONE BIOS file is recommended** for integration testing.

### Step 4: Verify BIOS Files

#### Check File Sizes
All BIOS files should be exactly **524,288 bytes (512 KB)**:

**Windows (PowerShell)**:
```powershell
Get-Item tests/fixtures/*.bin | Select-Object Name, Length
```

**Linux/Mac**:
```bash
ls -lh tests/fixtures/*.bin
```

#### Verify Checksums (Optional but Recommended)

**US BIOS (sega_101.bin)**:
```bash
# Expected MD5: f273555d7d91e8a5a6bfd9bcf066331c
md5sum tests/fixtures/sega_101.bin
```

**EU BIOS (sega_100.bin)**:
```bash
# Expected MD5: 2aba43c2f1526c5e898dfe1cafbfc53a
md5sum tests/fixtures/sega_100.bin
```

**JP BIOS v1.01 (Sega Saturn BIOS v1.01 (JAP).bin)**:
```bash
# Expected MD5: (check with known good dump)
md5sum tests/fixtures/"Sega Saturn BIOS v1.01 (JAP).bin"
```

**Note:** JP BIOS v1.003 (`sega1003.bin`) is NOT supported and should not be used.

## Running Tests

### Without BIOS Files

All unit tests will run normally. Integration tests requiring BIOS will be **automatically skipped**:

```bash
./build/bin/Release/brimir_tests.exe
```

Example output:
```
test cases: 38 | 38 passed
all tests passed
```

### With BIOS Files

When BIOS files are present, integration tests will run automatically:

```bash
./build/bin/Release/brimir_tests.exe
```

Example output:
```
test cases: 42 | 42 passed (4 skipped)
all tests passed
```

### Running Only BIOS Integration Tests

```bash
./build/bin/Release/brimir_tests.exe "[bios][integration]"
```

### Running Only BIOS Unit Tests (no real BIOS needed)

```bash
./build/bin/Release/brimir_tests.exe "[bios][unit]"
```

## Test Categories

### Unit Tests (`[bios][unit]`)
- [OK] Run without real BIOS files
- [OK] Test BIOS loading logic
- [OK] Test validation and error handling
- [OK] Fast execution

### Integration Tests (`[bios][integration]`)
- [WARNING] Require real BIOS files
- [OK] Test actual boot sequences
- [OK] Test with real Saturn hardware behavior
- [WARNING] Automatically skipped if BIOS not available

### Benchmark Tests (`[benchmark]`)
- [WARNING] Require real BIOS files
- [OK] Measure performance baselines
- [OK] Frame execution timing

## Expected Results

### With US BIOS Only
```
[OK] BIOS integration - Real BIOS files
  [OK] Load US BIOS if available
  ⊘ Load EU BIOS if available (skipped)
  ⊘ Load JP BIOS v1.01/v1.00 if available (skipped)
[OK] BIOS boot sequence
  [OK] BIOS loads and emulator runs
  [OK] Multiple frames with BIOS
[OK] BIOS file validation
  [OK] Verify BIOS file sizes
  [OK] BIOS files are readable
```

### With All BIOS Files
```
[OK] All BIOS integration tests pass
[OK] All supported BIOS variants tested (US, EU, JP v1.01/v1.00)
[OK] Region-specific behavior validated
```

**Note:** JP v1.003 is not tested as it is not supported.

## Troubleshooting

### "SKIP: No BIOS files found"
**Cause**: No BIOS files in `tests/fixtures/`  
**Solution**: Place at least one BIOS file as described above

### "BIOS load failed"
**Possible causes**:
- File size is not exactly 512 KB
- File is corrupted
- Incorrect filename
- File permissions issue

**Solutions**:
- Verify file size: `ls -l tests/fixtures/sega_101.bin`
- Check MD5 hash against known values
- Ensure filename matches exactly (case-sensitive on Linux/Mac)
- Check file permissions: `chmod 644 tests/fixtures/*.bin`

### "Integration tests don't run"
**Cause**: Tests are tagged with `[!mayfail]` which may be filtered  
**Solution**: Run all tests or specifically request integration tests:
```bash
./build/bin/Release/brimir_tests.exe "[bios][integration]"
```

## Best Practices

### For Developers

1. **Keep BIOS files local**: Never commit BIOS files to git
2. **Test with US BIOS**: Most common region for testing
3. **Test all regions**: If possible, test with US, EU, and JP BIOS
4. **Regular validation**: Run integration tests before major changes

### For CI/CD

- Unit tests run without BIOS (always pass)
- Integration tests can use encrypted test assets (optional)
- Performance benchmarks require real BIOS

### For Contributors

- All pull requests must pass unit tests
- Integration test results are helpful but not required
- Document any region-specific behavior discovered

## Additional Test Assets

### Game Disc Images (Optional)

You can also place test disc images in `tests/fixtures/games/`:

```
tests/fixtures/games/
├── test_game.cue
└── test_game.bin
```

Supported formats: `.iso`, `.cue/.bin`, `.chd`, `.ccd`, `.mds`

**Note**: Same legal requirements apply - you must own the games.

## Security

### .gitignore Protection

The repository is configured to **never** commit BIOS or ROM files:

```gitignore
# BIOS and ROM files (never commit)
tests/fixtures/*.bin
tests/fixtures/*.iso
tests/fixtures/*.cue
tests/fixtures/*.chd
tests/fixtures/games/
```

### Verification

Before committing, always verify:
```bash
git status
```

BIOS files should show as "untracked" or not appear at all.

## Getting Help

If you encounter issues with BIOS testing:

1. Check this guide for common solutions
2. Verify BIOS file size and checksum
3. Run unit tests first to ensure basic functionality
4. Check issue tracker for known problems
5. Open a new issue with test output and system information

## Summary

- [OK] BIOS files enable comprehensive testing
- [OK] Tests automatically skip if BIOS not available
- [OK] Place BIOS in `tests/fixtures/`
- [OK] At least one BIOS file recommended
- [OK] Never commit BIOS files to repository
- [OK] You must own a Saturn console legally

---

**Quick Start**:
```bash
# 1. Get your BIOS legally
# 2. Place in tests/fixtures/
cp /path/to/your/sega_101.bin tests/fixtures/
# 3. Run tests
./build/bin/Release/brimir_tests.exe
```









