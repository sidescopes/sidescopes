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
#include "platform/graphics.h"

// Windows 10 2004; absent from older SDKs.
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace sidescopes {
namespace {

class OpenGlScopeTexture final : public ScopeTexture {
public:
    OpenGlScopeTexture(int width, int height) : width_(width), height_(height) {
        glGenTextures(1, &texture_);
        glBindTexture(GL_TEXTURE_2D, texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     nullptr);
    }

    ~OpenGlScopeTexture() override { glDeleteTextures(1, &texture_); }

    void Upload(const ScopeImage& image) override {
        // A scope just toggled on can race one worker pass: the fetched
        // output predates the toggle and carries an empty image for it.
        if (image.rgba.size() < static_cast<std::size_t>(width_) * height_ * 4) return;
        glBindTexture(GL_TEXTURE_2D, texture_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE,
                        image.rgba.data());
    }

    [[nodiscard]] ImTextureID Id() const override { return static_cast<ImTextureID>(texture_); }

    [[nodiscard]] int Width() const override { return width_; }
    [[nodiscard]] int Height() const override { return height_; }

private:
    int width_;
    int height_;
    GLuint texture_ = 0;
};

class OpenGlGraphics final : public GraphicsBackend {
public:
    void SetWindowHints() override {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    }

    bool Init(GLFWwindow* window) override {
        window_ = window;
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
        // excludes itself. Best effort: unsupported before Windows 10
        // 2004, where the analysis-side masking still applies.
        SetWindowDisplayAffinity(glfwGetWin32Window(window), WDA_EXCLUDEFROMCAPTURE);
        if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) return false;
        if (!ImGui_ImplOpenGL3_Init("#version 150")) {
            ImGui_ImplGlfw_Shutdown();
            return false;
        }
        return true;
    }

    void Shutdown() override {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
    }

    std::unique_ptr<ScopeTexture> CreateScopeTexture(int width, int height) override {
        return std::make_unique<OpenGlScopeTexture>(width, height);
    }

    bool BeginFrame(int framebuffer_width, int framebuffer_height) override {
        framebuffer_width_ = framebuffer_width;
        framebuffer_height_ = framebuffer_height;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        return true;
    }

    void EndFrame() override {
        glViewport(0, 0, framebuffer_width_, framebuffer_height_);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window_);
        // Composition-tick pacing; see Init. Failure means the compositor
        // is gone (a remote session, say) - render unpaced rather than
        // not at all.
        DwmFlush();
    }

private:
    GLFWwindow* window_ = nullptr;
    int framebuffer_width_ = 0;
    int framebuffer_height_ = 0;
};

}  // namespace

std::unique_ptr<GraphicsBackend> CreateGraphicsBackend() {
    return std::make_unique<OpenGlGraphics>();
}

}  // namespace sidescopes
