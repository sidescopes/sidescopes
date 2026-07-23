// Metal rendering backend for the shared application shell. Owns the
// CAMetalLayer, the per-frame drawable and encoder, and the native window
// chrome that GLFW does not reach.

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "platform/graphics.h"

#include "imgui_impl_glfw.h"
#include "imgui_impl_metal.h"

namespace sidescopes {
namespace {

class MetalScopeTexture final : public ScopeTexture
{
public:
    MetalScopeTexture(id<MTLDevice> device, int width, int height)
        : m_width(width),
          m_height(height)
    {
        MTLTextureDescriptor* descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:width
                                                              height:height
                                                           mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead;
        descriptor.storageMode = MTLStorageModeManaged;
        m_texture = [device newTextureWithDescriptor:descriptor];
    }

    void upload(const ScopeImage& image) override
    {
        // A scope just toggled on can race one worker pass: the fetched
        // output predates the toggle and carries an empty image for it.
        // Uploading that null buffer is a GPU-side crash; skip the frame.
        if (image.rgba.size() < static_cast<std::size_t>(m_width) * m_height * 4) {
            return;
        }
        [m_texture replaceRegion:MTLRegionMake2D(0, 0, m_width, m_height)
                     mipmapLevel:0
                       withBytes:image.rgba.data()
                     bytesPerRow:static_cast<NSUInteger>(m_width) * 4];
    }

    [[nodiscard]] ImTextureID textureId() const override
    {
        // ARC requires the __bridge to hand the Metal texture out as a raw
        // pointer; ImTextureID is an integer handle ImGui only passes back.
        void* handle = (__bridge void*)m_texture;
        return reinterpret_cast<ImTextureID>(handle);
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
    id<MTLTexture> m_texture;
};

class MetalGraphics final : public GraphicsBackend
{
public:
    void setWindowHints() override
    {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    }

    bool init(GLFWwindow* window) override
    {
        m_device = MTLCreateSystemDefaultDevice();
        if (!m_device) {
            return false;
        }
        m_commandQueue = [m_device newCommandQueue];

        m_nativeWindow = glfwGetCocoaWindow(window);
        m_layer = [CAMetalLayer layer];
        m_layer.device = m_device;
        m_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        m_nativeWindow.contentView.layer = m_layer;
        m_nativeWindow.contentView.wantsLayer = YES;
        // During a live window resize macOS runs a modal tracking loop that
        // stalls the render loop; by default the layer stretches its last
        // frame to the new size, warping the scopes until release. Pinning
        // the contents to the top-left keeps the last frame 1:1 - blank space
        // when growing, cropped when shrinking - and the loop redraws
        // correctly the moment the drag ends.
        m_layer.contentsGravity = kCAGravityTopLeft;
        // Gravity makes the contents scale meaningful: without it the Retina
        // drawable displays at double size. The stretch gravity used to hide
        // that this was never set.
        m_layer.contentsScale = m_nativeWindow.backingScaleFactor;
        // The area beyond the pinned contents during a grow shows the layer
        // and window background; both match the application's black.
        m_layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
        m_nativeWindow.backgroundColor = NSColor.blackColor;
        // Above document and panel windows (Quick Look previews float higher
        // than ordinary floating windows), on every Space.
        m_nativeWindow.level = NSStatusWindowLevel;
        m_nativeWindow.collectionBehavior =
            NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary;

        if (!ImGui_ImplGlfw_InitForOther(window, true)) {
            return false;
        }
        if (!ImGui_ImplMetal_Init(m_device)) {
            ImGui_ImplGlfw_Shutdown();
            return false;
        }
        return true;
    }

    void shutdown() override
    {
        ImGui_ImplMetal_Shutdown();
        ImGui_ImplGlfw_Shutdown();
    }

    std::unique_ptr<ScopeTexture> createScopeTexture(int width, int height) override
    {
        return std::make_unique<MetalScopeTexture>(m_device, width, height);
    }

    bool beginFrame(int framebufferWidth, int framebufferHeight) override
    {
        m_layer.drawableSize = CGSizeMake(framebufferWidth, framebufferHeight);
        // The window may have moved to a display with a different scale.
        if (m_layer.contentsScale != m_nativeWindow.backingScaleFactor) {
            m_layer.contentsScale = m_nativeWindow.backingScaleFactor;
        }
        m_layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
        m_nativeWindow.backgroundColor = NSColor.blackColor;
        m_drawable = [m_layer nextDrawable];
        if (!m_drawable) {
            return false;
        }

        m_pass = [MTLRenderPassDescriptor renderPassDescriptor];
        m_pass.colorAttachments[0].texture = m_drawable.texture;
        m_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        m_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        m_pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        m_commands = [m_commandQueue commandBuffer];
        m_encoder = [m_commands renderCommandEncoderWithDescriptor:m_pass];

        ImGui_ImplMetal_NewFrame(m_pass);
        ImGui_ImplGlfw_NewFrame();
        return true;
    }

    void endFrame() override
    {
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), m_commands, m_encoder);
        [m_encoder endEncoding];
        [m_commands presentDrawable:m_drawable];
        [m_commands commit];
        m_encoder = nil;
        m_commands = nil;
        m_pass = nil;
        m_drawable = nil;
    }

    void* nativeWindowHandle() const override
    {
        return (__bridge void*)m_nativeWindow;
    }

private:
    id<MTLDevice> m_device = nil;
    id<MTLCommandQueue> m_commandQueue = nil;
    NSWindow* m_nativeWindow = nil;
    CAMetalLayer* m_layer = nil;
    id<CAMetalDrawable> m_drawable = nil;
    MTLRenderPassDescriptor* m_pass = nil;
    id<MTLCommandBuffer> m_commands = nil;
    id<MTLRenderCommandEncoder> m_encoder = nil;
};

}  // namespace

std::unique_ptr<GraphicsBackend> createGraphicsBackend()
{
    return std::make_unique<MetalGraphics>();
}

}  // namespace sidescopes
