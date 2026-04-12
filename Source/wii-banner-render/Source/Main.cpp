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
// The original was an interactive SFML windowed banner viewer. This version:
//   - Replaces the windowed renderer with a headless OpenGL offscreen
//     context (EGL pbuffer on Linux, CGL legacy 2.1 + EXT_framebuffer_object
//     FBO on macOS).
//   - Steps through the banner animation frame-by-frame, captures each
//     frame via glReadPixels, and pipes raw RGBA frames to ffmpeg for H.264
//     encoding.
//   - Adds CLI flags: --output, --aspect, --fps, --loops, --ffmpeg,
//     --language, and --font-archive (forwarded to Banner so the user can
//     point at an extracted Wii system font archive when one is available).

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#ifdef __APPLE__
// Apple deprecated desktop OpenGL in 10.14 in favour of Metal, but the legacy
// 2.1 profile we use here is still functional via Apple's GL-on-Metal driver
// (and is the only profile that supports the fixed-function pipeline that the
// upstream wii-banner-player rendering code relies on).  Silence the noisy
// deprecation warnings produced by every CGL/GL header include.
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>  // CGL
#else
#include <EGL/egl.h>
#endif

#include <GL/glew.h>

#include "Banner.h"
#include "Types.h"
#include "WrapGx.h"

static const int BANNER_HEIGHT = 456;
static const int BANNER_WIDTH_4X3 = 608;
static const int BANNER_WIDTH_16X9 = 810;
// Matches the crop factor used by the original wii-banner-player:
// slightly zooms in to trim 6px off each side of the 608px-wide banner.
static const float BANNER_CROP = static_cast<float>(BANNER_WIDTH_4X3) / (BANNER_WIDTH_4X3 - 12);

enum class RenderAspect
{
    Standard,
    Widescreen,
};

struct RenderProfile
{
    int width;
    int height;
    float layout_aspect;
    const char* ffmpeg_aspect;
};

static RenderProfile get_render_profile(RenderAspect aspect)
{
    switch (aspect)
    {
    case RenderAspect::Standard:
        return {BANNER_WIDTH_4X3, BANNER_HEIGHT, 4.f / 3.f, "4:3"};
    case RenderAspect::Widescreen:
        return {BANNER_WIDTH_16X9, BANNER_HEIGHT, 16.f / 9.f, "16:9"};
    }

    return {BANNER_WIDTH_4X3, BANNER_HEIGHT, 4.f / 3.f, "4:3"};
}

// ---------------------------------------------------------------------------
// Headless OpenGL context
//
// Linux: an EGL pbuffer surface backs the default framebuffer.
// macOS: a CGL context with no drawable plus an FBO + renderbuffers.
//        Apple Silicon Macs no longer ship EGL, but the legacy 2.1 profile
//        is still supported by Apple's GL-on-Metal driver, which is enough
//        for the fixed-function rendering done by wii-banner-player.
// ---------------------------------------------------------------------------

#ifdef __APPLE__

struct GLState
{
    CGLContextObj context = nullptr;
    GLuint fbo = 0;
    GLuint color_rb = 0;
    GLuint depth_stencil_rb = 0;

    ~GLState()
    {
        if (context)
        {
            // Tear down the FBO objects before destroying the context they
            // belong to.  Each delete is a no-op when the name is 0.
            if (fbo)
                glDeleteFramebuffersEXT(1, &fbo);
            if (color_rb)
                glDeleteRenderbuffersEXT(1, &color_rb);
            if (depth_stencil_rb)
                glDeleteRenderbuffersEXT(1, &depth_stencil_rb);

            CGLSetCurrentContext(nullptr);
            CGLDestroyContext(context);
        }
    }
};

static bool init_gl_context(GLState& gl)
{
    CGLPixelFormatAttribute attribs[] = {
        kCGLPFAAccelerated,
        kCGLPFAColorSize,    static_cast<CGLPixelFormatAttribute>(24),
        kCGLPFAAlphaSize,    static_cast<CGLPixelFormatAttribute>(8),
        kCGLPFADepthSize,    static_cast<CGLPixelFormatAttribute>(24),
        kCGLPFAStencilSize,  static_cast<CGLPixelFormatAttribute>(8),
        kCGLPFAOpenGLProfile,
            static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_Legacy),
        static_cast<CGLPixelFormatAttribute>(0)
    };

    CGLPixelFormatObj pix = nullptr;
    GLint npix = 0;
    CGLError err = CGLChoosePixelFormat(attribs, &pix, &npix);
    if (err != kCGLNoError || pix == nullptr)
    {
        std::cerr << "wii-banner-render: CGLChoosePixelFormat failed: "
                  << CGLErrorString(err) << "\n";
        return false;
    }

    err = CGLCreateContext(pix, nullptr, &gl.context);
    CGLDestroyPixelFormat(pix);
    if (err != kCGLNoError || gl.context == nullptr)
    {
        std::cerr << "wii-banner-render: CGLCreateContext failed: "
                  << CGLErrorString(err) << "\n";
        return false;
    }

    err = CGLSetCurrentContext(gl.context);
    if (err != kCGLNoError)
    {
        std::cerr << "wii-banner-render: CGLSetCurrentContext failed: "
                  << CGLErrorString(err) << "\n";
        return false;
    }

    return true;
}

static bool init_offscreen_fbo(GLState& gl, int width, int height)
{
    // Must be called *after* glewInit(), since the EXT_framebuffer_object
    // entry points (glGenFramebuffersEXT etc.) are loaded by GLEW.
    glGenFramebuffersEXT(1, &gl.fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gl.fbo);

    glGenRenderbuffersEXT(1, &gl.color_rb);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, gl.color_rb);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, width, height);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                                 GL_RENDERBUFFER_EXT, gl.color_rb);

    // Packed depth+stencil renderbuffer; the same name is attached to both
    // the depth and stencil attachment points so glClear with
    // GL_STENCIL_BUFFER_BIT and the layout renderer's stencil ops both work.
    glGenRenderbuffersEXT(1, &gl.depth_stencil_rb);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, gl.depth_stencil_rb);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT,
                             width, height);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT,
                                 GL_RENDERBUFFER_EXT, gl.depth_stencil_rb);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT,
                                 GL_RENDERBUFFER_EXT, gl.depth_stencil_rb);

    const GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    if (status != GL_FRAMEBUFFER_COMPLETE_EXT)
    {
        std::cerr << "wii-banner-render: offscreen framebuffer is incomplete"
                     " (status 0x" << std::hex << status << ")\n";
        return false;
    }

    return true;
}

#else  // !__APPLE__

struct GLState
{
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;

    ~GLState()
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

static bool init_gl_context(GLState& egl, int width, int height)
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

    EGLint pbuffer_attribs[] = {
        EGL_WIDTH,  width,
        EGL_HEIGHT, height,
        EGL_NONE
    };

    egl.surface = eglCreatePbufferSurface(egl.display, config, pbuffer_attribs);
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

#endif  // __APPLE__

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

static void print_usage(const char* prog)
{
    std::cerr
        << "Usage: " << prog << " <input.bnr> -o <output.mp4> [OPTIONS]\n\n"
        << "Render a Wii disc channel banner animation to a video file.\n\n"
        << "Options:\n"
        << "  -o, --output <path>     Output video file (required)\n"
        << "      --aspect <mode>     Banner aspect ratio: 4:3 or 16:9 (default: 4:3)\n"
        << "      --fps <n>           Frame rate (default: 60)\n"
        << "      --loops <n>         Number of animation loop cycles (default: 1)\n"
        << "      --ffmpeg <path>     Path to ffmpeg binary (default: ffmpeg)\n"
        << "      --language <lang>   Banner language code, e.g. ENG, JPN (default: ENG)\n"
        << "      --font-archive <p>  Optional path to a Wii system font archive\n"
        << "                          (e.g. an extracted 00000003.app from a Wii NAND).\n"
        << "                          When omitted, banner text is rendered using the\n"
        << "                          bundled Roboto Regular TTF instead of the real\n"
        << "                          Wii system fonts.\n"
        << "  -h, --help              Show this help\n";
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    std::string input_path;
    std::string output_path;
    std::string ffmpeg_path  = "ffmpeg";
    std::string language     = "ENG";
    std::string font_archive;  // optional Wii system font archive
    RenderAspect render_aspect = RenderAspect::Standard;
    int fps   = 60;
    int loops = 1;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];

        if ((arg == "-o" || arg == "--output") && i + 1 < argc)
        {
            output_path = argv[++i];
        }
        else if (arg == "--aspect" && i + 1 < argc)
        {
            const std::string aspect = argv[++i];
            if (aspect == "4:3")
                render_aspect = RenderAspect::Standard;
            else if (aspect == "16:9")
                render_aspect = RenderAspect::Widescreen;
            else
            {
                std::cerr << "wii-banner-render: invalid --aspect value: " << aspect << "\n";
                return 1;
            }
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
        else if (arg == "--font-archive" && i + 1 < argc)
        {
            font_archive = argv[++i];
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
    // Initialize the headless OpenGL context (EGL on Linux, CGL on macOS)
    // ------------------------------------------------------------------

    const RenderProfile profile = get_render_profile(render_aspect);

    GLState gl_state;
#ifdef __APPLE__
    if (!init_gl_context(gl_state))
        return 1;
#else
    if (!init_gl_context(gl_state, profile.width, profile.height))
        return 1;
#endif

    // Initialize GLEW against the now-current context.
    glewExperimental = GL_TRUE;
    const GLenum glew_err = glewInit();
    if (glew_err != GLEW_OK)
    {
        std::cerr << "wii-banner-render: glewInit failed: "
                  << glewGetErrorString(glew_err) << "\n";
        return 1;
    }

#ifdef __APPLE__
    // CGL contexts have no default drawable; we render into an FBO instead.
    // glReadPixels then reads from GL_COLOR_ATTACHMENT0 of the bound FBO.
    if (!init_offscreen_fbo(gl_state, profile.width, profile.height))
        return 1;
#endif

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
    glViewport(0, 0, profile.width, profile.height);

    // ------------------------------------------------------------------
    // Load banner
    // ------------------------------------------------------------------

    WiiBanner::Banner banner(input_path, font_archive);
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

    std::cerr << "wii-banner-render: " << profile.width << "x" << profile.height
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
        " -s " + std::to_string(profile.width) + "x" + std::to_string(profile.height) +
        " -r " + std::to_string(fps) +
        " -i pipe:0"
        " -vf vflip"
        " -c:v libx264"
        " -pix_fmt yuv420p"
        " -aspect " + std::string(profile.ffmpeg_aspect) +
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

    std::vector<uint8_t> pixels(profile.width * profile.height * 4);

    // Initialise to frame 0 then advance through the animation using
    // AdvanceFrame(), which handles the start->loop transition correctly.
    layout->SetFrame(0.0f);

    bool ffmpeg_ok = true;
    for (int f = 0; f < frames_to_render; ++f)
    {
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        glPushAttrib(GL_ALL_ATTRIB_BITS);
        layout->Render(profile.layout_aspect, BANNER_CROP);
        glPopAttrib();

        glReadPixels(0, 0, profile.width, profile.height,
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
