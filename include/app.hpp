#pragma once
#include "camera.hpp"
#include "renderer/renderer.hpp"
#include "timer.hpp"
#include "window.hpp"

namespace my_app
{

class App
{
  public:
    App();
    ~App();

    NO_COPY_NO_MOVE(App)

    void run();

  private:
    void draw_fps();
    void camera_update();
    void update();

    Window window;
    InputCamera camera;
    Renderer renderer;
    TimerData timer;
};

} // namespace my_app
