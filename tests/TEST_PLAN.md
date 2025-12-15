# Brimir Test Plan - Documentation-Based Testing

## Overview
This test plan is based on official Sega Saturn hardware documentation to ensure accuracy and prevent regressions during optimization.

## Test Categories

### 1. Hardware Initialization Tests
Based on: `Saturn_Boot_ROM_v0.8_-_Floppy_Disk_Information.pdf`, `ST-079B-R3-011895.pdf`

#### SH-2 CPU Initialization
- **Cache Configuration**
  - Master SH-2: 4KB 4-way write-through cache (default)
  - Slave SH-2: 2KB 2-way cache + 2KB RAM (default)
  - Cache purge operations (full and line-specific)
  - Cache-through addressing (base + 0x20000000)
  - Cache invalidation (base + 0x40000000, write 0)

- **Memory Regions**
  - Work RAM Low: 0x00200000 (1MB)
  - Work RAM High: 0x06000000 (1.5MB, accessible via SCU at 0x05900000)
  - First 64KB vector table initialization
  - Stack pointer setup

#### VDP1/VDP2 Initialization
Based on: `ST-058-R2-060194.pdf`, `ST-013-SP1-052794.pdf`

- **VDP2 Registers** (Base: 0x25E00000)
  - VRAM: 0x25E00000 - 0x25E7FFFF
  - Color RAM: 0x25F00000 - 0x25F00FFF
  - Registers: 0x25F80000 - 0x25F8011F
  - Reset behavior: Most registers cleared to 0
  - Access: Word/Long word only (no byte access)

- **VDP1 Registers** (Base: 0x25C00000)
  - VRAM: 0x25C00000 - 0x25C7FFFF
  - Framebuffer: 0x25C80000
  - Mode register version check (bits 15-12)

#### BIOS/IPL Requirements
- Size: 512KB (0x80000 bytes)
- Location: ROM area
- Boot sequence validation
- Memory manager initialization

### 2. Memory System Tests

#### Address Space Validation
- Work RAM addressing (low/high)
- SCU address translation (0x05900000 vs 0x06000000)
- Cache-through addressing (+0x20000000)
- Cache invalidation addressing (+0x40000000)
- VDP1/VDP2 VRAM boundaries

#### Cache Coherency Tests
Based on: `ST-097-R5-072694.pdf`, `ST-202-R1-120994.pdf`

- Cache hit behavior with DMA writes
- Cache-through reads for inter-CPU communication
- Cache line invalidation (16-byte granularity)
- Master/Slave CPU data sharing

### 3. Video System Tests

#### Resolution and Timing
Based on official VDP documentation

- **Standard Resolutions**
  - NTSC: 320x224, 320x240, 352x224, 352x240
  - Hi-res: 640x448, 640x480, 704x448, 704x480
  - Interlaced modes
  - Field selection (odd/even)

#### Pixel Format
- RGB565 format validation
- Pitch calculation (width * 2)
- Framebuffer alignment

#### Register Access
- VDP2 register buffer behavior
- V-BLANK synchronization for register writes
- Read-only vs Write-only register enforcement

### 4. CPU Instruction Tests

#### SH-2 Instruction Accuracy
- Instruction timing (cycles)
- Cache hit/miss timing impact
- Delay slot behavior
- Exception handling

#### Dual CPU Synchronization
- FRT (Free Running Timer) communication
- Interrupt priority handling
- Bus arbitration

### 5. SCU Tests

#### DMA Operations
Based on: `ST-097-R5-072694.pdf`

- Direct memory access timing
- Work RAM to VDP1 transfers
- Cache coherency during DMA
- A-bus setup for CD/SCSI

#### Interrupt Control
- VBI (V-Blank In) interrupt
- VBO (V-Blank Out) interrupt
- End code fetch interrupt
- A-BUS interrupt (CD/SCSI)

### 6. Regression Prevention Tests

#### Known Issue Tests
- Bitmap rendering (interlaced field selection)
- VDP2 layer priorities
- Color calculation modes
- Texture mapping accuracy

#### Performance Baseline Tests
- Frame timing consistency
- Audio sample generation rate
- Memory access patterns
- Cache hit rates

## Test Implementation Strategy

### Phase 1: Core Hardware Tests (Current)
- [x] Basic initialization
- [x] BIOS loading
- [x] Video output basics
- [ ] Memory addressing validation
- [ ] Cache behavior tests

### Phase 2: Hardware Accuracy Tests
- [ ] SH-2 cache operations
- [ ] VDP register reset values
- [ ] SCU DMA timing
- [ ] Interrupt handling

### Phase 3: Integration Tests
- [ ] BIOS boot sequence
- [ ] Game loading with proper initialization
- [ ] Save state accuracy
- [ ] Multi-frame consistency

### Phase 4: Regression Tests
- [ ] Automated visual regression tests
- [ ] Performance benchmarks
- [ ] Known game compatibility tests
- [ ] Edge case handling

## Test Data Requirements

### Required BIOS Files
- sega_101.bin (512KB) - Japanese v1.01
- sega_100.bin (512KB) - Japanese v1.00
- mpr-17933.bin (512KB) - US BIOS
- European BIOS variants

### Test ROMs
- Homebrew test ROMs
- Known-good game dumps (for integration testing)
- Synthetic test programs

## Documentation References

### Primary Sources
1. `ST-121-041594` - Boot ROM Hardware Initialization
2. `ST-058-R2-060194.pdf` - VDP2 User's Manual
3. `ST-013-SP1-052794.pdf` - VDP1 User's Manual
4. `ST-097-R5-072694.pdf` - SCU User's Manual
5. `ST-202-R1-120994.pdf` - CPU Communication Manual
6. `ST-TECH.pdf` - Technical Bulletin

### Key Findings
- Cache must be purged before use (write 0x10 to 0xFFFFFE92)
- VDP2 registers reset to 0 on power-on
- Work RAM first 64KB reserved for vectors
- Cache line size is 16 bytes
- No bus snoop - manual cache invalidation required

## Success Criteria

### Test Coverage Goals
- 90%+ code coverage for core emulation
- 100% coverage of documented hardware behavior
- All known regressions have test cases
- Performance tests establish baselines

### Quality Gates
- All tests pass before optimization changes
- No performance regression > 5%
- Visual regression tests match reference frames
- Known games boot and run correctly

## Next Steps

1. Implement hardware-accurate cache tests
2. Add VDP register initialization validation
3. Create memory addressing test suite
4. Build regression test framework
5. Document test coverage gaps

