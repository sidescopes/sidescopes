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

class MetalScopeTexture final : public ScopeTexture {
public:
    MetalScopeTexture(id<MTLDevice> device, int width, int height)
        : width_(width), height_(height) {
        MTLTextureDescriptor* descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:width
                                                              height:height
                                                           mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead;
        descriptor.storageMode = MTLStorageModeManaged;
        texture_ = [device newTextureWithDescriptor:descriptor];
    }

    void Upload(const ScopeImage& image) override {
        // A scope just toggled on can race one worker pass: the fetched
        // output predates the toggle and carries an empty image for it.
        // Uploading that null buffer is a GPU-side crash; skip the frame.
        if (image.rgba.size() < static_cast<std::size_t>(width_) * height_ * 4) return;
        [texture_ replaceRegion:MTLRegionMake2D(0, 0, width_, height_)
                    mipmapLevel:0
                      withBytes:image.rgba.data()
                    bytesPerRow:static_cast<NSUInteger>(width_) * 4];
    }

    [[nodiscard]] ImTextureID Id() const override {
        return reinterpret_cast<ImTextureID>((__bridge void*)texture_);
    }

    [[nodiscard]] int Width() const override { return width_; }
    [[nodiscard]] int Height() const override { return height_; }

private:
    int width_;
    int height_;
    id<MTLTexture> texture_;
};

class MetalGraphics final : public GraphicsBackend {
public:
    void SetWindowHints() override { glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); }

    bool Init(GLFWwindow* window) override {
        device_ = MTLCreateSystemDefaultDevice();
        if (!device_) return false;
        command_queue_ = [device_ newCommandQueue];

        native_window_ = glfwGetCocoaWindow(window);
        layer_ = [CAMetalLayer layer];
        layer_.device = device_;
        layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
        native_window_.contentView.layer = layer_;
        native_window_.contentView.wantsLayer = YES;
        // During a live window resize macOS runs a modal tracking loop that
        // stalls the render loop; by default the layer stretches its last
        // frame to the new size, warping the scopes until release. Pinning
        // the contents to the top-left keeps the last frame 1:1 - blank space
        // when growing, cropped when shrinking - and the loop redraws
        // correctly the moment the drag ends.
        layer_.contentsGravity = kCAGravityTopLeft;
        // Gravity makes the contents scale meaningful: without it the Retina
        // drawable displays at double size. The stretch gravity used to hide
        // that this was never set.
        layer_.contentsScale = native_window_.backingScaleFactor;
        // The area beyond the pinned contents during a grow shows the layer
        // and window background; both match the application's black.
        layer_.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
        native_window_.backgroundColor = NSColor.blackColor;
        // Above document and panel windows (Quick Look previews float higher
        // than ordinary floating windows), on every Space.
        native_window_.level = NSStatusWindowLevel;
        native_window_.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                            NSWindowCollectionBehaviorFullScreenAuxiliary;

        if (!ImGui_ImplGlfw_InitForOther(window, true)) return false;
        if (!ImGui_ImplMetal_Init(device_)) {
            ImGui_ImplGlfw_Shutdown();
            return false;
        }
        return true;
    }

    void Shutdown() override {
        ImGui_ImplMetal_Shutdown();
        ImGui_ImplGlfw_Shutdown();
    }

    std::unique_ptr<ScopeTexture> CreateScopeTexture(int width, int height) override {
        return std::make_unique<MetalScopeTexture>(device_, width, height);
    }

    bool BeginFrame(int framebuffer_width, int framebuffer_height) override {
        layer_.drawableSize = CGSizeMake(framebuffer_width, framebuffer_height);
        // The window may have moved to a display with a different scale.
        if (layer_.contentsScale != native_window_.backingScaleFactor)
            layer_.contentsScale = native_window_.backingScaleFactor;
        layer_.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
        native_window_.backgroundColor = NSColor.blackColor;
        drawable_ = [layer_ nextDrawable];
        if (!drawable_) return false;

        pass_ = [MTLRenderPassDescriptor renderPassDescriptor];
        pass_.colorAttachments[0].texture = drawable_.texture;
        pass_.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass_.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass_.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        commands_ = [command_queue_ commandBuffer];
        encoder_ = [commands_ renderCommandEncoderWithDescriptor:pass_];

        ImGui_ImplMetal_NewFrame(pass_);
        ImGui_ImplGlfw_NewFrame();
        return true;
    }

    void EndFrame() override {
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), commands_, encoder_);
        [encoder_ endEncoding];
        [commands_ presentDrawable:drawable_];
        [commands_ commit];
        encoder_ = nil;
        commands_ = nil;
        pass_ = nil;
        drawable_ = nil;
    }

private:
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> command_queue_ = nil;
    NSWindow* native_window_ = nil;
    CAMetalLayer* layer_ = nil;
    id<CAMetalDrawable> drawable_ = nil;
    MTLRenderPassDescriptor* pass_ = nil;
    id<MTLCommandBuffer> commands_ = nil;
    id<MTLRenderCommandEncoder> encoder_ = nil;
};

}  // namespace

std::unique_ptr<GraphicsBackend> CreateGraphicsBackend() {
    return std::make_unique<MetalGraphics>();
}

}  // namespace sidescopes
