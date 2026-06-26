cmake_minimum_required(VERSION 3.16)
project(imgui_api_viewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(IMGUI_API_VIEWER_WARNINGS "Enable extra warnings" ON)

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

find_package(glfw3 CONFIG QUIET)
if (NOT glfw3_FOUND)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GLFW REQUIRED glfw3)
endif()

# ---- ImGui sources (vendored) ----
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui)
set(IMGUI_SOURCES
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

# ---- ImPlot sources (vendored) ----
set(IMPLOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/implot)
set(IMPLOT_SOURCES
  ${IMPLOT_DIR}/implot.cpp
  ${IMPLOT_DIR}/implot_items.cpp
)

add_executable(imgui_api_viewer
  src/main.cpp
  src/telemetry_protocol.cpp
  src/config_manager.cpp
  ${IMGUI_SOURCES}
  ${IMPLOT_SOURCES}
)

target_include_directories(imgui_api_viewer PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${IMGUI_DIR}
  ${IMGUI_DIR}/backends
  ${IMPLOT_DIR}
)

# ---- Link deps ----
if (glfw3_FOUND)
  target_link_libraries(imgui_api_viewer PRIVATE glfw OpenGL::GL Threads::Threads)
else()
  target_include_directories(imgui_api_viewer PRIVATE ${GLFW_INCLUDE_DIRS})
  target_link_libraries(imgui_api_viewer PRIVATE ${GLFW_LIBRARIES} OpenGL::GL Threads::Threads)
  target_compile_options(imgui_api_viewer PRIVATE ${GLFW_CFLAGS_OTHER})
endif()

# Linux often needs libdl explicitly for OpenGL loaders / backends
if (UNIX AND NOT APPLE)
  target_link_libraries(imgui_api_viewer PRIVATE dl)
endif()

if (APPLE)
  target_link_libraries(imgui_api_viewer PRIVATE
    "-framework Cocoa"
    "-framework IOKit"
    "-framework CoreVideo"
    "-framework OpenGL"
  )
endif()

if (WIN32)
  target_link_libraries(imgui_api_viewer PRIVATE opengl32)
endif()

if (IMGUI_API_VIEWER_WARNINGS)
  if (MSVC)
    target_compile_options(imgui_api_viewer PRIVATE /W4 /permissive-)
  else()
    target_compile_options(imgui_api_viewer PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()
