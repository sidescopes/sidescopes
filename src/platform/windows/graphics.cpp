// OpenGL rendering backend for the shared application shell. OpenGL is
// healthy on Windows, GLFW owns the context, and the texture uploads only
// need functions exported by opengl32.dll since OpenGL 1.1 - no loader.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// After windows.h: dwmapi.h leans on the base types it defines.
#include <dwmapi.h>

// After windows.h: gl.h leans on the calling-convention macros it defines.
#include <GL/gl.h>

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "platform/desktop.h"
#include "platform/graphics.h"
#include "platform/windows/capture_visibility.h"

// Windows 10 2004; absent from older SDKs.
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace sidescopes {
namespace {

class OpenGlScopeTexture final : public ScopeTexture
{
public:
    OpenGlScopeTexture(int width, int height)
        : m_width(width),
          m_height(height)
    {
        glGenTextures(1, &m_texture);
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }

    ~OpenGlScopeTexture() override
    {
        glDeleteTextures(1, &m_texture);
    }

    void upload(const ScopeImage& image) override
    {
        // A scope just toggled on can race one worker pass: the fetched
        // output predates the toggle and carries an empty image for it.
        if (image.rgba.size() < static_cast<std::size_t>(m_width) * m_height * 4) {
            return;
        }
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
    }

    [[nodiscard]] ImTextureID textureId() const override
    {
        return static_cast<ImTextureID>(m_texture);
    }

    [[nodiscard]] int width() const override
    {
        return m_width;
    }

    [[nodiscard]] int height() const override
    {
        return m_height;
    }

private:
    int m_width;
    int m_height;
    GLuint m_texture = 0;
};

class OpenGlGraphics final : public GraphicsBackend
{
public:
    void setWindowHints() override
    {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    }

    bool init(GLFWwindow* window) override
    {
        m_window = window;
        glfwMakeContextCurrent(window);
        // Frame pacing comes from DwmFlush in EndFrame, not the swap
        // interval: measured on this pipeline (NVIDIA, DWM-composited
        // window), SwapBuffers with interval 1 never blocks, the frame
        // loop runs uncapped, and a whole core burns whenever anything
        // animates. The compositor tick is the honest windowed-mode
        // vblank, and waiting on it costs no CPU.
        glfwSwapInterval(0);
        // The scope window must never reach its own scopes: duplication
        // has no application-level capture exclusion, so the window
        // excludes itself unless the visibility toggle holds. Best
        // effort: unsupported before Windows 10 2004, where the
        // analysis-side masking still applies.
        SetWindowDisplayAffinity(glfwGetWin32Window(window),
                                 captureWindowsVisible() ? WDA_NONE : WDA_EXCLUDEFROMCAPTURE);
        if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) {
            return false;
        }
        if (!ImGui_ImplOpenGL3_Init("#version 150")) {
            ImGui_ImplGlfw_Shutdown();
            return false;
        }
        return true;
    }

    void shutdown() override
    {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
    }

    std::unique_ptr<ScopeTexture> createScopeTexture(int width, int height) override
    {
        return std::make_unique<OpenGlScopeTexture>(width, height);
    }

    bool beginFrame(int framebufferWidth, int framebufferHeight) override
    {
        m_framebufferWidth = framebufferWidth;
        m_framebufferHeight = framebufferHeight;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        return true;
    }

    void endFrame() override
    {
        glViewport(0, 0, m_framebufferWidth, m_framebufferHeight);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(m_window);
        // Composition-tick pacing; see Init. Failure means the compositor
        // is gone (a remote session, say) - render unpaced rather than
        // not at all.
        DwmFlush();
    }

    void* nativeWindowHandle() const override
    {
        return glfwGetWin32Window(m_window);
    }

private:
    GLFWwindow* m_window = nullptr;
    int m_framebufferWidth = 0;
    int m_framebufferHeight = 0;
};

}  // namespace

std::unique_ptr<GraphicsBackend> createGraphicsBackend()
{
    return std::make_unique<OpenGlGraphics>();
}

}  // namespace sidescopes
