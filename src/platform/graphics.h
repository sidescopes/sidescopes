#pragma once

#include <memory>

#include "core/scopes/scope_types.h"
#include "imgui.h"

struct GLFWwindow;

namespace sidescopes {

// The per-platform rendering backend behind the shared application shell:
// Metal on macOS, OpenGL on Windows. Unlike the rest of the platform
// layer this seam speaks ImGui - it owns the ImGui platform and renderer
// backends - so its implementations compile into the application target
// rather than the platform library.

// A texture the CPU-side scope images are uploaded into every time the
// analysis worker publishes a new version.
class ScopeTexture {
public:
    virtual ~ScopeTexture() = default;

    virtual void Upload(const ScopeImage& image) = 0;

    [[nodiscard]] virtual ImTextureID Id() const = 0;
    [[nodiscard]] virtual int Width() const = 0;
    [[nodiscard]] virtual int Height() const = 0;
};

class GraphicsBackend {
public:
    virtual ~GraphicsBackend() = default;

    // Window hints the backend needs in place before glfwCreateWindow:
    // the client API and, where one is used, the context version.
    virtual void SetWindowHints() = 0;

    // Everything after the window exists: the device, the ImGui platform
    // and renderer backends, and the native window chrome (always-on-top
    // level, black background, live-resize behavior). Runs after the
    // application installed its own GLFW callbacks, so the ImGui backend
    // chains them rather than the other way around.
    virtual bool Init(GLFWwindow* window) = 0;

    // Tears down what Init built; runs before ImGui::DestroyContext.
    virtual void Shutdown() = 0;

    virtual std::unique_ptr<ScopeTexture> CreateScopeTexture(int width, int height) = 0;

    // Starts a frame, including the ImGui backends' new-frame work;
    // false skips the frame (no drawable this instant).
    virtual bool BeginFrame(int framebuffer_width, int framebuffer_height) = 0;

    // Renders the finished ImGui draw data and presents it.
    virtual void EndFrame() = 0;
};

std::unique_ptr<GraphicsBackend> CreateGraphicsBackend();

}  // namespace sidescopes
