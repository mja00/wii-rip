cmake_minimum_required(VERSION 3.10)
project(wii-banner-render)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")

include_directories(Externals/dolphin-emu/Source)

# GLEW must be compiled from source with EGL support so glewInit() works without
# an X display. The system libGLEW.so is built in GLX mode; using it inside a
# headless EGL context causes glewInit() to fail with "No GLX display".
#
# GLEW lives outside the source tree (downloaded into the build directory by
# packaging/build_banner_render.sh) so its location is supplied via cache var.
set(GLEW_SOURCE_DIR "" CACHE PATH "Path to a GLEW source release (containing src/glew.c)")
if(NOT GLEW_SOURCE_DIR OR NOT EXISTS "${GLEW_SOURCE_DIR}/src/glew.c")
    message(FATAL_ERROR "GLEW_SOURCE_DIR must point at a GLEW source release. "
                        "packaging/build_banner_render.sh handles this automatically.")
endif()

add_library(glew_egl STATIC "${GLEW_SOURCE_DIR}/src/glew.c")
target_include_directories(glew_egl PUBLIC "${GLEW_SOURCE_DIR}/include")
target_compile_definitions(glew_egl PUBLIC GLEW_EGL GLEW_STATIC)

# Find EGL (required for headless offscreen rendering)
find_library(EGL_LIBRARY NAMES EGL REQUIRED)
if(NOT EGL_LIBRARY)
    message(FATAL_ERROR "EGL library not found. Install libegl-dev.")
endif()

target_link_libraries(glew_egl PUBLIC ${EGL_LIBRARY} GL)

add_executable(wii-banner-render
    Externals/dolphin-emu/Source/Blob.cpp
    Externals/dolphin-emu/Source/FileBlob.cpp
    Externals/dolphin-emu/Source/FileHandlerARC.cpp
    Externals/dolphin-emu/Source/TextureDecoder.cpp

    Source/Animator.cpp
    Source/Banner.cpp
    Source/Endian.cpp
    Source/Font.cpp
    Source/Layout.cpp
    Source/Main.cpp
    Source/Material.cpp
    Source/Pane.cpp
    Source/Picture.cpp
    Source/Sound.cpp
    Source/Textbox.cpp
    Source/Texture.cpp
    Source/Window.cpp
    Source/WrapGx.cpp
)

# Tell our own code to expect EGL-mode GLEW headers.
target_compile_definitions(wii-banner-render PRIVATE GLEW_EGL GLEW_STATIC)

# Older source files are missing <cmath> includes that newer GCC requires explicitly.
target_compile_options(wii-banner-render PRIVATE -include cmath)

target_link_libraries(wii-banner-render
    glew_egl
    ${EGL_LIBRARY}
    GL
    dl
    pthread
)
