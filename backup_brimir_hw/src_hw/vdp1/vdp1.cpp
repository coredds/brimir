/**
 * @file vdp1.cpp
 * @brief VDP1 Main Implementation
 * 
 * Based on Mednafen's VDP1 implementation with adaptations for Brimir.
 * Architecture inspired by beetle-saturn-libretro/mednafen/ss/vdp1.cpp
 * 
 * Copyright (C) 2015-2017 Mednafen Team (original implementation)
 * Copyright (C) 2025 Brimir Team (adaptations)
 * 
 * Licensed under GPL-2.0 (Mednafen) / GPL-3.0 (Brimir)
 */

#include <brimir/hw/vdp1/vdp1.hpp>
#include <brimir/hw/vdp1/vdp1_helpers.hpp>
#include <brimir/util/dev_log.hpp>
#include <brimir/util/bit_ops.hpp>

#include <algorithm>
#include <cstring>

namespace brimir::vdp1 {

// =============================================================================
// Dev Log Groups
// =============================================================================

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

    struct vdp1_render : public vdp1 {
        static constexpr std::string_view name = "VDP1-Render";
    };
} // namespace grp

// ============================================================================
// Constructor / Destructor
// ============================================================================

VDP1::VDP1(core::Scheduler& scheduler, sys::SH2Bus& bus)
    : m_scheduler(scheduler)
    , m_bus(bus)
{
    Init();
}

VDP1::~VDP1() = default;

// ============================================================================
// Initialization
// ============================================================================

void VDP1::Init()
{
    devlog::info<grp::vdp1>("Initializing...");
    
    // Initialize lookup tables (Mednafen style) - defined in vdp1_helpers.cpp
    InitGouraudLUT();
    
    // Map VRAM to bus
    // TODO: Map 0x05C00000-0x05C7FFFF to VRAM
    
    Reset(true);
    
    devlog::info<grp::vdp1>("Initialization complete");
}

void VDP1::Reset(bool hard)
{
    devlog::info<grp::vdp1>("Reset (hard={})", hard);
    
    if(hard)
    {
        // Power-on initialization of VRAM (Mednafen pattern)
        for(uint32 i = 0; i < 0x40000; i++)
        {
            uint16 val;
            
            if((i & 0xF) == 0)
                val = 0x8000;
            else if(i & 0x1)
                val = 0x5555;
            else
                val = 0xAAAA;
            
            m_state.VRAM[i] = val;
        }
        
        // Initialize framebuffers to white
        for(unsigned fb = 0; fb < 2; fb++)
            m_state.FB[fb].fill(0xFFFF);
        
        // Clear erase/write registers (undefined on power-on)
        m_state.EWDR = 0;
        m_state.EWLR = 0;
        m_state.EWRR = 0;
        
        // Clear clipping
        m_state.UserClipX0 = 0;
        m_state.UserClipY0 = 0;
        m_state.UserClipX1 = 0;
        m_state.UserClipY1 = 0;
        m_state.SysClipX = 0;
        m_state.SysClipY = 0;
        
        // Clear local coordinates
        m_state.LocalX = 0;
        m_state.LocalY = 0;
    }
    
    // Reset on every reset (not just power-on)
    m_state.FBDrawWhich = false;
    m_state.FBManualPending = false;
    m_state.FBVBErasePending = false;
    m_state.FBVBEraseActive = false;
    
    m_state.LOPR = 0;
    m_state.CurCommandAddr = 0;
    m_state.RetCommandAddr = -1;
    m_state.DrawingActive = false;
    
    // Confirmed register initialization on reset
    m_state.TVMR = 0;
    m_state.FBCR = 0;
    m_state.PTMR = 0;
    m_state.EDSR = 0;
    
    m_state.CycleCounter = 0;
    m_state.vb_status = false;
    m_state.hb_status = false;
    m_state.lastts = 0;
}

// ============================================================================
// Update / Main Loop
// ============================================================================

uint64 VDP1::Update(uint64 timestamp)
{
    // Command processing loop (simplified from Mednafen)
    while(m_state.DrawingActive && m_state.CycleCounter <= 0)
    {
        // Fetch command (16 words = 32 bytes)
        uint16 cmd_data[0x10];
        for(int i = 0; i < 0x10; i++)
        {
            cmd_data[i] = m_state.VRAM[(m_state.CurCommandAddr + i) & VRAM_MASK];
        }
        
        m_state.CycleCounter -= 16;  // Command fetch cost
        
        // Process command
        if(!(cmd_data[0] & 0xC000))
        {
            const unsigned cc = cmd_data[0] & 0xF;
            
            if(cc >= 0xC)
            {
                // Invalid command - stop drawing
                m_state.DrawingActive = false;
                break;
            }
            
            // Execute command (returns cycle cost)
            // TODO: Implement command table dispatch
            sint32 cycles = 0;
            switch(cc)
            {
                case 0x0: cycles = CMD_NormalSprite(cmd_data); break;
                case 0x1: cycles = CMD_ScaledSprite(cmd_data); break;
                case 0x2:
                case 0x3: cycles = CMD_DistortedSprite(cmd_data); break;
                case 0x4: cycles = CMD_Polygon(cmd_data); break;
                case 0x5:
                case 0x7: cycles = CMD_Polyline(cmd_data); break;
                case 0x6: cycles = CMD_Line(cmd_data); break;
                case 0x8:
                case 0xB: cycles = CMD_SetUserClip(cmd_data); break;
                case 0x9: cycles = CMD_SetSystemClip(cmd_data); break;
                case 0xA: cycles = CMD_SetLocalCoord(cmd_data); break;
            }
            
            m_state.CycleCounter -= cycles;
        }
        else if(cmd_data[0] & 0x8000)
        {
            // End command
            devlog::debug<grp::vdp1_cmd>("Drawing finished at 0x{:05X}", m_state.CurCommandAddr);
            m_state.DrawingActive = false;
            
            m_state.EDSR |= EDSR_CEF;
            
            // TODO: Trigger VDP1 draw end interrupt via SCU
            
            break;
        }
        
        // Advance to next command (commands are 0x10 words apart)
        m_state.CurCommandAddr = (m_state.CurCommandAddr + 0x10) & VRAM_MASK;
        
        // Handle command jump/link (bits 12-13)
        switch((cmd_data[0] >> 12) & 0x3)
        {
            case 0:  // Next
                break;
                
            case 1:  // Jump
                m_state.CurCommandAddr = (cmd_data[1] << 2) &~ 0xF;
                break;
                
            case 2:  // Call
                if(m_state.RetCommandAddr < 0)
                    m_state.RetCommandAddr = m_state.CurCommandAddr;
                m_state.CurCommandAddr = (cmd_data[1] << 2) &~ 0xF;
                break;
                
            case 3:  // Return
                if(m_state.RetCommandAddr >= 0)
                {
                    m_state.CurCommandAddr = m_state.RetCommandAddr;
                    m_state.RetCommandAddr = -1;
                }
                break;
        }
    }
    
    // Return next event time
    return timestamp + (m_state.DrawingActive ? 
        std::max<sint32>(UPDATE_TIMING_GRAN, 0 - m_state.CycleCounter) : 
        IDLE_TIMING_GRAN);
}

// ============================================================================
// Register Access
// ============================================================================
// Note: Actual implementation is in vdp1_registers.cpp

// ============================================================================
// HBlank/VBlank Events
// ============================================================================

void VDP1::SetHBVB(uint64 timestamp, bool new_hb, bool new_vb)
{
    // TODO: Implement HBlank/VBlank handling (framebuffer swap, erase trigger)
    m_state.hb_status = new_hb;
    m_state.vb_status = new_vb;
}

// ============================================================================
// Framebuffer Access for VDP2
// ============================================================================

bool VDP1::GetLine(int line, uint16* buf, unsigned w, 
                   uint32 rot_x, uint32 rot_y, 
                   uint32 rot_xinc, uint32 rot_yinc)
{
    // TODO: Implement scanline fetch for VDP2 composition
    // Returns false if VDP1 is not enabled for this line
    return false;
}

// ============================================================================
// Command Processing Stubs
// ============================================================================

void VDP1::StartDrawing()
{
    // Clear command end flag on draw start
    m_state.EDSR &= ~EDSR_CEF;
    
    m_state.CurCommandAddr = 0;
    m_state.RetCommandAddr = -1;
    m_state.DrawingActive = true;
    m_state.CycleCounter = UPDATE_TIMING_GRAN;
}

sint32 VDP1::CMD_SetUserClip(const uint16* cmd_data)
{
    m_state.UserClipX0 = cmd_data[0x6] & 0x3FF;
    m_state.UserClipY0 = cmd_data[0x7] & 0x1FF;
    m_state.UserClipX1 = cmd_data[0xA] & 0x3FF;
    m_state.UserClipY1 = cmd_data[0xB] & 0x1FF;
    
    return 0;  // No additional cycles
}

sint32 VDP1::CMD_SetSystemClip(const uint16* cmd_data)
{
    m_state.SysClipX = cmd_data[0xA] & 0x3FF;
    m_state.SysClipY = cmd_data[0xB] & 0x1FF;
    
    return 0;
}

sint32 VDP1::CMD_SetLocalCoord(const uint16* cmd_data)
{
    // Sign-extend 11-bit coordinates
    auto sign_extend_11 = [](uint16 val) -> sint32 {
        val &= 0x7FF;
        return (val & 0x400) ? (val | 0xFFFFF800) : val;
    };
    
    m_state.LocalX = sign_extend_11(cmd_data[0x6]);
    m_state.LocalY = sign_extend_11(cmd_data[0x7]);
    
    return 0;
}

// Drawing command stubs (to be implemented in separate files)

sint32 VDP1::CMD_NormalSprite(const uint16* cmd_data)
{
    // TODO: Implement normal sprite drawing
    devlog::warn<grp::vdp1_cmd>("CMD_NormalSprite stub");
    return 1000;  // Placeholder cycle count
}

sint32 VDP1::CMD_ScaledSprite(const uint16* cmd_data)
{
    // TODO: Implement scaled sprite drawing
    devlog::warn<grp::vdp1_cmd>("CMD_ScaledSprite stub");
    return 2000;
}

sint32 VDP1::CMD_DistortedSprite(const uint16* cmd_data)
{
    // TODO: Implement distorted sprite drawing
    devlog::warn<grp::vdp1_cmd>("CMD_DistortedSprite stub");
    return 3000;
}

sint32 VDP1::CMD_Polygon(const uint16* cmd_data)
{
    // TODO: Implement polygon drawing
    devlog::warn<grp::vdp1_cmd>("CMD_Polygon stub");
    return 1500;
}

sint32 VDP1::CMD_Polyline(const uint16* cmd_data)
{
    // TODO: Implement polyline drawing
    devlog::warn<grp::vdp1_cmd>("CMD_Polyline stub");
    return 1200;
}

sint32 VDP1::CMD_Line(const uint16* cmd_data)
{
    // TODO: Implement line drawing
    devlog::warn<grp::vdp1_cmd>("CMD_Line stub");
    return 800;
}

// ============================================================================
// Memory Access Helpers
// ============================================================================

uint8 VDP1::ReadVRAM8(uint32 addr) const
{
    // Read byte from VRAM (handle endianness)
    const uint32 word_addr = (addr >> 1) & VRAM_MASK;
    const uint16 word = m_state.VRAM[word_addr];
    return (addr & 1) ? (word & 0xFF) : (word >> 8);
}

uint16 VDP1::ReadVRAM16(uint32 addr) const
{
    return m_state.VRAM[(addr >> 1) & VRAM_MASK];
}

void VDP1::WriteVRAM8(uint32 addr, uint8 value)
{
    const uint32 word_addr = (addr >> 1) & VRAM_MASK;
    const uint16 word = m_state.VRAM[word_addr];
    
    if(addr & 1)
        m_state.VRAM[word_addr] = (word & 0xFF00) | value;
    else
        m_state.VRAM[word_addr] = (word & 0x00FF) | (value << 8);
}

void VDP1::WriteVRAM16(uint32 addr, uint16 value)
{
    m_state.VRAM[(addr >> 1) & VRAM_MASK] = value;
}

uint8 VDP1::ReadFB8(bool which, uint32 addr) const
{
    const uint32 word_addr = (addr >> 1) & FB_MASK;
    const uint16 word = m_state.FB[which][word_addr];
    return (addr & 1) ? (word & 0xFF) : (word >> 8);
}

uint16 VDP1::ReadFB16(bool which, uint32 addr) const
{
    return m_state.FB[which][(addr >> 1) & FB_MASK];
}

void VDP1::WriteFB8(bool which, uint32 addr, uint8 value)
{
    const uint32 word_addr = (addr >> 1) & FB_MASK;
    const uint16 word = m_state.FB[which][word_addr];
    
    if(addr & 1)
        m_state.FB[which][word_addr] = (word & 0xFF00) | value;
    else
        m_state.FB[which][word_addr] = (word & 0x00FF) | (value << 8);
}

void VDP1::WriteFB16(bool which, uint32 addr, uint16 value)
{
    m_state.FB[which][(addr >> 1) & FB_MASK] = value;
}

} // namespace brimir::vdp1

