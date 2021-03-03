#pragma once

#include "render/vulkan/context.hpp"

namespace gfx = vulkan;

inline constexpr uint FRAME_QUEUE_LENGTH = 2;

struct Renderer
{
    gfx::Context context;

    uint frame_count;

    // ImGuiPass
    Handle<gfx::GraphicsProgram> gui_program;
    Handle<gfx::RenderPass> gui_renderpass;
    Handle<gfx::Framebuffer> gui_framebuffer;

    Handle<gfx::Image> gui_font_atlas;
    Handle<gfx::Buffer> gui_font_atlas_staging;

    Handle<gfx::Buffer> gui_vertices;
    Handle<gfx::Buffer> gui_indices;
    Handle<gfx::Buffer> gui_options;

    // Command submission
    std::array<gfx::WorkPool, FRAME_QUEUE_LENGTH> work_pools;

    gfx::Fence fence;
    gfx::Fence transfer_done;
    u64 transfer_fence_value = 0;

    /// ---

    static Renderer create(const platform::Window *window);
    void destroy();

    void on_resize();
    void update();

};
