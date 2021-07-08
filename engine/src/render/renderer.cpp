#include "render/renderer.hpp"

#include "base/logger.hpp"
#include "base/intrinsics.hpp"

#include "base/numerics.hpp"
#include "camera.hpp"
#include "gltf.hpp"
#include "render/gpu_pool.hpp"
#include "render/vulkan/commands.hpp"
#include "render/vulkan/device.hpp"
#include "render/vulkan/resources.hpp"
#include "render/vulkan/utils.hpp"
#include "vulkan/vulkan_core.h"
#include "render/material.hpp"

#include "ui.hpp"
#include "scene.hpp"
#include "components/camera_component.hpp"
#include "components/mesh_component.hpp"
#include "components/transform_component.hpp"
#include "tools.hpp"

#include <stdexcept>
#include <tuple> // for std::tie
#include <imgui/imgui.h>
#include <stb_image.h>


Renderer Renderer::create(const platform::Window &window)
{
    Renderer renderer = {};

    auto &context = renderer.context;
    auto &physical_devices = context.physical_devices;
    auto &device = renderer.device;
    auto &surface = renderer.surface;

    // Initialize the API
    context = gfx::Context::create(true, &window);

    // Pick a GPU
    u32 i_selected = u32_invalid;
    u32 i_device = 0;
    for (auto& physical_device : physical_devices)
    {
        logger::info("Found device: {}\n", physical_device.properties.deviceName);
        if (i_device == u32_invalid && physical_device.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            logger::info("Prioritizing device {} because it is a discrete GPU.\n", physical_device.properties.deviceName);
            i_selected = i_device;
        }
        i_device += 1;
    }
    if (i_selected == u32_invalid)
    {
        i_selected = 0;
        logger::info("No discrete GPU found, defaulting to device #0: {}.\n", physical_devices[0].properties.deviceName);
    }

    // Create the GPU
    device = gfx::Device::create(context, {
            .physical_device = &physical_devices[i_selected],
            .push_constant_layout = {.size = sizeof(PushConstants)},
            .buffer_device_address = false
        });

    // Create empty images to full the slot #0 on bindless descriptors
    renderer.empty_sampled_image = device.create_image({.name = "Empty sampled image", .usages = gfx::sampled_image_usage | gfx::storage_image_usage});
    renderer.empty_storage_image = device.create_image({.name = "Empty storage image", .usages = gfx::storage_image_usage});

    // Create the drawing surface
    surface = gfx::Surface::create(context, device, window);

    for (auto &work_pool : renderer.work_pools)
    {
        device.create_work_pool(work_pool);
    }

    // Prepare the frame synchronizations
    renderer.fence = device.create_fence();

    renderer.dynamic_uniform_buffer = RingBuffer::create(device, {
            .name = "Dynamic Uniform",
            .size = 32_KiB,
            .gpu_usage = gfx::uniform_buffer_usage,
        });

    renderer.dynamic_vertex_buffer = RingBuffer::create(device, {
            .name = "Dynamic vertices",
            .size = 1_MiB,
            .gpu_usage = gfx::storage_buffer_usage,
        });

    renderer.dynamic_index_buffer = RingBuffer::create(device, {
            .name = "Dynamic indices",
            .size = 64_KiB,
            .gpu_usage = gfx::index_buffer_usage,
        });

    // Create Render targets
    renderer.swapchain_rt.clear_renderpass = device.find_or_create_renderpass({.colors = {{.format = surface.format.format}}});
    renderer.swapchain_rt.load_renderpass = device.find_or_create_renderpass({.colors = {{.format = surface.format.format, .load_op = VK_ATTACHMENT_LOAD_OP_LOAD}}});

    renderer.swapchain_rt.framebuffer = device.create_framebuffer({
            .width = surface.extent.width,
            .height = surface.extent.height,
            .attachments_format = {surface.format.format},
        });


    renderer.settings.resolution_dirty = true;
    renderer.settings.render_resolution = {surface.extent.width, surface.extent.height};

    renderer.hdr_rt.clear_renderpass  = device.find_or_create_renderpass({
            .colors = {{.format = VK_FORMAT_R32G32B32A32_SFLOAT, .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR}},
            .depth = {{.format = VK_FORMAT_D32_SFLOAT, .load_op = VK_ATTACHMENT_LOAD_OP_LOAD }},
        });

    renderer.hdr_rt.load_renderpass  = device.find_or_create_renderpass({
            .colors = {{.format = VK_FORMAT_R32G32B32A32_SFLOAT, .load_op = VK_ATTACHMENT_LOAD_OP_LOAD}},
            .depth = {{.format = VK_FORMAT_D32_SFLOAT, .load_op = VK_ATTACHMENT_LOAD_OP_LOAD}},
        });

    renderer.ldr_rt.clear_renderpass  = device.find_or_create_renderpass({
            .colors = {{.format = VK_FORMAT_R32G32B32A32_SFLOAT, .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR}},
            .depth = {{.format = VK_FORMAT_D32_SFLOAT, .load_op = VK_ATTACHMENT_LOAD_OP_LOAD}}
        });
    renderer.ldr_rt.load_renderpass  = device.find_or_create_renderpass({
            .colors = {{.format = VK_FORMAT_R32G32B32A32_SFLOAT, .load_op = VK_ATTACHMENT_LOAD_OP_LOAD}},
            .depth = {{.format = VK_FORMAT_D32_SFLOAT, .load_op = VK_ATTACHMENT_LOAD_OP_LOAD}}
        });

    renderer.depth_only_rt.load_renderpass = device.find_or_create_renderpass({
            .depth = {{.format = VK_FORMAT_D32_SFLOAT, .load_op = VK_ATTACHMENT_LOAD_OP_LOAD}},
        });

    renderer.depth_only_rt.clear_renderpass = device.find_or_create_renderpass({
            .depth = {{.format = VK_FORMAT_D32_SFLOAT, .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR}},
        });

    // Create ImGui pass
    auto &imgui_pass = renderer.imgui_pass;
    {
        gfx::GraphicsState gui_state = {};
        gui_state.vertex_shader   =  device.create_shader("shaders/gui.vert.spv");
        gui_state.fragment_shader =  device.create_shader("shaders/gui.frag.spv");
        gui_state.renderpass = renderer.swapchain_rt.clear_renderpass;
        gui_state.descriptors =  {
            {{.count = 1, .type = gfx::DescriptorType::DynamicBuffer}},
            {{.count = 1, .type = gfx::DescriptorType::StorageBuffer}},
        };
        imgui_pass.program = device.create_program("imgui", gui_state);

        gfx::RenderState state = {.alpha_blending = true};
        uint gui_default = device.compile(imgui_pass.program, state);
        UNUSED(gui_default);
    }

    auto &io = ImGui::GetIO();
    uchar *pixels = nullptr;
    int width  = 0;
    int height = 0;
    io.Fonts->Build();
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    usize font_atlas_size = width * height * sizeof(u32);

    imgui_pass.font_atlas = device.create_image({
            .name = "Font Atlas",
            .size = {static_cast<u32>(width), static_cast<u32>(height), 1},
            .format = VK_FORMAT_R8G8B8A8_UNORM,
        });

    imgui_pass.font_atlas_staging = device.create_buffer({
            .name = "Imgui font atlas staging",
            .size = font_atlas_size,
            .usage = gfx::source_buffer_usage,
            .memory_usage = VMA_MEMORY_USAGE_CPU_ONLY,
        });

    uchar *p_font_atlas = device.map_buffer<uchar>(imgui_pass.font_atlas_staging);
    for (uint i = 0; i < font_atlas_size; i++)
    {
        p_font_atlas[i] = pixels[i];
    }
    device.flush_buffer(imgui_pass.font_atlas_staging);
    imgui_pass.should_upload_atlas = true;

    ImGui::GetIO().Fonts->SetTexID((void *)((u64)device.get_image_sampled_index(imgui_pass.font_atlas)));

    #if 0
    // Create the luminance pass
    auto &tonemap_pass = renderer.tonemap_pass;

    tonemap_pass.tonemap = device.create_program("tonemap", {
        .shader = device.create_shader("shaders/tonemap.comp.glsl.spv"),
        .descriptors =  {
            {.type = gfx::DescriptorType::DynamicBuffer, .count = 1},
        },
    });

    renderer.path_tracing_program = device.create_program("pathtracer", {
        .shader = device.create_shader("shaders/path_tracer.comp.glsl.spv"),
        .descriptors =  {
            {.type = gfx::DescriptorType::DynamicBuffer, .count = 1},
            {.type = gfx::DescriptorType::StorageBuffer, .count = 1}, // vertex buffer
            {.type = gfx::DescriptorType::StorageBuffer, .count = 1}, // index buffer
            {.type = gfx::DescriptorType::StorageBuffer, .count = 1}, // render meshes buffer
            {.type = gfx::DescriptorType::StorageBuffer, .count = 1}, // material buffer
            {.type = gfx::DescriptorType::StorageBuffer, .count = 1}, // BVH nodes buffer
            {.type = gfx::DescriptorType::StorageBuffer, .count = 1}, // BVH faces buffer
        },
    });

    {
        renderer.path_tracing_hybrid_program = device.create_program("hybrid pathtracer", {
            .vertex_shader   =  device.create_shader("shaders/opaque.vert.spv"),
            .fragment_shader =  device.create_shader("shaders/hybrid.frag.spv"),
            .renderpass = renderer.hdr_rt.load_renderpass,
            .descriptors =  {
                {.type = gfx::DescriptorType::DynamicBuffer, .count = 1},
                {.type = gfx::DescriptorType::StorageBuffer, .count = 1}, // vertex buffer
                {.type = gfx::DescriptorType::StorageBuffer, .count = 1}, // index buffer
                {.type = gfx::DescriptorType::StorageBuffer, .count = 1}, // render meshes buffer
                {.type = gfx::DescriptorType::StorageBuffer, .count = 1}, // material buffer
                {.type = gfx::DescriptorType::StorageBuffer, .count = 1}, // BVH nodes buffer
                {.type = gfx::DescriptorType::StorageBuffer, .count = 1}, // BVH faces buffer
            },
        });

        gfx::RenderState render_state   = {};
        render_state.depth.test         = VK_COMPARE_OP_EQUAL;
        render_state.depth.enable_write = false;
        device.compile(renderer.path_tracing_hybrid_program, render_state);
    }

    renderer.taa = device.create_program("taa", {
        .shader = device.create_shader("shaders/taa.comp.glsl.spv"),
        .descriptors =  {
            {.type = gfx::DescriptorType::DynamicBuffer, .count = 1},
        },
    });

    tonemap_pass.build_histo = device.create_program("build luminance histogram", {
        .shader = device.create_shader("shaders/build_luminance_histo.comp.spv"),
        .descriptors =  {
            {.type = gfx::DescriptorType::DynamicBuffer, .count = 1},
            {.type = gfx::DescriptorType::StorageBuffer, .count = 1},
        },
    });

    tonemap_pass.average_histo = device.create_program("average luminance histogram", {
        .shader = device.create_shader("shaders/average_luminance_histo.comp.spv"),
        .descriptors =  {
            {.type = gfx::DescriptorType::DynamicBuffer, .count = 1},
            {.type = gfx::DescriptorType::StorageBuffer, .count = 1},
        },
    });

    tonemap_pass.histogram = device.create_buffer({
        .name  = "Luminance histogram",
        .size  = 256 * sizeof(uint),
        .usage = gfx::storage_buffer_usage,
    });

    tonemap_pass.average_luminance = device.create_image({
        .name          = "Average luminance",
        .size          = {1, 1, 1},
        .type          = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_R32_SFLOAT,
        .usages        = gfx::storage_image_usage,
    });

    // Create gltf program
    {
        gfx::GraphicsState state = {};
        state.vertex_shader      = device.create_shader("shaders/opaque.vert.spv");
        state.fragment_shader    = device.create_shader("shaders/opaque.frag.spv");
        state.renderpass        = renderer.hdr_rt.load_renderpass;
        state.descriptors =  {
            {.type = gfx::DescriptorType::DynamicBuffer, .count = 1},
            {.type = gfx::DescriptorType::StorageBuffer, .count = 1}, // vertices
            {.type = gfx::DescriptorType::StorageBuffer, .count = 1}, // render meshes
            {.type = gfx::DescriptorType::StorageBuffer, .count = 1}, // materials
        };
        renderer.opaque_program  = device.create_program("gltf opaque", state);

        gfx::RenderState render_state   = {};
        render_state.depth.test         = VK_COMPARE_OP_EQUAL;
        render_state.depth.enable_write = false;
        uint opaque_default             = device.compile(renderer.opaque_program, render_state);
        UNUSED(opaque_default);
    }
    {
        gfx::GraphicsState state = {};
        state.vertex_shader      = device.create_shader("shaders/opaque.vert.spv");
        state.fragment_shader    = device.create_shader("shaders/opaque_prepass.frag.spv");
        state.renderpass        = renderer.depth_only_rt.load_renderpass;
        state.descriptors =  {
            {.type = gfx::DescriptorType::DynamicBuffer, .count = 1},
            {.type = gfx::DescriptorType::StorageBuffer, .count = 1}, // vertices
            {.type = gfx::DescriptorType::StorageBuffer, .count = 1}, // render meshes
            {.type = gfx::DescriptorType::StorageBuffer, .count = 1}, // materials
        };
        renderer.opaque_prepass_program  = device.create_program("gltf opaque", state);

        gfx::RenderState render_state   = {};
        render_state.depth.test         = VK_COMPARE_OP_GREATER_OR_EQUAL;
        render_state.depth.enable_write = true;
        uint opaque_default             = device.compile(renderer.opaque_prepass_program, render_state);
        UNUSED(opaque_default);
    }
    #endif

    renderer.transfer_done = device.create_fence();

    // global set
    return renderer;
}

void Renderer::destroy()
{
    device.wait_idle();

    device.destroy_fence(fence);
    device.destroy_fence(transfer_done);

    for (auto &work_pool : work_pools)
    {
        device.destroy_work_pool(work_pool);
    }

    surface.destroy(context, device);
    device.destroy(context);
    context.destroy();
}

void* Renderer::bind_shader_options(gfx::ComputeWork &cmd, Handle<gfx::GraphicsProgram> program, usize options_len)
{
    auto [options, options_offset] = dynamic_uniform_buffer.allocate(device, options_len);
    cmd.bind_uniform_buffer(program, 0, dynamic_uniform_buffer.buffer, options_offset, options_len);
    return options;
}

void* Renderer::bind_shader_options(gfx::ComputeWork &cmd, Handle<gfx::ComputeProgram> program, usize options_len)
{
    auto [options, options_offset] = dynamic_uniform_buffer.allocate(device, options_len);
    cmd.bind_uniform_buffer(program, 0, dynamic_uniform_buffer.buffer, options_offset, options_len);
    return options;
}

void Renderer::reload_shader(std::string_view shader_name)
{
    device.wait_idle();

    logger::info("{} changed!\n", shader_name);

    // Find the shader that needs to be updated
    gfx::Shader *found = nullptr;
    for (auto &[shader_h, shader] : device.shaders) {
        if (shader_name == shader->filename) {
            assert(found == nullptr);
            found = &(*shader);
        }
    }

    if (!found) {
        assert(false);
        return;
    }

    gfx::Shader &shader = *found;

    Vec<Handle<gfx::Shader>> to_remove;

    // Update programs using this shader to the new shader
    for (auto &[program_h, program] : device.compute_programs)
    {
        if (program->state.shader.is_valid())
        {
            auto &compute_shader = *device.shaders.get(program->state.shader);
            if (compute_shader.filename == shader.filename)
            {
                Handle<gfx::Shader> new_shader = device.create_shader(shader_name);
                logger::info("Found a program using the shader, creating the new shader module #{}\n", new_shader.value());

                to_remove.push_back(program->state.shader);
                program->state.shader = new_shader;
                device.recreate_program_internal(*program);
            }
        }
    }

    // Destroy the old shaders
    for (Handle<gfx::Shader> shader_h : to_remove) {
        logger::info("Removing old shader #{}\n", shader_h.value());
        device.destroy_shader(shader_h);
    }
    logger::info("\n");
}

void Renderer::on_resize()
{
    device.wait_idle();
    surface.destroy_swapchain(device);
    surface.create_swapchain(device);

    device.destroy_framebuffer(swapchain_rt.framebuffer);
    swapchain_rt.framebuffer = device.create_framebuffer({
            .width = surface.extent.width,
            .height = surface.extent.height,
            .attachments_format = {surface.format.format},
        });
}

bool Renderer::start_frame()
{
    auto current_frame = frame_count % FRAME_QUEUE_LENGTH;

    // wait for fence, blocking: dont wait for the first QUEUE_LENGTH frames
    u64 wait_value = frame_count < FRAME_QUEUE_LENGTH ? 0 : frame_count-FRAME_QUEUE_LENGTH+1;
    device.wait_for_fences({fence, transfer_done}, {wait_value, wait_value});

    // reset the command buffers
    auto &work_pool = work_pools[current_frame];
    device.reset_work_pool(work_pool);

    dynamic_uniform_buffer.start_frame();
    dynamic_vertex_buffer.start_frame();
    dynamic_index_buffer.start_frame();

    // receipt contains the image acquired semaphore
    bool out_of_date_swapchain = device.acquire_next_swapchain(surface);
    return out_of_date_swapchain;
}

static void do_imgui_pass(Renderer &renderer, gfx::GraphicsWork& cmd, RenderTargets &output, ImGuiPass &pass_data, bool clear_rt = true)
{
    auto &device = renderer.device;

    ImDrawData *data = ImGui::GetDrawData();
    assert(sizeof(ImDrawVert) * static_cast<u32>(data->TotalVtxCount) < 1_MiB);
    assert(sizeof(ImDrawIdx)  * static_cast<u32>(data->TotalVtxCount) < 1_MiB);

    u32 vertices_size = data->TotalVtxCount * sizeof(ImDrawVert);
    u32 indices_size = data->TotalIdxCount * sizeof(ImDrawIdx);

    auto [p_vertices, vert_offset] = renderer.dynamic_vertex_buffer.allocate(device, vertices_size);
    auto *vertices = reinterpret_cast<ImDrawVert*>(p_vertices);

    auto [p_indices, ind_offset] = renderer.dynamic_index_buffer.allocate(device, indices_size);
    auto *indices = reinterpret_cast<ImDrawIdx*>(p_indices);


    struct PACKED ImguiOptions
    {
        float2 scale;
        float2 translation;
        u64 vertices_pointer;
        u32 first_vertex;
        u32 pad1;
        u32 texture_binding_per_draw[64];
    };

    auto *options = renderer.bind_shader_options<ImguiOptions>(cmd, pass_data.program);
    std::memset(options, 0, sizeof(ImguiOptions));
    options->scale = float2(2.0f / data->DisplaySize.x, 2.0f / data->DisplaySize.y);
    options->translation = float2(-1.0f - data->DisplayPos.x * options->scale.x, -1.0f - data->DisplayPos.y * options->scale.y);
    options->first_vertex = vert_offset / static_cast<u32>(sizeof(ImDrawVert));
    options->vertices_pointer = 0;

    // -- Upload ImGui's vertices and indices
    u32 i_draw = 0;
    for (int i = 0; i < data->CmdListsCount; i++)
    {
        const auto &cmd_list = *data->CmdLists[i];

        for (int i_vertex = 0; i_vertex < cmd_list.VtxBuffer.Size; i_vertex++)
        {
            vertices[i_vertex] = cmd_list.VtxBuffer.Data[i_vertex];
        }

        for (int i_index = 0; i_index < cmd_list.IdxBuffer.Size; i_index++)
        {
            indices[i_index] = cmd_list.IdxBuffer.Data[i_index];
        }

        vertices += cmd_list.VtxBuffer.Size;
        indices  += cmd_list.IdxBuffer.Size;

        // Check that texture are correct
        for (int command_index = 0; command_index < cmd_list.CmdBuffer.Size && i_draw < 64; command_index++)
        {
            const auto &draw_command = cmd_list.CmdBuffer[command_index];
            u32 texture_id = static_cast<u32>((u64)draw_command.TextureId);
            options->texture_binding_per_draw[i_draw] = texture_id;
            cmd.barrier(device.get_global_sampled_image(texture_id), gfx::ImageUsage::GraphicsShaderRead);
            i_draw += 1;
        }
    }

    // -- Update shader data

    cmd.barrier(output.image, gfx::ImageUsage::ColorAttachment);

    cmd.begin_pass(clear_rt ? output.clear_renderpass : output.load_renderpass, output.framebuffer, {output.image}, {{{.float32 = {0.0f, 0.0f, 0.0f, 1.0f}}}});

    float2 clip_off   = data->DisplayPos;       // (0,0) unless using multi-viewports
    float2 clip_scale = data->FramebufferScale; // (1,1) unless using retina display which are often (2,2)

    VkViewport viewport{};
    viewport.width    = data->DisplaySize.x * data->FramebufferScale.x;
    viewport.height   = data->DisplaySize.y * data->FramebufferScale.y;
    viewport.minDepth = 1.0f;
    viewport.maxDepth = 1.0f;
    cmd.set_viewport(viewport);

    cmd.bind_storage_buffer(pass_data.program, 1, renderer.dynamic_vertex_buffer.buffer);
    cmd.bind_pipeline(pass_data.program, 0);
    cmd.bind_index_buffer(renderer.dynamic_index_buffer.buffer, VK_INDEX_TYPE_UINT16, ind_offset);

    i32 vertex_offset = 0;
    u32 index_offset  = 0;
    i_draw = 0;
    for (int list = 0; list < data->CmdListsCount; list++)
    {
        const auto &cmd_list = *data->CmdLists[list];

        for (int command_index = 0; command_index < cmd_list.CmdBuffer.Size && i_draw < 64; command_index++)
        {
            const auto &draw_command = cmd_list.CmdBuffer[command_index];

            // Project scissor/clipping rectangles into framebuffer space
            ImVec4 clip_rect;
            clip_rect.x = (draw_command.ClipRect.x - clip_off.x) * clip_scale.x;
            clip_rect.y = (draw_command.ClipRect.y - clip_off.y) * clip_scale.y;
            clip_rect.z = (draw_command.ClipRect.z - clip_off.x) * clip_scale.x;
            clip_rect.w = (draw_command.ClipRect.w - clip_off.y) * clip_scale.y;

            // Apply scissor/clipping rectangle
            VkRect2D scissor;
            scissor.offset.x      = (static_cast<i32>(clip_rect.x) > 0) ? static_cast<i32>(clip_rect.x) : 0;
            scissor.offset.y      = (static_cast<i32>(clip_rect.y) > 0) ? static_cast<i32>(clip_rect.y) : 0;
            scissor.extent.width  = static_cast<u32>(clip_rect.z - clip_rect.x);
            scissor.extent.height = static_cast<u32>(clip_rect.w - clip_rect.y);

            cmd.set_scissor(scissor);
            cmd.push_constant<PushConstants>({.draw_idx = i_draw});
            cmd.draw_indexed({.vertex_count = draw_command.ElemCount, .index_offset = index_offset, .vertex_offset = vertex_offset});
            i_draw += 1;

            index_offset += draw_command.ElemCount;
        }
        vertex_offset += cmd_list.VtxBuffer.Size;
    }

    cmd.end_pass();
}

bool Renderer::end_frame(gfx::ComputeWork &cmd)
{
    // vulkan hack: hint the device to submit a semaphore to wait on before presenting
    cmd.prepare_present(surface);

    device.submit(cmd, {fence}, {frame_count+1});

    // present will wait for semaphore
    bool out_of_date_swapchain = device.present(surface, cmd);
    if (out_of_date_swapchain)
    {
        return true;
    }

    frame_count += 1;
    dynamic_uniform_buffer.end_frame();
    dynamic_vertex_buffer.end_frame();
    dynamic_index_buffer.end_frame();
    return false;
}


void Renderer::display_ui(UI::Context &ui)
{
    ImGuiWindowFlags fb_flags = 0;// ImGuiWindowFlags_NoDecoration;
    if (ui.begin_window("Framebuffer", true, fb_flags))
    {
        float2 max = ImGui::GetWindowContentRegionMax();
        float2 min = ImGui::GetWindowContentRegionMin();
        float2 size = float2(min.x < max.x ? max.x - min.x : min.x, min.y < max.y ? max.y - min.y : min.y);

        if (static_cast<uint>(size.x) != settings.render_resolution.x || static_cast<uint>(size.y) != settings.render_resolution.y)
        {
            settings.render_resolution.x = static_cast<uint>(size.x);
            settings.render_resolution.y = static_cast<uint>(size.y);
            settings.resolution_dirty = true;
        }

        ImGui::Image((void*)((u64)device.get_image_sampled_index(ldr_rt.image)), size);

        ui.end_window();
    }

    if (ui.begin_window("Shaders"))
    {
        if (ImGui::CollapsingHeader("Tonemapping"))
        {
            static std::array options{"Reinhard", "Exposure", "Clamp", "ACES"};
            tools::imgui_select("Tonemap", options.data(), options.size(), tonemap_pass.options.selected);
            ImGui::SliderFloat("Exposure", &tonemap_pass.options.exposure, 0.0f, 2.0f);
        }
        ui.end_window();
    }

    if (ui.begin_window("Settings"))
    {
        if (ImGui::CollapsingHeader("Renderer"))
        {
            ImGui::Checkbox("Enable TAA", &settings.enable_taa);
            ImGui::Checkbox("Enable Path tracing", &settings.enable_path_tracing);
        }
        ui.end_window();
    }
}

#if 0
static void load_mesh(Renderer &renderer, const Scene &scene, const LocalToWorldComponent &transform, const RenderMeshComponent &render_mesh_component)
{
    const auto &mesh     = *scene.meshes.get(render_mesh_component.mesh_handle);
    // const auto &material = *scene.materials.get(render_mesh_component.material_handle);

    const Vertex *mesh_vertices = scene.vertices.data() + mesh.vertex_offset;
    const u32 *mesh_indices     = scene.indices.data() + mesh.index_offset;

    bool success = true;

    // upload the vertices
    u32 vertices_offset;
    std::tie(success, vertices_offset) = renderer.vertex_buffer.allocate(mesh.vertex_count);
    if (!success) {
        logger::error("[Renderer] load_mesh(): vertex allocation failed.\n");
        return;
    }

    renderer.vertex_buffer.update(vertices_offset, mesh.vertex_count, mesh_vertices);

    // Because the first vertex index is different in the GpuPool and the scene, the indices need to be updated
    Vec<u32> new_indices(mesh.index_count);
    for (usize i_index = 0; i_index < mesh.index_count; i_index += 1) {
        new_indices[i_index] = mesh_indices[i_index] + vertices_offset - mesh.vertex_offset ;
    }
    u32 indices_offset;
    std::tie(success, indices_offset) = renderer.index_buffer.allocate(mesh.index_count);
    if (!success) {
        renderer.vertex_buffer.free(vertices_offset);
        logger::error("[Renderer] load_mesh(): index allocation failed.\n");
        return;
    }
    renderer.index_buffer.update(indices_offset, mesh.index_count, new_indices.data());

    RenderMeshData new_mesh_data = {
        .transform     = transform.transform,
        .mesh_handle   = render_mesh_component.mesh_handle,
        .i_material    = render_mesh_component.i_material,
        .vertex_offset = vertices_offset,
        .index_offset  = indices_offset,
        .index_count   = mesh.index_count,
    };

    u32 new_index;
    std::tie(success, new_index) = renderer.render_mesh_data.allocate(1);
    if (!success) {
        renderer.vertex_buffer.free(vertices_offset);
        renderer.index_buffer.free(indices_offset);
        logger::error("[Renderer] load_mesh(): render mesh data allocation failed.\n");
        return;
    }

    renderer.render_mesh_data.update(new_index, 1, &new_mesh_data);
    renderer.render_mesh_indices.push_back(new_index);

    renderer.render_mesh_data_dirty = true;
}
#endif

void Renderer::update(Scene &scene)
{
    if (start_frame())
    {
        on_resize();
        ImGui::EndFrame();
        return;
    }

    if (settings.resolution_dirty)
    {
        device.wait_idle();

        device.destroy_image(depth_buffer);
        depth_buffer = device.create_image({
                .name = "Depth buffer",
                .size = {settings.render_resolution.x, settings.render_resolution.y, 1},
                .format = VK_FORMAT_D32_SFLOAT,
                .usages = gfx::depth_attachment_usage,
            });

        device.destroy_image(hdr_rt.image);
        hdr_rt.image = device.create_image({
            .name = "HDR buffer",
            .size = {settings.render_resolution.x, settings.render_resolution.y, 1},
            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
            .usages = gfx::color_attachment_usage,
            });
        hdr_rt.depth = depth_buffer;

        for (uint i_history = 0; i_history < 2; i_history += 1)
        {
            device.destroy_image(history_buffers[i_history]);
            history_buffers[i_history] = device.create_image({
                .name = fmt::format("History buffer #{}", i_history),
                .size = {settings.render_resolution.x, settings.render_resolution.y, 1},
                .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                .usages = gfx::storage_image_usage,
            });
        }

        device.destroy_framebuffer(hdr_rt.framebuffer);
        hdr_rt.framebuffer = device.create_framebuffer({
                .width = settings.render_resolution.x,
                .height = settings.render_resolution.y,
                .attachments_format = {VK_FORMAT_R32G32B32A32_SFLOAT},
                .depth_format = VK_FORMAT_D32_SFLOAT,
            });

        device.destroy_image(ldr_rt.image);
        ldr_rt.image = device.create_image({
            .name = "LDR buffer",
            .size = {settings.render_resolution.x, settings.render_resolution.y, 1},
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .usages = gfx::color_attachment_usage,
            });


        device.destroy_framebuffer(ldr_rt.framebuffer);
        ldr_rt.framebuffer = device.create_framebuffer({
                .width = settings.render_resolution.x,
                .height = settings.render_resolution.y,
                .attachments_format = {VK_FORMAT_R8G8B8A8_UNORM},
            });

        device.destroy_framebuffer(depth_only_rt.framebuffer);
        depth_only_rt.framebuffer = device.create_framebuffer({
                .width = settings.render_resolution.x,
                .height = settings.render_resolution.y,
                .attachments_format = {},
                .depth_format = VK_FORMAT_D32_SFLOAT,
            });

        settings.resolution_dirty = false;
    }

    auto current_frame = frame_count % FRAME_QUEUE_LENGTH;
    auto &work_pool = work_pools[current_frame];
    swapchain_rt.image = surface.images[surface.current_image];

    // -- Transfer stuff
    gfx::TransferWork transfer_cmd = device.get_transfer_work(work_pool);
    transfer_cmd.begin();

    if (imgui_pass.should_upload_atlas)
    {
        transfer_cmd.clear_barrier(imgui_pass.font_atlas, gfx::ImageUsage::TransferDst);
        transfer_cmd.copy_buffer_to_image(imgui_pass.font_atlas_staging, imgui_pass.font_atlas);
        imgui_pass.should_upload_atlas = false;
        imgui_pass.transfer_done_value = frame_count+1;
    }

    transfer_cmd.end();
    device.submit(transfer_cmd, {transfer_done}, {frame_count+1});

    // -- Update global data
    CameraComponent *main_camera = nullptr;
    TransformComponent *main_camera_transform = nullptr;
    scene.world.for_each<TransformComponent, CameraComponent>([&](auto &transform, auto &camera){
        if (!main_camera)
        {
            main_camera = &camera;
            main_camera_transform = &transform;
        }
    });
    assert(main_camera != nullptr);
    main_camera->projection = camera::perspective(main_camera->fov, (float)settings.render_resolution.x/settings.render_resolution.y, main_camera->near_plane, main_camera->far_plane, &main_camera->projection_inverse);

    static float4x4 last_view = main_camera->view;
    static float4x4 last_proj = main_camera->projection;

    auto [global_data, global_offset]       = dynamic_uniform_buffer.allocate<GlobalUniform>(device);
    global_data->camera_view                = main_camera->view;
    global_data->camera_proj                = main_camera->projection;
    global_data->camera_view_inverse        = main_camera->view_inverse;
    global_data->camera_projection_inverse  = main_camera->projection_inverse;
    global_data->camera_previous_view       = last_view;
    global_data->camera_previous_projection = last_proj;
    global_data->camera_position            = float4(main_camera_transform->position, 1.0);
    global_data->vertex_buffer_ptr          = 0;
    global_data->primitive_buffer_ptr       = 0;
    global_data->resolution                 = float2(float(settings.render_resolution.x), float(settings.render_resolution.y));
    global_data->delta_t                    = 0.016f;
    global_data->frame_count                = frame_count;
    global_data->camera_moved               = main_camera->view != last_view || main_camera->projection != last_proj;
    global_data->render_texture_offset      = 0;
    global_data->jitter_offset              = float2(0.0);
    global_data->is_path_tracing            = settings.enable_path_tracing;

    last_view = main_camera->view;
    last_proj = main_camera->projection;

    device.bind_global_uniform_buffer(dynamic_uniform_buffer.buffer, global_offset, sizeof(GlobalUniform));
    device.update_globals();

    // -- Draw frame

    gfx::GraphicsWork cmd = device.get_graphics_work(work_pool);
    cmd.begin();

    // vulkan hack: this command buffer will wait for the image acquire semaphore
    cmd.wait_for_acquired(surface, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    cmd.barrier(hdr_rt.image, gfx::ImageUsage::ColorAttachment);

    VkViewport viewport{};
    viewport.width    = float(settings.render_resolution.x);
    viewport.height   = float(settings.render_resolution.y);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    cmd.set_viewport(viewport);

    VkRect2D scissor = {};
    scissor.extent.width  = settings.render_resolution.x;
    scissor.extent.height = settings.render_resolution.y;
    cmd.set_scissor(scissor);

#if 0

    // Depth prepass
    cmd.barrier(depth_buffer, gfx::ImageUsage::DepthAttachment);
    cmd.barrier(hdr_rt.image, gfx::ImageUsage::ColorAttachment);
    cmd.begin_pass(depth_only_rt.clear_renderpass, depth_only_rt.framebuffer, {depth_buffer}, {{{.float32 = {0.0f, 0.0f, 0.0f, 0.0f}}}});
    cmd.end_pass();

    // Opaque pass
    cmd.barrier(depth_buffer, gfx::ImageUsage::DepthAttachment);
    cmd.barrier(hdr_rt.image, gfx::ImageUsage::ColorAttachment);
    cmd.begin_pass(hdr_rt.clear_renderpass, hdr_rt.framebuffer, {hdr_rt.image, depth_buffer}, {{{.float32 = {0.0f, 0.0f, 0.0f, 1.0f}}}, {{.float32 = {0.0f, 0.0f, 0.0f, 0.0f}}}});
    cmd.end_pass();

    // TAA
    {
        cmd.barrier(depth_buffer, gfx::ImageUsage::ComputeShaderRead);
        cmd.barrier(hdr_rt.image, gfx::ImageUsage::ComputeShaderRead);
        cmd.barrier(history_buffers[(frame_count+1)%2], gfx::ImageUsage::ComputeShaderRead);
        cmd.barrier(history_buffers[(frame_count)%2], gfx::ImageUsage::ComputeShaderReadWrite);

        struct PACKED TaaOptions
        {
            uint sampled_depth_buffer;
            uint sampled_hdr_buffer;
            uint sampled_previous_history;
            uint storage_output_frame;
        };

        auto hdr_buffer_size              = device.get_image_size(hdr_rt.image);
        auto *options                     = this->bind_shader_options<TaaOptions>(cmd, taa);
        options->sampled_depth_buffer     = device.get_image_sampled_index(hdr_rt.depth);
        options->sampled_hdr_buffer       = device.get_image_sampled_index(hdr_rt.image);
        options->sampled_previous_history = device.get_image_sampled_index(history_buffers[(frame_count+1)%2]);
        options->storage_output_frame     = device.get_image_storage_index(history_buffers[frame_count%2]);
        cmd.bind_pipeline(taa);
        cmd.dispatch(dispatch_size(hdr_buffer_size, 16));
    }

    // Build luminance histogram
    {
        cmd.barrier(history_buffers[(frame_count)%2], gfx::ImageUsage::ComputeShaderRead);
        cmd.barrier(tonemap_pass.histogram, gfx::BufferUsage::ComputeShaderReadWrite);

        struct PACKED BuildHistoOptions
        {
            u64 luminance_buffer;
            float min_log_luminance;
            float one_over_log_luminance_range;
            uint sampled_hdr_texture;
        };

        auto *options                = this->bind_shader_options<BuildHistoOptions>(cmd, tonemap_pass.build_histo);
        options->luminance_buffer    = device.get_buffer_address(tonemap_pass.histogram);
        options->sampled_hdr_texture = device.get_image_sampled_index(history_buffers[(frame_count)%2]);
        options->min_log_luminance   = -10.f;
        options->one_over_log_luminance_range = 1.f / 12.f;

        cmd.fill_buffer(tonemap_pass.histogram, 0);

        cmd.bind_storage_buffer(tonemap_pass.build_histo, 1, tonemap_pass.histogram);
        cmd.bind_pipeline(tonemap_pass.build_histo);
        cmd.dispatch(dispatch_size(device.get_image_size(hdr_rt.image), 16));
    }

    // Reduce the histogram to an average value for the tonemapping
    {
        cmd.barrier(tonemap_pass.average_luminance, gfx::ImageUsage::ComputeShaderReadWrite);
        cmd.barrier(tonemap_pass.histogram, gfx::BufferUsage::ComputeShaderReadWrite);

        struct PACKED AverageHistoOptions
        {
            uint  pixel_count;
            float min_log_luminance;
            float log_luminance_range;
            float tau;
            u64 luminance_buffer;
            uint storage_luminance_output;
        };

        auto hdr_image_size = device.get_image_size(hdr_rt.image);

        auto *options                = this->bind_shader_options<AverageHistoOptions>(cmd, tonemap_pass.average_histo);
        options->pixel_count         = hdr_image_size.x * hdr_image_size.y;
        options->min_log_luminance   = -10.f;
        options->log_luminance_range = 12.f;
        options->tau                 = 1.1f;
        options->luminance_buffer    = device.get_buffer_address(tonemap_pass.histogram);
        options->storage_luminance_output = device.get_image_storage_index(tonemap_pass.average_luminance);

        cmd.bind_storage_buffer(tonemap_pass.average_histo, 1, tonemap_pass.histogram);
        cmd.bind_pipeline(tonemap_pass.average_histo);
        cmd.dispatch({1, 1, 1});
    }

    // Tonemap compute
    {
        cmd.barrier(tonemap_pass.average_luminance, gfx::ImageUsage::ComputeShaderRead);
        cmd.clear_barrier(ldr_rt.image, gfx::ImageUsage::ComputeShaderReadWrite);

        auto hdr_buffer_size              = device.get_image_size(history_buffers[(frame_count)%2]);
        auto *options                     = this->bind_shader_options<TonemapOptions>(cmd, tonemap_pass.tonemap);
        *options = tonemap_pass.options;
        options->sampled_hdr_buffer       = device.get_image_sampled_index(history_buffers[(frame_count)%2]);
        options->sampled_luminance_output = device.get_image_sampled_index(tonemap_pass.average_luminance);
        options->storage_output_frame     = device.get_image_storage_index(ldr_rt.image);
        cmd.bind_pipeline(tonemap_pass.tonemap);
        cmd.dispatch(dispatch_size(hdr_buffer_size, 16));

        cmd.barrier(ldr_rt.image, gfx::ImageUsage::GraphicsShaderRead);
    }
#endif

    ImGui::Render();
    if (device.get_fence_value(transfer_done) >= imgui_pass.transfer_done_value)
    {
        do_imgui_pass(*this, cmd, swapchain_rt, imgui_pass, true);
    }

    cmd.barrier(swapchain_rt.image, gfx::ImageUsage::Present);
    cmd.end();

    if (end_frame(cmd))
    {
        on_resize();
        return;
    }
}