#pragma once

#include <ymir/hw/vdp/renderer/vdp_renderer_base.hpp>

namespace ymir::vdp {

/// @brief Base type for all hardware renderers.
/// Defines some hardware rendere specific features and functions.
class HardwareVDPRendererBase : public IVDPRenderer {
public:
    HardwareVDPRendererBase(VDPRendererType type)
        : IVDPRenderer(type) {}

    virtual ~HardwareVDPRendererBase() = default;

    // -------------------------------------------------------------------------
    // Basics

    bool IsHardwareRenderer() const override {
        return true;
    }

    // -------------------------------------------------------------------------
    // Type casting and information

    HardwareVDPRendererBase *AsHardwareRenderer() override {
        return this;
    }
};

} // namespace ymir::vdp
