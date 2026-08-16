// WebGL rendering backend for the browser lab.
//
// Emscripten's GLFW port owns the WebGL2 context, and Dear ImGui's own
// GLFW and OpenGL3 backends drive it — the same two backends the Windows
// build uses, against OpenGL ES 3.0 instead of desktop OpenGL. The texture
// uploads need nothing beyond core GLES3, so there is no loader here
// either.
//
// The scope window has no self-capture problem to solve in a browser and no
// native handle to hand back, so both of those seams are simpler than their
// desktop counterparts rather than absent.

#include <GLES3/gl3.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "platform/graphics.h"

namespace sidescopes {
namespace {

class WebGlScopeTexture final : public ScopeTexture
{
public:
    WebGlScopeTexture(int width, int height)
        : m_width(width),
          m_height(height)
    {
        glGenTextures(1, &m_texture);
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // WebGL is strict where desktop GL is forgiving: a texture sampled
        // at a non-power-of-two size must clamp, or it samples as black.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }

    ~WebGlScopeTexture() override
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

class WebGlGraphics final : public GraphicsBackend
{
public:
    void setWindowHints() override
    {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    }

    bool init(GLFWwindow* window) override
    {
        m_window = window;
        glfwMakeContextCurrent(window);
        // The browser paces presentation through requestAnimationFrame, so
        // asking GLFW for a swap interval as well would be a second cap on
        // top of the compositor's own.
        glfwSwapInterval(1);
        if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) {
            return false;
        }
        // WebGL2 speaks GLSL ES 3.00; the desktop "#version 150" string
        // would compile to nothing here.
        if (!ImGui_ImplOpenGL3_Init("#version 300 es")) {
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
        return std::make_unique<WebGlScopeTexture>(width, height);
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
        // No swap: the browser presents the default framebuffer when the
        // animation-frame callback returns, and calling glfwSwapBuffers
        // under Emscripten is a no-op that only reads as pacing.
    }

    void* nativeWindowHandle() const override
    {
        // There is no native window to attach a region to, and nothing in
        // the lab asks for one.
        return nullptr;
    }

private:
    GLFWwindow* m_window = nullptr;
    int m_framebufferWidth = 0;
    int m_framebufferHeight = 0;
};

}  // namespace

std::unique_ptr<GraphicsBackend> createGraphicsBackend()
{
    return std::make_unique<WebGlGraphics>();
}

}  // namespace sidescopes
