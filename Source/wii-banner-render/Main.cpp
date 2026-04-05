/*
Copyright (c) 2010 - Wii Banner Player Project

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
claim that you wrote the original software. If you use this software
in a product, an acknowledgment in the product documentation would be
appreciated but is not required.

2. Altered source versions must be plainly marked as such, and must not be
misrepresented as being the original software.

3. This notice may not be removed or altered from any source
distribution.
*/

// NOTE: Altered source version of Main.cpp from the wii-banner-player project.
// The original was an interactive SFML windowed banner viewer. This version
// replaces the windowed renderer with a headless EGL offscreen context, steps
// through the banner animation frame-by-frame, captures each frame via
// glReadPixels, and pipes raw RGBA frames to ffmpeg for video encoding.

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <EGL/egl.h>
#include <GL/glew.h>

#include "Banner.h"
#include "Types.h"
#include "WrapGx.h"

static const int BANNER_WIDTH  = 608;
static const int BANNER_HEIGHT = 456;
static const float BANNER_ASPECT = static_cast<float>(BANNER_WIDTH) / BANNER_HEIGHT;
// Matches the crop factor used by the original wii-banner-player:
// slightly zooms in to trim 6px off each side of the 608px-wide banner.
static const float BANNER_CROP = static_cast<float>(BANNER_WIDTH) / (BANNER_WIDTH - 12);

// ---------------------------------------------------------------------------
// EGL headless context
// ---------------------------------------------------------------------------

struct EGLState
{
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;

    ~EGLState()
    {
        if (display != EGL_NO_DISPLAY)
        {
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (surface != EGL_NO_SURFACE)
                eglDestroySurface(display, surface);
            if (context != EGL_NO_CONTEXT)
                eglDestroyContext(display, context);
            eglTerminate(display);
        }
    }
};

static bool init_egl(EGLState& egl)
{
    egl.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl.display == EGL_NO_DISPLAY)
    {
        std::cerr << "wii-banner-render: eglGetDisplay failed\n";
        return false;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(egl.display, &major, &minor))
    {
        std::cerr << "wii-banner-render: eglInitialize failed (error 0x"
                  << std::hex << eglGetError() << ")\n";
        return false;
    }

    if (!eglBindAPI(EGL_OPENGL_API))
    {
        std::cerr << "wii-banner-render: eglBindAPI(EGL_OPENGL_API) failed\n";
        return false;
    }

    static const EGLint CONFIG_ATTRIBS[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,      24,
        EGL_STENCIL_SIZE,    8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };

    EGLConfig config;
    EGLint num_configs = 0;
    if (!eglChooseConfig(egl.display, CONFIG_ATTRIBS, &config, 1, &num_configs)
        || num_configs == 0)
    {
        std::cerr << "wii-banner-render: eglChooseConfig failed\n";
        return false;
    }

    static const EGLint PBUFFER_ATTRIBS[] = {
        EGL_WIDTH,  BANNER_WIDTH,
        EGL_HEIGHT, BANNER_HEIGHT,
        EGL_NONE
    };

    egl.surface = eglCreatePbufferSurface(egl.display, config, PBUFFER_ATTRIBS);
    if (egl.surface == EGL_NO_SURFACE)
    {
        std::cerr << "wii-banner-render: eglCreatePbufferSurface failed\n";
        return false;
    }

    egl.context = eglCreateContext(egl.display, config, EGL_NO_CONTEXT, nullptr);
    if (egl.context == EGL_NO_CONTEXT)
    {
        std::cerr << "wii-banner-render: eglCreateContext failed\n";
        return false;
    }

    if (!eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.context))
    {
        std::cerr << "wii-banner-render: eglMakeCurrent failed\n";
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

static void print_usage(const char* prog)
{
    std::cerr
        << "Usage: " << prog << " <input.bnr> -o <output.mp4> [OPTIONS]\n\n"
        << "Render a Wii disc channel banner animation to a video file.\n\n"
        << "Options:\n"
        << "  -o, --output <path>   Output video file (required)\n"
        << "      --fps <n>         Frame rate (default: 60)\n"
        << "      --loops <n>       Number of animation loop cycles (default: 1)\n"
        << "      --ffmpeg <path>   Path to ffmpeg binary (default: ffmpeg)\n"
        << "      --language <lang> Banner language code, e.g. ENG, JPN (default: ENG)\n"
        << "  -h, --help            Show this help\n";
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    std::string input_path;
    std::string output_path;
    std::string ffmpeg_path = "ffmpeg";
    std::string language    = "ENG";
    int fps   = 60;
    int loops = 1;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];

        if ((arg == "-o" || arg == "--output") && i + 1 < argc)
        {
            output_path = argv[++i];
        }
        else if (arg == "--fps" && i + 1 < argc)
        {
            fps = std::atoi(argv[++i]);
            if (fps <= 0)
            {
                std::cerr << "wii-banner-render: invalid --fps value\n";
                return 1;
            }
        }
        else if (arg == "--loops" && i + 1 < argc)
        {
            loops = std::atoi(argv[++i]);
            if (loops <= 0)
            {
                std::cerr << "wii-banner-render: invalid --loops value\n";
                return 1;
            }
        }
        else if (arg == "--ffmpeg" && i + 1 < argc)
        {
            ffmpeg_path = argv[++i];
        }
        else if (arg == "--language" && i + 1 < argc)
        {
            language = argv[++i];
        }
        else if (arg == "-h" || arg == "--help")
        {
            print_usage(argv[0]);
            return 0;
        }
        else if (!arg.empty() && arg[0] != '-' && input_path.empty())
        {
            input_path = arg;
        }
        else
        {
            std::cerr << "wii-banner-render: unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (input_path.empty())
    {
        std::cerr << "wii-banner-render: missing input file\n";
        print_usage(argv[0]);
        return 1;
    }
    if (output_path.empty())
    {
        std::cerr << "wii-banner-render: missing -o output path\n";
        print_usage(argv[0]);
        return 1;
    }

    // ------------------------------------------------------------------
    // Initialize EGL offscreen OpenGL context
    // ------------------------------------------------------------------

    EGLState egl;
    if (!init_egl(egl))
        return 1;

    // Initialize GLEW against the active EGL context.
    glewExperimental = GL_TRUE;
    const GLenum glew_err = glewInit();
    if (glew_err != GLEW_OK)
    {
        std::cerr << "wii-banner-render: glewInit failed: "
                  << glewGetErrorString(glew_err) << "\n";
        return 1;
    }

    // GX_Init sets up the GX-over-OpenGL shim used by the banner renderer.
    GX_Init(nullptr, 0);

    // Layout::Render() assumes the viewer has already established a normalized
    // top-left-origin orthographic projection.
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, 1.0, 1.0, 0.0, -1000.0, 1000.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // Set GL state expected by the layout renderer.
    glEnable(GL_STENCIL_TEST);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glViewport(0, 0, BANNER_WIDTH, BANNER_HEIGHT);

    // ------------------------------------------------------------------
    // Load banner
    // ------------------------------------------------------------------

    WiiBanner::Banner banner(input_path);
    banner.LoadBanner();

    WiiBanner::Layout* const layout = banner.GetBanner();
    if (!layout)
    {
        std::cerr << "wii-banner-render: failed to load banner from: " << input_path << "\n"
                  << "  The file may not contain a valid opening.bnr banner section.\n";
        return 1;
    }

    layout->SetLanguage(language);

    // ------------------------------------------------------------------
    // Determine frame count
    // ------------------------------------------------------------------

    const int loop_start  = static_cast<int>(layout->GetLoopStart());
    const int loop_length = static_cast<int>(layout->GetLoopEnd()) - loop_start;

    int frames_to_render;
    if (loop_length > 0)
        frames_to_render = loop_start + loop_length * loops;
    else
        frames_to_render = std::max(loop_start, 1);

    std::cerr << "wii-banner-render: " << BANNER_WIDTH << "x" << BANNER_HEIGHT
              << ", " << frames_to_render << " frames"
              << " (start=" << loop_start << ", loop=" << loop_length << "x" << loops << ")"
              << " at " << fps << " fps"
              << " (" << static_cast<float>(frames_to_render) / fps << "s)\n";

    // ------------------------------------------------------------------
    // Launch ffmpeg to encode raw RGBA frames into a video file.
    //
    // Raw frames are piped to ffmpeg's stdin. The vflip filter corrects
    // for OpenGL's bottom-left row order (glReadPixels returns rows
    // bottom-to-top, video expects top-to-bottom).
    // ------------------------------------------------------------------

    const std::string ffmpeg_cmd =
        "\"" + ffmpeg_path + "\""
        " -f rawvideo"
        " -pix_fmt rgba"
        " -s " + std::to_string(BANNER_WIDTH) + "x" + std::to_string(BANNER_HEIGHT) +
        " -r " + std::to_string(fps) +
        " -i pipe:0"
        " -vf vflip"
        " -c:v libx264"
        " -pix_fmt yuv420p"
        " -loglevel warning"
        " -y"
        " \"" + output_path + "\" 2>&1 >&2";

    FILE* ffmpeg = popen(ffmpeg_cmd.c_str(), "w");
    if (!ffmpeg)
    {
        std::cerr << "wii-banner-render: failed to launch ffmpeg: "
                  << strerror(errno) << "\n";
        std::cerr << "  command: " << ffmpeg_cmd << "\n";
        return 1;
    }

    // ------------------------------------------------------------------
    // Render loop
    // ------------------------------------------------------------------

    std::vector<uint8_t> pixels(BANNER_WIDTH * BANNER_HEIGHT * 4);

    // Initialise to frame 0 then advance through the animation using
    // AdvanceFrame(), which handles the start->loop transition correctly.
    layout->SetFrame(0.0f);

    bool ffmpeg_ok = true;
    for (int f = 0; f < frames_to_render; ++f)
    {
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        glPushAttrib(GL_ALL_ATTRIB_BITS);
        layout->Render(BANNER_ASPECT, BANNER_CROP);
        glPopAttrib();

        glReadPixels(0, 0, BANNER_WIDTH, BANNER_HEIGHT,
                     GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

        if (fwrite(pixels.data(), 1, pixels.size(), ffmpeg) != pixels.size())
        {
            std::cerr << "wii-banner-render: pipe to ffmpeg closed early at frame " << f << "\n";
            ffmpeg_ok = false;
            break;
        }

        layout->AdvanceFrame();
    }

    const int ffmpeg_ret = pclose(ffmpeg);
    if (!ffmpeg_ok || ffmpeg_ret != 0)
    {
        std::cerr << "wii-banner-render: ffmpeg exited with code " << ffmpeg_ret << "\n";
        return 1;
    }

    std::cerr << "wii-banner-render: wrote " << output_path << "\n";
    return 0;
}
