#include "shader_contract.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <faset/core/io.hpp>
#include <faset/render/render_graph.hpp>
#include <faset/render/lighting.hpp>
#include <faset/render/renderer.hpp>
#include <faset/render/temporal.hpp>
#include <faset/render/visibility.hpp>
#include <fstream>
#include <iostream>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vulkan/vulkan.h>

namespace faset::render {
namespace {
void check(VkResult result, const char* action) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(action) + " failed (Vulkan " + std::to_string(result) +
                                 ")");
}
struct GpuVertex {
    float clip[4], world[3], normal[3], color[4], material[2], uv[2];
    float previous_clip[4], motion_valid{};
};
// The GPU path keeps local geometry separate from the instance table. All layouts
// below are mirrored by gpu_scene.slang and checked by shader reflection tests.
struct SceneVertex {
    float position[3], normal[3], color[4], uv[2];
};
static_assert(sizeof(SceneVertex) == 48);
struct SceneInstance {
    Mat4 model;
    std::array<float, 4> normal0, normal1, normal2;
    std::array<float, 4> color, material;
    std::array<float, 4> center_extent, half_extent;
    std::array<float, 4> previous_center_extent, previous_half_extent;
    std::array<std::uint32_t, 4> metadata;
    Mat4 previous_model;
};
static_assert(sizeof(SceneInstance) == 288 && offsetof(SceneInstance, previous_model) == 224);
struct SceneView {
    Mat4 current_vp, previous_vp;
    std::array<float, 4> viewport, previous_viewport;
    std::array<std::uint32_t, 4> hzb_dimensions, previous_hzb_dimensions, flags;
};
static_assert(sizeof(SceneView) == 208);
struct SceneBin {
    std::uint32_t candidate_first, candidate_count, visible_base, capacity;
};
struct SceneCandidate {
    std::uint32_t instance_id, bin_index, flags, unused;
};
struct SceneIndirect {
    std::uint32_t vertex_count, instance_count, first_vertex, first_instance;
};
static_assert(sizeof(SceneBin) == 16 && sizeof(SceneCandidate) == 16 &&
              sizeof(SceneIndirect) == 16);
struct Push {
    Mat4 light_view_projection;
    std::array<float, 4> light_direction, eye;
};
static_assert(sizeof(Push) == 96, "Slang FrameParameters layout");
struct ScenePush {
    Push frame;
    std::array<std::uint32_t, 4> draw_info;
};
static_assert(sizeof(ScenePush) == 112);
struct LightingHeaderGpu {
    std::array<std::uint32_t, 4> counts{};
    std::array<float, 4> sun_direction_intensity{};
    std::array<float, 4> sun_color{};
    std::array<float, 4> camera_forward_shadow_distance{};
    std::array<float, 4> cascade_splits{};
};
struct LocalLightGpu {
    std::array<float, 4> position_range{};
    std::array<float, 4> direction_cos_outer{};
    std::array<float, 4> color_intensity{};
    std::array<float, 4> cone_type_shadow_view{};
    std::array<float, 4> reserved{};
};
struct ShadowViewGpu {
    Mat4 view_projection{identity};
    std::array<float, 4> tile_scale_offset{};
    std::array<float, 4> guarded_clamp{};
    std::array<float, 4> bias_flags{};
};
struct LightTilePush {
    Mat4 view_projection;
    std::array<float, 4> viewport;
    std::array<std::uint32_t, 4> dimensions;
};
static_assert(sizeof(LightTilePush) == 96);
static_assert(sizeof(LightingHeaderGpu) == 80 &&
              offsetof(LightingHeaderGpu, sun_direction_intensity) == 16 &&
              offsetof(LightingHeaderGpu, sun_color) == 32 &&
              offsetof(LightingHeaderGpu, camera_forward_shadow_distance) == 48 &&
              offsetof(LightingHeaderGpu, cascade_splits) == 64);
static_assert(sizeof(LocalLightGpu) == 80 &&
              offsetof(LocalLightGpu, direction_cos_outer) == 16 &&
              offsetof(LocalLightGpu, color_intensity) == 32 &&
              offsetof(LocalLightGpu, cone_type_shadow_view) == 48 &&
              offsetof(LocalLightGpu, reserved) == 64);
static_assert(sizeof(ShadowViewGpu) == 112 &&
              offsetof(ShadowViewGpu, tile_scale_offset) == 64 &&
              offsetof(ShadowViewGpu, guarded_clamp) == 80 &&
              offsetof(ShadowViewGpu, bias_flags) == 96);
std::array<float, 4> point(const Mat4& m, std::array<float, 4> p) {
    std::array<float, 4> o{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            o[r] += m[c * 4 + r] * p[c];
    return o;
}
struct Buffer {
    VkBuffer handle{};
    VkDeviceMemory memory{};
    VkDeviceSize size{}, allocation_size{};
};
struct Image {
    VkImage handle{};
    VkDeviceMemory memory{};
    VkImageView view{};
    std::vector<VkImageView> mip_views;
    std::uint32_t width{}, height{};
    std::uint32_t mip_levels{1};
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkDeviceSize allocation_size{};
};
struct Batch {
    std::uint32_t first{}, count{};
    const Texture* texture{};
    std::array<float, 4> clip_rect{};
};
struct SceneDrawBin {
    const Mesh* mesh{};
    const Texture* texture{};
    SceneIndirect command{};
    SceneBin range{};
};
struct PreparedScene {
    std::vector<SceneVertex> vertices;
    std::vector<SceneInstance> instances;
    std::vector<SceneCandidate> candidates;
    std::vector<SceneBin> bins;
    std::vector<SceneIndirect> commands;
    std::vector<const Texture*> textures;
    SceneView view{};
    std::uint32_t candidate_count{};
};
float projected_pixels(const Bounds& bounds, const Mat4& view_projection,
                       const std::array<float, 4>& viewport) {
    float min_x = std::numeric_limits<float>::infinity();
    float max_x = -min_x, min_y = min_x, max_y = -min_x;
    for (unsigned corner = 0; corner < 8; ++corner) {
        Vec3 p{corner & 1 ? bounds.max[0] : bounds.min[0],
               corner & 2 ? bounds.max[1] : bounds.min[1],
               corner & 4 ? bounds.max[2] : bounds.min[2]};
        auto clip = point(view_projection, {p[0], p[1], p[2], 1});
        if (!std::isfinite(clip[0]) || !std::isfinite(clip[1]) ||
            !std::isfinite(clip[3]) || clip[3] <= 0)
            return std::numeric_limits<float>::max();
        const float x = (clip[0] / clip[3] * .5f + .5f) * viewport[2];
        const float y = (clip[1] / clip[3] * .5f + .5f) * viewport[3];
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_y = std::min(min_y, y);
        max_y = std::max(max_y, y);
    }
    return std::max(max_x - min_x, max_y - min_y);
}
bool opaque_texture(const Texture* texture) {
    if (!texture)
        return true;
    for (std::size_t i = 3; i < texture->rgba.size(); i += 4)
        if (texture->rgba[i] != 255)
            return false;
    return true;
}
struct SceneResources {
    Buffer vertices, instances, candidates, bins, view;
    Buffer main_ids, post_ids, main_args, post_args, deferred_ids, deferred_count;
    // CPU-owned templates double as optional diagnostic readback destinations.
    Buffer main_args_stage, post_args_stage, deferred_count_stage;
    Image hzb[2];
    VkDescriptorSetLayout graphics_layout{}, cull_layout{}, hzb_layout{};
    VkDescriptorPool descriptor_pool{};
    VkDescriptorSet graphics_main{}, graphics_post{}, cull_main{}, cull_post{};
    std::vector<VkDescriptorSet> hzb_sets[2];
    VkPipelineLayout graphics_pipeline_layout{}, cull_pipeline_layout{}, hzb_pipeline_layout{};
    VkPipeline graphics_pipeline{}, cull_pipeline{}, post_pipeline{}, hzb_pipeline{};
    std::array<std::string, 5> shader_layouts{};
    std::uint32_t hzb_mips{}, hzb_current{};
    bool hzb_history_valid{}, available{};
    bool hzb_supported{}, hzb_extent_supported{};
    Mat4 previous_vp{identity};
    Mat4 previous_projection{identity};
    std::array<float, 4> previous_viewport{};
    std::string previous_view_id;
};
struct TemporalResources {
    Image scene_color, velocity, history_color[2], history_depth[2];
    Buffer pixel_counts, pixel_counts_stage;
    VkDescriptorSetLayout resolve_layout{}, composite_layout{};
    VkDescriptorPool descriptor_pool{};
    VkDescriptorSet resolve_sets[2]{}, composite_sets[2]{};
    VkPipelineLayout resolve_pipeline_layout{}, composite_pipeline_layout{};
    VkPipeline resolve_pipeline{}, composite_pipeline{};
    VkPipeline direct_pipeline{}, transparent_pipeline{}, sprite_pipeline{}, gpu_pipeline{};
    std::array<std::string, 3> shader_layouts{};
    std::array<std::string, 3> scene_shader_layouts{};
    TemporalCapabilities capabilities{};
    TemporalHistoryState history;
    Mat4 previous_jittered_vp{identity};
    Mat4 previous_unjittered_vp{identity};
    std::array<float, 2> previous_jitter{};
    std::uint64_t shader_generation{1};
    std::uint32_t internal_width{}, internal_height{}, completed_index{};
    bool has_completed_image{};
};
constexpr std::uint32_t timestamp_capacity = 40;
} // namespace
struct Renderer::Impl {
    RendererConfig config;
    SDL_Window* window{};
    std::string offscreen_clipboard;
    bool sdl{}, close{}, dirty_swapchain{};
    std::uint32_t width{}, height{};
    VkInstance instance{};
    VkDebugUtilsMessengerEXT messenger{};
    VkSurfaceKHR surface{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    std::uint32_t queue_family{};
    std::uint32_t max_compute_groups_x{}, max_compute_groups_y{},
                  max_storage_buffer_range{},
                  max_image_dimension{};
    bool independent_blend_supported{};
    VkCommandPool pool{};
    VkCommandBuffer command{};
    VkFence fence{};
    VkQueryPool timestamp_pool{};
    float timestamp_period{};
    std::uint32_t timestamp_bits{};
    VkSemaphore acquired{}, present_ready{};
    std::array<std::string, 4> shader_layouts{};
    VkSwapchainKHR swapchain{};
    VkFormat swap_format{};
    VkExtent2D swap_extent{};
    std::vector<VkImage> swap_images;
    std::vector<VkImageLayout> swap_layouts;
    Image color, depth, shadow, local_shadow;
    std::uint32_t sun_shadow_size{}, local_shadow_size{};
    Buffer vertices, readback;
    Buffer lighting_header, lighting_locals, lighting_views, light_tile_words,
           light_tile_readback;
    bool light_tiles_capable{};
    SceneResources scene;
    TemporalResources temporal;
    InstanceTracker instance_tracker;
    std::unordered_map<std::string, std::size_t> previous_lods;
    struct CachedBounds {
        std::weak_ptr<const Mesh> owner;
        Bounds local;
    };
    struct CachedOpacity {
        std::weak_ptr<const Texture> owner;
        std::uint64_t revision{};
        bool opaque{};
    };
    std::unordered_map<const Mesh*, CachedBounds> bounds_cache;
    std::unordered_map<const Texture*, CachedOpacity> opacity_cache;
    VkDescriptorSetLayout descriptor_layout{};
    VkDescriptorPool descriptor_pool{};
    VkDescriptorSetLayout lighting_layout{};
    VkDescriptorPool lighting_pool{};
    VkDescriptorSet lighting_set{};
    VkDescriptorSetLayout light_tile_layout{};
    VkDescriptorPool light_tile_pool{};
    VkDescriptorSet light_tile_set{};
    VkPipelineLayout light_tile_pipeline_layout{};
    VkPipeline light_tile_pipeline{};
    VkSampler shadow_sampler{}, color_sampler{};
    VkPipelineLayout pipeline_layout{};
    VkPipeline pipeline{}, ui_pipeline{}, shadow_pipeline{}, sprite_pipeline{},
               temporal_ui_pipeline{};
    PFN_vkCmdBeginDebugUtilsLabelEXT begin_gpu_label{};
    PFN_vkCmdEndDebugUtilsLabelEXT end_gpu_label{};
    struct GpuTexture {
        Image image;
        VkDescriptorSet descriptor{};
        std::shared_ptr<const Texture> source;
        std::uint64_t revision{};
    };
    std::unordered_map<const Texture*, GpuTexture> textures;
    std::shared_ptr<Texture> white;
    std::vector<std::uint8_t> last_pixels;
    FrameStats statistics;
    std::atomic<std::uint32_t> validation_errors{};
    ~Impl() {
        cleanup();
    }
    static VKAPI_ATTR VkBool32 VKAPI_CALL debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                VkDebugUtilsMessageTypeFlagsEXT,
                                                const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                void* user) {
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            static_cast<Impl*>(user)->validation_errors.fetch_add(1);
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            std::cerr << "[Vulkan] " << data->pMessage << '\n';
        return VK_FALSE;
    }
    void destroy(Buffer& b) {
        if (device) {
            if (b.handle)
                vkDestroyBuffer(device, b.handle, nullptr);
            if (b.memory)
                vkFreeMemory(device, b.memory, nullptr);
        }
        b = {};
    }
    void destroy(Image& i) {
        if (device) {
            for (auto view : i.mip_views)
                vkDestroyImageView(device, view, nullptr);
            if (i.view)
                vkDestroyImageView(device, i.view, nullptr);
            if (i.handle)
                vkDestroyImage(device, i.handle, nullptr);
            if (i.memory)
                vkFreeMemory(device, i.memory, nullptr);
        }
        i = {};
    }
    void destroy_scene_interfaces() {
        if (!device)
            return;
        for (auto* pipeline : {&scene.graphics_pipeline, &scene.cull_pipeline,
                               &scene.post_pipeline, &scene.hzb_pipeline}) {
            if (*pipeline)
                vkDestroyPipeline(device, *pipeline, nullptr);
            *pipeline = {};
        }
        for (auto* layout : {&scene.graphics_pipeline_layout, &scene.cull_pipeline_layout,
                             &scene.hzb_pipeline_layout}) {
            if (*layout)
                vkDestroyPipelineLayout(device, *layout, nullptr);
            *layout = {};
        }
        if (scene.descriptor_pool)
            vkDestroyDescriptorPool(device, scene.descriptor_pool, nullptr);
        scene.descriptor_pool = {};
        for (auto* layout : {&scene.graphics_layout, &scene.cull_layout,
                             &scene.hzb_layout}) {
            if (*layout)
                vkDestroyDescriptorSetLayout(device, *layout, nullptr);
            *layout = {};
        }
        scene.graphics_main = scene.graphics_post = scene.cull_main =
            scene.cull_post = {};
        for (auto& sets : scene.hzb_sets)
            sets.clear();
        scene.shader_layouts = {};
    }
    void destroy_temporal_interfaces() {
        if (!device)
            return;
        for (auto* pipeline : {&temporal.resolve_pipeline, &temporal.composite_pipeline,
                               &temporal.direct_pipeline, &temporal.transparent_pipeline,
                               &temporal.gpu_pipeline}) {
            if (*pipeline)
                vkDestroyPipeline(device, *pipeline, nullptr);
            *pipeline = {};
        }
        for (auto* layout : {&temporal.resolve_pipeline_layout,
                             &temporal.composite_pipeline_layout}) {
            if (*layout)
                vkDestroyPipelineLayout(device, *layout, nullptr);
            *layout = {};
        }
        if (temporal.descriptor_pool)
            vkDestroyDescriptorPool(device, temporal.descriptor_pool, nullptr);
        temporal.descriptor_pool = {};
        for (auto* layout : {&temporal.resolve_layout, &temporal.composite_layout}) {
            if (*layout)
                vkDestroyDescriptorSetLayout(device, *layout, nullptr);
            *layout = {};
        }
        temporal.resolve_sets[0] = temporal.resolve_sets[1] = {};
        temporal.composite_sets[0] = temporal.composite_sets[1] = {};
    }
    void cleanup() {
        if (device)
            vkDeviceWaitIdle(device);
        for (auto& [_, texture] : textures)
            destroy(texture.image);
        destroy(scene.vertices);
        destroy(scene.instances);
        destroy(scene.candidates);
        destroy(scene.bins);
        destroy(scene.view);
        destroy(scene.main_ids);
        destroy(scene.post_ids);
        destroy(scene.main_args);
        destroy(scene.post_args);
        destroy(scene.deferred_ids);
        destroy(scene.deferred_count);
        destroy(scene.main_args_stage);
        destroy(scene.post_args_stage);
        destroy(scene.deferred_count_stage);
        destroy(scene.hzb[0]);
        destroy(scene.hzb[1]);
        destroy(temporal.scene_color);
        destroy(temporal.velocity);
        for (auto& image : temporal.history_color)
            destroy(image);
        for (auto& image : temporal.history_depth)
            destroy(image);
        destroy(temporal.pixel_counts);
        destroy(temporal.pixel_counts_stage);
        destroy(vertices);
        destroy(readback);
        destroy(lighting_header);
        destroy(lighting_locals);
        destroy(lighting_views);
        destroy(light_tile_words);
        destroy(light_tile_readback);
        destroy(color);
        destroy(depth);
        destroy(shadow);
        destroy(local_shadow);
        if (device) {
            destroy_temporal_interfaces();
            destroy_scene_interfaces();
            destroy_light_tile_interfaces();
            if (pipeline)
                vkDestroyPipeline(device, pipeline, nullptr);
            if (ui_pipeline)
                vkDestroyPipeline(device, ui_pipeline, nullptr);
            if (shadow_pipeline)
                vkDestroyPipeline(device, shadow_pipeline, nullptr);
            if (sprite_pipeline)
                vkDestroyPipeline(device, sprite_pipeline, nullptr);
            if (temporal_ui_pipeline)
                vkDestroyPipeline(device, temporal_ui_pipeline, nullptr);
            if (pipeline_layout)
                vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
            if (descriptor_pool)
                vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
            if (lighting_pool)
                vkDestroyDescriptorPool(device, lighting_pool, nullptr);
            if (descriptor_layout)
                vkDestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
            if (lighting_layout)
                vkDestroyDescriptorSetLayout(device, lighting_layout, nullptr);
            if (shadow_sampler)
                vkDestroySampler(device, shadow_sampler, nullptr);
            if (color_sampler)
                vkDestroySampler(device, color_sampler, nullptr);
            if (swapchain)
                vkDestroySwapchainKHR(device, swapchain, nullptr);
            if (timestamp_pool)
                vkDestroyQueryPool(device, timestamp_pool, nullptr);
            if (acquired)
                vkDestroySemaphore(device, acquired, nullptr);
            if (present_ready)
                vkDestroySemaphore(device, present_ready, nullptr);
            if (fence)
                vkDestroyFence(device, fence, nullptr);
            if (pool)
                vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (surface)
            vkDestroySurfaceKHR(instance, surface, nullptr);
        if (messenger) {
            auto fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (fn)
                fn(instance, messenger, nullptr);
        }
        if (instance)
            vkDestroyInstance(instance, nullptr);
        if (window)
            SDL_DestroyWindow(window);
        if (sdl)
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags properties,
                              VkMemoryPropertyFlags preferred = 0) {
        VkPhysicalDeviceMemoryProperties p{};
        vkGetPhysicalDeviceMemoryProperties(physical, &p);
        if (preferred)
            for (std::uint32_t i = 0; i < p.memoryTypeCount; ++i)
                if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags &
                                           (properties | preferred)) == (properties | preferred))
                    return i;
        for (std::uint32_t i = 0; i < p.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & properties) == properties)
                return i;
        throw std::runtime_error("Required Vulkan memory type is unavailable");
    }
    Buffer make_buffer(VkDeviceSize bytes, VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags properties, VkMemoryPropertyFlags preferred = 0) {
        Buffer b{};
        b.size = bytes;
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = bytes;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device, &info, nullptr, &b.handle), "Create buffer");
        try {
            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(device, b.handle, &req);
            VkMemoryAllocateInfo alloc{};
            alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize = req.size;
            alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, properties, preferred);
            check(vkAllocateMemory(device, &alloc, nullptr, &b.memory), "Allocate buffer memory");
            b.allocation_size = req.size;
            check(vkBindBufferMemory(device, b.handle, b.memory, 0), "Bind buffer memory");
        } catch (...) {
            destroy(b);
            throw;
        }
        return b;
    }
    Image make_image(std::uint32_t w, std::uint32_t h, VkFormat format, VkImageUsageFlags usage,
                     VkImageAspectFlags aspect, std::uint32_t mip_levels = 1) {
        Image image{};
        image.width = w;
        image.height = h;
        image.mip_levels = mip_levels;
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {w, h, 1};
        info.mipLevels = mip_levels;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateImage(device, &info, nullptr, &image.handle), "Create image");
        try {
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(device, image.handle, &req);
            VkMemoryAllocateInfo alloc{};
            alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize = req.size;
            alloc.memoryTypeIndex =
                memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check(vkAllocateMemory(device, &alloc, nullptr, &image.memory),
                  "Allocate image memory");
            image.allocation_size = req.size;
            check(vkBindImageMemory(device, image.handle, image.memory, 0), "Bind image memory");
            VkImageViewCreateInfo view{};
            view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image = image.handle;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = format;
            view.subresourceRange = {aspect, 0, mip_levels, 0, 1};
            check(vkCreateImageView(device, &view, nullptr, &image.view), "Create image view");
            if (mip_levels > 1) {
                image.mip_views.reserve(mip_levels);
                for (std::uint32_t level = 0; level < mip_levels; ++level) {
                    view.subresourceRange = {aspect, level, 1, 0, 1};
                    VkImageView mip_view{};
                    check(vkCreateImageView(device, &view, nullptr, &mip_view),
                          "Create mip view");
                    image.mip_views.push_back(mip_view);
                }
            }
        } catch (...) {
            destroy(image);
            throw;
        }
        return image;
    }
    void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout& before, VkImageLayout after,
                    VkImageAspectFlags aspect, std::uint32_t mip_levels = 1) {
        // Conservative dependencies make the first single-queue backend auditable.
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = before == VK_IMAGE_LAYOUT_UNDEFINED
                                   ? VK_PIPELINE_STAGE_2_NONE
                                   : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask =
            before == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.oldLayout = before;
        barrier.newLayout = after;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {aspect, 0, mip_levels, 0, 1};
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
        before = after;
    }
    void transition(VkCommandBuffer cmd, Image& image, VkImageLayout after,
                    VkImageAspectFlags aspect) {
        transition(cmd, image.handle, image.layout, after, aspect, image.mip_levels);
    }
    void begin() {
        check(vkResetCommandBuffer(command, 0), "Reset command buffer");
        VkCommandBufferBeginInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command, &info), "Begin command buffer");
    }
    void submit(bool present = false) {
        check(vkEndCommandBuffer(command), "End command buffer");
        check(vkResetFences(device, 1, &fence), "Reset fence");
        VkCommandBufferSubmitInfo cmd{};
        cmd.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd.commandBuffer = command;
        VkSubmitInfo2 info{};
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        info.commandBufferInfoCount = 1;
        info.pCommandBufferInfos = &cmd;
        VkSemaphoreSubmitInfo wait{}, signal{};
        wait.sType = signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        if (present) {
            wait.semaphore = acquired;
            wait.stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            signal.semaphore = present_ready;
            signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            info.waitSemaphoreInfoCount = 1;
            info.pWaitSemaphoreInfos = &wait;
            info.signalSemaphoreInfoCount = 1;
            info.pSignalSemaphoreInfos = &signal;
        }
        check(vkQueueSubmit2(queue, 1, &info, fence), "Submit frame");
        check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "Wait frame fence");
    }
    void initialize(const RendererConfig& c) {
        config = c;
        width = c.width;
        height = c.height;
        if (!width || !height)
            throw std::invalid_argument("Renderer dimensions must be nonzero");
        (void)temporal_internal_extent(width, height, c.temporal_mode, c.render_scale);
        std::vector<const char*> extensions;
        if (!c.headless) {
            if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
                throw std::runtime_error(SDL_GetError());
            sdl = true;
            window = SDL_CreateWindow(
                c.title.c_str(), static_cast<int>(width), static_cast<int>(height),
                SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
            if (!window)
                throw std::runtime_error(SDL_GetError());
            Uint32 count{};
            auto names = SDL_Vulkan_GetInstanceExtensions(&count);
            if (!names)
                throw std::runtime_error(SDL_GetError());
            extensions.assign(names, names + count);
            SDL_StartTextInput(window);
        }
        std::uint32_t count{};
        check(vkEnumerateInstanceLayerProperties(&count, nullptr), "Enumerate layers");
        std::vector<VkLayerProperties> layers(count);
        check(vkEnumerateInstanceLayerProperties(&count, layers.data()), "Enumerate layers");
        check(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr),
              "Enumerate instance extensions");
        std::vector<VkExtensionProperties> instance_extensions(count);
        check(vkEnumerateInstanceExtensionProperties(nullptr, &count, instance_extensions.data()),
              "Enumerate instance extensions");
        const bool debug_utils =
            std::any_of(instance_extensions.begin(), instance_extensions.end(), [](const auto& p) {
                return std::strcmp(p.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
            });
        bool validation =
            c.validation && debug_utils && std::any_of(layers.begin(), layers.end(), [](auto& p) {
                return std::strcmp(p.layerName, "VK_LAYER_KHRONOS_validation") == 0;
            });
        statistics.validation_enabled = validation;
        if (c.validation && !validation)
            std::cerr << "[Faset] Vulkan validation layer/debug-utils unavailable; validation "
                         "disabled.\n";
        if (debug_utils)
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "Faset Engine";
        app.apiVersion = VK_API_VERSION_1_3;
        VkDebugUtilsMessengerCreateInfoEXT debug_info{};
        debug_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debug_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        debug_info.pfnUserCallback = debug;
        debug_info.pUserData = this;
        const char* validation_name = "VK_LAYER_KHRONOS_validation";
        VkInstanceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        if (validation) {
            info.enabledLayerCount = 1;
            info.ppEnabledLayerNames = &validation_name;
            info.pNext = &debug_info;
        }
        check(vkCreateInstance(&info, nullptr, &instance), "Create Vulkan instance");
        if (validation) {
            auto fn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            if (fn)
                check(fn(instance, &debug_info, nullptr, &messenger),
                      "Create validation messenger");
        }
        if (window && !SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface))
            throw std::runtime_error(SDL_GetError());
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "Enumerate GPUs");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "Enumerate GPUs");
        int best = -1;
        for (auto gpu : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(gpu, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_3)
                continue;
            VkPhysicalDeviceVulkan13Features f13{};
            f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            VkPhysicalDeviceFeatures2 features{};
            features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features.pNext = &f13;
            vkGetPhysicalDeviceFeatures2(gpu, &features);
            if (!f13.synchronization2 || !f13.dynamicRendering)
                continue;
            VkFormatProperties color_props{}, depth_props{}, hzb_props{}, velocity_props{};
            vkGetPhysicalDeviceFormatProperties(gpu, VK_FORMAT_R8G8B8A8_UNORM, &color_props);
            vkGetPhysicalDeviceFormatProperties(gpu, VK_FORMAT_D32_SFLOAT, &depth_props);
            vkGetPhysicalDeviceFormatProperties(gpu, VK_FORMAT_R32_SFLOAT, &hzb_props);
            vkGetPhysicalDeviceFormatProperties(gpu, VK_FORMAT_R16G16B16A16_SFLOAT,
                                               &velocity_props);
            if (!(color_props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ||
                !(depth_props.optimalTilingFeatures &
                  VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ||
                !(depth_props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
                continue;
            std::uint32_t n{};
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, nullptr);
            std::vector<VkQueueFamilyProperties> queues(n);
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, queues.data());
            for (std::uint32_t i = 0; i < n; ++i) {
                VkBool32 supports = VK_TRUE;
                if (surface)
                    check(vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &supports),
                          "Query present support");
                int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                            : properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                                                                                              : 1;
                if (supports && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && score > best) {
                    best = score;
                    physical = gpu;
                    queue_family = i;
                    statistics.device = properties.deviceName;
                    timestamp_period = properties.limits.timestampPeriod;
                    timestamp_bits = queues[i].timestampValidBits;
                    max_compute_groups_x = properties.limits.maxComputeWorkGroupCount[0];
                    max_compute_groups_y = properties.limits.maxComputeWorkGroupCount[1];
                    max_storage_buffer_range = properties.limits.maxStorageBufferRange;
                    max_image_dimension = properties.limits.maxImageDimension2D;
                    independent_blend_supported = features.features.independentBlend;
                    light_tiles_capable =
                        (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0 &&
                        properties.limits.maxComputeWorkGroupInvocations >= 64 &&
                        properties.limits.maxComputeWorkGroupSize[0] >= 64 &&
                        properties.limits.maxPerStageDescriptorStorageBuffers >= 4 &&
                        properties.limits.maxDescriptorSetStorageBuffers >= 4 &&
                        max_compute_groups_x > 0;
                    scene.available = (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0 &&
                                      properties.limits.maxPerStageDescriptorStorageBuffers >= 8 &&
                                      properties.limits.maxDescriptorSetStorageBuffers >= 8 &&
                                      properties.limits.maxComputeWorkGroupInvocations >= 64 &&
                                      properties.limits.maxComputeWorkGroupSize[0] >= 64 &&
                                      max_compute_groups_x > 0;
                    scene.hzb_supported = scene.available &&
                        (hzb_props.optimalTilingFeatures &
                         (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                          VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                          VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                          VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) ==
                        (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                         VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                         VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                         VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    temporal.capabilities.compute =
                        (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0 &&
                        properties.limits.maxComputeWorkGroupInvocations >= 64 &&
                        properties.limits.maxComputeWorkGroupSize[0] >= 8 &&
                        properties.limits.maxComputeWorkGroupSize[1] >= 8 &&
                        properties.limits.maxPerStageDescriptorSampledImages >= 5 &&
                        properties.limits.maxPerStageDescriptorStorageImages >= 2;
                    const auto velocity_features =
                        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
                    temporal.capabilities.formats = independent_blend_supported &&
                        (color_props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
                        (velocity_props.optimalTilingFeatures & velocity_features) ==
                            velocity_features &&
                        (hzb_props.optimalTilingFeatures &
                         (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                          VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) ==
                            (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                             VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
                    temporal.capabilities.extent =
                        width <= max_image_dimension && height <= max_image_dimension &&
                        (width + 7) / 8 <= max_compute_groups_x &&
                        (height + 7) / 8 <= max_compute_groups_y;
                }
            }
        }
        if (!physical)
            throw std::runtime_error("No Vulkan 1.3 device supports dynamic rendering, "
                                     "synchronization2 and required color/depth formats");
        float priority = 1;
        VkDeviceQueueCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = queue_family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan13Features f13{};
        f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        f13.synchronization2 = VK_TRUE;
        f13.dynamicRendering = VK_TRUE;
        VkDeviceCreateInfo di{};
        di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        di.pNext = &f13;
        VkPhysicalDeviceFeatures enabled_features{};
        enabled_features.independentBlend = independent_blend_supported ? VK_TRUE : VK_FALSE;
        di.pEnabledFeatures = &enabled_features;
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &qi;
        const char* swap_extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        if (surface) {
            di.enabledExtensionCount = 1;
            di.ppEnabledExtensionNames = &swap_extension;
        }
        check(vkCreateDevice(physical, &di, nullptr, &device), "Create Vulkan device");
        vkGetDeviceQueue(device, queue_family, 0, &queue);
        if (debug_utils) {
            begin_gpu_label = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
                vkGetDeviceProcAddr(device, "vkCmdBeginDebugUtilsLabelEXT"));
            end_gpu_label = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
                vkGetDeviceProcAddr(device, "vkCmdEndDebugUtilsLabelEXT"));
        }
        statistics.gpu_labels_enabled = begin_gpu_label && end_gpu_label;
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.queueFamilyIndex = queue_family;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(device, &pi, nullptr, &pool), "Create command pool");
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &ai, &command), "Allocate command buffer");
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(vkCreateFence(device, &fi, nullptr, &fence), "Create frame fence");
        VkSemaphoreCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        check(vkCreateSemaphore(device, &si, nullptr, &acquired), "Create acquire semaphore");
        check(vkCreateSemaphore(device, &si, nullptr, &present_ready), "Create present semaphore");
        if (timestamp_bits) {
            VkQueryPoolCreateInfo query{};
            query.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            query.queryType = VK_QUERY_TYPE_TIMESTAMP;
            query.queryCount = timestamp_capacity;
            check(vkCreateQueryPool(device, &query, nullptr, &timestamp_pool),
                  "Create GPU timestamp queries");
        }
        const auto shadow_usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_SAMPLED_BIT;
        for (const auto size : {2048u, 1024u}) {
            if (size > max_image_dimension)
                continue;
            try {
                shadow = make_image(size, size, VK_FORMAT_D32_SFLOAT, shadow_usage,
                                    VK_IMAGE_ASPECT_DEPTH_BIT);
                sun_shadow_size = size;
                break;
            } catch (const std::exception&) {
                // Optional atlas allocation may fail; try the bounded half-size profile.
            }
        }
        if (!shadow.handle)
            shadow = make_image(1, 1, VK_FORMAT_D32_SFLOAT, shadow_usage,
                                VK_IMAGE_ASPECT_DEPTH_BIT);
        for (const auto size : {2048u, 1024u}) {
            if (size > max_image_dimension)
                continue;
            try {
                local_shadow = make_image(size, size, VK_FORMAT_D32_SFLOAT,
                                          shadow_usage, VK_IMAGE_ASPECT_DEPTH_BIT);
                local_shadow_size = size;
                break;
            } catch (const std::exception&) {
                // Local shadows are optional; all affected lights remain unshadowed.
            }
        }
        make_targets();
        light_tile_words = make_buffer(16,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        make_descriptors();
        if (light_tiles_capable) {
            try {
                make_light_tile_descriptors();
            } catch (const std::exception&) {
                destroy_light_tile_interfaces();
                light_tiles_capable = false;
            }
        }
        make_pipelines();
        if (scene.available && (c.visibility_mode != VisibilityMode::Direct ||
                                c.temporal_mode != TemporalMode::Off))
            make_scene_descriptors_and_pipelines();
        make_temporal_interfaces_and_pipelines();
        white = std::make_shared<Texture>();
        white->width = white->height = 1;
        white->rgba = {255, 255, 255, 255};
        upload_texture(white);
        if (surface)
            make_swapchain();
    }
    void make_targets() {
        check(vkDeviceWaitIdle(device), "Wait resize");
        destroy(color);
        destroy(depth);
        destroy(readback);
        destroy(scene.hzb[0]);
        destroy(scene.hzb[1]);
        destroy(temporal.scene_color);
        destroy(temporal.velocity);
        for (auto& image : temporal.history_color)
            destroy(image);
        for (auto& image : temporal.history_depth)
            destroy(image);
        destroy(temporal.pixel_counts);
        destroy(temporal.pixel_counts_stage);
        temporal.has_completed_image = false;
        scene.hzb_history_valid = false;
        temporal.capabilities.extent = width <= max_image_dimension &&
            height <= max_image_dimension && (width + 7) / 8 <= max_compute_groups_x &&
            (height + 7) / 8 <= max_compute_groups_y;
        const auto effective = select_effective_temporal_mode(config.temporal_mode,
                                                               temporal.capabilities);
        const auto internal = temporal_internal_extent(width, height, effective,
            effective == TemporalMode::Upscale ? config.render_scale : 1.f);
        temporal.internal_width = internal[0];
        temporal.internal_height = internal[1];
        color = make_image(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT);
        depth = make_image(internal[0], internal[1], VK_FORMAT_D32_SFLOAT,
                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT,
                           VK_IMAGE_ASPECT_DEPTH_BIT);
        if (effective != TemporalMode::Off) {
            temporal.scene_color = make_image(internal[0], internal[1],
                VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT);
            temporal.velocity = make_image(internal[0], internal[1],
                VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT);
            for (auto& image : temporal.history_color)
                image = make_image(width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT);
            for (auto& image : temporal.history_depth)
                image = make_image(width, height, VK_FORMAT_R32_SFLOAT,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT);
            temporal.pixel_counts = make_buffer(2 * sizeof(std::uint32_t),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            temporal.completed_index = 0;
        }
        const auto padded_width = std::bit_ceil(internal[0]);
        const auto padded_height = std::bit_ceil(internal[1]);
        scene.hzb_extent_supported = scene.hzb_supported &&
            padded_width <= max_image_dimension && padded_height <= max_image_dimension &&
            (padded_width + 7u) / 8u <= max_compute_groups_x;
        if (scene.hzb_extent_supported &&
            config.visibility_mode == VisibilityMode::GpuOcclusion) {
            scene.hzb_mips = std::bit_width(std::max(padded_width, padded_height));
            for (auto& pyramid : scene.hzb)
                pyramid = make_image(padded_width, padded_height, VK_FORMAT_R32_SFLOAT,
                                     VK_IMAGE_USAGE_SAMPLED_BIT |
                                         VK_IMAGE_USAGE_STORAGE_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                     VK_IMAGE_ASPECT_COLOR_BIT, scene.hzb_mips);
            scene.hzb_current = 0;
        } else {
            scene.hzb_mips = 0;
        }
        // CPU reads this allocation every frame. Prefer cached coherent memory when available.
        readback =
            make_buffer(VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        last_pixels.clear();
        if (scene.graphics_layout)
            refresh_scene_descriptors();
        if (temporal.resolve_layout && effective != TemporalMode::Off)
            refresh_temporal_descriptors();
    }
    void refresh_temporal_descriptors() {
        if (!temporal.resolve_layout || !temporal.scene_color.handle)
            return;
        if (temporal.descriptor_pool)
            vkDestroyDescriptorPool(device, temporal.descriptor_pool, nullptr);
        temporal.descriptor_pool = {};
        const std::array<VkDescriptorPoolSize, 3> sizes{{
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 12},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}}};
        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = 4;
        pool_info.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        pool_info.pPoolSizes = sizes.data();
        check(vkCreateDescriptorPool(device, &pool_info, nullptr, &temporal.descriptor_pool),
              "Create temporal descriptor pool");
        const std::array<VkDescriptorSetLayout, 4> layouts{
            temporal.resolve_layout, temporal.resolve_layout,
            temporal.composite_layout, temporal.composite_layout};
        VkDescriptorSetAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocation.descriptorPool = temporal.descriptor_pool;
        allocation.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
        allocation.pSetLayouts = layouts.data();
        std::array<VkDescriptorSet, 4> sets{};
        check(vkAllocateDescriptorSets(device, &allocation, sets.data()),
              "Allocate temporal descriptors");
        temporal.resolve_sets[0] = sets[0];
        temporal.resolve_sets[1] = sets[1];
        temporal.composite_sets[0] = sets[2];
        temporal.composite_sets[1] = sets[3];
        for (std::uint32_t next = 0; next < 2; ++next) {
            const std::array<VkDescriptorImageInfo, 7> images{{
                {VK_NULL_HANDLE, temporal.scene_color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {VK_NULL_HANDLE, depth.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                {VK_NULL_HANDLE, temporal.velocity.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {VK_NULL_HANDLE, temporal.history_color[1 - next].view,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {VK_NULL_HANDLE, temporal.history_depth[1 - next].view,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {VK_NULL_HANDLE, temporal.history_color[next].view, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, temporal.history_depth[next].view, VK_IMAGE_LAYOUT_GENERAL}}};
            std::array<VkWriteDescriptorSet, 9> writes{};
            for (std::uint32_t binding = 0; binding < 7; ++binding) {
                writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[binding].dstSet = temporal.resolve_sets[next];
                writes[binding].dstBinding = binding;
                writes[binding].descriptorCount = 1;
                writes[binding].descriptorType = binding < 5
                    ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                writes[binding].pImageInfo = &images[binding];
            }
            const VkDescriptorBufferInfo count_buffer{temporal.pixel_counts.handle, 0,
                                                       2 * sizeof(std::uint32_t)};
            writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[7].dstSet = temporal.resolve_sets[next];
            writes[7].dstBinding = 7;
            writes[7].descriptorCount = 1;
            writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[7].pBufferInfo = &count_buffer;
            writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[8].dstSet = temporal.composite_sets[next];
            writes[8].dstBinding = 0;
            writes[8].descriptorCount = 1;
            writes[8].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            const VkDescriptorImageInfo composite_image{
                VK_NULL_HANDLE, temporal.history_color[next].view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            writes[8].pImageInfo = &composite_image;
            vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        }
    }
    void make_swapchain() {
        if (!surface)
            return;
        int w{}, h{};
        SDL_GetWindowSizeInPixels(window, &w, &h);
        if (w <= 0 || h <= 0)
            return;
        check(vkDeviceWaitIdle(device), "Wait swapchain");
        VkSurfaceCapabilitiesKHR caps{};
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps),
              "Read surface capabilities");
        std::uint32_t count{};
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr),
              "Read surface formats");
        std::vector<VkSurfaceFormatKHR> formats(count);
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data()),
              "Read surface formats");
        if (formats.empty())
            throw std::runtime_error("Window surface has no formats");
        auto chosen = formats.front();
        for (auto f : formats)
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                chosen = f;
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physical, chosen.format, &properties);
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
            !(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
            throw std::runtime_error("Window surface does not support transfer presentation");
        swap_extent = caps.currentExtent;
        if (swap_extent.width == UINT32_MAX)
            swap_extent = {std::clamp(static_cast<std::uint32_t>(w), caps.minImageExtent.width,
                                      caps.maxImageExtent.width),
                           std::clamp(static_cast<std::uint32_t>(h), caps.minImageExtent.height,
                                      caps.maxImageExtent.height)};
        count = caps.minImageCount + 1;
        if (caps.maxImageCount)
            count = std::min(count, caps.maxImageCount);
        VkSwapchainCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface = surface;
        info.minImageCount = count;
        info.imageFormat = chosen.format;
        info.imageColorSpace = chosen.colorSpace;
        info.imageExtent = swap_extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if (!(caps.supportedCompositeAlpha & info.compositeAlpha)) {
            for (auto a :
                 {VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                  VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
                if (caps.supportedCompositeAlpha & a) {
                    info.compositeAlpha = a;
                    break;
                }
        }
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        info.oldSwapchain = swapchain;
        VkSwapchainKHR next{};
        check(vkCreateSwapchainKHR(device, &info, nullptr, &next), "Create swapchain");
        if (swapchain)
            vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = next;
        swap_format = chosen.format;
        check(vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr), "Get swapchain images");
        swap_images.resize(count);
        check(vkGetSwapchainImagesKHR(device, swapchain, &count, swap_images.data()),
              "Get swapchain images");
        swap_layouts.assign(count, VK_IMAGE_LAYOUT_UNDEFINED);
        dirty_swapchain = false;
        if (width != swap_extent.width || height != swap_extent.height) {
            width = swap_extent.width;
            height = swap_extent.height;
            make_targets();
        }
    }
    void make_descriptors() {
        std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
        for (std::uint32_t i = 0; i < 4; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType =
                i % 2 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 4;
        li.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(device, &li, nullptr, &descriptor_layout),
              "Create descriptor layout");
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2048},
                                        {VK_DESCRIPTOR_TYPE_SAMPLER, 2048}};
        VkDescriptorPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pi.maxSets = 1024;
        pi.poolSizeCount = 2;
        pi.pPoolSizes = sizes;
        check(vkCreateDescriptorPool(device, &pi, nullptr, &descriptor_pool),
              "Create descriptor pool");
        std::array<VkDescriptorSetLayoutBinding, 5> lighting_bindings{};
        for (std::uint32_t i = 0; i < lighting_bindings.size(); ++i)
            lighting_bindings[i] = {i,
                                    i == 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                           : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                    1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        li.bindingCount = static_cast<std::uint32_t>(lighting_bindings.size());
        li.pBindings = lighting_bindings.data();
        check(vkCreateDescriptorSetLayout(device, &li, nullptr, &lighting_layout),
              "Create lighting descriptor layout");
        VkDescriptorPoolSize lighting_sizes[] = {
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}};
        pi.flags = 0;
        pi.maxSets = 1;
        pi.poolSizeCount = 2;
        pi.pPoolSizes = lighting_sizes;
        check(vkCreateDescriptorPool(device, &pi, nullptr, &lighting_pool),
              "Create lighting descriptor pool");
        VkDescriptorSetAllocateInfo lighting_allocation{};
        lighting_allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        lighting_allocation.descriptorPool = lighting_pool;
        lighting_allocation.descriptorSetCount = 1;
        lighting_allocation.pSetLayouts = &lighting_layout;
        check(vkAllocateDescriptorSets(device, &lighting_allocation, &lighting_set),
              "Allocate lighting descriptors");
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0;
        check(vkCreateSampler(device, &si, nullptr, &shadow_sampler), "Create shadow sampler");
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        check(vkCreateSampler(device, &si, nullptr, &color_sampler), "Create color sampler");
    }
    void destroy_light_tile_interfaces() {
        if (!device)
            return;
        if (light_tile_pipeline)
            vkDestroyPipeline(device, light_tile_pipeline, nullptr);
        if (light_tile_pipeline_layout)
            vkDestroyPipelineLayout(device, light_tile_pipeline_layout, nullptr);
        if (light_tile_pool)
            vkDestroyDescriptorPool(device, light_tile_pool, nullptr);
        if (light_tile_layout)
            vkDestroyDescriptorSetLayout(device, light_tile_layout, nullptr);
        light_tile_pipeline = {};
        light_tile_pipeline_layout = {};
        light_tile_pool = {};
        light_tile_layout = {};
        light_tile_set = {};
    }
    void make_light_tile_descriptors() {
        const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
        VkDescriptorSetLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layout.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(device, &layout, nullptr, &light_tile_layout),
              "Create light tile descriptor layout");
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = 1;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &size;
        check(vkCreateDescriptorPool(device, &pool_info, nullptr, &light_tile_pool),
              "Create light tile descriptor pool");
        VkDescriptorSetAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocation.descriptorPool = light_tile_pool;
        allocation.descriptorSetCount = 1;
        allocation.pSetLayouts = &light_tile_layout;
        check(vkAllocateDescriptorSets(device, &allocation, &light_tile_set),
              "Allocate light tile descriptors");
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                 sizeof(LightTilePush)};
        VkPipelineLayoutCreateInfo pipeline{};
        pipeline.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline.setLayoutCount = 1;
        pipeline.pSetLayouts = &light_tile_layout;
        pipeline.pushConstantRangeCount = 1;
        pipeline.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(device, &pipeline, nullptr, &light_tile_pipeline_layout),
              "Create light tile pipeline layout");
    }
    VkDescriptorSet upload_texture(std::shared_ptr<const Texture> source) {
        if (!source)
            source = white;
        if (!source || !source->width || !source->height ||
            source->rgba.size() != std::size_t(source->width) * source->height * 4)
            throw std::invalid_argument("Texture requires width * height * 4 RGBA bytes");
        auto found = textures.find(source.get());
        if (found != textures.end() && found->second.revision == source->revision)
            return found->second.descriptor;
        check(vkDeviceWaitIdle(device), "Wait texture upload");
        GpuTexture texture{};
        texture.source = source;
        texture.revision = source->revision;
        texture.image =
            make_image(source->width, source->height,
                       source->srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
        Buffer staging{};
        try {
            staging = make_buffer(source->rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            void* mapped{};
            check(vkMapMemory(device, staging.memory, 0, staging.size, 0, &mapped),
                  "Map texture staging");
            std::memcpy(mapped, source->rgba.data(), source->rgba.size());
            vkUnmapMemory(device, staging.memory);
            begin();
            transition(command, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_ASPECT_COLOR_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {source->width, source->height, 1};
            vkCmdCopyBufferToImage(command, staging.handle, texture.image.handle,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            transition(command, texture.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_IMAGE_ASPECT_COLOR_BIT);
            submit();
            destroy(staging);
            VkDescriptorSetAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = descriptor_pool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &descriptor_layout;
            check(vkAllocateDescriptorSets(device, &ai, &texture.descriptor),
                  "Allocate texture descriptor");
            VkDescriptorImageInfo images[] = {
                {VK_NULL_HANDLE, shadow.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                {shadow_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED},
                {VK_NULL_HANDLE, texture.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {color_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED}};
            std::array<VkWriteDescriptorSet, 4> writes{};
            for (std::uint32_t i = 0; i < 4; ++i) {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = texture.descriptor;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType =
                    i % 2 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                writes[i].pImageInfo = &images[i];
            }
            vkUpdateDescriptorSets(device, 4, writes.data(), 0, nullptr);
        } catch (...) {
            destroy(staging);
            destroy(texture.image);
            throw;
        }
        if (found != textures.end()) {
            destroy(found->second.image);
            vkFreeDescriptorSets(device, descriptor_pool, 1, &found->second.descriptor);
            found->second = std::move(texture);
            return found->second.descriptor;
        }
        auto [inserted, _] = textures.emplace(source.get(), std::move(texture));
        return inserted->second.descriptor;
    }
    std::filesystem::path shader_directory() const {
        if (!config.shader_directory.empty())
            return config.shader_directory;
        std::vector<std::filesystem::path> roots;
        const char* base = SDL_GetBasePath();
        if (base)
            roots.emplace_back(faset::path_from_utf8(base) / "shaders");
        roots.emplace_back(std::filesystem::current_path() / "shaders");
        roots.emplace_back(faset::path_from_utf8(FASET_SHADER_DIRECTORY));
        for (const auto& root : roots)
            if (std::filesystem::is_regular_file(faset::native_io_path(root / "vertexMain.spv")))
                return root;
        throw std::runtime_error("Compiled Slang shader bundle is missing");
    }
    VkShaderModule shader(const detail::ShaderCode& code) {
        VkShaderModuleCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = code.words.size() * sizeof(std::uint32_t);
        ci.pCode = code.words.data();
        VkShaderModule result{};
        check(vkCreateShaderModule(device, &ci, nullptr, &result), "Create shader module");
        return result;
    }
    void make_pipelines() {
        const auto shaders = detail::load_shader_bundle(shader_directory());
        for (std::size_t i = 0; i < shaders.size(); ++i)
            if (!shader_layouts[i].empty() && shader_layouts[i] != shaders[i].layout_fingerprint)
                throw std::runtime_error(
                    "Shader layout changed; the current pipeline was preserved");
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                 sizeof(Push)};
        VkPipelineLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        const std::array<VkDescriptorSetLayout, 2> set_layouts{descriptor_layout, lighting_layout};
        li.setLayoutCount = static_cast<std::uint32_t>(set_layouts.size());
        li.pSetLayouts = set_layouts.data();
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(device, &li, nullptr, &pipeline_layout),
              "Create pipeline layout");
        VkShaderModule vertex{}, fragment{}, shadow_vertex{};
        try {
            vertex = shader(shaders[0]);
            fragment = shader(shaders[1]);
            shadow_vertex = shader(shaders[2]);
            for (int mode = 0; mode < 5; ++mode) {
                bool shadow_pass = mode == 2, ui = mode == 1 || mode == 4,
                     sprite = mode == 3, temporal_ui = mode == 4;
                VkPipelineShaderStageCreateInfo stages[2]{};
                stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
                stages[0].module = shadow_pass ? shadow_vertex : vertex;
                stages[0].pName = "main";
                stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
                stages[1].module = fragment;
                stages[1].pName = "main";
                VkVertexInputBindingDescription binding{0, sizeof(GpuVertex),
                                                        VK_VERTEX_INPUT_RATE_VERTEX};
                VkVertexInputAttributeDescription attrs[] = {
                    {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, clip)},
                    {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, world)},
                    {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, normal)},
                    {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, color)},
                    {4, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GpuVertex, material)},
                    {5, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GpuVertex, uv)}};
                VkPipelineVertexInputStateCreateInfo vi{};
                vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
                vi.vertexBindingDescriptionCount = 1;
                vi.pVertexBindingDescriptions = &binding;
                vi.vertexAttributeDescriptionCount = shadow_pass ? 1 : 6;
                vi.pVertexAttributeDescriptions = shadow_pass ? attrs + 1 : attrs;
                VkPipelineInputAssemblyStateCreateInfo ia{};
                ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
                ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                VkPipelineViewportStateCreateInfo vp{};
                vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
                vp.viewportCount = vp.scissorCount = 1;
                VkPipelineRasterizationStateCreateInfo rs{};
                rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
                rs.polygonMode = VK_POLYGON_MODE_FILL;
                rs.cullMode = VK_CULL_MODE_NONE;
                rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
                rs.lineWidth = 1;
                rs.depthBiasEnable = shadow_pass;
                rs.depthBiasConstantFactor = 1.25f;
                rs.depthBiasSlopeFactor = 1.75f;
                VkPipelineMultisampleStateCreateInfo ms{};
                ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
                ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
                VkPipelineDepthStencilStateCreateInfo ds{};
                ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
                ds.depthTestEnable = !ui;
                ds.depthWriteEnable = !ui && !sprite;
                ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
                VkPipelineColorBlendAttachmentState blend{};
                blend.colorWriteMask = 15;
                blend.blendEnable = VK_TRUE;
                blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                blend.colorBlendOp = VK_BLEND_OP_ADD;
                blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                blend.alphaBlendOp = VK_BLEND_OP_ADD;
                VkPipelineColorBlendStateCreateInfo cb{};
                cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
                cb.attachmentCount = shadow_pass ? 0 : 1;
                cb.pAttachments = &blend;
                VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
                VkPipelineDynamicStateCreateInfo dynamic{};
                dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
                dynamic.dynamicStateCount = 2;
                dynamic.pDynamicStates = states;
                VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
                VkPipelineRenderingCreateInfo rendering{};
                rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
                rendering.colorAttachmentCount = shadow_pass ? 0 : 1;
                rendering.pColorAttachmentFormats = &format;
                rendering.depthAttachmentFormat = temporal_ui ? VK_FORMAT_UNDEFINED
                                                                : VK_FORMAT_D32_SFLOAT;
                VkGraphicsPipelineCreateInfo pi{};
                pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
                pi.pNext = &rendering;
                pi.stageCount = shadow_pass ? 1 : 2;
                pi.pStages = stages;
                pi.pVertexInputState = &vi;
                pi.pInputAssemblyState = &ia;
                pi.pViewportState = &vp;
                pi.pRasterizationState = &rs;
                pi.pMultisampleState = &ms;
                pi.pDepthStencilState = &ds;
                pi.pColorBlendState = &cb;
                pi.pDynamicState = &dynamic;
                pi.layout = pipeline_layout;
                auto* output = temporal_ui ? &temporal_ui_pipeline
                               : shadow_pass ? &shadow_pipeline
                               : ui        ? &ui_pipeline
                               : sprite    ? &sprite_pipeline
                                           : &pipeline;
                check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, output),
                      "Create graphics pipeline");
            }
            if (light_tiles_capable) {
                VkShaderModule tile_shader{};
                try {
                    tile_shader = shader(shaders[3]);
                    VkComputePipelineCreateInfo tile{};
                    tile.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
                    tile.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    tile.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                    tile.stage.module = tile_shader;
                    tile.stage.pName = "main";
                    tile.layout = light_tile_pipeline_layout;
                    check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &tile,
                                                   nullptr, &light_tile_pipeline),
                          "Create light tile compute pipeline");
                } catch (const std::exception&) {
                    // Optional acceleration: keep the validated forward renderer.
                    if (light_tile_pipeline)
                        vkDestroyPipeline(device, light_tile_pipeline, nullptr);
                    light_tile_pipeline = {};
                    light_tiles_capable = false;
                }
                if (tile_shader)
                    vkDestroyShaderModule(device, tile_shader, nullptr);
            }
        } catch (...) {
            vkDestroyShaderModule(device, vertex, nullptr);
            vkDestroyShaderModule(device, fragment, nullptr);
            vkDestroyShaderModule(device, shadow_vertex, nullptr);
            throw;
        }
        for (std::size_t i = 0; i < shaders.size(); ++i)
            shader_layouts[i] = shaders[i].layout_fingerprint;
        vkDestroyShaderModule(device, vertex, nullptr);
        vkDestroyShaderModule(device, fragment, nullptr);
        vkDestroyShaderModule(device, shadow_vertex, nullptr);
    }
    void refresh_scene_descriptors() {
        if (!scene.graphics_layout)
            return;
        if (scene.descriptor_pool)
            vkDestroyDescriptorPool(device, scene.descriptor_pool, nullptr);
        scene.graphics_main = scene.graphics_post = scene.cull_main = scene.cull_post = {};
        for (auto& sets : scene.hzb_sets)
            sets.clear();
        const std::uint32_t hzb_sets = scene.hzb_mips * 2;
        const std::array<VkDescriptorPoolSize, 3> sizes{{
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16 + hzb_sets},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, hzb_sets}}};
        VkDescriptorPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets = 4 + hzb_sets;
        pool.poolSizeCount = hzb_sets ? static_cast<std::uint32_t>(sizes.size()) : 2;
        pool.pPoolSizes = sizes.data();
        check(vkCreateDescriptorPool(device, &pool, nullptr, &scene.descriptor_pool),
              "Create GPU visibility descriptor pool");
        const std::array<VkDescriptorSetLayout, 4> layouts{
            scene.graphics_layout, scene.graphics_layout, scene.cull_layout, scene.cull_layout};
        VkDescriptorSetAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocation.descriptorPool = scene.descriptor_pool;
        allocation.descriptorSetCount = 4;
        allocation.pSetLayouts = layouts.data();
        std::array<VkDescriptorSet, 4> sets{};
        check(vkAllocateDescriptorSets(device, &allocation, sets.data()),
              "Allocate GPU visibility descriptors");
        scene.graphics_main = sets[0];
        scene.graphics_post = sets[1];
        scene.cull_main = sets[2];
        scene.cull_post = sets[3];
        if (!scene.hzb_mips)
            return;
        for (std::uint32_t image = 0; image < 2; ++image) {
            auto& target_sets = scene.hzb_sets[image];
            target_sets.resize(scene.hzb_mips);
            std::vector<VkDescriptorSetLayout> mip_layouts(scene.hzb_mips, scene.hzb_layout);
            allocation.descriptorSetCount = scene.hzb_mips;
            allocation.pSetLayouts = mip_layouts.data();
            check(vkAllocateDescriptorSets(device, &allocation, target_sets.data()),
                  "Allocate HZB mip descriptors");
            for (std::uint32_t mip = 0; mip < scene.hzb_mips; ++mip) {
                const auto source = mip == 0 ? depth.view : scene.hzb[image].mip_views[mip - 1];
                const auto output = scene.hzb[image].mip_views.empty()
                    ? scene.hzb[image].view : scene.hzb[image].mip_views[mip];
                VkDescriptorImageInfo images[] = {
                    {VK_NULL_HANDLE, source,
                     mip == 0 ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
                              : VK_IMAGE_LAYOUT_GENERAL},
                    {VK_NULL_HANDLE, output, VK_IMAGE_LAYOUT_GENERAL}};
                VkWriteDescriptorSet writes[2]{};
                for (std::uint32_t binding = 0; binding < 2; ++binding) {
                    writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[binding].dstSet = target_sets[mip];
                    writes[binding].dstBinding = binding;
                    writes[binding].descriptorCount = 1;
                    writes[binding].descriptorType = binding == 0
                        ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    writes[binding].pImageInfo = &images[binding];
                }
                vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
            }
        }
    }
    void make_scene_descriptors_and_pipelines() {
        std::array<VkDescriptorSetLayoutBinding, 3> graphics{};
        for (std::uint32_t i = 0; i < graphics.size(); ++i)
            graphics[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT,
                           nullptr};
        VkDescriptorSetLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout.bindingCount = static_cast<std::uint32_t>(graphics.size());
        layout.pBindings = graphics.data();
        check(vkCreateDescriptorSetLayout(device, &layout, nullptr, &scene.graphics_layout),
              "Create GPU scene descriptor layout");
        std::array<VkDescriptorSetLayoutBinding, 10> cull{};
        for (std::uint32_t i = 0; i < cull.size(); ++i)
            cull[i] = {i, i == 7 || i == 8 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                            : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                       1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        layout.bindingCount = static_cast<std::uint32_t>(cull.size());
        layout.pBindings = cull.data();
        check(vkCreateDescriptorSetLayout(device, &layout, nullptr, &scene.cull_layout),
              "Create GPU cull descriptor layout");
        std::array<VkDescriptorSetLayoutBinding, 2> hzb{{
            {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
        layout.bindingCount = static_cast<std::uint32_t>(hzb.size());
        layout.pBindings = hzb.data();
        check(vkCreateDescriptorSetLayout(device, &layout, nullptr, &scene.hzb_layout),
              "Create HZB descriptor layout");
        const std::array<VkDescriptorSetLayout, 3> scene_layouts{
            descriptor_layout, lighting_layout, scene.graphics_layout};
        VkPushConstantRange graphics_push{VK_SHADER_STAGE_VERTEX_BIT |
                                              VK_SHADER_STAGE_FRAGMENT_BIT,
                                          0, sizeof(ScenePush)};
        VkPipelineLayoutCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_info.setLayoutCount = static_cast<std::uint32_t>(scene_layouts.size());
        pipeline_info.pSetLayouts = scene_layouts.data();
        pipeline_info.pushConstantRangeCount = 1;
        pipeline_info.pPushConstantRanges = &graphics_push;
        check(vkCreatePipelineLayout(device, &pipeline_info, nullptr,
                                     &scene.graphics_pipeline_layout),
              "Create GPU scene pipeline layout");
        VkPushConstantRange compute_push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        pipeline_info.setLayoutCount = 1;
        pipeline_info.pSetLayouts = &scene.cull_layout;
        pipeline_info.pPushConstantRanges = &compute_push;
        check(vkCreatePipelineLayout(device, &pipeline_info, nullptr,
                                     &scene.cull_pipeline_layout),
              "Create GPU cull pipeline layout");
        pipeline_info.pSetLayouts = &scene.hzb_layout;
        check(vkCreatePipelineLayout(device, &pipeline_info, nullptr,
                                     &scene.hzb_pipeline_layout),
              "Create HZB pipeline layout");
        make_scene_pipelines();
        refresh_scene_descriptors();
    }
    void make_scene_pipelines() {
        const auto gpu_shaders = detail::load_gpu_shader_bundle(shader_directory());
        const auto baseline_shaders = detail::load_shader_bundle(shader_directory());
        for (std::size_t i = 0; i < gpu_shaders.size(); ++i)
            if (!scene.shader_layouts[i].empty() &&
                scene.shader_layouts[i] != gpu_shaders[i].layout_fingerprint)
                throw std::runtime_error("GPU shader layout changed; current pipeline preserved");
        VkShaderModule vertex = shader(gpu_shaders[0]);
        VkShaderModule fragment = shader(baseline_shaders[1]);
        VkShaderModule main_cull = shader(gpu_shaders[2]);
        VkShaderModule hzb_shader = scene.hzb_supported ? shader(gpu_shaders[3])
                                                        : VK_NULL_HANDLE;
        VkShaderModule post_cull = shader(gpu_shaders[4]);
        try {
            VkPipelineShaderStageCreateInfo stages[2]{};
            for (auto& stage : stages)
                stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = vertex;
            stages[0].pName = "main";
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = fragment;
            stages[1].pName = "main";
            VkVertexInputBindingDescription binding{0, sizeof(SceneVertex),
                                                    VK_VERTEX_INPUT_RATE_VERTEX};
            const std::array<VkVertexInputAttributeDescription, 4> attributes{{
                {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SceneVertex, position)},
                {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SceneVertex, normal)},
                {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SceneVertex, color)},
                {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(SceneVertex, uv)}}};
            VkPipelineVertexInputStateCreateInfo input{};
            input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            input.vertexBindingDescriptionCount = 1;
            input.pVertexBindingDescriptions = &binding;
            input.vertexAttributeDescriptionCount = attributes.size();
            input.pVertexAttributeDescriptions = attributes.data();
            VkPipelineInputAssemblyStateCreateInfo assembly{};
            assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport{};
            viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewport.viewportCount = viewport.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo raster{};
            raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.cullMode = VK_CULL_MODE_NONE;
            raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            raster.lineWidth = 1;
            VkPipelineMultisampleStateCreateInfo samples{};
            samples.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo depth_state{};
            depth_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            depth_state.depthTestEnable = VK_TRUE;
            depth_state.depthWriteEnable = VK_TRUE;
            depth_state.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
            VkPipelineColorBlendAttachmentState blend{};
            blend.colorWriteMask = 15;
            blend.blendEnable = VK_TRUE;
            blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            VkPipelineColorBlendStateCreateInfo color_blend{};
            color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            color_blend.attachmentCount = 1;
            color_blend.pAttachments = &blend;
            const std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                               VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamic{};
            dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamic.dynamicStateCount = dynamic_states.size();
            dynamic.pDynamicStates = dynamic_states.data();
            const VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
            VkPipelineRenderingCreateInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachmentFormats = &color_format;
            rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
            VkGraphicsPipelineCreateInfo graphics_pipeline{};
            graphics_pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            graphics_pipeline.pNext = &rendering;
            graphics_pipeline.stageCount = 2;
            graphics_pipeline.pStages = stages;
            graphics_pipeline.pVertexInputState = &input;
            graphics_pipeline.pInputAssemblyState = &assembly;
            graphics_pipeline.pViewportState = &viewport;
            graphics_pipeline.pRasterizationState = &raster;
            graphics_pipeline.pMultisampleState = &samples;
            graphics_pipeline.pDepthStencilState = &depth_state;
            graphics_pipeline.pColorBlendState = &color_blend;
            graphics_pipeline.pDynamicState = &dynamic;
            graphics_pipeline.layout = scene.graphics_pipeline_layout;
            check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphics_pipeline,
                                            nullptr, &scene.graphics_pipeline),
                  "Create GPU scene pipeline");
            auto make_compute = [&](VkShaderModule module, VkPipelineLayout layout,
                                    VkPipeline& output) {
                VkComputePipelineCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
                info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                info.stage.module = module;
                info.stage.pName = "main";
                info.layout = layout;
                check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr,
                                               &output), "Create GPU visibility compute pipeline");
            };
            make_compute(main_cull, scene.cull_pipeline_layout, scene.cull_pipeline);
            make_compute(post_cull, scene.cull_pipeline_layout, scene.post_pipeline);
            if (scene.hzb_supported)
                make_compute(hzb_shader, scene.hzb_pipeline_layout, scene.hzb_pipeline);
            for (std::size_t i = 0; i < gpu_shaders.size(); ++i)
                scene.shader_layouts[i] = gpu_shaders[i].layout_fingerprint;
        } catch (...) {
            for (auto module : {vertex, fragment, main_cull, hzb_shader, post_cull})
                vkDestroyShaderModule(device, module, nullptr);
            throw;
        }
        for (auto module : {vertex, fragment, main_cull, hzb_shader, post_cull})
            vkDestroyShaderModule(device, module, nullptr);
    }
    void make_temporal_interfaces_and_pipelines() {
        // Validate the complete shader package even on devices that fall back to Off.
        const auto post = detail::load_temporal_shader_bundle(shader_directory());
        const auto raster = detail::load_temporal_scene_shader_bundle(shader_directory());
        for (std::size_t i = 0; i < post.size(); ++i)
            if (!temporal.shader_layouts[i].empty() &&
                temporal.shader_layouts[i] != post[i].layout_fingerprint)
                throw std::runtime_error("Temporal shader layout changed; active pipeline preserved");
        for (std::size_t i = 0; i < raster.size(); ++i)
            if (!temporal.scene_shader_layouts[i].empty() &&
                temporal.scene_shader_layouts[i] != raster[i].layout_fingerprint)
                throw std::runtime_error("Temporal scene layout changed; active pipeline preserved");
        if (!temporal.capabilities.compute || !temporal.capabilities.formats)
            return;
        if (!temporal.resolve_layout) {
            std::array<VkDescriptorSetLayoutBinding, 8> bindings{};
            for (std::uint32_t i = 0; i < bindings.size(); ++i)
                bindings[i] = {i, i < 5 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                        : i < 7 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                               1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo descriptor_info{};
            descriptor_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            descriptor_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
            descriptor_info.pBindings = bindings.data();
            check(vkCreateDescriptorSetLayout(device, &descriptor_info, nullptr,
                                              &temporal.resolve_layout),
                  "Create temporal resolve descriptor layout");
            bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            descriptor_info.bindingCount = 1;
            check(vkCreateDescriptorSetLayout(device, &descriptor_info, nullptr,
                                              &temporal.composite_layout),
                  "Create temporal composite descriptor layout");
            VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 80};
            VkPipelineLayoutCreateInfo layout_info{};
            layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            layout_info.setLayoutCount = 1;
            layout_info.pSetLayouts = &temporal.resolve_layout;
            layout_info.pushConstantRangeCount = 1;
            layout_info.pPushConstantRanges = &push;
            check(vkCreatePipelineLayout(device, &layout_info, nullptr,
                                         &temporal.resolve_pipeline_layout),
                  "Create temporal resolve pipeline layout");
            layout_info.pSetLayouts = &temporal.composite_layout;
            layout_info.pushConstantRangeCount = 0;
            check(vkCreatePipelineLayout(device, &layout_info, nullptr,
                                         &temporal.composite_pipeline_layout),
                  "Create temporal composite pipeline layout");
        }
        std::array<VkShaderModule, 6> modules{};
        try {
            modules[0] = shader(post[0]);
            modules[1] = shader(post[1]);
            modules[2] = shader(post[2]);
            modules[3] = shader(raster[0]);
            modules[4] = shader(raster[1]);
            modules[5] = shader(raster[2]);
            VkComputePipelineCreateInfo compute{};
            compute.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            compute.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            compute.stage.module = modules[0];
            compute.stage.pName = "main";
            compute.layout = temporal.resolve_pipeline_layout;
            check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &compute, nullptr,
                                           &temporal.resolve_pipeline),
                  "Create temporal resolve pipeline");
            const auto make_graphics = [&](VkShaderModule vertex, VkShaderModule fragment,
                                           VkPipelineLayout layout, bool gpu, bool composite,
                                           bool depth_write, VkPipeline& output) {
                std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
                for (auto& stage : stages) {
                    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    stage.pName = "main";
                }
                stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
                stages[0].module = vertex;
                stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
                stages[1].module = fragment;
                const VkVertexInputBindingDescription binding{
                    0, static_cast<std::uint32_t>(gpu ? sizeof(SceneVertex)
                                                      : sizeof(GpuVertex)),
                    VK_VERTEX_INPUT_RATE_VERTEX};
                const std::array<VkVertexInputAttributeDescription, 8> direct_attrs{{
                    {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, clip)},
                    {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, world)},
                    {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, normal)},
                    {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, color)},
                    {4, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GpuVertex, material)},
                    {5, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GpuVertex, uv)},
                    {6, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                     offsetof(GpuVertex, previous_clip)},
                    {7, 0, VK_FORMAT_R32_SFLOAT, offsetof(GpuVertex, motion_valid)}}};
                const std::array<VkVertexInputAttributeDescription, 4> gpu_attrs{{
                    {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SceneVertex, position)},
                    {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SceneVertex, normal)},
                    {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SceneVertex, color)},
                    {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(SceneVertex, uv)}}};
                VkPipelineVertexInputStateCreateInfo input{};
                input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
                input.vertexBindingDescriptionCount = 1;
                input.pVertexBindingDescriptions = &binding;
                input.vertexAttributeDescriptionCount = composite ? 1
                    : gpu ? static_cast<std::uint32_t>(gpu_attrs.size())
                          : static_cast<std::uint32_t>(direct_attrs.size());
                input.pVertexAttributeDescriptions = gpu ? gpu_attrs.data()
                                                             : direct_attrs.data();
                VkPipelineInputAssemblyStateCreateInfo assembly{};
                assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
                assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                VkPipelineViewportStateCreateInfo viewport{};
                viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
                viewport.viewportCount = viewport.scissorCount = 1;
                VkPipelineRasterizationStateCreateInfo raster_state{};
                raster_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
                raster_state.polygonMode = VK_POLYGON_MODE_FILL;
                raster_state.cullMode = VK_CULL_MODE_NONE;
                raster_state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
                raster_state.lineWidth = 1;
                VkPipelineMultisampleStateCreateInfo samples{};
                samples.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
                samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
                VkPipelineDepthStencilStateCreateInfo depth_state{};
                depth_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
                depth_state.depthTestEnable = !composite;
                depth_state.depthWriteEnable = !composite && depth_write;
                depth_state.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
                VkPipelineColorBlendAttachmentState color_blend{};
                color_blend.colorWriteMask = 15;
                color_blend.blendEnable = !composite;
                color_blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                color_blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                color_blend.colorBlendOp = VK_BLEND_OP_ADD;
                color_blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                color_blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                color_blend.alphaBlendOp = VK_BLEND_OP_ADD;
                VkPipelineColorBlendAttachmentState velocity_blend{};
                velocity_blend.colorWriteMask = 15;
                const std::array<VkPipelineColorBlendAttachmentState, 2> blends{
                    color_blend, velocity_blend};
                VkPipelineColorBlendStateCreateInfo blend_state{};
                blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
                blend_state.attachmentCount = composite ? 1 : 2;
                blend_state.pAttachments = blends.data();
                const std::array<VkDynamicState, 2> states{VK_DYNAMIC_STATE_VIEWPORT,
                                                            VK_DYNAMIC_STATE_SCISSOR};
                VkPipelineDynamicStateCreateInfo dynamic{};
                dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
                dynamic.dynamicStateCount = static_cast<std::uint32_t>(states.size());
                dynamic.pDynamicStates = states.data();
                const std::array<VkFormat, 2> formats{VK_FORMAT_R8G8B8A8_UNORM,
                                                       VK_FORMAT_R16G16B16A16_SFLOAT};
                VkPipelineRenderingCreateInfo rendering{};
                rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
                rendering.colorAttachmentCount = composite ? 1 : 2;
                rendering.pColorAttachmentFormats = formats.data();
                rendering.depthAttachmentFormat = composite ? VK_FORMAT_UNDEFINED
                                                           : VK_FORMAT_D32_SFLOAT;
                VkGraphicsPipelineCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
                info.pNext = &rendering;
                info.stageCount = 2;
                info.pStages = stages.data();
                info.pVertexInputState = &input;
                info.pInputAssemblyState = &assembly;
                info.pViewportState = &viewport;
                info.pRasterizationState = &raster_state;
                info.pMultisampleState = &samples;
                info.pDepthStencilState = &depth_state;
                info.pColorBlendState = &blend_state;
                info.pDynamicState = &dynamic;
                info.layout = layout;
                check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info,
                                                nullptr, &output),
                      "Create temporal graphics pipeline");
            };
            make_graphics(modules[1], modules[2], temporal.composite_pipeline_layout,
                          false, true, false, temporal.composite_pipeline);
            make_graphics(modules[3], modules[4], pipeline_layout, false, false, true,
                          temporal.direct_pipeline);
            make_graphics(modules[3], modules[4], pipeline_layout, false, false, false,
                          temporal.transparent_pipeline);
            if (scene.graphics_pipeline_layout)
                make_graphics(modules[5], modules[4], scene.graphics_pipeline_layout,
                              true, false, true, temporal.gpu_pipeline);
            for (std::size_t i = 0; i < post.size(); ++i)
                temporal.shader_layouts[i] = post[i].layout_fingerprint;
            for (std::size_t i = 0; i < raster.size(); ++i)
                temporal.scene_shader_layouts[i] = raster[i].layout_fingerprint;
        } catch (...) {
            for (auto module : modules)
                if (module)
                    vkDestroyShaderModule(device, module, nullptr);
            throw;
        }
        for (auto module : modules)
            vkDestroyShaderModule(device, module, nullptr);
        if (!temporal.descriptor_pool)
            refresh_temporal_descriptors();
    }
    GpuVertex gpu_vertex(const Vertex& v, const DrawItem& item, const Mat4& vp,
                         const Mat4* previous_model = nullptr,
                         const Mat4* previous_vp = nullptr) {
        GpuVertex out{};
        auto world = point(item.model, {v.position[0], v.position[1], v.position[2], 1});
        auto clip = point(vp, world);
        std::copy(clip.begin(), clip.end(), out.clip);
        if (previous_model && previous_vp) {
            const auto previous_world = point(*previous_model,
                                               {v.position[0], v.position[1], v.position[2], 1});
            const auto previous_clip = point(*previous_vp, previous_world);
            std::copy(previous_clip.begin(), previous_clip.end(), out.previous_clip);
            out.motion_valid = 1.f;
        }
        std::copy_n(world.begin(), 3, out.world);
        // Inverse-transpose 3x3, including nonuniform scale. Singular models have no valid normal.
        const auto& m = item.model;
        Vec3 a{m[0], m[1], m[2]}, b{m[4], m[5], m[6]}, c{m[8], m[9], m[10]};
        auto cross = [](Vec3 x, Vec3 y) {
            return Vec3{x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2],
                        x[0] * y[1] - x[1] * y[0]};
        };
        auto ca = cross(b, c), cb = cross(c, a), cc = cross(a, b);
        float determinant = a[0] * ca[0] + a[1] * ca[1] + a[2] * ca[2];
        for (int i = 0; i < 3; ++i)
            out.normal[i] =
                determinant != 0
                    ? (ca[i] * v.normal[0] + cb[i] * v.normal[1] + cc[i] * v.normal[2]) *
                          (determinant < 0 ? -1.f : 1.f)
                    : 0;
        float normal_length = std::hypot(out.normal[0], out.normal[1], out.normal[2]);
        if (normal_length > 0)
            for (float& component : out.normal)
                component /= normal_length;
        for (int i = 0; i < 4; ++i)
            out.color[i] = item.color[i] * v.color[i];
        out.material[0] = item.roughness;
        out.material[1] = item.metallic;
        out.uv[0] = v.uv[0];
        out.uv[1] = v.uv[1];
        return out;
    }
    bool outside(const std::vector<GpuVertex>& data, std::size_t start) const {
        for (int plane = 0; plane < 6; ++plane) {
            bool all = true;
            for (std::size_t i = start; i < data.size(); ++i) {
                auto& p = data[i].clip;
                float d = plane == 0   ? p[0] + p[3]
                          : plane == 1 ? p[3] - p[0]
                          : plane == 2 ? p[1] + p[3]
                          : plane == 3 ? p[3] - p[1]
                          : plane == 4 ? p[2]
                                       : p[3] - p[2];
                if (d >= 0) {
                    all = false;
                    break;
                }
            }
            if (all)
                return true;
        }
        return false;
    }
    void quad(std::vector<GpuVertex>& data, const Quad& q) {
        const float xy[4][2] = {{q.x, q.y},
                                {q.x + q.width, q.y},
                                {q.x + q.width, q.y + q.height},
                                {q.x, q.y + q.height}};
        const float uv[4][2] = {{q.uv_rect[0], q.uv_rect[1]},
                                {q.uv_rect[2], q.uv_rect[1]},
                                {q.uv_rect[2], q.uv_rect[3]},
                                {q.uv_rect[0], q.uv_rect[3]}};
        for (auto i : {0, 1, 2, 0, 2, 3}) {
            GpuVertex v{};
            v.clip[0] = xy[i][0] / float(width) * 2 - 1;
            v.clip[1] = xy[i][1] / float(height) * 2 - 1;
            v.clip[3] = 1;
            std::copy(q.color.begin(), q.color.end(), v.color);
            v.uv[0] = uv[i][0];
            v.uv[1] = uv[i][1];
            v.material[0] = q.texture && q.texture->srgb ? 1.f : 0.f;
            data.push_back(v);
        }
    }
    void draw_debug_text(std::vector<GpuVertex>& data, const Text& text) {
        // Small diagnostic alphabet only. The editor supplies shaped Unicode text as texture quads.
        static const std::unordered_map<char, std::array<unsigned char, 7>> glyphs = {
            {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}},
            {'C', {14, 17, 16, 16, 16, 17, 14}}, {'D', {30, 17, 17, 17, 17, 17, 30}},
            {'E', {31, 16, 16, 30, 16, 16, 31}}, {'F', {31, 16, 16, 30, 16, 16, 16}},
            {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}},
            {'I', {14, 4, 4, 4, 4, 4, 14}},      {'J', {7, 2, 2, 2, 18, 18, 12}},
            {'K', {17, 18, 20, 24, 20, 18, 17}}, {'L', {16, 16, 16, 16, 16, 16, 31}},
            {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 25, 21, 19, 17, 17, 17}},
            {'O', {14, 17, 17, 17, 17, 17, 14}}, {'P', {30, 17, 17, 30, 16, 16, 16}},
            {'Q', {14, 17, 17, 17, 21, 18, 13}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
            {'S', {15, 16, 16, 14, 1, 1, 30}},   {'T', {31, 4, 4, 4, 4, 4, 4}},
            {'U', {17, 17, 17, 17, 17, 17, 14}}, {'V', {17, 17, 17, 17, 17, 10, 4}},
            {'W', {17, 17, 17, 21, 21, 27, 17}}, {'X', {17, 17, 10, 4, 10, 17, 17}},
            {'Y', {17, 17, 10, 4, 4, 4, 4}},     {'Z', {31, 1, 2, 4, 8, 16, 31}},
            {'0', {14, 17, 19, 21, 25, 17, 14}}, {'1', {4, 12, 4, 4, 4, 4, 14}},
            {'2', {14, 17, 1, 2, 4, 8, 31}},     {'3', {30, 1, 1, 14, 1, 1, 30}},
            {'4', {2, 6, 10, 18, 31, 2, 2}},     {'5', {31, 16, 16, 30, 1, 1, 30}},
            {'6', {14, 16, 16, 30, 17, 17, 14}}, {'7', {31, 1, 2, 4, 8, 8, 8}},
            {'8', {14, 17, 17, 14, 17, 17, 14}}, {'9', {14, 17, 17, 15, 1, 1, 14}},
            {'.', {0, 0, 0, 0, 0, 12, 12}},      {':', {0, 12, 12, 0, 12, 12, 0}},
            {'-', {0, 0, 0, 31, 0, 0, 0}},       {'/', {1, 1, 2, 4, 8, 16, 16}},
            {'_', {0, 0, 0, 0, 0, 0, 31}},       {'(', {2, 4, 8, 8, 8, 4, 2}},
            {')', {8, 4, 2, 2, 2, 4, 8}},        {'+', {0, 4, 4, 31, 4, 4, 0}},
            {'=', {0, 0, 31, 0, 31, 0, 0}},      {'[', {14, 8, 8, 8, 8, 8, 14}},
            {']', {14, 2, 2, 2, 2, 2, 14}},      {'?', {14, 17, 1, 2, 4, 0, 4}},
            {'!', {4, 4, 4, 4, 4, 0, 4}}};
        float x = text.x, y = text.y, unit = text.size / 7;
        for (unsigned char c : text.value) {
            if (c == '\n') {
                x = text.x;
                y += text.size * 1.4f;
                continue;
            }
            if (c >= 'a' && c <= 'z')
                c -= 32;
            if (c != ' ') {
                auto it = glyphs.find(static_cast<char>(c));
                auto pattern = it == glyphs.end()
                                   ? std::array<unsigned char, 7>{31, 17, 17, 17, 17, 17, 31}
                                   : it->second;
                for (int row = 0; row < 7; ++row)
                    for (int col = 0; col < 5; ++col)
                        if (pattern[row] & (1 << (4 - col)))
                            quad(data, {x + col * unit, y + row * unit, unit, unit, text.color});
            }
            x += 6 * unit;
        }
    }
    void upload_scene_buffer(Buffer& buffer, const void* bytes, std::size_t count,
                             VkBufferUsageFlags usage) {
        const VkDeviceSize required = std::max<VkDeviceSize>(16, count);
        if (required > max_storage_buffer_range)
            throw std::overflow_error("GPU scene buffer exceeds maxStorageBufferRange");
        if (buffer.size < required) {
            destroy(buffer);
            buffer = make_buffer(required, usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        }
        if (count && bytes) {
            void* mapped{};
            check(vkMapMemory(device, buffer.memory, 0, buffer.size, 0, &mapped),
                  "Map GPU scene buffer");
            std::memcpy(mapped, bytes, count);
            vkUnmapMemory(device, buffer.memory);
        }
    }
    void reserve_scene_output(Buffer& buffer, std::size_t count,
                              VkBufferUsageFlags usage = 0) {
        const VkDeviceSize required = std::max<VkDeviceSize>(16, count);
        if (required > max_storage_buffer_range)
            throw std::overflow_error("GPU scene buffer exceeds maxStorageBufferRange");
        if (buffer.size < required) {
            destroy(buffer);
            buffer = make_buffer(required, usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        }
    }
    template <typename T>
    void upload_scene_vector(Buffer& buffer, const std::vector<T>& values,
                             VkBufferUsageFlags usage = 0) {
        upload_scene_buffer(buffer, values.data(), values.size() * sizeof(T), usage);
    }
    void update_lighting_descriptors() {
        const std::array<VkDescriptorBufferInfo, 4> buffers{{
            {lighting_header.handle, 0, lighting_header.size},
            {lighting_locals.handle, 0, lighting_locals.size},
            {lighting_views.handle, 0, lighting_views.size},
            {light_tile_words.handle, 0, light_tile_words.size}}};
        const VkDescriptorImageInfo atlas{VK_NULL_HANDLE,
                                          local_shadow.handle ? local_shadow.view : shadow.view,
                                          VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
        std::array<VkWriteDescriptorSet, 5> writes{};
        for (std::uint32_t i = 0; i < writes.size(); ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = lighting_set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                               : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if (i == 3)
                writes[i].pImageInfo = &atlas;
            else
                writes[i].pBufferInfo = &buffers[i == 4 ? 3 : i];
        }
        vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
    void update_light_tile_descriptors() {
        if (!light_tile_set)
            return;
        const std::array<VkDescriptorBufferInfo, 2> buffers{{
            {lighting_locals.handle, 0, lighting_locals.size},
            {light_tile_words.handle, 0, light_tile_words.size}}};
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (std::uint32_t i = 0; i < writes.size(); ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = light_tile_set;
            writes[i].dstBinding = i;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].descriptorCount = 1;
            writes[i].pBufferInfo = &buffers[i];
        }
        vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
    void update_scene_descriptors(bool occlusion) {
        auto write_buffers = [&](VkDescriptorSet set, std::span<const Buffer* const> buffers,
                                 std::uint32_t first_binding) {
            std::vector<VkDescriptorBufferInfo> infos(buffers.size());
            std::vector<VkWriteDescriptorSet> writes(buffers.size());
            for (std::size_t i = 0; i < buffers.size(); ++i) {
                infos[i] = {buffers[i]->handle, 0, buffers[i]->size};
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = set;
                writes[i].dstBinding = first_binding + static_cast<std::uint32_t>(i);
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[i].pBufferInfo = &infos[i];
            }
            vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        };
        const std::array<const Buffer*, 3> main_graphics{&scene.instances, &scene.main_ids,
                                                          &scene.view};
        const std::array<const Buffer*, 3> post_graphics{&scene.instances, &scene.post_ids,
                                                          &scene.view};
        write_buffers(scene.graphics_main, main_graphics, 0);
        write_buffers(scene.graphics_post, post_graphics, 0);
        const std::array<const Buffer*, 7> main_cull{
            &scene.instances, &scene.candidates, &scene.bins, &scene.main_ids,
            &scene.main_args, &scene.deferred_ids, &scene.deferred_count};
        const std::array<const Buffer*, 7> post_cull{
            &scene.instances, &scene.candidates, &scene.bins, &scene.post_ids,
            &scene.post_args, &scene.deferred_ids, &scene.deferred_count};
        write_buffers(scene.cull_main, main_cull, 0);
        write_buffers(scene.cull_post, post_cull, 0);
        for (auto set : {scene.cull_main, scene.cull_post}) {
            const Image* previous = occlusion ? &scene.hzb[1 - scene.hzb_current] : &shadow;
            const Image* current = occlusion ? &scene.hzb[scene.hzb_current] : &shadow;
            const VkImageLayout previous_layout = occlusion ? VK_IMAGE_LAYOUT_GENERAL
                                                             : VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
            const VkImageLayout current_layout = previous == &shadow
                ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
            VkDescriptorImageInfo images[] = {
                {VK_NULL_HANDLE, previous->view, previous_layout},
                {VK_NULL_HANDLE, current->view, current_layout}};
            VkWriteDescriptorSet writes[2]{};
            for (std::uint32_t i = 0; i < 2; ++i) {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = set;
                writes[i].dstBinding = 7 + i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                writes[i].pImageInfo = &images[i];
            }
            vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
            const std::array<const Buffer*, 1> view{&scene.view};
            write_buffers(set, view, 9);
        }
    }
    void scene_barrier(VkPipelineStageFlags2 source_stages, VkAccessFlags2 source_access,
                       VkPipelineStageFlags2 destination_stages, VkAccessFlags2 destination_access) {
        VkMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        barrier.srcStageMask = source_stages;
        barrier.srcAccessMask = source_access;
        barrier.dstStageMask = destination_stages;
        barrier.dstAccessMask = destination_access;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(command, &dependency);
    }
    Bounds world_bounds(const std::shared_ptr<const Mesh>& mesh, const Mat4& model) {
        auto found = bounds_cache.find(mesh.get());
        if (found == bounds_cache.end() || found->second.owner.lock() != mesh) {
            auto local = local_bounds(*mesh);
            found = bounds_cache.insert_or_assign(mesh.get(), CachedBounds{mesh, local}).first;
        }
        return transformed_bounds(found->second.local, model);
    }
    bool is_opaque(const std::shared_ptr<const Texture>& texture) {
        if (!texture)
            return true;
        auto found = opacity_cache.find(texture.get());
        if (found == opacity_cache.end() || found->second.owner.lock() != texture ||
            found->second.revision != texture->revision) {
            const bool opaque = opaque_texture(texture.get());
            found = opacity_cache.insert_or_assign(
                texture.get(), CachedOpacity{texture, texture->revision, opaque}).first;
        }
        return found->second.opaque;
    }
    void render(const Snapshot& snapshot) {
        auto start = std::chrono::steady_clock::now();
        statistics.draw_calls = statistics.culled_meshes = statistics.gpu_label_count = 0;
        statistics.submitted_local_lights = statistics.omitted_local_lights = 0;
        statistics.requested_sun_cascades = statistics.effective_sun_cascades =
            statistics.sun_shadow_caster_draws = 0;
        statistics.sun_shadow_atlas_bytes = sun_shadow_size ? shadow.allocation_size : 0;
        statistics.gpu_sun_shadow_ms = 0;
        statistics.requested_local_shadow_faces = statistics.local_shadow_faces =
            statistics.local_shadow_tiles = statistics.dropped_shadow_faces =
            statistics.dropped_point_shadow_faces =
            statistics.shadow_atlas_full_drops =
            statistics.shadow_caster_budget_drops =
            statistics.shadow_unavailable_drops =
            statistics.shadow_caster_draws = 0;
        statistics.local_shadow_atlas_bytes = local_shadow_size
            ? local_shadow.allocation_size : 0;
        statistics.gpu_local_shadow_ms = 0;
        statistics.gpu_light_tiles_ms = 0;
        statistics.light_tile_count = 0;
        statistics.light_tile_counts_valid = false;
        statistics.light_tile_candidate_count = statistics.light_tile_overflow_count = 0;
        statistics.effective_lighting_path = "forward";
        statistics.gpu_bins = statistics.gpu_visible_instances =
            statistics.gpu_frustum_rejected = statistics.gpu_occlusion_deferred =
                statistics.gpu_post_visible = 0;
        statistics.lod_counts = {};
        statistics.visibility_counters_valid = false;
        statistics.requested_temporal_mode = config.temporal_mode;
        statistics.temporal_history_valid = false;
        statistics.temporal_valid_motion_instances = 0;
        statistics.temporal_jitter = {};
        statistics.temporal_counters_valid = false;
        statistics.temporal_accepted_pixels = statistics.temporal_rejected_pixels = 0;
        statistics.gpu_temporal_resolve_ms = statistics.gpu_temporal_composite_ms =
            statistics.gpu_ui_ms = 0;
        statistics.graph_passes.clear();
        for (auto it = bounds_cache.begin(); it != bounds_cache.end();)
            it = it->second.owner.expired() ? bounds_cache.erase(it) : std::next(it);
        for (auto it = opacity_cache.begin(); it != opacity_cache.end();)
            it = it->second.owner.expired() ? opacity_cache.erase(it) : std::next(it);
        bool can_present = surface != VK_NULL_HANDLE;
        if (surface) {
            // A capture may render between normal event-loop iterations. Keep the window
            // system progressing without consuming events intended for the editor.
            SDL_PumpEvents();
            int w{}, h{};
            SDL_GetWindowSizeInPixels(window, &w, &h);
            can_present =
                w > 0 && h > 0 &&
                !(SDL_GetWindowFlags(window) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED));
            if (can_present && (dirty_swapchain || !swapchain))
                make_swapchain();
        }
        // A swapchain resize can recreate temporal targets and destroy their
        // diagnostic staging buffer. Decide the actual paths and allocate the
        // readback only after that resource lifetime boundary.
        statistics.effective_temporal_mode = select_effective_temporal_mode(
            config.temporal_mode, temporal.capabilities);
        statistics.temporal_fallback_reason = temporal_fallback_reason(
            config.temporal_mode, temporal.capabilities);
        statistics.temporal_internal_width = temporal.internal_width;
        statistics.temporal_internal_height = temporal.internal_height;
        const bool temporal_active = statistics.effective_temporal_mode != TemporalMode::Off;
        statistics.requested_visibility_mode = config.visibility_mode;
        statistics.effective_visibility_mode = select_effective_visibility_mode(
            config.visibility_mode, scene.available, scene.hzb_supported && scene.hzb_mips);
        const bool gpu_active = statistics.effective_visibility_mode != VisibilityMode::Direct;
        const bool occlusion = statistics.effective_visibility_mode ==
            VisibilityMode::GpuOcclusion;
        statistics.gpu_visibility_active = gpu_active;
        statistics.hzb_valid = false;
        bool collect_temporal_counts = temporal_active && config.temporal_diagnostics;
        if (collect_temporal_counts && !temporal.pixel_counts_stage.handle) {
            try {
                temporal.pixel_counts_stage = make_buffer(2 * sizeof(std::uint32_t),
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
            } catch (const std::exception&) {
                collect_temporal_counts = false;
            }
        }
        // Retire atlas/image resources no longer retained by a caller.
        for (auto it = textures.begin(); it != textures.end();) {
            if (it->first != white.get() && it->second.source.use_count() == 1) {
                destroy(it->second.image);
                vkFreeDescriptorSets(device, descriptor_pool, 1, &it->second.descriptor);
                it = textures.erase(it);
            } else
                ++it;
        }
        const VkDescriptorSet white_descriptor = upload_texture(white);
        for (const auto& q : snapshot.ui_quads)
            if (q.texture)
                upload_texture(q.texture);
        for (const auto& draw : snapshot.draws)
            if (draw.texture)
                upload_texture(draw.texture);
        for (const auto& sprite : snapshot.sprites)
            if (sprite.texture)
                upload_texture(sprite.texture);
        for (const auto& triangles : snapshot.ui_triangles)
            if (triangles.texture)
                upload_texture(triangles.texture);
        std::array<float, 4> scene_viewport{0, 0, float(width), float(height)};
        if (snapshot.scene_rect[2] > 0 && snapshot.scene_rect[3] > 0) {
            const auto& rect = snapshot.scene_rect;
            const float x = std::clamp(rect[0], 0.f, float(width));
            const float y = std::clamp(rect[1], 0.f, float(height));
            scene_viewport = {x, y, std::max(0.f, std::min(rect[2], float(width) - x)),
                              std::max(0.f, std::min(rect[3], float(height) - y))};
        }
        const auto output_scene_viewport = scene_viewport;
        if (temporal_active) {
            const float sx = float(temporal.internal_width) / float(width);
            const float sy = float(temporal.internal_height) / float(height);
            scene_viewport = {scene_viewport[0] * sx, scene_viewport[1] * sy,
                              scene_viewport[2] * sx, scene_viewport[3] * sy};
            statistics.temporal_jitter = temporal_jitter(
                statistics.frame,
                std::max(1u, static_cast<std::uint32_t>(std::ceil(scene_viewport[2]))),
                std::max(1u, static_cast<std::uint32_t>(std::ceil(scene_viewport[3]))));
        }
        Mat4 raster_vp = snapshot.view_projection;
        if (temporal_active)
            for (int column = 0; column < 4; ++column) {
                raster_vp[column * 4] +=
                    statistics.temporal_jitter[0] * raster_vp[column * 4 + 3];
                raster_vp[column * 4 + 1] +=
                    statistics.temporal_jitter[1] * raster_vp[column * 4 + 3];
            }
        const auto raster_width = temporal_active ? temporal.internal_width : width;
        const auto raster_height = temporal_active ? temporal.internal_height : height;
        const std::string view_id = snapshot.view_id.empty() ? "default" : snapshot.view_id;
        TemporalHistoryKey temporal_key;
        temporal_key.view_id = view_id;
        temporal_key.output_width = width;
        temporal_key.output_height = height;
        temporal_key.internal_width = temporal.internal_width;
        temporal_key.internal_height = temporal.internal_height;
        temporal_key.scene_rect = output_scene_viewport;
        temporal_key.projection = snapshot.projection;
        temporal_key.view_projection = snapshot.view_projection;
        temporal_key.camera_eye = snapshot.eye;
        temporal_key.mode = statistics.effective_temporal_mode;
        temporal_key.render_scale = temporal_active ? config.render_scale : 1.f;
        temporal_key.shader_generation = temporal.shader_generation;
        temporal_key.camera_cut = snapshot.camera_cut;
        auto temporal_decision = temporal.history.prepare(temporal_key);
        if (temporal_active && temporal_decision.valid && !temporal.has_completed_image)
            temporal_decision = {false, TemporalResetReason::FirstFrame};
        statistics.temporal_history_valid = temporal_active && temporal_decision.valid;
        statistics.temporal_reset_reason = temporal_active
            ? temporal_decision.reason
            : statistics.temporal_fallback_reason == TemporalFallbackReason::None
                  ? temporal_decision.reason : TemporalResetReason::Unsupported;
        const bool history_compatible = occlusion && scene.hzb_history_valid &&
            !snapshot.camera_cut && scene.previous_view_id == view_id &&
            scene.previous_viewport == scene_viewport &&
            scene.previous_projection == snapshot.projection;
        if (occlusion)
            scene.hzb_current = history_compatible ? 1 - scene.hzb_current : 0;
        PreparedScene gpu_frame;
        gpu_frame.view.current_vp = raster_vp;
        gpu_frame.view.previous_vp = scene.previous_vp;
        gpu_frame.view.viewport = scene_viewport;
        gpu_frame.view.previous_viewport = scene.previous_viewport;
        if (scene.hzb_mips) {
            const auto dimensions = std::array<std::uint32_t, 4>{
                std::bit_ceil(temporal.internal_width),
                std::bit_ceil(temporal.internal_height), scene.hzb_mips, 0};
            gpu_frame.view.hzb_dimensions = dimensions;
            gpu_frame.view.previous_hzb_dimensions = dimensions;
        }
        gpu_frame.view.flags[0] = history_compatible ? 1 : 0;
        statistics.hzb_valid = history_compatible;
        struct SelectedDraw {
            const DrawItem* source;
            std::shared_ptr<const Mesh> mesh;
            bool gpu{};
            bool opaque{};
            bool motion_valid{};
            Mat4 previous_model{identity};
        };
        std::vector<SelectedDraw> selected_draws;
        selected_draws.reserve(snapshot.draws.size());
        std::vector<ShadowCasterBounds> shadow_casters;
        shadow_casters.reserve(snapshot.draws.size());
        struct BuildingBin {
            const Mesh* mesh{};
            const Texture* texture{};
            std::uint32_t first_vertex{}, vertex_count{};
            std::vector<std::uint32_t> instances;
        };
        std::vector<BuildingBin> building_bins;
        std::unordered_map<const Mesh*, std::pair<std::uint32_t, std::uint32_t>> mesh_ranges;
        std::unordered_map<std::string, std::size_t> current_lods;
        for (std::size_t source_index = 0; source_index < snapshot.draws.size(); ++source_index) {
            const auto& item = snapshot.draws[source_index];
            if (!item.mesh || item.mesh->vertices.empty())
                continue;
            const auto source_bounds = world_bounds(item.mesh, item.model);
            if (item.cast_shadow) {
                if (source_index > UINT32_MAX)
                    throw std::overflow_error("Shadow source draw index exceeds 32-bit capacity");
                shadow_casters.push_back(
                    {source_bounds, static_cast<std::uint32_t>(source_index)});
            }
            std::vector<float> thresholds;
            std::vector<std::uint8_t> available;
            std::shared_ptr<const Mesh> selected_mesh = item.mesh;
            std::size_t lod = 0;
            if (!item.lod_meshes.empty()) {
                available.push_back(1);
                for (std::size_t level = 0; level < item.lod_meshes.size(); ++level) {
                    thresholds.push_back(192.f / float(std::uint32_t(1) <<
                                                      std::min<std::size_t>(level, 20)));
                    available.push_back(item.lod_meshes[level] &&
                                                !item.lod_meshes[level]->vertices.empty() ? 1 : 0);
                }
                auto previous = previous_lods.find(item.instance_key);
                lod = select_lod(projected_pixels(source_bounds, snapshot.view_projection,
                                                  scene_viewport),
                                 previous != previous_lods.end() && !item.instance_key.empty()
                                     ? previous->second : SIZE_MAX,
                                 thresholds, .12f, available);
                if (lod)
                    selected_mesh = item.lod_meshes[lod - 1];
            }
            statistics.lod_counts[std::min<std::size_t>(lod, 3)]++;
            if (!item.instance_key.empty())
                current_lods[item.instance_key] = lod;
            const auto bounds = selected_mesh == item.mesh
                ? source_bounds : world_bounds(selected_mesh, item.model);
            InstanceUpdate previous{};
            if (!item.instance_key.empty())
                previous = instance_tracker.update(item.instance_key, selected_mesh,
                                                   item.model, bounds, view_id);
            const bool opaque = item.color[3] >= 1.f && is_opaque(item.texture);
            const bool eligible = gpu_active && opaque;
            const bool motion_valid = temporal_active && temporal_decision.valid &&
                previous.previous_valid && opaque;
            if (motion_valid)
                ++statistics.temporal_valid_motion_instances;
            selected_draws.push_back({&item, selected_mesh, eligible, opaque, motion_valid,
                                      previous.previous_valid ? previous.previous_model : item.model});
            if (!eligible)
                continue;
            auto [range_it, inserted] = mesh_ranges.try_emplace(selected_mesh.get());
            if (inserted) {
                const std::size_t first = gpu_frame.vertices.size();
                auto emit = [&](std::uint32_t index) {
                    if (index >= selected_mesh->vertices.size())
                        throw std::out_of_range("Mesh index outside vertex range");
                    const auto& v = selected_mesh->vertices[index];
                    SceneVertex gpu_vertex{};
                    std::copy(v.position.begin(), v.position.end(), gpu_vertex.position);
                    std::copy(v.normal.begin(), v.normal.end(), gpu_vertex.normal);
                    std::copy(v.color.begin(), v.color.end(), gpu_vertex.color);
                    std::copy(v.uv.begin(), v.uv.end(), gpu_vertex.uv);
                    gpu_frame.vertices.push_back(gpu_vertex);
                };
                if (selected_mesh->indices.empty())
                    for (std::uint32_t i = 0; i < selected_mesh->vertices.size(); ++i)
                        emit(i);
                else
                    for (auto i : selected_mesh->indices)
                        emit(i);
                const auto count = gpu_frame.vertices.size() - first;
                if (count % 3 || first > UINT32_MAX || count > UINT32_MAX)
                    throw std::invalid_argument("GPU scene mesh must fit complete triangles");
                range_it->second = {static_cast<std::uint32_t>(first),
                                    static_cast<std::uint32_t>(count)};
            }
            if (!range_it->second.second)
                continue;
            auto bin_it = std::find_if(building_bins.begin(), building_bins.end(),
                                       [&](const BuildingBin& bin) {
                                           return bin.mesh == selected_mesh.get() &&
                                                  bin.texture == (item.texture
                                                      ? item.texture.get() : white.get());
                                       });
            if (bin_it == building_bins.end()) {
                const auto [first, count] = range_it->second;
                building_bins.push_back({selected_mesh.get(), item.texture
                                              ? item.texture.get() : white.get(),
                                         first, count, {}});
                bin_it = std::prev(building_bins.end());
            }
            const auto& m = item.model;
            Vec3 a{m[0], m[1], m[2]}, b{m[4], m[5], m[6]}, c{m[8], m[9], m[10]};
            auto cross = [](Vec3 x, Vec3 y) {
                return Vec3{x[1] * y[2] - x[2] * y[1],
                            x[2] * y[0] - x[0] * y[2],
                            x[0] * y[1] - x[1] * y[0]};
            };
            const auto ca = cross(b, c), cb = cross(c, a), cc = cross(a, b);
            const float determinant = a[0] * ca[0] + a[1] * ca[1] + a[2] * ca[2];
            const float sign = determinant < 0 ? -1.f : 1.f;
            SceneInstance instance{};
            instance.model = m;
            for (int row = 0; row < 3; ++row) {
                auto& normal = row == 0 ? instance.normal0
                             : row == 1 ? instance.normal1 : instance.normal2;
                normal = {ca[row] * sign, cb[row] * sign, cc[row] * sign, 0};
            }
            instance.color = item.color;
            instance.material = {item.roughness, item.metallic, 0, 0};
            for (int axis = 0; axis < 3; ++axis) {
                instance.center_extent[axis] = (bounds.min[axis] + bounds.max[axis]) * .5f;
                instance.half_extent[axis] = (bounds.max[axis] - bounds.min[axis]) * .5f;
                instance.previous_center_extent[axis] =
                    (previous.previous_bounds.min[axis] +
                     previous.previous_bounds.max[axis]) * .5f;
                instance.previous_half_extent[axis] =
                    (previous.previous_bounds.max[axis] -
                     previous.previous_bounds.min[axis]) * .5f;
            }
            instance.metadata = gpu_instance_metadata(previous, history_compatible,
                                                       temporal_decision.valid);
            instance.previous_model = previous.previous_valid ? previous.previous_model
                                                               : item.model;
            if (gpu_frame.instances.size() >= UINT32_MAX)
                throw std::overflow_error("GPU scene instance capacity exceeded");
            bin_it->instances.push_back(static_cast<std::uint32_t>(gpu_frame.instances.size()));
            gpu_frame.instances.push_back(instance);
        }
        previous_lods = std::move(current_lods);
        for (const auto& bin : building_bins) {
            if (gpu_frame.candidates.size() > UINT32_MAX - bin.instances.size())
                throw std::overflow_error("GPU scene candidate capacity exceeded");
            const auto first_candidate = static_cast<std::uint32_t>(gpu_frame.candidates.size());
            const auto visible_base = first_candidate;
            const auto bin_index = static_cast<std::uint32_t>(gpu_frame.bins.size());
            for (auto id : bin.instances)
                gpu_frame.candidates.push_back({id, bin_index, 0, 0});
            gpu_frame.bins.push_back({first_candidate,
                                      static_cast<std::uint32_t>(bin.instances.size()),
                                      visible_base,
                                      static_cast<std::uint32_t>(bin.instances.size())});
            // gpuVertexMain/gpuShadowMain read raw Vulkan InstanceIndex.
            // Keep firstInstance at zero; visible_base is supplied separately.
            gpu_frame.commands.push_back({bin.vertex_count, 0, bin.first_vertex, 0});
            gpu_frame.textures.push_back(bin.texture);
        }
        gpu_frame.candidate_count = static_cast<std::uint32_t>(gpu_frame.candidates.size());
        ShadowBudget shadow_budget;
        shadow_budget.sun_atlas_size = sun_shadow_size;
        shadow_budget.sun_atlas_available = sun_shadow_size != 0;
        shadow_budget.local_atlas_size = local_shadow_size;
        shadow_budget.local_atlas_available = local_shadow_size != 0;
        const auto shadow_plan = build_shadow_plan(snapshot, shadow_casters, shadow_budget);
        const bool sun_raster = sun_shadow_size &&
            std::any_of(shadow_plan.sun_views.begin(), shadow_plan.sun_views.end(),
                        [](const ShadowView& view) {
                            return view.valid && !view.caster_indices.empty();
                        });
        statistics.requested_sun_cascades = shadow_plan.requested_sun_cascades;
        statistics.effective_sun_cascades = sun_raster
            ? shadow_plan.effective_sun_cascades : 0;
        if (sun_raster)
            for (const auto& view : shadow_plan.sun_views)
                if (view.valid)
                    statistics.sun_shadow_caster_draws +=
                        static_cast<std::uint32_t>(view.caster_indices.size());
        const bool local_raster = local_shadow_size &&
            std::any_of(shadow_plan.local_views.begin(), shadow_plan.local_views.end(),
                        [](const ShadowView& view) {
                            return view.valid && !view.caster_indices.empty();
                        });
        statistics.requested_local_shadow_faces = shadow_plan.local_faces_requested;
        statistics.local_shadow_tiles = shadow_plan.local_faces_used;
        statistics.local_shadow_faces = local_raster ? shadow_plan.local_faces_used : 0;
        statistics.dropped_shadow_faces = shadow_plan.dropped_local_faces;
        statistics.dropped_point_shadow_faces = shadow_plan.dropped_point_faces;
        for (const auto& assignment : shadow_plan.local_assignments) {
            if (assignment.valid || assignment.reason == ShadowDropReason::None)
                continue;
            const auto& light = snapshot.local_lights[assignment.source_index];
            const auto faces = light.kind == LocalLight::Kind::Point ? 6u : 1u;
            if (assignment.reason == ShadowDropReason::TileBudget)
                statistics.shadow_atlas_full_drops += faces;
            else if (assignment.reason == ShadowDropReason::CasterBudget)
                statistics.shadow_caster_budget_drops += faces;
            else if (assignment.reason == ShadowDropReason::Unavailable)
                statistics.shadow_unavailable_drops += faces;
        }
        for (const auto& view : shadow_plan.sun_views) {
            if (view.reason == ShadowDropReason::CasterBudget)
                ++statistics.shadow_caster_budget_drops;
            else if (view.reason == ShadowDropReason::Unavailable)
                ++statistics.shadow_unavailable_drops;
        }
        statistics.shadow_caster_draws = statistics.sun_shadow_caster_draws;
        if (local_raster)
            for (const auto& view : shadow_plan.local_views)
                statistics.shadow_caster_draws +=
                    static_cast<std::uint32_t>(view.caster_indices.size());
        std::vector<GpuVertex> data;
        std::vector<Batch> scene_batches, transparent_batches, sprite_batches, ui_batches;
        for (const auto& selected : selected_draws) {
            const auto& item = *selected.source;
            if (selected.gpu)
                continue;
            auto first = data.size();
            const auto& mesh = *selected.mesh;
            auto emit = [&](std::uint32_t index) {
                if (index >= mesh.vertices.size())
                    throw std::out_of_range("Mesh index outside vertex range");
                data.push_back(gpu_vertex(mesh.vertices[index], item, raster_vp,
                                          selected.motion_valid ? &selected.previous_model : nullptr,
                                          selected.motion_valid ? &scene.previous_vp : nullptr));
            };
            if (mesh.indices.empty())
                for (std::uint32_t i = 0; i < mesh.vertices.size(); ++i)
                    emit(i);
            else
                for (auto i : mesh.indices)
                    emit(i);
            auto count = static_cast<std::uint32_t>(data.size() - first);
            if (count % 3)
                throw std::invalid_argument(
                    "Mesh triangle vertex count must be divisible by three");
            if (!count)
                continue;
            Batch batch{static_cast<std::uint32_t>(first), count,
                        item.texture ? item.texture.get() : white.get()};
            if (outside(data, first))
                ++statistics.culled_meshes;
            else if (!selected.opaque && (gpu_active || temporal_active))
                transparent_batches.push_back(batch);
            else
                scene_batches.push_back(batch);
        }
        struct OrderedSprite {
            const Sprite* sprite;
            float depth;
        };
        std::vector<OrderedSprite> ordered_sprites;
        ordered_sprites.reserve(snapshot.sprites.size());
        for (const auto& sprite : snapshot.sprites) {
            const auto clip =
                point(snapshot.view_projection,
                      {sprite.position[0], sprite.position[1], sprite.position[2], 1});
            const auto depth =
                clip[3] != 0 ? clip[2] / clip[3] : std::numeric_limits<float>::infinity();
            ordered_sprites.push_back(
                {&sprite, std::isfinite(depth) ? depth : std::numeric_limits<float>::infinity()});
        }
        std::stable_sort(ordered_sprites.begin(), ordered_sprites.end(),
                         [](const auto& a, const auto& b) {
                             if (a.sprite->layer != b.sprite->layer)
                                 return a.sprite->layer < b.sprite->layer;
                             return a.depth > b.depth;
                         });
        for (const auto& ordered : ordered_sprites) {
            const auto& sprite = *ordered.sprite;
            auto first = static_cast<std::uint32_t>(data.size());
            float c = std::cos(sprite.rotation), s = std::sin(sprite.rotation);
            for (auto i : {0, 1, 2, 0, 2, 3}) {
                const float corners[4][2] = {{-.5f, -.5f}, {.5f, -.5f}, {.5f, .5f}, {-.5f, .5f}};
                float x = corners[i][0] * sprite.size[0], y = corners[i][1] * sprite.size[1];
                auto clip = point(raster_vp,
                                  {sprite.position[0] + c * x - s * y,
                                   sprite.position[1] + s * x + c * y, sprite.position[2], 1});
                GpuVertex vertex{};
                std::copy(clip.begin(), clip.end(), vertex.clip);
                std::copy(sprite.color.begin(), sprite.color.end(), vertex.color);
                vertex.uv[0] = corners[i][0] + .5f;
                vertex.uv[1] = .5f - corners[i][1];
                vertex.material[0] = sprite.texture && sprite.texture->srgb ? 1.f : 0.f;
                data.push_back(vertex);
            }
            sprite_batches.push_back(
                {first, 6, sprite.texture ? sprite.texture.get() : white.get()});
        }
        for (const auto& q : snapshot.ui_quads) {
            auto first = static_cast<std::uint32_t>(data.size());
            quad(data, q);
            const Texture* texture = q.texture ? q.texture.get() : white.get();
            if (!ui_batches.empty() && ui_batches.back().texture == texture)
                ui_batches.back().count += 6;
            else
                ui_batches.push_back({first, 6, texture});
        }
        auto text_first = static_cast<std::uint32_t>(data.size());
        for (const auto& text : snapshot.ui_text)
            draw_debug_text(data, text);
        if (data.size() > text_first)
            ui_batches.push_back(
                {text_first, static_cast<std::uint32_t>(data.size() - text_first), white.get()});
        for (const auto& triangles : snapshot.ui_triangles) {
            if (triangles.vertices.size() % 3 != 0)
                throw std::invalid_argument("UI triangle list must contain complete triangles");
            const auto first = static_cast<std::uint32_t>(data.size());
            for (const auto& source : triangles.vertices) {
                GpuVertex vertex{};
                vertex.clip[0] = source.position[0] / float(width) * 2 - 1;
                vertex.clip[1] = source.position[1] / float(height) * 2 - 1;
                vertex.clip[3] = 1;
                std::copy(source.color.begin(), source.color.end(), vertex.color);
                std::copy(source.uv.begin(), source.uv.end(), vertex.uv);
                vertex.material[0] = triangles.texture && triangles.texture->srgb ? 1.f : 0.f;
                data.push_back(vertex);
            }
            ui_batches.push_back({first, static_cast<std::uint32_t>(triangles.vertices.size()),
                                  triangles.texture ? triangles.texture.get() : white.get(),
                                  triangles.clip_rect});
        }
        const auto composite_first = static_cast<std::uint32_t>(data.size());
        if (temporal_active)
            for (const auto xy : {Vec2{-1, -1}, Vec2{3, -1}, Vec2{-1, 3}}) {
                GpuVertex vertex{};
                vertex.clip[0] = xy[0];
                vertex.clip[1] = xy[1];
                vertex.clip[3] = 1;
                data.push_back(vertex);
            }
        std::vector<Batch> shadow_batch_by_source(snapshot.draws.size());
        if (sun_raster || local_raster) {
            std::vector<std::uint8_t> required(snapshot.draws.size());
            for (const auto& view : shadow_plan.sun_views)
                if (sun_raster && view.valid)
                    for (const auto source : view.caster_indices)
                        required.at(source) = 1;
            for (const auto& view : shadow_plan.local_views)
                if (local_raster && view.valid)
                    for (const auto source : view.caster_indices)
                        required.at(source) = 1;
            for (std::size_t source = 0; source < required.size(); ++source) {
                if (!required[source])
                    continue;
                const auto& item = snapshot.draws[source];
                if (!item.mesh)
                    continue;
                const auto first = data.size();
                const auto& mesh = *item.mesh; // Source LOD 0, independent of camera/P2 LOD.
                auto emit = [&](std::uint32_t index) {
                    if (index >= mesh.vertices.size())
                        throw std::out_of_range("Shadow mesh index outside vertex range");
                    data.push_back(gpu_vertex(mesh.vertices[index], item,
                                              snapshot.view_projection));
                };
                if (mesh.indices.empty())
                    for (std::uint32_t i = 0; i < mesh.vertices.size(); ++i)
                        emit(i);
                else
                    for (const auto index : mesh.indices)
                        emit(index);
                const auto count = data.size() - first;
                if (count % 3 || first > UINT32_MAX || count > UINT32_MAX)
                    throw std::invalid_argument("Shadow mesh must fit complete triangles");
                shadow_batch_by_source[source] =
                    {static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(count),
                     white.get()};
            }
        }
        statistics.vertices = static_cast<std::uint32_t>(data.size() + gpu_frame.vertices.size());
        auto byte_count = std::max<std::size_t>(sizeof(GpuVertex), data.size() * sizeof(GpuVertex));
        if (vertices.size < byte_count) {
            destroy(vertices);
            vertices = make_buffer(byte_count, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
        void* mapped{};
        check(vkMapMemory(device, vertices.memory, 0, vertices.size, 0, &mapped), "Map vertices");
        if (!data.empty())
            std::memcpy(mapped, data.data(), data.size() * sizeof(GpuVertex));
        vkUnmapMemory(device, vertices.memory);
        if (gpu_active) {
            statistics.gpu_bins = static_cast<std::uint32_t>(gpu_frame.bins.size());
            upload_scene_vector(scene.vertices, gpu_frame.vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            upload_scene_vector(scene.instances, gpu_frame.instances);
            upload_scene_vector(scene.candidates, gpu_frame.candidates);
            upload_scene_vector(scene.bins, gpu_frame.bins);
            upload_scene_buffer(scene.view, &gpu_frame.view, sizeof(gpu_frame.view), 0);
            reserve_scene_output(scene.main_ids,
                                 gpu_frame.candidate_count * sizeof(std::uint32_t));
            reserve_scene_output(scene.post_ids,
                                 gpu_frame.candidate_count * sizeof(std::uint32_t));
            reserve_scene_output(scene.main_args,
                                 gpu_frame.commands.size() * sizeof(SceneIndirect),
                                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
            reserve_scene_output(scene.post_args,
                                 gpu_frame.commands.size() * sizeof(SceneIndirect),
                                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
            reserve_scene_output(scene.deferred_ids,
                                 gpu_frame.candidate_count * sizeof(std::uint32_t));
            upload_scene_vector(scene.main_args_stage, gpu_frame.commands,
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            upload_scene_vector(scene.post_args_stage, gpu_frame.commands,
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            const std::uint32_t zero = 0;
            reserve_scene_output(scene.deferred_count, sizeof(zero));
            upload_scene_buffer(scene.deferred_count_stage, &zero, sizeof(zero),
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            update_scene_descriptors(occlusion);
        }
        std::optional<SunLight> sun = snapshot.sun;
        if (!sun && !snapshot.authored_lights_present && snapshot.local_lights.empty())
            sun = SunLight{"legacy-sun", snapshot.light_direction, {1, 1, 1, 1}, 1, true};
        Vec3 direction = sun ? sun->direction : snapshot.light_direction;
        float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
                                 direction[2] * direction[2]);
        if (!std::isfinite(length) || length < 1e-5f) {
            if (sun && sun->stable_id != "legacy-sun")
                throw std::invalid_argument("Authored sun direction must be finite and nonzero");
            direction = {-.5f, -1, -.3f};
            length = std::sqrt(1.34f);
        }
        for (auto& v : direction)
            v /= length;
        LightingHeaderGpu lighting{};
        lighting.counts[1] = sun ? 1u : 0u;
        lighting.counts[2] = sun_raster ? 1u : 0u;
        lighting.sun_direction_intensity = {direction[0], direction[1], direction[2],
                                            sun ? sun->intensity : 0};
        lighting.sun_color = sun ? sun->color : Color{0, 0, 0, 1};
        if (sun && (!std::isfinite(sun->intensity) || sun->intensity < 0 ||
                    std::any_of(sun->color.begin(), sun->color.end(),
                                [](float v) { return !std::isfinite(v) || v < 0; })))
            throw std::invalid_argument("Authored sun radiance must be finite and nonnegative");
        lighting.camera_forward_shadow_distance = {0, 0, -1, 80};
        if (snapshot.camera_frustum) {
            const auto& view = snapshot.camera_frustum->view;
            lighting.camera_forward_shadow_distance = {-view[2], -view[6], -view[10], 80};
        }
        lighting.counts[3] = static_cast<std::uint32_t>(shadow_plan.sun_views.size());
        std::vector<ShadowViewGpu> gpu_shadow_views;
        gpu_shadow_views.reserve(std::max<std::size_t>(1, shadow_plan.sun_views.size()));
        for (std::size_t i = 0; i < shadow_plan.sun_views.size(); ++i) {
            const auto& view = shadow_plan.sun_views[i];
            ShadowViewGpu gpu{};
            gpu.view_projection = view.view_projection;
            gpu.tile_scale_offset = view.atlas_scale_offset;
            gpu.guarded_clamp = view.guarded_clamp;
            gpu.bias_flags = {.0008f, .003f,
                              sun_shadow_size ? 1.f / float(sun_shadow_size) : 0.f,
                              sun_raster && view.valid ? 1.f : 0.f};
            gpu_shadow_views.push_back(gpu);
            if (i < lighting.cascade_splits.size())
                lighting.cascade_splits[i] = view.split_far;
        }
        for (const auto& view : shadow_plan.local_views) {
            ShadowViewGpu gpu{};
            gpu.view_projection = view.view_projection;
            gpu.tile_scale_offset = view.atlas_scale_offset;
            gpu.guarded_clamp = view.guarded_clamp;
            gpu.bias_flags = {.0008f, .003f,
                              local_shadow_size ? 1.f / float(local_shadow_size) : 0.f,
                              local_raster && view.valid ? 1.f : 0.f};
            gpu_shadow_views.push_back(gpu);
        }
        std::vector<const LocalShadowAssignment*> assignments(snapshot.local_lights.size());
        for (const auto& assignment : shadow_plan.local_assignments)
            assignments.at(assignment.source_index) = &assignment;
        statistics.omitted_local_lights = shadow_plan.omitted_local_lights;
        std::vector<LocalLightGpu> gpu_lights;
        gpu_lights.reserve(shadow_plan.submitted_local_indices.size());
        for (const auto source : shadow_plan.submitted_local_indices) {
            const auto& local = snapshot.local_lights[source];
            auto spot_direction = local.direction;
            float spot_length = std::hypot(spot_direction[0], spot_direction[1],
                                           spot_direction[2]);
            if (!std::isfinite(spot_length) || spot_length < 1e-6f) {
                if (local.kind == LocalLight::Kind::Spot)
                    throw std::invalid_argument("Spotlight direction must be finite and nonzero");
                spot_direction = {0, 0, -1};
                spot_length = 1;
            }
            for (auto& axis : spot_direction)
                axis /= spot_length;
            LocalLightGpu gpu{};
            gpu.position_range = {local.position[0], local.position[1], local.position[2],
                                  local.range};
            const bool spot = local.kind == LocalLight::Kind::Spot;
            gpu.direction_cos_outer = {spot_direction[0], spot_direction[1], spot_direction[2],
                                       spot ? std::cos(local.outer_angle) : 0.f};
            gpu.color_intensity = {local.color[0], local.color[1], local.color[2],
                                   local.intensity};
            gpu.cone_type_shadow_view = {spot ? std::cos(local.inner_angle) : 1.f,
                                         spot ? 1.f : 0.f, -1, 0};
            const auto* assignment = assignments.at(source);
            if (local_raster && assignment && assignment->valid) {
                gpu.cone_type_shadow_view[2] = float(
                    shadow_plan.sun_views.size() + assignment->first_view);
                gpu.cone_type_shadow_view[3] = float(assignment->face_count);
            }
            gpu_lights.push_back(gpu);
        }
        lighting.counts[0] = static_cast<std::uint32_t>(gpu_lights.size());
        statistics.submitted_local_lights = lighting.counts[0];
        if (gpu_lights.empty())
            gpu_lights.push_back({}); // Descriptors always point at a full initialized record.
        if (gpu_shadow_views.empty())
            gpu_shadow_views.push_back({}); // Always bind an initialized record.
        upload_scene_buffer(lighting_header, &lighting, sizeof(lighting), 0);
        upload_scene_vector(lighting_locals, gpu_lights);
        upload_scene_vector(lighting_views, gpu_shadow_views);
        constexpr std::uint32_t light_tile_side = 16;
        constexpr std::uint32_t light_tile_stride_words = 66;
        constexpr std::uint32_t light_tile_capacity = 64;
        const std::uint32_t light_tiles_x = raster_width / light_tile_side +
                                            (raster_width % light_tile_side != 0);
        const std::uint32_t light_tiles_y = raster_height / light_tile_side +
                                            (raster_height % light_tile_side != 0);
        const std::uint64_t light_tile_count =
            std::uint64_t(light_tiles_x) * light_tiles_y;
        const std::uint64_t light_tile_bytes =
            (4u + light_tile_count * light_tile_stride_words) * sizeof(std::uint32_t);
        // The fixed 1080p reference scene has broad overlapping lights: 32 and
        // 64 nearly fill every tile, and 128 overflows every tile. Until a
        // validated runtime occupancy predictor exists, Auto keeps the measured
        // faster full scan. The explicit mode supports sparse-light projects.
        const bool requested_light_tiles =
            config.lighting_mode == LightingMode::Tiled;
        bool use_light_tiles = requested_light_tiles && lighting.counts[0] > 0 &&
            light_tiles_capable && light_tile_pipeline && light_tile_set &&
            std::all_of(scene_viewport.begin(), scene_viewport.end(),
                        [](float value) { return std::isfinite(value); }) &&
            scene_viewport[2] > 0 && scene_viewport[3] > 0 &&
            light_tile_count <= std::numeric_limits<std::uint32_t>::max() &&
            light_tile_bytes <= max_storage_buffer_range &&
            (light_tile_count + 63) / 64 <= max_compute_groups_x;
        if (use_light_tiles && light_tile_words.size < light_tile_bytes) {
            try {
                auto replacement = make_buffer(light_tile_bytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                destroy(light_tile_words);
                light_tile_words = replacement;
            } catch (const std::exception&) {
                use_light_tiles = false;
            }
        }
        if (use_light_tiles) {
            statistics.effective_lighting_path = "tiled";
            statistics.light_tile_count = static_cast<std::uint32_t>(light_tile_count);
        }
        bool collect_light_tile_counts = use_light_tiles && config.visibility_diagnostics;
        if (collect_light_tile_counts && light_tile_readback.size < light_tile_bytes) {
            try {
                auto replacement = make_buffer(light_tile_bytes,
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
                destroy(light_tile_readback);
                light_tile_readback = replacement;
            } catch (const std::exception&) {
                collect_light_tile_counts = false;
            }
        }
        update_lighting_descriptors();
        if (use_light_tiles)
            update_light_tile_descriptors();
        Vec3 light_eye{-direction[0] * 30, -direction[1] * 30, -direction[2] * 30};
        Vec3 light_up = std::abs(direction[1]) > .98f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
        Push push{sun_raster && !shadow_plan.sun_views.empty()
                      ? shadow_plan.sun_views.front().view_projection
                      : multiply(orthographic(-20, 20, -20, 20, .1f, 80),
                                 look_at(light_eye, {0, 0, 0}, light_up)),
                  {direction[0], direction[1], direction[2], 0},
                  {snapshot.eye[0], snapshot.eye[1], snapshot.eye[2], 1}};
        std::optional<std::uint32_t> swap_index;
        if (can_present) {
            std::uint32_t index{};
            // Compositors can withhold images while a window is occluded. Rendering and
            // editor capture must remain available even when presentation cannot advance.
            const auto result =
                vkAcquireNextImageKHR(device, swapchain, 0, acquired, VK_NULL_HANDLE, &index);
            if (result == VK_ERROR_OUT_OF_DATE_KHR) {
                dirty_swapchain = true;
            } else if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
                swap_index = index;
                if (result == VK_SUBOPTIMAL_KHR)
                    dirty_swapchain = true;
            } else if (result != VK_NOT_READY && result != VK_TIMEOUT) {
                check(result, "Acquire swapchain image");
            }
        }
        begin();
        std::uint32_t timestamp_cursor = 0;
        std::vector<std::string> timestamp_labels;
        if (timestamp_pool) {
            vkCmdResetQueryPool(command, timestamp_pool, 0, timestamp_capacity);
            vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                 timestamp_pool, timestamp_cursor++);
        }
        VkDeviceSize offset{};
        vkCmdBindVertexBuffers(command, 0, 1, &vertices.handle, &offset);
        vkCmdPushConstants(command, pipeline_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        auto set_viewport = [&](std::uint32_t w, std::uint32_t h) {
            VkViewport viewport{0, 0, float(w), float(h), 0, 1};
            VkRect2D scissor{{0, 0}, {w, h}};
            vkCmdSetViewport(command, 0, 1, &viewport);
            vkCmdSetScissor(command, 0, 1, &scissor);
        };
        auto set_scene_viewport = [&] {
            VkViewport viewport{scene_viewport[0], scene_viewport[1],
                                scene_viewport[2], scene_viewport[3], 0, 1};
            VkRect2D scissor{};
            if (temporal_active) {
                const auto left = static_cast<std::int32_t>(std::floor(scene_viewport[0]));
                const auto top = static_cast<std::int32_t>(std::floor(scene_viewport[1]));
                const auto right = static_cast<std::int32_t>(std::ceil(
                    scene_viewport[0] + scene_viewport[2]));
                const auto bottom = static_cast<std::int32_t>(std::ceil(
                    scene_viewport[1] + scene_viewport[3]));
                scissor = {{left, top},
                           {static_cast<std::uint32_t>(std::max(0, right - left)),
                            static_cast<std::uint32_t>(std::max(0, bottom - top))}};
            } else {
                scissor = {{static_cast<int>(scene_viewport[0]),
                            static_cast<int>(scene_viewport[1])},
                           {static_cast<std::uint32_t>(scene_viewport[2]),
                            static_cast<std::uint32_t>(scene_viewport[3])}};
            }
            vkCmdSetViewport(command, 0, 1, &viewport);
            vkCmdSetScissor(command, 0, 1, &scissor);
        };
        auto bind_material = [&](VkDescriptorSet material) {
            const std::array<VkDescriptorSet, 2> sets{material, lighting_set};
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                                    0, static_cast<std::uint32_t>(sets.size()), sets.data(),
                                    0, nullptr);
        };
        auto draw_transparent = [&] {
            if (transparent_batches.empty())
                return;
            vkCmdBindVertexBuffers(command, 0, 1, &vertices.handle, &offset);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              temporal_active ? temporal.transparent_pipeline : pipeline);
            vkCmdPushConstants(command, pipeline_layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(push), &push);
            for (auto batch : transparent_batches) {
                auto descriptor = textures.at(batch.texture).descriptor;
                bind_material(descriptor);
                vkCmdDraw(command, batch.count, 1, batch.first, 0);
                ++statistics.draw_calls;
            }
        };
        auto draw_sprites = [&] {
            vkCmdBindVertexBuffers(command, 0, 1, &vertices.handle, &offset);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              temporal_active ? temporal.transparent_pipeline : sprite_pipeline);
            vkCmdPushConstants(command, pipeline_layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(push), &push);
            for (auto batch : sprite_batches) {
                auto descriptor = textures.at(batch.texture).descriptor;
                bind_material(descriptor);
                vkCmdDraw(command, batch.count, 1, batch.first, 0);
                ++statistics.draw_calls;
            }
        };
        auto draw_ui = [&] {
            vkCmdBindVertexBuffers(command, 0, 1, &vertices.handle, &offset);
            set_viewport(width, height);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              temporal_active ? temporal_ui_pipeline : ui_pipeline);
            for (auto batch : ui_batches) {
                VkRect2D scissor{{0, 0}, {width, height}};
                if (batch.clip_rect[2] > 0 && batch.clip_rect[3] > 0) {
                    const auto& clip = batch.clip_rect;
                    const float x = std::clamp(clip[0], 0.f, float(width));
                    const float y = std::clamp(clip[1], 0.f, float(height));
                    const float right = std::clamp(clip[0] + clip[2], x, float(width));
                    const float bottom = std::clamp(clip[1] + clip[3], y, float(height));
                    scissor.offset = {static_cast<int>(x), static_cast<int>(y)};
                    scissor.extent = {static_cast<unsigned>(right) - static_cast<unsigned>(x),
                                      static_cast<unsigned>(bottom) - static_cast<unsigned>(y)};
                }
                if (!scissor.extent.width || !scissor.extent.height)
                    continue;
                vkCmdSetScissor(command, 0, 1, &scissor);
                auto descriptor = textures.at(batch.texture).descriptor;
                bind_material(descriptor);
                vkCmdDraw(command, batch.count, 1, batch.first, 0);
                ++statistics.draw_calls;
            }
        };
        auto dispatch_scene = [&](VkPipeline compute_pipeline, VkDescriptorSet descriptor) {
            if (!gpu_frame.candidate_count)
                return;
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                              compute_pipeline);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    scene.cull_pipeline_layout, 0, 1, &descriptor, 0,
                                    nullptr);
            const std::uint64_t max_threads = std::uint64_t(max_compute_groups_x) * 64;
            for (std::uint64_t base = 0; base < gpu_frame.candidate_count;
                 base += max_threads) {
                const auto remaining = std::uint64_t(gpu_frame.candidate_count) - base;
                const auto groups = static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(max_compute_groups_x,
                                            (remaining + 63) / 64));
                const std::array<std::uint32_t, 4> parameters{
                    gpu_frame.candidate_count, gpu_frame.candidate_count,
                    static_cast<std::uint32_t>(base), 0};
                vkCmdPushConstants(command, scene.cull_pipeline_layout,
                                   VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(parameters), parameters.data());
                vkCmdDispatch(command, groups, 1, 1);
            }
        };
        RenderGraph graph;
        if (temporal_active)
            graph.import("history_previous");
        auto add_pass = [&](std::string name, std::vector<std::string> reads,
                            std::vector<std::string> writes, RenderGraph::Callback callback) {
            const auto label_name = name;
            statistics.graph_passes.push_back(label_name);
            graph.add(std::move(name), std::move(reads), std::move(writes),
                      [this, &timestamp_cursor, &timestamp_labels, label_name,
                       callback = std::move(callback)] {
                          if (statistics.gpu_labels_enabled) {
                              VkDebugUtilsLabelEXT label{};
                              label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
                              label.pLabelName = label_name.c_str();
                              label.color[0] = .25f;
                              label.color[1] = .65f;
                              label.color[2] = .9f;
                              label.color[3] = 1.f;
                              begin_gpu_label(command, &label);
                              ++statistics.gpu_label_count;
                          }
                          struct EndLabel {
                              Impl& renderer;
                              ~EndLabel() {
                                  if (renderer.statistics.gpu_labels_enabled)
                                      renderer.end_gpu_label(renderer.command);
                              }
                          } end{*this};
                          callback();
                          if (timestamp_pool && timestamp_cursor < timestamp_capacity) {
                              vkCmdWriteTimestamp2(command,
                                                   VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                                                   timestamp_pool, timestamp_cursor++);
                              timestamp_labels.push_back(label_name);
                          }
                      });
        };
        auto raster_shadow_atlas = [&](Image& atlas, std::uint32_t atlas_size,
                                       const std::vector<ShadowView>& views) {
                transition(command, atlas, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_ASPECT_DEPTH_BIT);
                VkRenderingAttachmentInfo attachment{};
                attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                attachment.imageView = atlas.view;
                attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                attachment.clearValue.depthStencil = {1, 0};
                VkRenderingInfo rendering{};
                rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                rendering.renderArea = {{0, 0}, {atlas_size, atlas_size}};
                rendering.layerCount = 1;
                rendering.pDepthAttachment = &attachment;
                vkCmdBeginRendering(command, &rendering);
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  shadow_pipeline);
                for (const auto& view : views) {
                    if (!view.valid || view.caster_indices.empty())
                        continue;
                    const auto guard = (view.tile_size - view.usable_size) / 2;
                    const auto x = view.tile_origin_x + guard;
                    const auto y = view.tile_origin_y + guard;
                    const VkViewport viewport{float(x), float(y), float(view.usable_size),
                                              float(view.usable_size), 0, 1};
                    const VkRect2D scissor{{static_cast<std::int32_t>(x),
                                             static_cast<std::int32_t>(y)},
                                            {view.usable_size, view.usable_size}};
                    vkCmdSetViewport(command, 0, 1, &viewport);
                    vkCmdSetScissor(command, 0, 1, &scissor);
                    auto view_push = push;
                    view_push.light_view_projection = view.view_projection;
                    vkCmdPushConstants(command, pipeline_layout,
                                       VK_SHADER_STAGE_VERTEX_BIT |
                                           VK_SHADER_STAGE_FRAGMENT_BIT,
                                       0, sizeof(view_push), &view_push);
                    for (const auto source : view.caster_indices) {
                        const auto& batch = shadow_batch_by_source.at(source);
                        if (!batch.count)
                            continue;
                        vkCmdDraw(command, batch.count, 1, batch.first, 0);
                        ++statistics.draw_calls;
                    }
                }
                vkCmdEndRendering(command);
                transition(command, atlas, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                           VK_IMAGE_ASPECT_DEPTH_BIT);
        };
        if (sun_raster)
            add_pass("SunShadowAtlas", {}, {"shadow"}, [&] {
                raster_shadow_atlas(shadow, sun_shadow_size, shadow_plan.sun_views);
            });
        else
            add_pass("ShadowFallback", {}, {"shadow"}, [&] {
                // A bound descriptor still needs a matching image layout, even when
                // every graphics shader branch treats its shadow as unshadowed.
                transition(command, shadow, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                           VK_IMAGE_ASPECT_DEPTH_BIT);
            });
        if (local_raster)
            add_pass("LocalShadowAtlas", {}, {"local_shadow"}, [&] {
                raster_shadow_atlas(local_shadow, local_shadow_size,
                                    shadow_plan.local_views);
            });
        else
            add_pass("LocalShadowFallback", {}, {"local_shadow"}, [&] {
                if (local_shadow.handle)
                    transition(command, local_shadow,
                               VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                               VK_IMAGE_ASPECT_DEPTH_BIT);
            });
        if (use_light_tiles)
            add_pass("LightTileBuild", {}, {"light_tiles"}, [&] {
                scene_barrier(VK_PIPELINE_STAGE_2_HOST_BIT,
                              VK_ACCESS_2_HOST_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                  light_tile_pipeline);
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        light_tile_pipeline_layout, 0, 1,
                                        &light_tile_set, 0, nullptr);
                const LightTilePush tile_push{
                    raster_vp, scene_viewport,
                    {light_tiles_x, light_tiles_y, lighting.counts[0], light_tile_capacity}};
                vkCmdPushConstants(command, light_tile_pipeline_layout,
                                   VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(tile_push), &tile_push);
                vkCmdDispatch(command,
                              static_cast<std::uint32_t>((light_tile_count + 63) / 64), 1, 1);
                scene_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
            });
        else
            add_pass("LightTileFallback", {}, {"light_tiles"}, [&] {
                vkCmdFillBuffer(command, light_tile_words.handle, 0, 16, 0);
                scene_barrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
            });
        if (gpu_active)
            add_pass("MainCull", {"shadow"},
                     {"main_indirect", "main_visible", "deferred_ids"}, [&] {
                scene_barrier(VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              VK_ACCESS_2_TRANSFER_READ_BIT);
                const VkDeviceSize command_bytes =
                    gpu_frame.commands.size() * sizeof(SceneIndirect);
                if (command_bytes) {
                    const VkBufferCopy copy{0, 0, command_bytes};
                    vkCmdCopyBuffer(command, scene.main_args_stage.handle,
                                    scene.main_args.handle, 1, &copy);
                    vkCmdCopyBuffer(command, scene.post_args_stage.handle,
                                    scene.post_args.handle, 1, &copy);
                }
                const VkBufferCopy count_copy{0, 0, sizeof(std::uint32_t)};
                vkCmdCopyBuffer(command, scene.deferred_count_stage.handle,
                                scene.deferred_count.handle, 1, &count_copy);
                scene_barrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                  VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
                if (occlusion) {
                    for (auto& pyramid : scene.hzb) {
                        if (pyramid.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
                            transition(command, pyramid, VK_IMAGE_LAYOUT_GENERAL,
                                       VK_IMAGE_ASPECT_COLOR_BIT);
                            VkClearColorValue far_depth{};
                            far_depth.float32[0] = 1.f;
                            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                                          pyramid.mip_levels, 0, 1};
                            vkCmdClearColorImage(command, pyramid.handle,
                                                 VK_IMAGE_LAYOUT_GENERAL, &far_depth, 1,
                                                 &range);
                        }
                    }
                    scene_barrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                  VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                }
                scene_barrier(VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
                if (gpu_frame.candidate_count) {
                    dispatch_scene(scene.cull_pipeline, scene.cull_main);
                    scene_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                  VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                                      VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                                  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
                }
            });
        Image& raster_color = temporal_active ? temporal.scene_color : color;
        add_pass(occlusion || temporal_active ? "MainRaster" : "ForwardAndUI",
                 gpu_active ? std::vector<std::string>{"shadow", "local_shadow",
                                                       "light_tiles", "main_indirect", "main_visible"}
                            : std::vector<std::string>{"shadow", "local_shadow", "light_tiles"},
                 temporal_active ? std::vector<std::string>{"scene_color", "depth", "scene_velocity"}
                                 : std::vector<std::string>{"color", "depth"}, [&] {
            transition(command, raster_color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_ASPECT_COLOR_BIT);
            transition(command, depth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_ASPECT_DEPTH_BIT);
            VkRenderingAttachmentInfo ca{};
            ca.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            ca.imageView = raster_color.view;
            ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            std::copy(snapshot.clear_color.begin(), snapshot.clear_color.end(),
                      ca.clearValue.color.float32);
            VkRenderingAttachmentInfo da{};
            da.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            da.imageView = depth.view;
            da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            da.storeOp = occlusion || temporal_active ? VK_ATTACHMENT_STORE_OP_STORE
                                   : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            da.clearValue.depthStencil = {1, 0};
            VkRenderingAttachmentInfo va{};
            std::array<VkRenderingAttachmentInfo, 2> color_attachments{ca, va};
            if (temporal_active) {
                transition(command, temporal.velocity,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_ASPECT_COLOR_BIT);
                va.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                va.imageView = temporal.velocity.view;
                va.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                va.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                va.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                color_attachments[1] = va;
            }
            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea = {{0, 0}, {raster_width, raster_height}};
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = temporal_active ? 2 : 1;
            rendering.pColorAttachments = color_attachments.data();
            rendering.pDepthAttachment = &da;
            vkCmdBeginRendering(command, &rendering);
            set_scene_viewport();
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              temporal_active ? temporal.direct_pipeline : pipeline);
            vkCmdPushConstants(command, pipeline_layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(push), &push);
            bind_material(white_descriptor);
            if (gpu_active && !gpu_frame.bins.empty()) {
                VkDeviceSize scene_offset{};
                vkCmdBindVertexBuffers(command, 0, 1, &scene.vertices.handle, &scene_offset);
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  temporal_active ? temporal.gpu_pipeline
                                                  : scene.graphics_pipeline);
                for (std::uint32_t bin = 0; bin < gpu_frame.bins.size(); ++bin) {
                    auto descriptor = textures.at(gpu_frame.textures[bin]).descriptor;
                    const std::array<VkDescriptorSet, 3> sets{descriptor, lighting_set,
                                                               scene.graphics_main};
                    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            scene.graphics_pipeline_layout, 0, sets.size(),
                                            sets.data(), 0, nullptr);
                    ScenePush draw_push{push, {gpu_frame.bins[bin].visible_base, 0, 0, 0}};
                    vkCmdPushConstants(command, scene.graphics_pipeline_layout,
                                       VK_SHADER_STAGE_VERTEX_BIT |
                                           VK_SHADER_STAGE_FRAGMENT_BIT,
                                       0, sizeof(draw_push), &draw_push);
                    vkCmdDrawIndirect(command, scene.main_args.handle,
                                      VkDeviceSize(bin) * sizeof(SceneIndirect), 1,
                                      sizeof(SceneIndirect));
                    ++statistics.draw_calls;
                }
                vkCmdBindVertexBuffers(command, 0, 1, &vertices.handle, &offset);
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  temporal_active ? temporal.direct_pipeline : pipeline);
                vkCmdPushConstants(command, pipeline_layout,
                                   VK_SHADER_STAGE_VERTEX_BIT |
                                       VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(push), &push);
            }
            for (auto batch : scene_batches) {
                auto descriptor = textures.at(batch.texture).descriptor;
                bind_material(descriptor);
                vkCmdDraw(command, batch.count, 1, batch.first, 0);
                ++statistics.draw_calls;
            }
            if (!occlusion) {
                draw_transparent();
                draw_sprites();
                if (!temporal_active)
                    draw_ui();
            }
            vkCmdEndRendering(command);
        });
        if (occlusion) {
            add_pass("BuildCurrentHZB", {"depth"}, {"current_hzb"}, [&] {
                transition(command, depth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                           VK_IMAGE_ASPECT_DEPTH_BIT);
                auto& pyramid = scene.hzb[scene.hzb_current];
                transition(command, pyramid, VK_IMAGE_LAYOUT_GENERAL,
                           VK_IMAGE_ASPECT_COLOR_BIT);
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                  scene.hzb_pipeline);
                const auto padded_width = std::bit_ceil(raster_width);
                const auto padded_height = std::bit_ceil(raster_height);
                for (std::uint32_t mip = 0; mip < scene.hzb_mips; ++mip) {
                    const std::uint32_t source_width = mip == 0 ? raster_width
                        : std::max(1u, padded_width >> (mip - 1));
                    const std::uint32_t source_height = mip == 0 ? raster_height
                        : std::max(1u, padded_height >> (mip - 1));
                    const std::uint32_t output_width =
                        std::max(1u, padded_width >> mip);
                    const std::uint32_t output_height =
                        std::max(1u, padded_height >> mip);
                    const auto descriptor = scene.hzb_sets[scene.hzb_current][mip];
                    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                            scene.hzb_pipeline_layout, 0, 1,
                                            &descriptor, 0, nullptr);
                    const std::array<std::uint32_t, 4> dimensions{
                        source_width, source_height, output_width, output_height};
                    vkCmdPushConstants(command, scene.hzb_pipeline_layout,
                                       VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                       sizeof(dimensions), dimensions.data());
                    vkCmdDispatch(command, (output_width + 7) / 8,
                                  (output_height + 7) / 8, 1);
                    scene_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                }
            });
            add_pass("PostCull", {"deferred_ids", "current_hzb"},
                     {"post_indirect", "post_visible"}, [&] {
                scene_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                if (gpu_frame.candidate_count) {
                    dispatch_scene(scene.post_pipeline, scene.cull_post);
                    scene_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                  VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                                      VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                                  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
                }
            });
            add_pass(temporal_active ? "PostRasterScene" : "PostRasterAndUI",
                     {temporal_active ? "scene_color" : "color", "depth",
                      "post_indirect", "post_visible"},
                     temporal_active ? std::vector<std::string>{"scene_color", "depth",
                                                                 "scene_velocity"}
                                     : std::vector<std::string>{"color", "depth"}, [&] {
                transition(command, raster_color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_ASPECT_COLOR_BIT);
                transition(command, depth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_ASPECT_DEPTH_BIT);
                VkRenderingAttachmentInfo ca{};
                ca.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                ca.imageView = raster_color.view;
                ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingAttachmentInfo da{};
                da.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                da.imageView = depth.view;
                da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                da.storeOp = temporal_active ? VK_ATTACHMENT_STORE_OP_STORE
                                            : VK_ATTACHMENT_STORE_OP_DONT_CARE;
                VkRenderingAttachmentInfo va{};
                std::array<VkRenderingAttachmentInfo, 2> color_attachments{ca, va};
                if (temporal_active) {
                    transition(command, temporal.velocity,
                               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               VK_IMAGE_ASPECT_COLOR_BIT);
                    va.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                    va.imageView = temporal.velocity.view;
                    va.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    va.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                    va.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    color_attachments[1] = va;
                }
                VkRenderingInfo rendering{};
                rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                rendering.renderArea = {{0, 0}, {raster_width, raster_height}};
                rendering.layerCount = 1;
                rendering.colorAttachmentCount = temporal_active ? 2 : 1;
                rendering.pColorAttachments = color_attachments.data();
                rendering.pDepthAttachment = &da;
                vkCmdBeginRendering(command, &rendering);
                set_scene_viewport();
                if (!gpu_frame.bins.empty()) {
                    VkDeviceSize scene_offset{};
                    vkCmdBindVertexBuffers(command, 0, 1, &scene.vertices.handle,
                                           &scene_offset);
                    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      temporal_active ? temporal.gpu_pipeline
                                                      : scene.graphics_pipeline);
                    for (std::uint32_t bin = 0; bin < gpu_frame.bins.size(); ++bin) {
                        auto descriptor = textures.at(gpu_frame.textures[bin]).descriptor;
                        const std::array<VkDescriptorSet, 3> sets{descriptor, lighting_set,
                                                                   scene.graphics_post};
                        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                scene.graphics_pipeline_layout, 0,
                                                sets.size(), sets.data(), 0, nullptr);
                        ScenePush draw_push{push, {gpu_frame.bins[bin].visible_base,
                                                   0, 0, 0}};
                        vkCmdPushConstants(command, scene.graphics_pipeline_layout,
                                           VK_SHADER_STAGE_VERTEX_BIT |
                                               VK_SHADER_STAGE_FRAGMENT_BIT,
                                           0, sizeof(draw_push), &draw_push);
                        vkCmdDrawIndirect(command, scene.post_args.handle,
                                          VkDeviceSize(bin) * sizeof(SceneIndirect), 1,
                                          sizeof(SceneIndirect));
                        ++statistics.draw_calls;
                    }
                }
                draw_transparent();
                draw_sprites();
                if (!temporal_active)
                    draw_ui();
                vkCmdEndRendering(command);
            });
        }
        const std::uint32_t next_history = temporal.has_completed_image
            ? 1 - temporal.completed_index : 0;
        if (temporal_active) {
            add_pass("TemporalResolve",
                     {"scene_color", "depth", "scene_velocity", "history_previous"},
                     {"resolved_color", "resolved_depth"}, [&] {
                if (collect_temporal_counts) {
                    vkCmdFillBuffer(command, temporal.pixel_counts.handle, 0,
                                    2 * sizeof(std::uint32_t), 0);
                    scene_barrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                  VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
                }
                transition(command, temporal.scene_color,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_IMAGE_ASPECT_COLOR_BIT);
                transition(command, depth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                           VK_IMAGE_ASPECT_DEPTH_BIT);
                transition(command, temporal.velocity,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_IMAGE_ASPECT_COLOR_BIT);
                transition(command, temporal.history_color[1 - next_history],
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_IMAGE_ASPECT_COLOR_BIT);
                transition(command, temporal.history_depth[1 - next_history],
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_IMAGE_ASPECT_COLOR_BIT);
                transition(command, temporal.history_color[next_history],
                           VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
                transition(command, temporal.history_depth[next_history],
                           VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
                struct ResolvePush {
                    std::array<std::uint32_t, 4> dimensions;
                    std::array<float, 4> output_rect, internal_rect;
                    std::array<std::uint32_t, 4> flags;
                    std::array<float, 4> jitter_motion;
                };
                static_assert(sizeof(ResolvePush) == 80);
                const bool static_camera = statistics.temporal_history_valid &&
                    temporal.previous_unjittered_vp == snapshot.view_projection;
                ResolvePush parameters{{width, height, raster_width, raster_height},
                                       output_scene_viewport, scene_viewport,
                                       {statistics.temporal_history_valid ? 1u : 0u,
                                        collect_temporal_counts ? 1u : 0u, 0, 0},
                                       {(statistics.temporal_jitter[0] -
                                         temporal.previous_jitter[0]) * .5f,
                                        (statistics.temporal_jitter[1] -
                                         temporal.previous_jitter[1]) * .5f,
                                        static_camera ? 1.f : 0.f, 0.f}};
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                  temporal.resolve_pipeline);
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        temporal.resolve_pipeline_layout, 0, 1,
                                        &temporal.resolve_sets[next_history], 0, nullptr);
                vkCmdPushConstants(command, temporal.resolve_pipeline_layout,
                                   VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(parameters),
                                   &parameters);
                vkCmdDispatch(command, (width + 7) / 8, (height + 7) / 8, 1);
            });
            add_pass("TemporalComposite", {"resolved_color"}, {"color"}, [&] {
                transition(command, temporal.history_color[next_history],
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_IMAGE_ASPECT_COLOR_BIT);
                transition(command, color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_ASPECT_COLOR_BIT);
                VkRenderingAttachmentInfo attachment{};
                attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                attachment.imageView = color.view;
                attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                std::copy(snapshot.clear_color.begin(), snapshot.clear_color.end(),
                          attachment.clearValue.color.float32);
                VkRenderingInfo rendering{};
                rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                rendering.renderArea = {{0, 0}, {width, height}};
                rendering.layerCount = 1;
                rendering.colorAttachmentCount = 1;
                rendering.pColorAttachments = &attachment;
                vkCmdBeginRendering(command, &rendering);
                set_viewport(width, height);
                vkCmdBindVertexBuffers(command, 0, 1, &vertices.handle, &offset);
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  temporal.composite_pipeline);
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        temporal.composite_pipeline_layout, 0, 1,
                                        &temporal.composite_sets[next_history], 0, nullptr);
                vkCmdDraw(command, 3, 1, composite_first, 0);
                vkCmdEndRendering(command);
            });
            add_pass("UI", {"color"}, {"color"}, [&] {
                if (ui_batches.empty())
                    return;
                transition(command, color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_ASPECT_COLOR_BIT);
                VkRenderingAttachmentInfo attachment{};
                attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                attachment.imageView = color.view;
                attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingInfo rendering{};
                rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                rendering.renderArea = {{0, 0}, {width, height}};
                rendering.layerCount = 1;
                rendering.colorAttachmentCount = 1;
                rendering.pColorAttachments = &attachment;
                vkCmdBeginRendering(command, &rendering);
                draw_ui();
                vkCmdEndRendering(command);
            });
        }
        add_pass("Readback", collect_light_tile_counts
                     ? std::vector<std::string>{"color", "light_tiles"}
                     : std::vector<std::string>{"color"}, {"capture"}, [&] {
            transition(command, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_IMAGE_ASPECT_COLOR_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {width, height, 1};
            vkCmdCopyImageToBuffer(command, color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   readback.handle, 1, &copy);
            if (gpu_active && config.visibility_diagnostics) {
                // The staging buffers were transfer sources at the start of
                // this frame; finish those reads before reusing them as copies'
                // destinations for diagnostic counters.
                scene_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                  VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                  VK_ACCESS_2_TRANSFER_READ_BIT,
                              VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              VK_ACCESS_2_TRANSFER_READ_BIT |
                                  VK_ACCESS_2_TRANSFER_WRITE_BIT);
                const VkDeviceSize command_bytes =
                    gpu_frame.commands.size() * sizeof(SceneIndirect);
                if (command_bytes) {
                    const VkBufferCopy command_copy{0, 0, command_bytes};
                    vkCmdCopyBuffer(command, scene.main_args.handle,
                                    scene.main_args_stage.handle, 1, &command_copy);
                    if (occlusion)
                        vkCmdCopyBuffer(command, scene.post_args.handle,
                                        scene.post_args_stage.handle, 1, &command_copy);
                }
                const VkBufferCopy count_copy{0, 0, sizeof(std::uint32_t)};
                vkCmdCopyBuffer(command, scene.deferred_count.handle,
                                scene.deferred_count_stage.handle, 1, &count_copy);
                scene_barrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_HOST_BIT,
                              VK_ACCESS_2_HOST_READ_BIT);
            }
            if (collect_light_tile_counts) {
                scene_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              VK_ACCESS_2_TRANSFER_READ_BIT);
                const VkBufferCopy tile_copy{0, 0, light_tile_bytes};
                vkCmdCopyBuffer(command, light_tile_words.handle,
                                light_tile_readback.handle, 1, &tile_copy);
                scene_barrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_HOST_BIT,
                              VK_ACCESS_2_HOST_READ_BIT);
            }
            if (collect_temporal_counts) {
                scene_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              VK_ACCESS_2_TRANSFER_READ_BIT);
                const VkBufferCopy count_copy{0, 0, 2 * sizeof(std::uint32_t)};
                vkCmdCopyBuffer(command, temporal.pixel_counts.handle,
                                temporal.pixel_counts_stage.handle, 1, &count_copy);
                scene_barrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_HOST_BIT,
                              VK_ACCESS_2_HOST_READ_BIT);
            }
        });
        if (swap_index)
            add_pass("Presentation", {"color"}, {"swapchain"}, [&] {
                auto index = *swap_index;
                transition(command, swap_images[index], swap_layouts[index],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.srcOffsets[1] = {static_cast<int>(width), static_cast<int>(height), 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.dstOffsets[1] = {static_cast<int>(swap_extent.width),
                                      static_cast<int>(swap_extent.height), 1};
                vkCmdBlitImage(command, color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               swap_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                               VK_FILTER_NEAREST);
                transition(command, swap_images[index], swap_layouts[index],
                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT);
            });
        graph.execute();
        submit(swap_index.has_value());
        if (timestamp_pool) {
            std::array<std::uint64_t, timestamp_capacity> stamps{};
            check(vkGetQueryPoolResults(device, timestamp_pool, 0, timestamp_cursor,
                                        timestamp_cursor * sizeof(std::uint64_t), stamps.data(),
                                        sizeof(std::uint64_t),
                                        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                  "Read GPU timestamps");
            auto milliseconds = [&](std::uint64_t before, std::uint64_t after) {
                auto delta = after - before;
                if (timestamp_bits < 64)
                    delta &= (std::uint64_t(1) << timestamp_bits) - 1;
                return double(delta) * timestamp_period / 1000000.0;
            };
            statistics.gpu_ms = milliseconds(stamps[0], stamps[timestamp_cursor - 1]);
            statistics.gpu_main_cull_ms = statistics.gpu_main_raster_ms =
                statistics.gpu_hzb_ms = statistics.gpu_post_cull_ms =
                    statistics.gpu_post_raster_ms = 0;
            for (std::size_t i = 0; i < timestamp_labels.size(); ++i) {
                const auto elapsed = milliseconds(stamps[i], stamps[i + 1]);
                const auto& label = timestamp_labels[i];
                if (label == "MainCull") statistics.gpu_main_cull_ms = elapsed;
                else if (label == "SunShadowAtlas") statistics.gpu_sun_shadow_ms = elapsed;
                else if (label == "LocalShadowAtlas") statistics.gpu_local_shadow_ms = elapsed;
                else if (label == "LightTileBuild") statistics.gpu_light_tiles_ms = elapsed;
                else if (label == "MainRaster" || label == "ForwardAndUI")
                    statistics.gpu_main_raster_ms = elapsed;
                else if (label == "BuildCurrentHZB") statistics.gpu_hzb_ms = elapsed;
                else if (label == "PostCull") statistics.gpu_post_cull_ms = elapsed;
                else if (label == "PostRasterAndUI" || label == "PostRasterScene")
                    statistics.gpu_post_raster_ms = elapsed;
                else if (label == "TemporalResolve")
                    statistics.gpu_temporal_resolve_ms = elapsed;
                else if (label == "TemporalComposite")
                    statistics.gpu_temporal_composite_ms = elapsed;
                else if (label == "UI")
                    statistics.gpu_ui_ms = elapsed;
            }
        }
        if (swap_index) {
            VkPresentInfoKHR present{};
            present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &present_ready;
            present.swapchainCount = 1;
            present.pSwapchains = &swapchain;
            present.pImageIndices = &*swap_index;
            auto result = vkQueuePresentKHR(queue, &present);
            if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
                dirty_swapchain = true;
            else
                check(result, "Present frame");
            check(vkQueueWaitIdle(queue), "Wait presentation");
        }
        const auto readback_started = std::chrono::steady_clock::now();
        last_pixels.resize(std::size_t(width) * height * 4);
        check(vkMapMemory(device, readback.memory, 0, readback.size, 0, &mapped),
              "Map captured frame");
        std::memcpy(last_pixels.data(), mapped, last_pixels.size());
        vkUnmapMemory(device, readback.memory);
        statistics.readback_cpu_ms = std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - readback_started)
                                         .count();
        if (gpu_active && config.visibility_diagnostics) {
            void* counts{};
            check(vkMapMemory(device, scene.main_args_stage.memory, 0,
                              scene.main_args_stage.size, 0,
                              &counts), "Read GPU scene indirect counts");
            auto* commands = static_cast<const SceneIndirect*>(counts);
            std::uint32_t main_visible{};
            for (std::size_t i = 0; i < gpu_frame.commands.size(); ++i)
                main_visible += commands[i].instance_count;
            vkUnmapMemory(device, scene.main_args_stage.memory);
            check(vkMapMemory(device, scene.deferred_count_stage.memory, 0,
                              scene.deferred_count_stage.size, 0, &counts),
                  "Read GPU deferred count");
            statistics.gpu_occlusion_deferred = *static_cast<const std::uint32_t*>(counts);
            vkUnmapMemory(device, scene.deferred_count_stage.memory);
            if (occlusion && statistics.gpu_occlusion_deferred) {
                check(vkMapMemory(device, scene.post_args_stage.memory, 0,
                                  scene.post_args_stage.size, 0,
                                  &counts), "Read GPU post counts");
                commands = static_cast<const SceneIndirect*>(counts);
                for (std::size_t i = 0; i < gpu_frame.commands.size(); ++i)
                    statistics.gpu_post_visible += commands[i].instance_count;
                vkUnmapMemory(device, scene.post_args_stage.memory);
            }
            statistics.gpu_visible_instances = main_visible + statistics.gpu_post_visible;
            statistics.gpu_frustum_rejected = gpu_frame.candidate_count -
                std::min(gpu_frame.candidate_count,
                         main_visible + statistics.gpu_occlusion_deferred);
            statistics.culled_meshes += statistics.gpu_frustum_rejected;
            statistics.visibility_counters_valid = true;
        }
        if (collect_light_tile_counts) {
            void* mapped_tiles{};
            check(vkMapMemory(device, light_tile_readback.memory, 0,
                              light_tile_bytes, 0, &mapped_tiles),
                  "Read light tile diagnostics");
            const auto* words = static_cast<const std::uint32_t*>(mapped_tiles);
            if (words[0] != light_tiles_x || words[1] != 1 ||
                words[2] != light_tiles_y || words[3] != light_tile_capacity) {
                vkUnmapMemory(device, light_tile_readback.memory);
                throw std::runtime_error("Light tile diagnostic header is inconsistent");
            }
            for (std::uint32_t tile = 0; tile < light_tile_count; ++tile) {
                const auto base = 4u + tile * light_tile_stride_words;
                if (words[base] > light_tile_capacity || words[base + 1] > 1) {
                    vkUnmapMemory(device, light_tile_readback.memory);
                    throw std::runtime_error("Light tile diagnostic record is invalid");
                }
                statistics.light_tile_candidate_count += words[base];
                statistics.light_tile_overflow_count += words[base + 1];
            }
            vkUnmapMemory(device, light_tile_readback.memory);
            statistics.light_tile_counts_valid = true;
        }
        if (collect_temporal_counts) {
            void* mapped_counts{};
            check(vkMapMemory(device, temporal.pixel_counts_stage.memory, 0,
                              temporal.pixel_counts_stage.size, 0, &mapped_counts),
                  "Read temporal pixel diagnostics");
            const auto* words = static_cast<const std::uint32_t*>(mapped_counts);
            statistics.temporal_accepted_pixels = words[0];
            statistics.temporal_rejected_pixels = words[1];
            vkUnmapMemory(device, temporal.pixel_counts_stage.memory);
            statistics.temporal_counters_valid = true;
        }
        instance_tracker.finish_frame();
        scene.previous_vp = raster_vp;
        scene.previous_projection = snapshot.projection;
        scene.previous_viewport = scene_viewport;
        scene.previous_view_id = view_id;
        scene.hzb_history_valid = occlusion;
        temporal.previous_jittered_vp = raster_vp;
        temporal.previous_unjittered_vp = snapshot.view_projection;
        temporal.previous_jitter = statistics.temporal_jitter;
        temporal.history.complete(temporal_key);
        if (temporal_active) {
            temporal.completed_index = temporal.has_completed_image
                ? 1 - temporal.completed_index : 0;
            temporal.has_completed_image = true;
        } else {
            temporal.has_completed_image = false;
        }
        ++statistics.frame;
        statistics.gpu_allocated_bytes = vertices.allocation_size + readback.allocation_size +
                                         color.allocation_size + depth.allocation_size +
                                         shadow.allocation_size +
                                         local_shadow.allocation_size +
                                         lighting_header.allocation_size +
                                         lighting_locals.allocation_size +
                                         lighting_views.allocation_size +
                                         light_tile_words.allocation_size +
                                         light_tile_readback.allocation_size;
        statistics.texture_count = static_cast<std::uint32_t>(textures.size());
        for (const auto& [_, texture] : textures)
            statistics.gpu_allocated_bytes += texture.image.allocation_size;
        if (scene.available) {
            for (const Buffer* buffer : {&scene.vertices, &scene.instances, &scene.candidates,
                                         &scene.bins, &scene.view, &scene.main_ids,
                                         &scene.post_ids, &scene.main_args, &scene.post_args,
                                         &scene.deferred_ids, &scene.deferred_count,
                                         &scene.main_args_stage, &scene.post_args_stage,
                                         &scene.deferred_count_stage})
                statistics.gpu_allocated_bytes += buffer->allocation_size;
            for (const auto& image : scene.hzb)
                statistics.gpu_allocated_bytes += image.allocation_size;
        }
        statistics.gpu_allocated_bytes += temporal.scene_color.allocation_size +
            temporal.velocity.allocation_size + temporal.pixel_counts.allocation_size +
            temporal.pixel_counts_stage.allocation_size;
        for (const auto& image : temporal.history_color)
            statistics.gpu_allocated_bytes += image.allocation_size;
        for (const auto& image : temporal.history_depth)
            statistics.gpu_allocated_bytes += image.allocation_size;
        statistics.validation_errors = validation_errors.load();
        statistics.cpu_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
    }
};
Renderer::Renderer(const RendererConfig& config) : impl_(std::make_unique<Impl>()) {
    impl_->initialize(config);
}
Renderer::~Renderer() = default;
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;
void Renderer::render(const Snapshot& snapshot) {
    impl_->render(snapshot);
}
bool Renderer::reload_shaders(std::string& error) {
    auto& r = *impl_;
    check(vkDeviceWaitIdle(r.device), "Wait shader reload");
    auto previous_layout = r.pipeline_layout;
    auto previous = r.pipeline;
    auto previous_ui = r.ui_pipeline;
    auto previous_shadow = r.shadow_pipeline;
    auto previous_sprite = r.sprite_pipeline;
    auto previous_temporal_ui = r.temporal_ui_pipeline;
    auto previous_light_tile = r.light_tile_pipeline;
    auto previous_light_tiles_capable = r.light_tiles_capable;
    auto previous_layout_fingerprints = r.shader_layouts;
    auto previous_gpu_fingerprints = r.scene.shader_layouts;
    auto previous_gpu = r.scene.graphics_pipeline;
    auto previous_cull = r.scene.cull_pipeline;
    auto previous_post = r.scene.post_pipeline;
    auto previous_hzb = r.scene.hzb_pipeline;
    const auto previous_temporal_layouts = r.temporal.shader_layouts;
    const auto previous_temporal_scene_layouts = r.temporal.scene_shader_layouts;
    const std::array<VkPipeline, 5> previous_temporal_pipelines{
        r.temporal.resolve_pipeline, r.temporal.composite_pipeline,
        r.temporal.direct_pipeline, r.temporal.transparent_pipeline,
        r.temporal.gpu_pipeline};
    r.pipeline_layout = {};
    r.pipeline = {};
    r.ui_pipeline = {};
    r.shadow_pipeline = {};
    r.sprite_pipeline = {};
    r.temporal_ui_pipeline = {};
    r.light_tile_pipeline = {};
    r.scene.graphics_pipeline = r.scene.cull_pipeline = r.scene.post_pipeline =
        r.scene.hzb_pipeline = {};
    r.temporal.resolve_pipeline = r.temporal.composite_pipeline =
        r.temporal.direct_pipeline = r.temporal.transparent_pipeline =
            r.temporal.gpu_pipeline = {};
    try {
        r.make_pipelines();
        if (r.scene.graphics_pipeline_layout)
            r.make_scene_pipelines();
        r.make_temporal_interfaces_and_pipelines();
    } catch (const std::exception& exception) {
        if (r.pipeline)
            vkDestroyPipeline(r.device, r.pipeline, nullptr);
        if (r.ui_pipeline)
            vkDestroyPipeline(r.device, r.ui_pipeline, nullptr);
        if (r.shadow_pipeline)
            vkDestroyPipeline(r.device, r.shadow_pipeline, nullptr);
        if (r.sprite_pipeline)
            vkDestroyPipeline(r.device, r.sprite_pipeline, nullptr);
        if (r.temporal_ui_pipeline)
            vkDestroyPipeline(r.device, r.temporal_ui_pipeline, nullptr);
        if (r.light_tile_pipeline)
            vkDestroyPipeline(r.device, r.light_tile_pipeline, nullptr);
        if (r.pipeline_layout)
            vkDestroyPipelineLayout(r.device, r.pipeline_layout, nullptr);
        for (auto pipeline : {r.scene.graphics_pipeline, r.scene.cull_pipeline,
                              r.scene.post_pipeline, r.scene.hzb_pipeline})
            if (pipeline)
                vkDestroyPipeline(r.device, pipeline, nullptr);
        for (auto pipeline : {r.temporal.resolve_pipeline, r.temporal.composite_pipeline,
                              r.temporal.direct_pipeline, r.temporal.transparent_pipeline,
                              r.temporal.gpu_pipeline})
            if (pipeline)
                vkDestroyPipeline(r.device, pipeline, nullptr);
        r.pipeline_layout = previous_layout;
        r.pipeline = previous;
        r.ui_pipeline = previous_ui;
        r.shadow_pipeline = previous_shadow;
        r.sprite_pipeline = previous_sprite;
        r.temporal_ui_pipeline = previous_temporal_ui;
        r.light_tile_pipeline = previous_light_tile;
        r.light_tiles_capable = previous_light_tiles_capable;
        r.scene.graphics_pipeline = previous_gpu;
        r.scene.cull_pipeline = previous_cull;
        r.scene.post_pipeline = previous_post;
        r.scene.hzb_pipeline = previous_hzb;
        r.temporal.resolve_pipeline = previous_temporal_pipelines[0];
        r.temporal.composite_pipeline = previous_temporal_pipelines[1];
        r.temporal.direct_pipeline = previous_temporal_pipelines[2];
        r.temporal.transparent_pipeline = previous_temporal_pipelines[3];
        r.temporal.gpu_pipeline = previous_temporal_pipelines[4];
        r.shader_layouts = previous_layout_fingerprints;
        r.scene.shader_layouts = previous_gpu_fingerprints;
        r.temporal.shader_layouts = previous_temporal_layouts;
        r.temporal.scene_shader_layouts = previous_temporal_scene_layouts;
        error = exception.what();
        return false;
    }
    vkDestroyPipeline(r.device, previous, nullptr);
    vkDestroyPipeline(r.device, previous_ui, nullptr);
    vkDestroyPipeline(r.device, previous_shadow, nullptr);
    vkDestroyPipeline(r.device, previous_sprite, nullptr);
    vkDestroyPipeline(r.device, previous_temporal_ui, nullptr);
    if (previous_light_tile)
        vkDestroyPipeline(r.device, previous_light_tile, nullptr);
    for (auto pipeline : {previous_gpu, previous_cull, previous_post, previous_hzb})
        if (pipeline)
            vkDestroyPipeline(r.device, pipeline, nullptr);
    for (auto pipeline : previous_temporal_pipelines)
        if (pipeline)
            vkDestroyPipeline(r.device, pipeline, nullptr);
    vkDestroyPipelineLayout(r.device, previous_layout, nullptr);
    ++r.temporal.shader_generation;
    error.clear();
    return true;
}
void Renderer::resize(std::uint32_t w, std::uint32_t h) {
    if (!w || !h)
        return;
    if (impl_->window) {
        SDL_SetWindowSize(impl_->window, static_cast<int>(w), static_cast<int>(h));
        impl_->dirty_swapchain = true;
    } else if (w != impl_->width || h != impl_->height) {
        impl_->width = w;
        impl_->height = h;
        impl_->make_targets();
    }
}
void Renderer::set_visibility_mode(VisibilityMode mode) {
    if (impl_->config.visibility_mode == mode)
        return;
    if (mode != VisibilityMode::Direct && impl_->scene.available &&
        !impl_->scene.graphics_layout) {
        check(vkDeviceWaitIdle(impl_->device), "Wait visibility switch");
        try {
            impl_->make_scene_descriptors_and_pipelines();
            if (impl_->temporal.resolve_layout) {
                auto& temporal = impl_->temporal;
                // Retain the active pipelines and descriptor interfaces until the
                // GPU variant is complete; a bad shader package must leave TAA usable.
                const std::array<VkPipeline*, 5> pipelines{
                    &temporal.resolve_pipeline, &temporal.composite_pipeline,
                    &temporal.direct_pipeline, &temporal.transparent_pipeline,
                    &temporal.gpu_pipeline};
                std::array<VkPipeline, 5> previous{};
                const auto previous_layouts = temporal.shader_layouts;
                const auto previous_scene_layouts = temporal.scene_shader_layouts;
                for (std::size_t i = 0; i < pipelines.size(); ++i)
                    previous[i] = std::exchange(*pipelines[i], VkPipeline{});
                try {
                    impl_->make_temporal_interfaces_and_pipelines();
                } catch (...) {
                    for (std::size_t i = 0; i < pipelines.size(); ++i) {
                        if (*pipelines[i])
                            vkDestroyPipeline(impl_->device, *pipelines[i], nullptr);
                        *pipelines[i] = previous[i];
                    }
                    temporal.shader_layouts = previous_layouts;
                    temporal.scene_shader_layouts = previous_scene_layouts;
                    throw;
                }
                for (auto pipeline : previous)
                    if (pipeline)
                        vkDestroyPipeline(impl_->device, pipeline, nullptr);
            }
        } catch (...) {
            impl_->destroy_scene_interfaces();
            throw;
        }
    }
    impl_->config.visibility_mode = mode;
    if (mode == VisibilityMode::GpuOcclusion && impl_->scene.hzb_supported &&
        !impl_->scene.hzb[0].handle)
        impl_->make_targets();
    impl_->scene.hzb_history_valid = false;
    impl_->instance_tracker.invalidate_view(impl_->scene.previous_view_id);
}
VisibilityMode Renderer::visibility_mode() const {
    return impl_->config.visibility_mode;
}
void Renderer::set_temporal_mode(TemporalMode mode, float scale) {
    (void)temporal_internal_extent(impl_->width, impl_->height, mode, scale);
    if (impl_->config.temporal_mode == mode && impl_->config.render_scale == scale)
        return;
    impl_->config.temporal_mode = mode;
    impl_->config.render_scale = scale;
    impl_->make_targets();
}
TemporalMode Renderer::temporal_mode() const {
    return impl_->config.temporal_mode;
}
float Renderer::render_scale() const {
    return impl_->config.render_scale;
}
void Renderer::set_visibility_diagnostics(bool enabled) {
    impl_->config.visibility_diagnostics = enabled;
}
void Renderer::set_temporal_diagnostics(bool enabled) {
    impl_->config.temporal_diagnostics = enabled;
    if (!enabled)
        impl_->destroy(impl_->temporal.pixel_counts_stage);
}
std::optional<HzbDebugImage> Renderer::hzb_debug_image(std::uint32_t mip) {
    auto& renderer = *impl_;
    if (!renderer.scene.hzb_history_valid || !renderer.scene.hzb_supported)
        return std::nullopt;
    if (mip >= renderer.scene.hzb_mips)
        throw std::out_of_range("HZB debug mip outside pyramid");
    auto& pyramid = renderer.scene.hzb[renderer.scene.hzb_current];
    const std::uint32_t w = std::max(1u, pyramid.width >> mip);
    const std::uint32_t h = std::max(1u, pyramid.height >> mip);
    auto staging = renderer.make_buffer(VkDeviceSize(w) * h * sizeof(float),
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    try {
        renderer.begin();
        renderer.transition(renderer.command, pyramid, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_IMAGE_ASPECT_COLOR_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
        copy.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(renderer.command, pyramid.handle,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.handle, 1,
                               &copy);
        renderer.transition(renderer.command, pyramid, VK_IMAGE_LAYOUT_GENERAL,
                            VK_IMAGE_ASPECT_COLOR_BIT);
        renderer.submit();
        HzbDebugImage result;
        result.width = w;
        result.height = h;
        result.rgba.resize(std::size_t(w) * h * 4);
        void* mapped{};
        check(vkMapMemory(renderer.device, staging.memory, 0, staging.size, 0, &mapped),
              "Map HZB debug image");
        const auto* depth = static_cast<const float*>(mapped);
        for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
            const float value = std::isfinite(depth[i]) ? std::clamp(depth[i], 0.f, 1.f) : 1.f;
            const auto gray = static_cast<std::uint8_t>(std::lround(std::pow(value, 32.f) * 255));
            result.rgba[4 * i] = result.rgba[4 * i + 1] =
                result.rgba[4 * i + 2] = gray;
            result.rgba[4 * i + 3] = 255;
        }
        vkUnmapMemory(renderer.device, staging.memory);
        renderer.destroy(staging);
        return result;
    } catch (...) {
        renderer.destroy(staging);
        throw;
    }
}
std::uint32_t Renderer::width() const {
    return impl_->width;
}
std::uint32_t Renderer::height() const {
    return impl_->height;
}
float Renderer::display_scale() const {
    const float scale = impl_->window ? SDL_GetWindowDisplayScale(impl_->window) : 1.f;
    return std::isfinite(scale) && scale > 0.f ? scale : 1.f;
}
bool Renderer::should_close() const {
    return impl_->close;
}
const FrameStats& Renderer::stats() const {
    return impl_->statistics;
}
std::vector<std::uint8_t> Renderer::pixels() const {
    return impl_->last_pixels;
}
void Renderer::capture(const std::filesystem::path& path) {
    if (impl_->last_pixels.empty())
        throw std::runtime_error("Cannot capture before a completed frame");
    std::ofstream out(faset::native_io_path(path), std::ios::binary);
    if (!out)
        throw std::runtime_error("Cannot write screenshot: " + faset::path_to_utf8(path));
    out << "P6\n" << width() << ' ' << height() << "\n255\n";
    for (std::size_t i = 0; i < impl_->last_pixels.size(); i += 4)
        out.write(reinterpret_cast<const char*>(impl_->last_pixels.data() + i), 3);
    if (!out)
        throw std::runtime_error("Screenshot write failed");
}
void Renderer::set_title(const std::string& title) {
    if (impl_->window)
        SDL_SetWindowTitle(impl_->window, title.c_str());
}
void Renderer::set_text_input(bool enabled) {
    if (!impl_->window)
        return;
    if (enabled)
        SDL_StartTextInput(impl_->window);
    else
        SDL_StopTextInput(impl_->window);
}
void Renderer::set_text_input_area(float x, float y, float width, float height) {
    if (!impl_->window)
        return;
    int w{}, h{}, pw{}, ph{};
    SDL_GetWindowSize(impl_->window, &w, &h);
    SDL_GetWindowSizeInPixels(impl_->window, &pw, &ph);
    float sx = pw > 0 ? float(w) / float(pw) : 1, sy = ph > 0 ? float(h) / float(ph) : 1;
    SDL_Rect rectangle{int(x * sx), int(y * sy), std::max(1, int(width * sx)),
                       std::max(1, int(height * sy))};
    if (!SDL_SetTextInputArea(impl_->window, &rectangle, 0))
        throw std::runtime_error(SDL_GetError());
}
void Renderer::set_clipboard(const std::string& text) {
    if (!impl_->window) {
        impl_->offscreen_clipboard = text;
        return;
    }
    if (!SDL_SetClipboardText(text.c_str()))
        throw std::runtime_error(SDL_GetError());
}
std::string Renderer::clipboard() const {
    if (!impl_->window)
        return impl_->offscreen_clipboard;
    char* text = SDL_GetClipboardText();
    if (!text)
        return {};
    std::string result = text;
    SDL_free(text);
    return result;
}
std::vector<Event> Renderer::poll_events() {
    std::vector<Event> result;
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        Event item;
        bool emit = true;
        auto modifiers = SDL_GetModState();
        item.control = (modifiers & SDL_KMOD_CTRL) != 0;
        item.shift = (modifiers & SDL_KMOD_SHIFT) != 0;
        item.alt = (modifiers & SDL_KMOD_ALT) != 0;
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            item.type = Event::Type::Quit;
            impl_->close = true;
            break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            item.type = Event::Type::Resize;
            item.x = float(event.window.data1);
            item.y = float(event.window.data2);
            impl_->dirty_swapchain = true;
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            item.type = Event::Type::FocusGained;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            item.type = Event::Type::FocusLost;
            break;
        case SDL_EVENT_MOUSE_MOTION:
            item.type = Event::Type::MouseMove;
            item.x = event.motion.x;
            item.y = event.motion.y;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            item.type = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? Event::Type::MouseDown
                                                                  : Event::Type::MouseUp;
            item.x = event.button.x;
            item.y = event.button.y;
            item.button = event.button.button;
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            item.type = Event::Type::Wheel;
            item.x = event.wheel.x;
            item.y = event.wheel.y;
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            item.type =
                event.type == SDL_EVENT_KEY_DOWN ? Event::Type::KeyDown : Event::Type::KeyUp;
            item.key = SDL_GetKeyName(event.key.key);
            item.repeat = event.key.repeat;
            break;
        case SDL_EVENT_TEXT_INPUT:
            item.type = Event::Type::TextInput;
            item.text = event.text.text;
            break;
        case SDL_EVENT_TEXT_EDITING:
            item.type = Event::Type::TextEditing;
            item.text = event.edit.text;
            item.edit_start = event.edit.start;
            item.edit_length = event.edit.length;
            break;
        default:
            emit = false;
        }
        // Rendering/UI coordinates use drawable pixels; SDL pointer events use logical window
        // units.
        if (impl_->window &&
            (item.type == Event::Type::MouseMove || item.type == Event::Type::MouseDown ||
             item.type == Event::Type::MouseUp)) {
            int w{}, h{}, pw{}, ph{};
            SDL_GetWindowSize(impl_->window, &w, &h);
            SDL_GetWindowSizeInPixels(impl_->window, &pw, &ph);
            if (w > 0 && h > 0) {
                item.x *= float(pw) / float(w);
                item.y *= float(ph) / float(h);
            }
        }
        if (emit)
            result.push_back(std::move(item));
    }
    return result;
}
} // namespace faset::render
