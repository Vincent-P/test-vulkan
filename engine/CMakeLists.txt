set(SOURCE_FILES
  src/asset_manager.cpp
  src/main.cpp
  src/app.cpp
  src/camera.cpp
  src/ecs.cpp
  src/scene.cpp
  src/glb.cpp
  src/inputs.cpp
  src/tools.cpp
  src/ui.cpp
  src/render/renderer.cpp
  src/render/render_timings.cpp
  src/render/base_renderer.cpp
  src/render/ring_buffer.cpp
  src/render/streamer.cpp
  src/render/vulkan/context.cpp
  src/render/vulkan/commands.cpp
  src/render/vulkan/descriptor_set.cpp
  src/render/vulkan/bindless_set.cpp
  src/render/vulkan/image.cpp
  src/render/vulkan/buffer.cpp
  src/render/vulkan/device.cpp
  src/render/vulkan/shader.cpp
  src/render/vulkan/surface.cpp
  src/render/vulkan/graphics_pipeline.cpp
  src/render/vulkan/compute_pipeline.cpp)

add_executable(engine ${SOURCE_FILES})
install(TARGETS engine RUNTIME)

set_target_properties(engine PROPERTIES CXX_STANDARD 20)
target_compile_options(engine PRIVATE ${APP_CXX_FLAGS})

target_include_directories(engine PRIVATE src)
target_include_directories(engine SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party)

target_link_libraries(engine
  exo
  cross
  imgui
  simdjson
  fmt
  stb_image
  meshopt
  vma
  Vulkan::Vulkan
  Threads::Threads)

add_subdirectory(shaders/)
add_dependencies(engine shaders)
