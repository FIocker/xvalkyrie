#include "backends/graphics/vulkan/offscreen.hpp"
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vulkan/vulkan.hpp>

namespace xv::graphics::vulkan {
namespace {
constexpr auto format = vk::Format::eR8G8B8A8Unorm;
constexpr auto color_layout = vk::ImageLayout::eColorAttachmentOptimal;
constexpr auto transfer_layout = vk::ImageLayout::eTransferSrcOptimal;
constexpr auto image_usage =
    vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc;
constexpr vk::ImageSubresourceRange color_range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

struct Adapter {
    vk::PhysicalDevice gpu;
    std::uint32_t family;
};
Adapter select_adapter(vk::Instance instance, vk::Extent2D extent) {
    for (const auto gpu : instance.enumeratePhysicalDevices()) {
        vk::ImageFormatProperties properties;
        const auto result = gpu.getImageFormatProperties(
            format, vk::ImageType::e2D, vk::ImageTiling::eOptimal, image_usage, {}, &properties);
        if (result == vk::Result::eErrorFormatNotSupported)
            continue;
        if (result != vk::Result::eSuccess)
            throw std::runtime_error("Cannot query format support: " + vk::to_string(result));
        if (properties.maxExtent.width < extent.width ||
            properties.maxExtent.height < extent.height)
            continue;
        const auto queues = gpu.getQueueFamilyProperties();
        for (std::uint32_t i = 0; i < queues.size(); ++i)
            if (queues[i].queueCount && (queues[i].queueFlags & vk::QueueFlagBits::eGraphics))
                return {gpu, i};
    }
    throw std::runtime_error("No Vulkan graphics queue with RGBA8 attachment/readback support");
}

std::uint32_t memory_type(vk::PhysicalDevice gpu, std::uint32_t allowed,
                          vk::MemoryPropertyFlags required, vk::MemoryPropertyFlags preferred) {
    const auto properties = gpu.getMemoryProperties();
    for (auto flags : {required | preferred, required}) {
        for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i)
            if ((allowed & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
    }
    throw std::runtime_error("No compatible Vulkan memory type");
}

vk::ClearValue clear_value(Rgba8 color) {
    std::array<float, 4> normalized{};
    for (std::size_t i = 0; i < color.size(); ++i)
        normalized[i] = static_cast<float>(color[i]) / 255.0f;
    return vk::ClearValue(vk::ClearColorValue(normalized));
}

vk::UniqueRenderPass make_render_pass(vk::Device device) {
    vk::AttachmentDescription attachment{};
    attachment.setFormat(format)
        .setSamples(vk::SampleCountFlagBits::e1)
        .setLoadOp(vk::AttachmentLoadOp::eClear)
        .setStoreOp(vk::AttachmentStoreOp::eStore)
        .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare)
        .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
        .setInitialLayout(vk::ImageLayout::eUndefined)
        .setFinalLayout(color_layout);
    const vk::AttachmentReference ref{0, color_layout};
    vk::SubpassDescription subpass{};
    subpass.setPipelineBindPoint(vk::PipelineBindPoint::eGraphics).setColorAttachments(ref);
    return device.createRenderPassUnique(
        vk::RenderPassCreateInfo{}.setAttachments(attachment).setSubpasses(subpass));
}

void record(vk::CommandBuffer cmd, const ClearPass &pass, vk::RenderPass render_pass,
            vk::Framebuffer framebuffer, vk::Image image, vk::Buffer buffer) {
    cmd.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    const auto background = clear_value(pass.background);
    const vk::Rect2D full{{0, 0}, {pass.width, pass.height}};
    cmd.beginRenderPass(
        vk::RenderPassBeginInfo{render_pass, framebuffer, full}.setClearValues(background),
        vk::SubpassContents::eInline);
    for (const auto &r : pass.rectangles) {
        const vk::ClearAttachment clear{vk::ImageAspectFlagBits::eColor, 0, clear_value(r.color)};
        const vk::ClearRect rectangle{
            {{static_cast<int>(r.x), static_cast<int>(r.y)}, {r.width, r.height}}, 0, 1};
        cmd.clearAttachments(clear, rectangle);
    }
    cmd.endRenderPass();

    const vk::ImageMemoryBarrier to_copy{vk::AccessFlagBits::eColorAttachmentWrite,
                                         vk::AccessFlagBits::eTransferRead,
                                         color_layout,
                                         transfer_layout,
                                         VK_QUEUE_FAMILY_IGNORED,
                                         VK_QUEUE_FAMILY_IGNORED,
                                         image,
                                         color_range};
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                        vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, to_copy);
    vk::BufferImageCopy copy{};
    copy.setImageSubresource({vk::ImageAspectFlagBits::eColor, 0, 0, 1})
        .setImageExtent({pass.width, pass.height, 1});
    cmd.copyImageToBuffer(image, transfer_layout, buffer, copy);
    const vk::MemoryBarrier to_host{vk::AccessFlagBits::eTransferWrite,
                                    vk::AccessFlagBits::eHostRead};
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost, {},
                        to_host, {}, {});
    cmd.end();
}
} // namespace

Readback render(const ClearPass &pass) {
    const auto pixels = pass.validated_pixel_count();
    static_assert(sizeof(Rgba8) == 4);
    const auto size = pixels * sizeof(Rgba8);
    const vk::ApplicationInfo app{"XValkyrie host graphics check", 1, nullptr, 0,
                                  VK_API_VERSION_1_0};
    auto instance = vk::createInstanceUnique(vk::InstanceCreateInfo{}.setPApplicationInfo(&app));
    const auto [gpu, family] = select_adapter(*instance, {pass.width, pass.height});
    const float priority = 1;
    const vk::DeviceQueueCreateInfo queue_info{{}, family, 1, &priority};
    auto device = gpu.createDeviceUnique(vk::DeviceCreateInfo{}.setQueueCreateInfos(queue_info));
    const auto queue = device->getQueue(family, 0);

    // Bound images and buffers must be destroyed before their memory allocations.
    vk::UniqueDeviceMemory image_memory, buffer_memory;
    const vk::ImageCreateInfo image_info{{},
                                         vk::ImageType::e2D,
                                         format,
                                         {pass.width, pass.height, 1},
                                         1,
                                         1,
                                         vk::SampleCountFlagBits::e1,
                                         vk::ImageTiling::eOptimal,
                                         image_usage};
    auto image = device->createImageUnique(image_info);
    const auto im = device->getImageMemoryRequirements(*image);
    image_memory = device->allocateMemoryUnique(
        {im.size,
         memory_type(gpu, im.memoryTypeBits, {}, vk::MemoryPropertyFlagBits::eDeviceLocal)});
    device->bindImageMemory(*image, *image_memory, 0);
    auto buffer = device->createBufferUnique({{}, size, vk::BufferUsageFlagBits::eTransferDst});
    const auto bm = device->getBufferMemoryRequirements(*buffer);
    const auto host_type =
        memory_type(gpu, bm.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible,
                    vk::MemoryPropertyFlagBits::eHostCoherent);
    buffer_memory = device->allocateMemoryUnique({bm.size, host_type});
    device->bindBufferMemory(*buffer, *buffer_memory, 0);

    auto view = device->createImageViewUnique(
        {{}, *image, vk::ImageViewType::e2D, format, {}, color_range});
    auto render_pass = make_render_pass(*device);
    auto framebuffer =
        device->createFramebufferUnique({{}, *render_pass, 1, &*view, pass.width, pass.height, 1});
    auto pool =
        device->createCommandPoolUnique({vk::CommandPoolCreateFlagBits::eTransient, family});
    const auto cmd =
        device->allocateCommandBuffers({*pool, vk::CommandBufferLevel::ePrimary, 1}).front();
    record(cmd, pass, *render_pass, *framebuffer, *image, *buffer);
    auto fence = device->createFenceUnique({});
    queue.submit(vk::SubmitInfo{}.setCommandBuffers(cmd), *fence);
    if (device->waitForFences(*fence, true, std::numeric_limits<std::uint64_t>::max()) !=
        vk::Result::eSuccess)
        throw std::runtime_error("Vulkan readback fence did not signal");

    Readback output{gpu.getProperties().deviceName.data(), std::vector<Rgba8>(pixels)};
    const void *mapped = device->mapMemory(*buffer_memory, 0, VK_WHOLE_SIZE);
    const auto flags = gpu.getMemoryProperties().memoryTypes[host_type].propertyFlags;
    if (!(flags & vk::MemoryPropertyFlagBits::eHostCoherent))
        device->invalidateMappedMemoryRanges(
            vk::MappedMemoryRange{*buffer_memory, 0, VK_WHOLE_SIZE});
    std::memcpy(output.pixels.data(), mapped, size);
    device->unmapMemory(*buffer_memory);
    return output;
}
} // namespace xv::graphics::vulkan
