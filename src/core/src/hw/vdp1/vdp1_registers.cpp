/**
 * @file vdp1_registers.cpp
 * @brief VDP1 Register Access Implementation
 * 
 * Based on Mednafen's VDP1 implementation with adaptations for Brimir.
 * 
 * Copyright (C) 2015-2017 Mednafen Team (original implementation)
 * Copyright (C) 2025 Brimir Team (adaptations)
 * 
 * Licensed under GPL-2.0 (Mednafen) / GPL-3.0 (Brimir)
 */

#include <brimir/hw/vdp1/vdp1.hpp>
#include <brimir/util/dev_log.hpp>

namespace brimir::vdp1 {

// Dev log groups (shared with vdp1.cpp)
namespace grp {
    struct base {
        static constexpr bool enabled = true;
        static constexpr devlog::Level level = devlog::level::debug;
    };

    struct vdp1 : public base {
        static constexpr std::string_view name = "VDP1";
    };

    struct vdp1_cmd : public vdp1 {
        static constexpr std::string_view name = "VDP1-Cmd";
    };
}

// =============================================================================
// VDP1 Register Map
// =============================================================================
//
// Base address: 0x05D00000
// Relative address range: 0x000000 - 0x1FFFFF (2MB address space)
//
// Address ranges:
//   0x000000 - 0x07FFFF: VRAM (512KB)
//   0x080000 - 0x0FFFFF: Framebuffer (512KB, dual 256KB buffers)
//   0x100000 - 0x10017F: Registers
//
// Registers (word-aligned, 16-bit access):
//   0x100000 (0x00): TVMR - TV Mode Register (R/W)
//   0x100002 (0x01): FBCR - Framebuffer Change Register (R/W)
//   0x100004 (0x02): PTMR - Plot Trigger Register (R/W)
//   0x100006 (0x03): EWDR - Erase/Write Data Register (R/W)
//   0x100008 (0x04): EWLR - Erase/Write Upper Left Register (R/W)
//   0x10000A (0x05): EWRR - Erase/Write Lower Right Register (R/W)
//   0x10000C (0x06): ENDR - Drawing End Register (W)
//   0x100010 (0x08): EDSR - End Status Register (R)
//   0x100012 (0x09): LOPR - Last Operation Register (R)
//   0x100014 (0x0A): COPR - Current Operation Register (R)
//   0x100016 (0x0B): MODR - Mode Status Register (R)
//
// =============================================================================

void VDP1::WriteRegister(unsigned reg_index, uint16 value)
{
    // Update VDP1 before register write
    Update(m_scheduler.CurrentCount());
    
    switch(reg_index)
    {
        case 0x0:  // TVMR - TV Mode Register
            m_state.TVMR = value & 0xF;
            devlog::debug<grp::vdp1>("TVMR = 0x{:04X} (8BPP={}, ROTATE={}, HDTV={}, VBE={})",
                value,
                (m_state.TVMR & TVMR_8BPP) ? 1 : 0,
                (m_state.TVMR & TVMR_ROTATE) ? 1 : 0,
                (m_state.TVMR & TVMR_HDTV) ? 1 : 0,
                (m_state.TVMR & TVMR_VBE) ? 1 : 0);
            break;
            
        case 0x1:  // FBCR - Framebuffer Change Register
            m_state.FBCR = value & 0x1F;
            m_state.FBManualPending |= ((value & FBCR_FCM) != 0);
            devlog::debug<grp::vdp1>("FBCR = 0x{:04X} (FCT={}, FCM={}, DIL={}, DIE={}, EOS={})",
                value,
                (m_state.FBCR & FBCR_FCT) ? 1 : 0,
                (m_state.FBCR & FBCR_FCM) ? 1 : 0,
                (m_state.FBCR & FBCR_DIL) ? 1 : 0,
                (m_state.FBCR & FBCR_DIE) ? 1 : 0,
                (m_state.FBCR & FBCR_EOS) ? 1 : 0);
            break;
            
        case 0x2:  // PTMR - Plot Trigger Register
            m_state.PTMR = value & 0x3;
            if(value & 0x1)
            {
                devlog::info<grp::vdp1_cmd>("Plot trigger - starting drawing");
                StartDrawing();
                // TODO: Schedule next VDP1 event
            }
            break;
            
        case 0x3:  // EWDR - Erase/Write Data Register
            m_state.EWDR = value;
            devlog::debug<grp::vdp1>("EWDR = 0x{:04X}", value);
            break;
            
        case 0x4:  // EWLR - Erase/Write Upper Left Register
            m_state.EWLR = value & 0x7FFF;
            devlog::debug<grp::vdp1>("EWLR = 0x{:04X}", value);
            break;
            
        case 0x5:  // EWRR - Erase/Write Lower Right Register
            m_state.EWRR = value;
            devlog::debug<grp::vdp1>("EWRR = 0x{:04X}", value);
            break;
            
        case 0x6:  // ENDR - Drawing End Register (force stop)
            if(m_state.DrawingActive)
            {
                devlog::warn<grp::vdp1_cmd>("Forced termination of VDP1 drawing");
                m_state.DrawingActive = false;
                if(m_state.CycleCounter < 0)
                    m_state.CycleCounter = 0;
                // TODO: Schedule next VDP1 event (idle)
            }
            break;
            
        default:
            devlog::warn<grp::vdp1>("Unknown write of value 0x{:04X} to register 0x{:02X}",
                value, reg_index << 1);
            break;
    }
}

uint16 VDP1::ReadRegister(unsigned reg_index) const
{
    switch(reg_index)
    {
        case 0x8:  // EDSR - End Status Register
            return m_state.EDSR;
            
        case 0x9:  // LOPR - Last Operation Register
            return m_state.LOPR;
            
        case 0xA:  // COPR - Current Operation Register
            return m_state.CurCommandAddr >> 2;
            
        case 0xB:  // MODR - Mode Status Register
        {
            // Bit 12: Version (always 1)
            // Bit 8: PTM1 (from PTMR)
            // Bits 7-4: EOS, DIE, DIL, FCM (from FBCR)
            // Bits 3-0: VBE, HDTV, ROTATE, 8BPP (from TVMR)
            uint16 modr = 0;
            modr |= (0x1 << 12);                    // Version
            modr |= ((m_state.PTMR & 0x2) << 7);    // PTM1
            modr |= ((m_state.FBCR & 0x1E) << 3);   // FBCR bits
            modr |= (m_state.TVMR << 0);            // TVMR bits
            return modr;
        }
        
        default:
            devlog::warn<grp::vdp1>("Unknown read from register 0x{:02X}", reg_index << 1);
            return 0;
    }
}

// =============================================================================
// Bus Interface - Memory-Mapped Access
// =============================================================================

void VDP1::Write8(uint32 addr, uint8 value)
{
    addr &= 0x1FFFFF;  // Mask to 2MB address space
    
    // VRAM (0x000000 - 0x07FFFF)
    if(addr < 0x80000)
    {
        WriteVRAM8(addr, value);
        return;
    }
    
    // Framebuffer (0x080000 - 0x0FFFFF)
    if(addr < 0x100000)
    {
        uint32 fb_addr = addr - 0x80000;
        
        // Handle rotation mode addressing (8BPP + ROTATE)
        if((m_state.TVMR & (TVMR_8BPP | TVMR_ROTATE)) == (TVMR_8BPP | TVMR_ROTATE))
        {
            fb_addr = (fb_addr & 0x1FF) | ((fb_addr << 1) & 0x3FC00) | ((fb_addr >> 8) & 0x200);
        }
        
        WriteFB8(m_state.FBDrawWhich, fb_addr, value);
        return;
    }
    
    // Registers (0x100000+) - convert byte write to 16-bit
    // Saturn uses 16-bit data bus, byte writes write to both bytes
    uint16 value16 = value | (value << 8);
    WriteRegister((addr - 0x100000) >> 1, value16);
}

void VDP1::Write16(uint32 addr, uint16 value)
{
    addr &= 0x1FFFFE;  // Mask to 2MB, word-aligned
    
    // VRAM (0x000000 - 0x07FFFF)
    if(addr < 0x80000)
    {
        WriteVRAM16(addr, value);
        return;
    }
    
    // Framebuffer (0x080000 - 0x0FFFFF)
    if(addr < 0x100000)
    {
        uint32 fb_addr = addr - 0x80000;
        
        // Handle rotation mode addressing (8BPP + ROTATE)
        if((m_state.TVMR & (TVMR_8BPP | TVMR_ROTATE)) == (TVMR_8BPP | TVMR_ROTATE))
        {
            fb_addr = (fb_addr & 0x1FF) | ((fb_addr << 1) & 0x3FC00) | ((fb_addr >> 8) & 0x200);
        }
        
        WriteFB16(m_state.FBDrawWhich, fb_addr, value);
        return;
    }
    
    // Registers (0x100000+)
    WriteRegister((addr - 0x100000) >> 1, value);
}

uint16 VDP1::Read16(uint32 addr)
{
    addr &= 0x1FFFFE;  // Mask to 2MB, word-aligned
    
    // VRAM (0x000000 - 0x07FFFF)
    if(addr < 0x80000)
    {
        return ReadVRAM16(addr);
    }
    
    // Framebuffer (0x080000 - 0x0FFFFF)
    if(addr < 0x100000)
    {
        uint32 fb_addr = addr - 0x80000;
        
        // Handle rotation mode addressing (8BPP + ROTATE)
        if((m_state.TVMR & (TVMR_8BPP | TVMR_ROTATE)) == (TVMR_8BPP | TVMR_ROTATE))
        {
            fb_addr = (fb_addr & 0x1FF) | ((fb_addr << 1) & 0x3FC00) | ((fb_addr >> 8) & 0x200);
        }
        
        return ReadFB16(m_state.FBDrawWhich, fb_addr);
    }
    
    // Registers (0x100000+)
    return ReadRegister((addr - 0x100000) >> 1);
}

} // namespace brimir::vdp1

