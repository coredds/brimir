# Brimir BIOS Support

## Supported BIOS Files

Brimir supports multiple Sega Saturn BIOS versions. You can place any of these files in RetroArch's `system` directory:

### US (North America) BIOS
- **sega_101.bin** - US BIOS v1.01 *(most common, recommended)*
- **mpr-17933.bin** - US BIOS v1.00

### EU (Europe/PAL) BIOS
- **sega_100.bin** - EU BIOS v1.00 *(most common)*
- **Sega Saturn BIOS (EUR).bin** - EU BIOS (alternate naming)

### JP (Japan) BIOS
- **Sega Saturn BIOS v1.01 (JAP).bin** - JP BIOS v1.01 *(recommended)*
- **Sega Saturn BIOS v1.00 (JAP).bin** - JP BIOS v1.00

**⚠️ IMPORTANT:** Japanese BIOS v1.003 (`sega1003.bin`) is **NOT supported** due to compatibility issues. Use v1.01 or v1.00 instead.

## BIOS Selection in RetroArch

### Quick Menu → Core Options → System Settings → BIOS Selection

You can choose:

- **Auto-detect** *(default)* - Tries all BIOS files in priority order (US → EU → JP)
- **US v1.01** - Uses `sega_101.bin` only
- **US v1.00** - Uses `mpr-17933.bin` only
- **EU v1.00** - Uses `sega_100.bin` only
- **EU** - Uses `Sega Saturn BIOS (EUR).bin` only
- **JP v1.01** - Uses `Sega Saturn BIOS v1.01 (JAP).bin` only
- **JP v1.00** - Uses `Sega Saturn BIOS v1.00 (JAP).bin` only

**Note:** JP BIOS v1.003 is not available as it is not supported by the core.

## Auto-Detection Priority

When set to **Auto-detect**, Brimir will search in this order:

1. US v1.01 (sega_101.bin)
2. US v1.00 (mpr-17933.bin)
3. EU v1.00 (sega_100.bin)
4. EU (Sega Saturn BIOS (EUR).bin)
5. JP v1.01 (Sega Saturn BIOS v1.01 (JAP).bin)
6. JP v1.00 (Sega Saturn BIOS v1.00 (JAP).bin)

The first BIOS found will be loaded.

**Note:** JP v1.003 (sega1003.bin) is intentionally excluded as it is not compatible with the core.

## Troubleshooting

### "Failed to load BIOS" Error

If you see this error:
1. Check that at least one BIOS file is in RetroArch's `system` directory
2. Verify the filename matches exactly (case-sensitive on Linux/macOS)
3. Ensure the file is not corrupted (should be exactly 524,288 bytes / 512 KB)

### Clock Keeps Resetting

- The internal backup RAM stores clock settings
- Ensure the game is saving properly (check for `.srm` file in RetroArch's `saves` directory)
- Try a different BIOS version if the issue persists

### Region Mismatch

- Japanese games work best with JP BIOS
- US/EU games work with any BIOS when "Autodetect Region" is enabled
- You can manually select region in **Core Options → System Settings → Console Region**

## Notes

- **BIOS files are copyrighted** and must be obtained legally from your own Sega Saturn console
- All supported BIOS versions are functionally equivalent for most games
- Some games may require specific regional BIOS versions
- Changing BIOS requires reloading the game to take effect

