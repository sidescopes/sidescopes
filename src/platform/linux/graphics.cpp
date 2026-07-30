// OpenGL rendering backend for the shared application shell, as on Windows.
// GLFW owns the context; the texture uploads only need OpenGL 1.1 entry
// points, which the GLX/EGL libraries export directly - no loader.

#include <GL/gl.h>

#include <cstdlib>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "platform/graphics.h"

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
        // Swap-interval pacing: unlike Windows, whose compositor tick is
        // waited on explicitly, the GLX/EGL swap blocks honestly here, and
        // the frame loop's own redraw cap keeps the rate bounded where a
        // headless server never blocks.
        glfwSwapInterval(1);
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
    }

    void* nativeWindowHandle() const override
    {
        // No native handle crosses the seam here: the Linux desktop services
        // never address the window by identity, so the GLFW handle itself is
        // the most useful thing rememberApplicationWindow can keep.
        return m_window;
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

void setPlatformInitHints()
{
    // Prefer X11 (XWayland on a Wayland desktop) while a display offers one:
    // GLFW's Wayland backend can neither read nor place windows, and the
    // saved-session placement depends on both. Pure-Wayland sessions without
    // XWayland fall through to the native backend, whose placement simply
    // stays wherever the compositor put it.
    if (std::getenv("DISPLAY") != nullptr) {
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    }
}

}  // namespace sidescopes
