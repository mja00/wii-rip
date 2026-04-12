cmake_minimum_required(VERSION 3.10)
project(wii-banner-render)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")

include_directories(Externals/dolphin-emu/Source)

# Build GLEW from source so we can control its build mode per platform.  On
# Linux we compile it with GLEW_EGL so glewInit() works without an X display
# (the system libGLEW.so is built in GLX mode and fails inside a headless EGL
# context).  On macOS there is no EGL/GLX at all; we build the same vendored
# GLEW source as a plain desktop-GL static library and link Apple's OpenGL
# framework.  Externals/glew is downloaded by build_banner_render.sh before
# cmake runs.
add_library(glew_local STATIC Externals/glew/src/glew.c)
target_include_directories(glew_local PUBLIC Externals/glew/include)
target_compile_definitions(glew_local PUBLIC GLEW_STATIC)

if(APPLE)
    # Apple's OpenGL framework provides desktop GL (legacy 2.1 profile) plus
    # CGL, which Main.cpp uses for headless context creation.  GL is
    # deprecated on macOS but still functional via Apple's GL-on-Metal driver.
    find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
    target_link_libraries(glew_local PUBLIC ${OPENGL_FRAMEWORK})
else()
    target_compile_definitions(glew_local PUBLIC GLEW_EGL)

    # Find EGL (required for headless offscreen rendering on Linux)
    find_library(EGL_LIBRARY NAMES EGL REQUIRED)
    if(NOT EGL_LIBRARY)
        message(FATAL_ERROR "EGL library not found. Install libegl-dev.")
    endif()
    target_link_libraries(glew_local PUBLIC ${EGL_LIBRARY} GL)
endif()

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
    Source/TextRenderer.cpp
    Source/Texture.cpp
    Source/Window.cpp
    Source/WrapGx.cpp
)

# stb_truetype.h is consumed by Source/TextRenderer.cpp; expose its include
# path so the implementation can pick it up. The header is single-file
# public-domain code.
target_include_directories(wii-banner-render PRIVATE Externals/stb)

# Older source files are missing <cmath> includes that newer GCC requires explicitly.
target_compile_options(wii-banner-render PRIVATE -include cmath)

# glew_local is PUBLIC about its compile definitions and link dependencies,
# so consumers inherit GLEW_STATIC plus the right windowing library / EGL+GL
# (Linux) or OpenGL.framework (macOS) automatically.
target_link_libraries(wii-banner-render glew_local)

if(NOT APPLE)
    # Linux: pthread + dl are needed by glew/Dolphin sources; on macOS they
    # are part of libSystem and do not need to be linked explicitly.
    target_link_libraries(wii-banner-render dl pthread)
endif()
