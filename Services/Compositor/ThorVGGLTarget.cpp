/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define GL_SILENCE_DEPRECATION
#include <Compositor/ThorVGGLTarget.h>
#include <Compositor/ThorVGRaster.h>
#include <IOSurface/IOSurface.h>
#include <OpenGL/CGLIOSurface.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

namespace Compositor {

struct GLContext {
    CGLContextObj handle { nullptr };
    u64 painted_passes { 0 };
    ~GLContext()
    {
        if (handle) {
            if (CGLGetCurrentContext() == handle)
                CGLSetCurrentContext(nullptr);
            CGLReleaseContext(handle);
        }
    }
};
static thread_local GLContext s_context;
bool ThorVGGLTarget::enabled() { return s_context.handle; }
void const* ThorVGGLTarget::context_identity() { return s_context.handle; }

ErrorOr<void> ThorVGGLTarget::initialize()
{
    if (enabled())
        return {};
    CGLPixelFormatAttribute attributes[] = {
        kCGLPFAOpenGLProfile, static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_GL4_Core),
        kCGLPFAAccelerated, kCGLPFAAllowOfflineRenderers, static_cast<CGLPixelFormatAttribute>(0)
    };
    CGLPixelFormatObj format = nullptr;
    GLint count = 0;
    if (CGLChoosePixelFormat(attributes, &format, &count) != kCGLNoError || !format)
        return Error::from_string_literal("ThorVG GL could not find an accelerated OpenGL pixel format");
    CGLContextObj context = nullptr;
    auto result = CGLCreateContext(format, nullptr, &context);
    CGLReleasePixelFormat(format);
    if (result != kCGLNoError || !context)
        return Error::from_string_literal("ThorVG GL could not create an OpenGL context");
    if (CGLSetCurrentContext(context) != kCGLNoError) {
        CGLReleaseContext(context);
        return Error::from_string_literal("ThorVG GL could not activate its OpenGL context");
    }
    if (!thorvg_runtime_ready() || !std::unique_ptr<tvg::GlCanvas>(tvg::GlCanvas::gen())) {
        CGLSetCurrentContext(nullptr);
        CGLReleaseContext(context);
        return Error::from_string_literal("ThorVG GL needs a ThorVG library built with -Dengines=cpu,gl");
    }
    s_context.handle = context;
    dbgln("ThorVG OpenGL page renderer: {}", reinterpret_cast<char const*>(glGetString(GL_RENDERER)));
    return {};
}

ThorVGGLTarget::ThorVGGLTarget(Core::IOSurfaceHandle surface, Gfx::IntSize size)
    : m_surface(move(surface))
    , m_size(size)
{
}

ErrorOr<NonnullOwnPtr<ThorVGGLTarget>> ThorVGGLTarget::create(Core::IOSurfaceHandle const& surface, Gfx::IntSize size)
{
    VERIFY(enabled());
    auto target = adopt_own(*new ThorVGGLTarget(Core::IOSurfaceHandle::from_ref(surface.core_foundation_pointer()), size));
    glGenTextures(1, &target->m_texture);
    glBindTexture(GL_TEXTURE_RECTANGLE, target->m_texture);
    auto result = CGLTexImageIOSurface2D(s_context.handle, GL_TEXTURE_RECTANGLE, GL_RGBA8, size.width(), size.height(), GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, static_cast<IOSurfaceRef>(surface.core_foundation_pointer()), 0);
    glBindTexture(GL_TEXTURE_RECTANGLE, 0);
    if (result != kCGLNoError)
        return Error::from_string_literal("Could not bind the ThorVG backing IOSurface to OpenGL");
    glGenFramebuffers(1, &target->m_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, target->m_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, target->m_texture, 0);
    auto complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!complete)
        return Error::from_string_literal("Incomplete ThorVG IOSurface framebuffer");
    return target;
}

ThorVGGLTarget::~ThorVGGLTarget()
{
    glDeleteFramebuffers(1, &m_framebuffer);
    glDeleteTextures(1, &m_texture);
}

bool ThorVGGLTarget::bind(tvg::GlCanvas& canvas) const
{
    return canvas.target(nullptr, nullptr, s_context.handle, m_framebuffer, m_size.width(), m_size.height(), tvg::ColorSpace::ABGR8888S) == tvg::Result::Success;
}

u64 ThorVGGLTarget::painted_passes() { return s_context.painted_passes; }

void ThorVGGLTarget::finish_paint(bool succeeded) const
{
    finish();
    if (succeeded)
        ++s_context.painted_passes;
}

void ThorVGGLTarget::finish() const
{
    // ThorVG sync submits GL commands. The Rust compositor may immediately sample the pixels on the CPU
    // or hand the IOSurface to another process, so wait for the GPU writes to complete.
    glFinish();
}

void ThorVGGLTarget::clear(Gfx::IntRect clip, Gfx::Color color) const
{
    if (clip.is_empty())
        return;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_framebuffer);
    glEnable(GL_SCISSOR_TEST);
    glScissor(clip.x(), clip.y(), clip.width(), clip.height());
    float alpha = color.alpha() / 255.f;
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(alpha * color.red() / 255.f, alpha * color.green() / 255.f, alpha * color.blue() / 255.f, alpha);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    finish();
}

void ThorVGGLTarget::copy_from(ThorVGGLTarget const& source, Gfx::IntRect src, Gfx::IntRect dst, Gfx::IntRect clip) const
{
    if (clip.is_empty())
        return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, source.m_framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_framebuffer);
    glEnable(GL_SCISSOR_TEST);
    glScissor(clip.x(), clip.y(), clip.width(), clip.height());
    glBlitFramebuffer(src.left(), src.top(), src.right(), src.bottom(), dst.left(), dst.top(), dst.right(), dst.bottom(), GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    finish();
}

void ThorVGGLTarget::lock_pixels(bool read_only) const
{
    VERIFY(IOSurfaceLock(static_cast<IOSurfaceRef>(m_surface.core_foundation_pointer()), read_only ? kIOSurfaceLockReadOnly : 0, nullptr) == kIOReturnSuccess);
}

void ThorVGGLTarget::unlock_pixels(bool read_only) const
{
    VERIFY(IOSurfaceUnlock(static_cast<IOSurfaceRef>(m_surface.core_foundation_pointer()), read_only ? kIOSurfaceLockReadOnly : 0, nullptr) == kIOReturnSuccess);
    if (!read_only) {
        glBindTexture(GL_TEXTURE_RECTANGLE, m_texture);
        glBindTexture(GL_TEXTURE_RECTANGLE, 0);
    }
}

}
