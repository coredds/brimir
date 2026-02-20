/**
 * @file vdp2_registers.cpp
 * @brief VDP2 Register Handling
 * 
 * Based on Mednafen's VDP2 implementation with adaptations for Brimir.
 * 
 * Copyright (C) 2015-2019 Mednafen Team (original implementation)
 * Copyright (C) 2025 Brimir Team (adaptations)
 * 
 * Licensed under GPL-2.0 (Mednafen) / GPL-3.0 (Brimir)
 */

#include <brimir/hw/vdp2/vdp2.hpp>
#include <brimir/util/dev_log.hpp>

namespace brimir::vdp2 {

namespace grp {
    struct vdp2_regs {
        static constexpr bool enabled = true;
        static constexpr devlog::Level level = devlog::level::debug;
        static constexpr std::string_view name = "VDP2-Regs";
    };
}

// =============================================================================
// Register Addresses
// =============================================================================

// Register address offsets (from 0x25F80000)
enum RegAddr : uint32 {
    // Screen Mode
    TVMD    = 0x000,    // TV Screen Mode
    EXTEN   = 0x002,    // External Signal Enable
    TVSTAT  = 0x004,    // Screen Status (read-only)
    VRSIZE  = 0x006,    // VRAM Size
    HCNT    = 0x008,    // H Counter (read-only)
    VCNT    = 0x00A,    // V Counter (read-only)
    
    // RAM Control
    RAMCTL  = 0x00E,    // RAM Control
    
    // VRAM Cycle Patterns
    CYCA0L  = 0x010,
    CYCA0U  = 0x012,
    CYCA1L  = 0x014,
    CYCA1U  = 0x016,
    CYCB0L  = 0x018,
    CYCB0U  = 0x01A,
    CYCB1L  = 0x01C,
    CYCB1U  = 0x01E,
    
    // Background Control
    BGON    = 0x020,    // BG Control
    MZCTL   = 0x022,    // Mosaic Control
    SFSEL   = 0x024,    // Special Function Code Select
    SFCODE  = 0x026,    // Special Function Code
    CHCTLA  = 0x028,    // Character Control A (NBG0/1)
    CHCTLB  = 0x02A,    // Character Control B (NBG2/3/RBG0)
    
    // Bitmap Palette
    BMPNA   = 0x02C,    // Bitmap Palette Number A
    BMPNB   = 0x02E,    // Bitmap Palette Number B
    
    // Pattern Name Control
    PNCN0   = 0x030,    // NBG0
    PNCN1   = 0x032,    // NBG1
    PNCN2   = 0x034,    // NBG2
    PNCN3   = 0x036,    // NBG3
    PNCR    = 0x038,    // RBG0
    
    // Plane Size
    PLSZ    = 0x03A,
    
    // Map Offset
    MPOFN   = 0x03C,    // NBG
    MPOFR   = 0x03E,    // RBG
    
    // Map Registers (NBG0-3)
    MPABN0  = 0x040,
    MPCDN0  = 0x042,
    MPABN1  = 0x044,
    MPCDN1  = 0x046,
    MPABN2  = 0x048,
    MPCDN2  = 0x04A,
    MPABN3  = 0x04C,
    MPCDN3  = 0x04E,
    
    // Map Registers (RBG0)
    MPABRA  = 0x050,
    MPCDRA  = 0x052,
    MPEFRA  = 0x054,
    MPGHRA  = 0x056,
    MPIJRA  = 0x058,
    MPKLRA  = 0x05A,
    MPMNRA  = 0x05C,
    MPOPRA  = 0x05E,
    
    // Map Registers (RBG1)
    MPABRB  = 0x060,
    MPCDRB  = 0x062,
    MPEFRB  = 0x064,
    MPGHRB  = 0x066,
    MPIJRB  = 0x068,
    MPKLRB  = 0x06A,
    MPMNRB  = 0x06C,
    MPOPRB  = 0x06E,
    
    // Scroll (NBG0)
    SCXIN0  = 0x070,
    SCXDN0  = 0x072,
    SCYIN0  = 0x074,
    SCYDN0  = 0x076,
    
    // Zoom (NBG0)
    ZMXIN0  = 0x078,
    ZMXDN0  = 0x07A,
    ZMYIN0  = 0x07C,
    ZMYDN0  = 0x07E,
    
    // Scroll (NBG1)
    SCXIN1  = 0x080,
    SCXDN1  = 0x082,
    SCYIN1  = 0x084,
    SCYDN1  = 0x086,
    
    // Zoom (NBG1)
    ZMXIN1  = 0x088,
    ZMXDN1  = 0x08A,
    ZMYIN1  = 0x08C,
    ZMYDN1  = 0x08E,
    
    // Scroll (NBG2/3)
    SCXN2   = 0x090,
    SCYN2   = 0x092,
    SCXN3   = 0x094,
    SCYN3   = 0x096,
    
    // Zoom/Scroll Control
    ZMCTL   = 0x098,
    SCRCTL  = 0x09A,
    
    // Vertical Cell Scroll
    VCSTA   = 0x09C,
    
    // Line Scroll Table
    LSTA0U  = 0x0A0,
    LSTA0L  = 0x0A2,
    LSTA1U  = 0x0A4,
    LSTA1L  = 0x0A6,
    
    // Line Color Screen
    LCTA    = 0x0A8,
    
    // Back Screen
    BKTA    = 0x0AC,
    
    // Rotation Parameters
    RPMD    = 0x0B0,
    RPRCTL  = 0x0B2,
    KTCTL   = 0x0B4,
    KTAOF   = 0x0B6,
    OVPNRA  = 0x0B8,
    OVPNRB  = 0x0BA,
    RPTA    = 0x0BC,
    
    // Window Control
    WPSX0   = 0x0C0,
    WPSY0   = 0x0C2,
    WPEX0   = 0x0C4,
    WPEY0   = 0x0C6,
    WPSX1   = 0x0C8,
    WPSY1   = 0x0CA,
    WPEX1   = 0x0CC,
    WPEY1   = 0x0CE,
    WCTLA   = 0x0D0,
    WCTLB   = 0x0D2,
    WCTLC   = 0x0D4,
    WCTLD   = 0x0D6,
    LWTA0U  = 0x0D8,
    LWTA0L  = 0x0DA,
    LWTA1U  = 0x0DC,
    LWTA1L  = 0x0DE,
    
    // Sprite Control
    SPCTL   = 0x0E0,
    SDCTL   = 0x0E2,
    CRAOFA  = 0x0E4,
    CRAOFB  = 0x0E6,
    LNCLEN  = 0x0E8,
    SFPRMD  = 0x0EA,
    CCCTL   = 0x0EC,
    SFCCMD  = 0x0EE,
    
    // Priority
    PRISA   = 0x0F0,
    PRISB   = 0x0F2,
    PRISC   = 0x0F4,
    PRISD   = 0x0F6,
    PRINA   = 0x0F8,
    PRINB   = 0x0FA,
    PRIR    = 0x0FC,
    
    // Color Calculation Ratio
    CCRSA   = 0x100,
    CCRSB   = 0x102,
    CCRSC   = 0x104,
    CCRSD   = 0x106,
    CCRNA   = 0x108,
    CCRNB   = 0x10A,
    CCRR    = 0x10C,
    CCRLB   = 0x10E,
    
    // Color Offset
    CLOFEN  = 0x110,
    CLOFSL  = 0x112,
    COAR    = 0x114,
    COAG    = 0x116,
    COAB    = 0x118,
    COBR    = 0x11A,
    COBG    = 0x11C,
    COBB    = 0x11E,
};

// =============================================================================
// Register Read
// =============================================================================

uint16 ReadReg(uint32 address) {
    const uint32 offset = address & 0x1FE;
    
    switch (offset) {
        case TVSTAT:
            // Return status: VBlank, HBlank, etc.
            return (VBlankOut ? 0x0008 : 0) |
                   (HBlankOut ? 0x0004 : 0) |
                   (PAL ? 0x0001 : 0);
            
        case HCNT:
            return static_cast<uint16>(HCounter & 0x3FF);
            
        case VCNT:
            return static_cast<uint16>(VCounter & 0x3FF);
            
        default:
            devlog::debug<grp::vdp2_regs>("Read reg 0x{:03X}", offset);
            return 0;
    }
}

// =============================================================================
// Register Write
// =============================================================================

void WriteReg(uint32 address, uint16 value) {
    const uint32 offset = address & 0x1FE;
    
    switch (offset) {
        case TVMD:
            Regs.displayOn = (value & 0x8000) != 0;
            Regs.borderMode = (value & 0x0100) != 0;
            Regs.interlace = static_cast<InterlaceMode>((value >> 6) & 0x3);
            Regs.vRes = static_cast<VResMode>((value >> 4) & 0x3);
            Regs.hRes = static_cast<HResMode>(value & 0x7);
            // Update global HRes/VRes used by rendering
            // Horizontal: 320, 352, 640, 704 (bits 0-2, bit 2 is hi-res enable)
            // Vertical: 224, 240, 256 (bits 4-5), doubled if interlaced (bits 6-7)
            {
                static constexpr uint32 hResTable[] = {320, 352, 640, 704, 320, 352, 640, 704};
                static constexpr uint32 vResBaseTable[] = {224, 240, 256, 256};
                HRes = hResTable[static_cast<uint8>(Regs.hRes) & 0x7];
                VResBase = vResBaseTable[static_cast<uint8>(Regs.vRes) & 0x3];
                // InterlaceMode: 0=non-interlace, 1=single-density, 2=double-density, 3=reserved
                InterlaceDouble = (static_cast<uint8>(Regs.interlace) == 2);
                VRes = InterlaceDouble ? (VResBase * 2) : VResBase;
            }
            devlog::debug<grp::vdp2_regs>("TVMD: disp={} border={} interlace={} vres={} hres={} ({}x{})",
                Regs.displayOn, Regs.borderMode, 
                static_cast<int>(Regs.interlace),
                static_cast<int>(Regs.vRes),
                static_cast<int>(Regs.hRes),
                HRes, VRes);
            break;
            
        case EXTEN:
            Regs.exLatchEnable = (value & 0x0200) != 0;
            Regs.exSyncEnable = (value & 0x0100) != 0;
            Regs.exBGEnable = (value & 0x0002) != 0;
            Regs.dispAreaSelect = (value & 0x0001) != 0;
            break;
            
        case RAMCTL:
            Regs.cramMode = static_cast<CRAMMode>((value >> 12) & 0x3);
            Regs.vramMode = (value >> 8) & 0x3;
            Regs.coeffTableEnable = (value & 0x8000) != 0;
            break;
            
        case BGON:
            Regs.bgon = value;
            devlog::debug<grp::vdp2_regs>("BGON: 0x{:04X}", value);
            break;
            
        case CHCTLA:
            Regs.chctla = value;
            break;
            
        case CHCTLB:
            Regs.chctlb = value;
            break;
            
        case BMPNA:
            Regs.bmpna = value;
            break;
            
        case PNCN0:
        case PNCN1:
        case PNCN2:
        case PNCN3:
            Regs.pncn[(offset - PNCN0) / 2] = value;
            break;
            
        case PNCR:
            Regs.pncr = value;
            break;
            
        case PLSZ:
            Regs.plsz = value;
            break;
            
        case MPOFN:
            Regs.mpofn = value;
            break;
            
        case MPOFR:
            Regs.mpofr = value;
            break;
            
        // Scroll registers (NBG0)
        case SCXIN0:
            Regs.scrollX[0] = static_cast<sint16>(value & 0x7FF);
            break;
        case SCYIN0:
            Regs.scrollY[0] = static_cast<sint16>(value & 0x7FF);
            break;
            
        // Scroll registers (NBG1)
        case SCXIN1:
            Regs.scrollX[1] = static_cast<sint16>(value & 0x7FF);
            break;
        case SCYIN1:
            Regs.scrollY[1] = static_cast<sint16>(value & 0x7FF);
            break;
            
        // Scroll registers (NBG2)
        case SCXN2:
            Regs.scrollX[2] = static_cast<sint16>(value & 0x7FF);
            break;
        case SCYN2:
            Regs.scrollY[2] = static_cast<sint16>(value & 0x7FF);
            break;
            
        // Scroll registers (NBG3)
        case SCXN3:
            Regs.scrollX[3] = static_cast<sint16>(value & 0x7FF);
            break;
        case SCYN3:
            Regs.scrollY[3] = static_cast<sint16>(value & 0x7FF);
            break;
            
        // Priority
        case PRINA:
            Regs.nbgPrio[0] = value & 0x7;
            Regs.nbgPrio[1] = (value >> 8) & 0x7;
            break;
        case PRINB:
            Regs.nbgPrio[2] = value & 0x7;
            Regs.nbgPrio[3] = (value >> 8) & 0x7;
            break;
        case PRIR:
            Regs.rbgPrio = value & 0x7;
            break;
            
        // Color calculation
        case CCCTL:
            Regs.ccctl = value;
            break;
            
        default:
            devlog::debug<grp::vdp2_regs>("Write reg 0x{:03X} = 0x{:04X}", offset, value);
            break;
    }
}

// =============================================================================
// VRAM Access
// =============================================================================

uint8 ReadVRAM8(uint32 address) {
    const uint32 offset = address & (VRAM_SIZE - 1);
    const uint16 word = VRAM[offset >> 1];
    return (offset & 1) ? (word & 0xFF) : (word >> 8);
}

void WriteVRAM8(uint32 address, uint8 value) {
    const uint32 offset = address & (VRAM_SIZE - 1);
    uint16& word = VRAM[offset >> 1];
    if (offset & 1) {
        word = (word & 0xFF00) | value;
    } else {
        word = (word & 0x00FF) | (value << 8);
    }
}

uint16 ReadVRAM16(uint32 address) {
    const uint32 offset = (address & (VRAM_SIZE - 1)) >> 1;
    return VRAM[offset];
}

void WriteVRAM16(uint32 address, uint16 value) {
    const uint32 offset = (address & (VRAM_SIZE - 1)) >> 1;
    VRAM[offset] = value;
}

// =============================================================================
// CRAM Access
// =============================================================================

uint16 ReadCRAM16(uint32 address) {
    const uint32 offset = (address & (CRAM_SIZE - 1)) >> 1;
    return CRAM[offset];
}

void WriteCRAM16(uint32 address, uint16 value) {
    const uint32 offset = (address & (CRAM_SIZE - 1)) >> 1;
    CRAM[offset] = value;
}

} // namespace brimir::vdp2


