/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <AK/OwnPtr.h>
#include <LibCore/IOSurface.h>
#include <LibGfx/Color.h>
#include <LibGfx/Rect.h>

namespace tvg {

struct GlCanvas;

}

namespace Compositor {

// One framebuffer per backing store. All targets live on the compositor thread and share its GL context.
class ThorVGGLTarget {
    AK_MAKE_NONCOPYABLE(ThorVGGLTarget);
    AK_MAKE_NONMOVABLE(ThorVGGLTarget);

public:
    AK_ALLOC_WITH_KMALLOC;
    static ErrorOr<void> initialize();
    static bool enabled();
    static void const* context_identity();
    static ErrorOr<NonnullOwnPtr<ThorVGGLTarget>> create(Core::IOSurfaceHandle const&, Gfx::IntSize);
    ~ThorVGGLTarget();

    bool bind(tvg::GlCanvas&) const;
    static u64 painted_passes();
    void finish_paint(bool succeeded) const;
    void clear(Gfx::IntRect device_clip, Gfx::Color) const;
    void copy_from(ThorVGGLTarget const& source, Gfx::IntRect src, Gfx::IntRect dst, Gfx::IntRect device_clip) const;
    void lock_pixels(bool read_only) const;
    void unlock_pixels(bool read_only) const;

private:
    void finish() const;
    ThorVGGLTarget(Core::IOSurfaceHandle, Gfx::IntSize);
    Core::IOSurfaceHandle m_surface;
    Gfx::IntSize m_size;
    unsigned m_texture { 0 };
    unsigned m_framebuffer { 0 };
};

class ScopedThorVGPixelAccess {
    AK_MAKE_NONCOPYABLE(ScopedThorVGPixelAccess);
    AK_MAKE_NONMOVABLE(ScopedThorVGPixelAccess);

public:
    ScopedThorVGPixelAccess(ThorVGGLTarget const* target, bool read_only)
        : m_target(target)
        , m_read_only(read_only)
    {
        if (m_target)
            m_target->lock_pixels(read_only);
    }
    ~ScopedThorVGPixelAccess()
    {
        if (m_target)
            m_target->unlock_pixels(m_read_only);
    }

private:
    ThorVGGLTarget const* m_target;
    bool m_read_only;
};

}
