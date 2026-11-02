#include "gpu_pass_profile.hpp"

namespace brimir::gpu {

const char *GpuPassProfile::Name(Label label) {
    static constexpr std::array<const char *, kLabelCount> kNames{
        "boundary",       "vdp1_copy",       "vdp1_raster",        "vdp1_batch_range", "vdp1_bucket",
        "vdp1_bucket_sums", "vdp1_scatter",  "vdp1_resolve",       "vdp1_erase",       "vdp1_fb_write",
        "vdp1_other",     "nx_copy",         "nx_raster",          "nx_batch_range",   "nx_bucket",
        "nx_bucket_sums", "nx_scatter",      "nx_resolve",         "nx_erase",         "nx_fb_write",
        "nx_rebuild",     "nx_resolve_fast", "nx_filter",     "vdp2_sprite",     "vdp2_sprite_nx",     "vdp2_other"};
    return label < kLabelCount ? kNames[label] : "?";
}

GpuPassProfile::GpuPassProfile(VulkanDevice &device) : m_device(device) {
    uint32_t families = 0;
    volk::vkGetPhysicalDeviceQueueFamilyProperties(m_device.Physical(), &families, nullptr);
    std::vector<VkQueueFamilyProperties> props(families);
    volk::vkGetPhysicalDeviceQueueFamilyProperties(m_device.Physical(), &families, props.data());
    const uint32_t bits = m_device.QueueFamily() < families ? props[m_device.QueueFamily()].timestampValidBits : 0;
    if (bits == 0) {
        return;
    }
    m_mask = bits >= 64 ? ~uint64_t{0} : (uint64_t{1} << bits) - 1;
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = kQueriesPerContext * kMaxContexts;
    if (m_device.Table().vkCreateQueryPool(m_device.Device(), &info, nullptr, &m_pool) != VK_SUCCESS) {
        m_pool = VK_NULL_HANDLE;
    }
    m_results.resize(kQueriesPerContext);
}

GpuPassProfile::~GpuPassProfile() {
    if (m_pool != VK_NULL_HANDLE) {
        m_device.Table().vkDestroyQueryPool(m_device.Device(), m_pool, nullptr);
    }
}

void GpuPassProfile::Begin(uint32_t context) {
    m_current = -1;
    if (m_pool == VK_NULL_HANDLE || context >= kMaxContexts) {
        return;
    }
    Collect(context);
    m_current = static_cast<int32_t>(context);
}

void GpuPassProfile::Dropped() {
    if (m_current >= 0) {
        m_count[m_current] = 0;
        m_reset[m_current] = false;
        m_labels[m_current].clear();
    }
}

void GpuPassProfile::Stamp(VkCommandBuffer cmd, Label label) {
    if (m_current < 0) {
        return;
    }
    const uint32_t c = static_cast<uint32_t>(m_current);
    if (m_count[c] >= kQueriesPerContext) {
        ++m_overflow;
        return;
    }
    const auto &vk = m_device.Table();
    if (!m_reset[c]) {
        vk.vkCmdResetQueryPool(cmd, m_pool, c * kQueriesPerContext, kQueriesPerContext);
        m_reset[c] = true;
    }
    vk.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool, c * kQueriesPerContext + m_count[c]);
    ++m_count[c];
    m_labels[c].push_back(label);
}

void GpuPassProfile::Collect(uint32_t c) {
    const uint32_t count = m_count[c];
    m_count[c] = 0;
    m_reset[c] = false;
    std::vector<Label> labels = std::move(m_labels[c]);
    m_labels[c].clear();
    if (count < 2) {
        return;
    }
    if (m_device.Table().vkGetQueryPoolResults(m_device.Device(), m_pool, c * kQueriesPerContext, count,
                                               count * sizeof(uint64_t), m_results.data(), sizeof(uint64_t),
                                               VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
        return;
    }
    const double period = m_device.Properties().limits.timestampPeriod * 1e-3;
    for (uint32_t i = 1; i < count; ++i) {
        if (labels[i] != kBoundary) {
            m_micros[labels[i]] += static_cast<double>((m_results[i] - m_results[i - 1]) & m_mask) * period;
        }
    }
}

std::array<double, GpuPassProfile::kLabelCount> GpuPassProfile::Take() {
    const std::array<double, kLabelCount> micros = m_micros;
    m_micros.fill(0);
    return micros;
}

uint64_t GpuPassProfile::TakeOverflow() {
    const uint64_t overflow = m_overflow;
    m_overflow = 0;
    return overflow;
}

} // namespace brimir::gpu
