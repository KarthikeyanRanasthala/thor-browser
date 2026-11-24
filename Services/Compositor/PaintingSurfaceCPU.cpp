/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/CompositorCoreRustFFI.h>
#include <Compositor/PaintingSurface.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Bitmap.h>

#ifdef ENABLE_THORVG_GL
#    include <Compositor/ThorVGGLTarget.h>
#    include <LibGfx/SharedImageBuffer.h>
#endif

namespace Gfx {

struct PaintingSurface::Impl {
    AK_ALLOC_WITH_KMALLOC;
    struct State {
        AffineTransform transform;
        IntRect clip;
    };
    NonnullRefPtr<Bitmap> bitmap;
    State state;
    Vector<State> stack;
    Function<void(PaintingSurface&)> on_flush;
#ifdef ENABLE_THORVG_GL
    OwnPtr<Compositor::ThorVGGLTarget> gl_target {};
#endif
};

void PaintingSurface::destroy_impl(void* impl) { delete static_cast<Impl*>(impl); }
PaintingSurface::Impl& PaintingSurface::impl() const { return *static_cast<Impl*>(Compositor::RustFFI::compositor_painting_surface_raster_data(this)); }
NonnullRefPtr<PaintingSurface> PaintingSurface::wrap_bitmap(Bitmap& bitmap, TextAntialiasing)
{
    auto impl = make<Impl>(NonnullRefPtr { bitmap }, Impl::State { {}, bitmap.rect() }, Vector<Impl::State> {}, nullptr);
    return adopt_impl(bitmap.size(), move(impl));
}

NonnullRefPtr<PaintingSurface> PaintingSurface::adopt_impl(IntSize size, NonnullOwnPtr<Impl> impl)
{
    void const* context = nullptr;
#ifdef ENABLE_THORVG_GL
    if (impl->gl_target)
        context = Compositor::ThorVGGLTarget::context_identity();
#endif
    return adopt_ref(const_cast<PaintingSurface&>(*Compositor::RustFFI::compositor_painting_surface_create(size, context, impl.leak_ptr(), destroy_impl)));
}

#ifdef ENABLE_THORVG_GL
ErrorOr<NonnullRefPtr<PaintingSurface>> PaintingSurface::wrap_shared_gl_buffer(SharedImageBuffer& buffer)
{
    auto bitmap = buffer.bitmap();
    auto target = TRY(Compositor::ThorVGGLTarget::create(buffer.iosurface_handle(), bitmap->size()));
    auto impl = make<Impl>(bitmap, Impl::State { {}, bitmap->rect() }, Vector<Impl::State> {}, nullptr, move(target));
    return adopt_impl(bitmap->size(), move(impl));
}

Compositor::ThorVGGLTarget const* PaintingSurface::gl_target() const { return impl().gl_target.ptr(); }
#endif

ErrorOr<NonnullRefPtr<PaintingSurface>> PaintingSurface::try_create_with_size(IntSize size, BitmapFormat format, AlphaType alpha, RefPtr<SkiaBackendContext>, ColorSpace const&, TextAntialiasing aa)
{
#ifdef ENABLE_THORVG_GL
    if (Compositor::ThorVGGLTarget::enabled() && format == BitmapFormat::BGRA8888 && alpha == AlphaType::Premultiplied) {
        auto buffer = TRY(SharedImageBuffer::try_create(size));
        return wrap_shared_gl_buffer(buffer);
    }
#endif
    auto bitmap = TRY(Bitmap::create(format, alpha, size));
    return wrap_bitmap(*bitmap, aa);
}
NonnullRefPtr<PaintingSurface> PaintingSurface::create_with_size(IntSize size, BitmapFormat format, AlphaType alpha, RefPtr<SkiaBackendContext> context, ColorSpace const& color, TextAntialiasing aa)
{
    return MUST(try_create_with_size(size, format, alpha, move(context), color, aa));
}

int PaintingSurface::save_state()
{
    impl().stack.append(impl().state);
    return static_cast<int>(impl().stack.size());
}
void PaintingSurface::restore_state(int count)
{
    while (!impl().stack.is_empty() && static_cast<int>(impl().stack.size()) >= count)
        impl().state = impl().stack.take_last();
}
void PaintingSurface::translate(float x, float y) { impl().state.transform.translate(x, y); }
void PaintingSurface::scale(float x, float y) { impl().state.transform.scale(x, y); }
void PaintingSurface::clip_rect(IntRect rect)
{
    impl().state.clip.intersect(impl().state.transform.map(rect.to_type<float>()).to_rounded<int>());
}
IntRect PaintingSurface::device_clip() const { return impl().state.clip; }
FloatMatrix4x4 PaintingSurface::paint_matrix() const { return impl().state.transform.to_matrix(); }
Bitmap* PaintingSurface::backing_bitmap() const { return impl().bitmap.ptr(); }
IntRect PaintingSurface::rect() const { return impl().bitmap->rect(); }
IntSize PaintingSurface::size() const { return impl().bitmap->size(); }
void PaintingSurface::notify_content_will_change() { }
RefPtr<SkiaBackendContext> PaintingSurface::skia_backend_context() const { return {}; }
bool PaintingSurface::wraps_shared_image() const
{
#ifdef ENABLE_THORVG_GL
    return gl_target() != nullptr;
#else
    return false;
#endif
}
void PaintingSurface::set_on_flush(Function<void(PaintingSurface&)> callback) { impl().on_flush = move(callback); }
void PaintingSurface::flush()
{
    if (impl().on_flush)
        impl().on_flush(*this);
}
void PaintingSurface::ref() const { Compositor::RustFFI::compositor_painting_surface_retain(this); }
void PaintingSurface::unref() const { Compositor::RustFFI::compositor_painting_surface_release(this); }

static void copy_pixels(Bitmap& target, Bitmap const& source, IntPoint offset)
{
    if (&target == &source)
        return;
    auto area = target.rect().intersected(source.rect().translated(offset));
    if (area.is_empty())
        return;
    for (int y = area.top(); y < area.bottom(); ++y) {
        if (target.format() == source.format() && target.alpha_type() == source.alpha_type()) {
            memcpy(target.scanline(y) + area.x(), source.scanline(y - offset.y()) + area.x() - offset.x(), area.width() * sizeof(u32));
            continue;
        }
        for (int x = area.left(); x < area.right(); ++x) {
            auto color = source.get_pixel(x - offset.x(), y - offset.y());
            if (source.alpha_type() != target.alpha_type()) {
                if (source.alpha_type() == AlphaType::Premultiplied && color.alpha() != 0) {
                    auto straight = [&](u8 v) { return min(255, (v * 255 + color.alpha() / 2) / color.alpha()); };
                    color = Color(straight(color.red()), straight(color.green()), straight(color.blue()), color.alpha());
                }
                color.set_alpha(color.alpha(), target.alpha_type());
            }
            target.set_pixel(x, y, color);
        }
    }
}
void PaintingSurface::read_into_bitmap(Bitmap& bitmap, IntPoint origin, ColorSpace const&) const
{
#ifdef ENABLE_THORVG_GL
    Compositor::ScopedThorVGPixelAccess access(gl_target(), true);
#endif
    copy_pixels(bitmap, *backing_bitmap(), { -origin.x(), -origin.y() });
}
void PaintingSurface::write_from_bitmap(Bitmap const& bitmap, IntPoint position, ColorSpace const&)
{
#ifdef ENABLE_THORVG_GL
    Compositor::ScopedThorVGPixelAccess access(gl_target(), false);
#endif
    copy_pixels(*backing_bitmap(), bitmap, position);
}
void PaintingSurface::copy_from_surface(PaintingSurface& source)
{
    if (this == &source)
        return;
#ifdef ENABLE_THORVG_GL
    Compositor::ScopedThorVGPixelAccess access(source.gl_target(), true);
#endif
    write_from_bitmap(*source.backing_bitmap());
}

}
