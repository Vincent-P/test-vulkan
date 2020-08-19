#pragma once

#include "types.hpp"
#include <glm/gtc/quaternion.hpp>

namespace my_app
{

namespace UI
{
    struct Context;
};

class Window;
class TimerData;

struct Camera
{
    float3 position;
    float3 front;
    float3 up;

    float pitch;
    float yaw;
    float roll;
    glm::quat rotation;

    float4x4 view;
    float4x4 projection;

    float4x4 update_view();
    [[nodiscard]] inline float4x4 get_view() const { return view; }
    [[nodiscard]] inline float4x4 get_projection() const { return projection; }

    float4x4 perspective(float fov, float aspect_ratio, float near_plane, float far_plane);
    float4x4 ortho_square(float size, float near_plane, float far_plane);

    static Camera create(float3 position);
};

struct InputCamera
{
    Camera _internal;
    Window *p_window;
    UI::Context *p_ui;
    TimerData *p_timer;

    double last_xpos;
    double last_ypos;

    static InputCamera create(Window &window, TimerData& timer, UI::Context &ui, float3 position);
    void on_mouse_movement(double xpos, double ypos);
    void update();
};

} // namespace my_app
