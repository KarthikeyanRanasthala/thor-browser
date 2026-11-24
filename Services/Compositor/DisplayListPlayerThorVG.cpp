/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/DisplayListPlayerThorVG.h>
#include <Compositor/PaintingSurface.h>
#include <Compositor/SkiaUtils.h>
#include <Compositor/ThorVGRaster.h>
#include <LibCompositing/DisplayList/DisplayListCommand.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Gradients.h>
#include <LibGfx/PathData.h>
#include <algorithm>
#include <core/SkCanvas.h>
#include <core/SkPathBuilder.h>
#include <core/SkRRect.h>

namespace Compositor {

using namespace Compositing;
namespace Replay = Compositing::RustFFI;

static bool s_enabled;
void DisplayListPlayerThorVG::set_enabled(bool enabled) { s_enabled = enabled; }
bool DisplayListPlayerThorVG::enabled() { return s_enabled; }

static tvg::ColorSpace pixel_format(Gfx::BitmapFormat format, Gfx::AlphaType alpha)
{
    bool bgra = format == Gfx::BitmapFormat::BGRA8888 || format == Gfx::BitmapFormat::BGRx8888;
    if (alpha == Gfx::AlphaType::Unpremultiplied)
        return bgra ? tvg::ColorSpace::ARGB8888S : tvg::ColorSpace::ABGR8888S;
    return bgra ? tvg::ColorSpace::ARGB8888 : tvg::ColorSpace::ABGR8888;
}

static tvg::Matrix tvg_matrix(SkMatrix const& m)
{
    return { m.getScaleX(), m.getSkewX(), m.getTranslateX(), m.getSkewY(), m.getScaleY(), m.getTranslateY(), 0, 0, 1 };
}

static SkPath rounded_rect(SkRect r, Gfx::CornerRadii const& corners)
{
    if (!corners.has_any_radius())
        return SkPath::Rect(r);
    auto radius = [](auto v) -> SkVector { return { static_cast<float>(v.horizontal_radius), static_cast<float>(v.vertical_radius) }; };
    SkVector radii[] = { radius(corners.top_left), radius(corners.top_right), radius(corners.bottom_right), radius(corners.bottom_left) };
    SkRRect rr;
    rr.setRectRadii(r, radii);
    return SkPath::RRect(rr);
}

static tvg::BlendMethod blend_method(Gfx::CompositingAndBlendingOperator op)
{
    using Op = Gfx::CompositingAndBlendingOperator;
    using Blend = tvg::BlendMethod;
    switch (op) {
    case Op::Multiply:
        return Blend::Multiply;
    case Op::Screen:
        return Blend::Screen;
    case Op::Overlay:
        return Blend::Overlay;
    case Op::Darken:
        return Blend::Darken;
    case Op::Lighten:
        return Blend::Lighten;
    case Op::ColorDodge:
        return Blend::ColorDodge;
    case Op::ColorBurn:
        return Blend::ColorBurn;
    case Op::HardLight:
        return Blend::HardLight;
    case Op::SoftLight:
        return Blend::SoftLight;
    case Op::Difference:
        return Blend::Difference;
    case Op::Exclusion:
        return Blend::Exclusion;
    case Op::Hue:
        return Blend::Hue;
    case Op::Saturation:
        return Blend::Saturation;
    case Op::Color:
        return Blend::Color;
    case Op::Luminosity:
        return Blend::Luminosity;
    case Op::Lighter:
    case Op::PlusLighter:
        return Blend::Add;
    default:
        return Blend::Normal;
    }
}

struct DisplayListPlayerThorVG::Impl {
    AK_ALLOC_WITH_KMALLOC;
    struct State {
        tvg::Scene* scene {};
        SkM44 matrix;
        bool hidden {};
    };
    Vector<State> stack;
    State state;
    std::unique_ptr<tvg::SwCanvas> canvas;
    DrawnFonts const* fonts {};
    DrawnImageFrames const* images {};
    HashMap<unsigned, unsigned> skipped;
    unsigned glyphs_painted {};
    HashTable<unsigned> reported_skips;
    bool reported_text {};
    bool has_draws {};

    void open(Gfx::PaintingSurface& surface)
    {
        auto* bitmap = surface.backing_bitmap();
        VERIFY(bitmap && thorvg_runtime_ready());
        surface.notify_content_will_change();
        canvas.reset(tvg::SwCanvas::gen());
        VERIFY(canvas && canvas->target(bitmap->begin(), bitmap->pitch() / 4, bitmap->width(), bitmap->height(), pixel_format(bitmap->format(), bitmap->alpha_type())) == tvg::Result::Success);
        state = { tvg::Scene::gen(), surface.canvas().getLocalToDevice(), false };
        auto clip = surface.canvas().getDeviceClipBounds();
        if (clip.isEmpty())
            state.hidden = true;
        else
            VERIFY(canvas->viewport(clip.x(), clip.y(), clip.width(), clip.height()) == tvg::Result::Success);
        VERIFY(canvas->add(state.scene) == tvg::Result::Success);
    }

    void finish()
    {
        VERIFY(stack.is_empty());
        if (has_draws) {
            auto status = canvas->draw(false);
            if (status != tvg::Result::Success)
                dbgln("ThorVG page draw failed ({})", static_cast<int>(status));
            canvas->sync();
        }
        canvas.reset();
        state = {};
        has_draws = false;
    }

    void scope(bool hide = false)
    {
        stack.append(state);
        state.hidden |= hide;
    }
    void pop() { state = stack.take_last(); }
    void group(float opacity = 1, tvg::BlendMethod blend = tvg::BlendMethod::Normal)
    {
        scope();
        if (state.hidden)
            return;
        auto* scene = tvg::Scene::gen();
        scene->blend(blend);
        scene->opacity(static_cast<u8>(clamp(opacity, 0.f, 1.f) * 255 + .5f));
        VERIFY(state.scene->add(scene) == tvg::Result::Success);
        state.scene = scene;
    }
    void clip(SkPath const& path)
    {
        if (state.hidden)
            return;
        auto* shape = tvg::Shape::gen();
        auto m = state.matrix.asM33();
        if (m.hasPerspective() || !append_thorvg_path(*shape, path)) {
            tvg::Paint::rel(shape);
            state.hidden = true;
            return;
        }
        shape->transform(tvg_matrix(m));
        shape->fillRule(path.getFillType() == SkPathFillType::kEvenOdd ? tvg::FillRule::EvenOdd : tvg::FillRule::NonZero);
        VERIFY(state.scene->clip(shape) == tvg::Result::Success);
    }

    bool add(tvg::Paint* paint, SkMatrix const& m)
    {
        if (state.hidden || m.hasPerspective() || !m.isFinite() || paint->transform(tvg_matrix(m)) != tvg::Result::Success || state.scene->add(paint) != tvg::Result::Success) {
            tvg::Paint::rel(paint);
            return false;
        }
        has_draws = true;
        return true;
    }

    bool path(SkPath const& path, Gfx::Color color, SkMatrix const& matrix, float stroke = 0, tvg::StrokeCap cap = tvg::StrokeCap::Butt, tvg::StrokeJoin join = tvg::StrokeJoin::Miter, float miter = 4, tvg::BlendMethod blend = tvg::BlendMethod::Normal)
    {
        if (state.hidden || path.isEmpty())
            return false;
        auto* shape = tvg::Shape::gen();
        if (!append_thorvg_path(*shape, path)) {
            tvg::Paint::rel(shape);
            return false;
        }
        shape->blend(blend);
        if (stroke > 0) {
            shape->fill(0, 0, 0, 0);
            shape->strokeWidth(stroke);
            shape->strokeFill(color.red(), color.green(), color.blue(), color.alpha());
            shape->strokeCap(cap);
            shape->strokeJoin(join);
            shape->strokeMiterlimit(miter);
        } else {
            shape->fill(color.red(), color.green(), color.blue(), color.alpha());
            shape->fillRule(path.getFillType() == SkPathFillType::kEvenOdd ? tvg::FillRule::EvenOdd : tvg::FillRule::NonZero);
        }
        if (!add(shape, matrix))
            return false;
        return true;
    }

    void rect(auto const& r, Gfx::Color color, tvg::BlendMethod blend = tvg::BlendMethod::Normal)
    {
        path(SkPath::Rect(Gfx::to_skia_rect(r)), color, state.matrix.asM33(), 0, tvg::StrokeCap::Butt, tvg::StrokeJoin::Miter, 4, blend);
    }

    void pixels(u32 const* data, size_t pitch, int w, int h, tvg::ColorSpace format, SkRect src, SkRect dst)
    {
        if (state.hidden || src.isEmpty() || dst.isEmpty() || !data || !SkRect::MakeIWH(w, h).contains(src))
            return;
        // Images can outlive the callback during batched replay. Give ThorVG a private,
        // tightly packed copy of just the sampled part (including fractional edge pixels).
        auto area = src.roundOut();
        auto* copy = static_cast<u32*>(calloc(static_cast<size_t>(area.width()) * area.height(), sizeof(u32)));
        if (!copy)
            return;
        for (int y = 0; y < area.height(); ++y)
            memcpy(copy + static_cast<size_t>(y) * area.width(), data + (area.y() + y) * pitch + area.x(), area.width() * sizeof(u32));
        auto* image = tvg::Picture::gen();
        if (image->load(copy, area.width(), area.width(), area.height(), format, false) != tvg::Result::Success) {
            free(copy);
            tvg::Paint::rel(image);
            return;
        }
        group();
        clip(SkPath::Rect(dst));
        auto m = state.matrix.asM33();
        src.offset(-area.x(), -area.y());
        m.preConcat(SkMatrix::RectToRect(src, dst));
        add(image, m);
        pop();
    }

    void surface(Gfx::PaintingSurface const& surface, SkRect src, SkRect dst)
    {
        if (auto* bmp = surface.backing_bitmap())
            pixels(bmp->begin(), bmp->pitch() / 4, bmp->width(), bmp->height(), pixel_format(bmp->format(), bmp->alpha_type()), src, dst);
    }

    void command(DisplayListCommandType type, u8 const* bytes, ReadonlyBytes payload);
    Replay::FfiRasterPainter callbacks();
};

void DisplayListPlayerThorVG::Impl::command(DisplayListCommandType type, u8 const* bytes, ReadonlyBytes payload)
{
    if (state.hidden)
        return;
    auto read = [&]<typename T>() { return read_display_list_object<T>({ bytes, sizeof(T) }); };
    auto data = [&](DisplayListDataSpan span) { return payload.slice(span.offset, span.size); };
    auto m = state.matrix.asM33();
    auto gradient = [&](std::unique_ptr<tvg::Fill> fill, DisplayListGradientColorStops const& stops, Gfx::IntRect rect, Gfx::CompositingAndBlendingOperator op) {
        auto colors = data(stops.colors);
        auto positions = data(stops.positions);
        auto count = colors.size() / sizeof(Gfx::Color);
        if (!count || positions.size() != count * sizeof(float))
            return;
        Vector<tvg::Fill::ColorStop, 8> values;
        for (size_t i = 0; i < count; ++i) {
            auto color = read_display_list_object<Gfx::Color>(colors.slice(i * sizeof(Gfx::Color)));
            auto offset = read_display_list_object<float>(positions.slice(i * sizeof(float)));
            values.append({ offset, color.red(), color.green(), color.blue(), color.alpha() });
        }
        if (fill->colorStops(values.data(), values.size()) != tvg::Result::Success)
            return;
        fill->spread(stops.repeating ? tvg::FillSpread::Repeat : tvg::FillSpread::Pad);
        auto* shape = tvg::Shape::gen();
        shape->appendRect(rect.x(), rect.y(), rect.width(), rect.height(), 0, 0);
        shape->fill(fill.release());
        shape->blend(blend_method(op));
        add(shape, m);
    };
    switch (type) {
    case DisplayListCommandType::PaintLinearGradient: {
        auto c = read.operator()<PaintLinearGradient>();
        auto rect = c.gradient_rect.to_type<float>();
        auto length = Gfx::calculate_gradient_length(rect.size(), c.gradient_angle);
        auto center = rect.center();
        auto y = center.y() + (.5f - c.first_stop_position) * length;
        auto fill = std::unique_ptr<tvg::LinearGradient>(tvg::LinearGradient::gen());
        fill->linear(center.x(), y, center.x(), y - length * c.repeat_length);
        SkMatrix rotation;
        rotation.setRotate(c.gradient_angle, center.x(), center.y());
        fill->transform(tvg_matrix(rotation));
        gradient(std::move(fill), c.color_stops, c.gradient_rect, c.compositing_and_blending_operator);
        return;
    }
    case DisplayListCommandType::PaintRadialGradient: {
        auto c = read.operator()<PaintRadialGradient>();
        if (c.size.is_empty())
            break;
        auto center = c.center.translated(c.rect.location());
        auto fill = std::unique_ptr<tvg::RadialGradient>(tvg::RadialGradient::gen());
        fill->radial(center.x(), center.y(), c.size.height(), center.x(), center.y(), 0);
        SkMatrix scale;
        scale.setScale(static_cast<float>(c.size.width()) / c.size.height(), 1.f, center.x(), center.y());
        fill->transform(tvg_matrix(scale));
        gradient(std::move(fill), c.color_stops, c.rect, c.compositing_and_blending_operator);
        return;
    }
    case DisplayListCommandType::DrawGlyphRun: {
        auto run = read.operator()<DrawGlyphRun>();
        auto const& font = fonts->font(run.font_id);
        if (font.is_invisible() || !(run.scale > 0))
            return;
        if (run.orientation != Gfx::GlyphRunOrientation::Horizontal) {
            bool ccw = run.orientation == Gfx::GlyphRunOrientation::VerticalCounterclockwise;
            m.preTranslate(ccw ? 0 : run.rect.width(), ccw ? run.rect.height() : 0);
            m.preRotate(ccw ? -90 : 90, run.rect.x(), run.rect.y());
        }
        m.preTranslate(run.translation.x(), run.translation.y());
        auto bytes = data(run.glyphs);
        ReadonlySpan<DisplayListGlyph> glyphs { reinterpret_cast<DisplayListGlyph const*>(bytes.data()), bytes.size() / sizeof(DisplayListGlyph) };
        Vector<u32, 32> glyph_ids;
        for (auto const& glyph : glyphs)
            glyph_ids.append(glyph.glyph_id);
        auto ascent = font.pixel_metrics().ascent;
        Gfx::with_glyph_outlines(font, glyph_ids.span(), [&](size_t i, Gfx::PathData const& outline) {
            if (outline.verbs.is_empty())
                return;
            auto position = m;
            position.preScale(run.scale, run.scale);
            position.preTranslate(glyphs[i].position.x(), glyphs[i].position.y() + ascent);
            if (path(Gfx::to_skia_path(outline), run.color, position, run.stroke_width / run.scale))
                ++glyphs_painted;
        });
        return;
    }
    case DisplayListCommandType::FillPath: {
        auto fill = read.operator()<FillPath>();
        if (fill.paint_kind != PathPaintKind::Color)
            break;
        auto outline = Gfx::skia_path_from_serialized_bytes(data(fill.path_data));
        outline.setFillType(Gfx::to_skia_path_fill_type(fill.winding_rule));
        path(outline, fill.color, m, 0, tvg::StrokeCap::Butt, tvg::StrokeJoin::Miter, 4, blend_method(fill.compositing_and_blending_operator));
        return;
    }
    case DisplayListCommandType::StrokePath: {
        auto stroke = read.operator()<StrokePath>();
        if (stroke.paint_kind != PathPaintKind::Color || stroke.dash_array.size || stroke.thickness <= 0)
            break;
        auto outline = Gfx::skia_path_from_serialized_bytes(data(stroke.path_data));
        constexpr tvg::StrokeCap caps[] = { tvg::StrokeCap::Butt, tvg::StrokeCap::Round, tvg::StrokeCap::Square };
        constexpr tvg::StrokeJoin joins[] = { tvg::StrokeJoin::Miter, tvg::StrokeJoin::Round, tvg::StrokeJoin::Bevel };
        path(outline, stroke.color, m, stroke.thickness, caps[Gfx::to_skia_cap(stroke.cap_style)], joins[Gfx::to_skia_join(stroke.join_style)], stroke.miter_limit);
        return;
    }
    case DisplayListCommandType::PaintCaret: {
        auto c = read.operator()<PaintCaret>();
        rect(c.rect, c.color);
        return;
    }
    case DisplayListCommandType::DrawRect: {
        auto c = read.operator()<DrawRect>();
        path(SkPath::Rect(Gfx::to_skia_rect(c.rect)), c.color, m, 1);
        return;
    }
    case DisplayListCommandType::DrawEllipse: {
        auto c = read.operator()<DrawEllipse>();
        path(SkPath::Oval(Gfx::to_skia_rect(c.rect)), c.color, m, c.thickness);
        return;
    }
    case DisplayListCommandType::DrawLine: {
        auto c = read.operator()<DrawLine>();
        if (c.style != Gfx::LineStyle::Solid)
            break;
        path(SkPath::Line(Gfx::to_skia_point(c.from), Gfx::to_skia_point(c.to)), c.color, m, c.thickness);
        return;
    }
    case DisplayListCommandType::FillRoundedRectRing: {
        auto c = read.operator()<FillRoundedRectRing>();
        auto outer = Gfx::to_skia_rect(c.rect);
        auto inner = SkRect::MakeLTRB(outer.left() + c.left_width, outer.top() + c.top_width, outer.right() - c.right_width, outer.bottom() - c.bottom_width);
        auto corners = c.corner_radii;
        auto inset = [](auto& r, int x, int y) {
            r.horizontal_radius = max(0, r.horizontal_radius - x);
            r.vertical_radius = max(0, r.vertical_radius - y);
        };
        inset(corners.top_left, c.left_width, c.top_width);
        inset(corners.top_right, c.right_width, c.top_width);
        inset(corners.bottom_left, c.left_width, c.bottom_width);
        inset(corners.bottom_right, c.right_width, c.bottom_width);
        SkPathBuilder ring(SkPathFillType::kEvenOdd);
        ring.addPath(rounded_rect(outer, c.corner_radii));
        if (!inner.isEmpty())
            ring.addPath(rounded_rect(inner, corners));
        path(ring.detach(), c.color, m);
        return;
    }
    case DisplayListCommandType::DrawScaledDecodedImageFrame: {
        if (!images)
            break;
        auto c = read.operator()<DrawScaledDecodedImageFrame>();
        auto const& frame = images->image_frame(c.frame_id).frame();
        pixels(reinterpret_cast<u32 const*>(frame.pixels().data()), frame.pitch() / 4, frame.width(), frame.height(),
            pixel_format(frame.format(), frame.alpha_type()), c.src_rect.has_value() ? Gfx::to_skia_rect(c.src_rect.value()) : SkRect::MakeIWH(frame.width(), frame.height()), Gfx::to_skia_rect(c.dst_rect));
        return;
    }
    default:
        break;
    }
    ++skipped.ensure(static_cast<unsigned>(type));
}

Replay::FfiRasterPainter DisplayListPlayerThorVG::Impl::callbacks()
{
    return {
        .context = this,
        .canvas_matrix = [](void* c) {
            Gfx::FloatMatrix4x4 m;
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    m[i, j] = static_cast<Impl*>(c)->state.matrix.rc(i, j);
            return m; },
        .set_matrix = [](void* c, Gfx::FloatMatrix4x4 const* m) {
            auto& matrix = static_cast<Impl*>(c)->state.matrix;
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    matrix.setRC(i, j, (*m)[i, j]); },
        .would_be_fully_clipped_by_painter = [](void* c, Gfx::IntRect) { return static_cast<Impl*>(c)->state.hidden; },
        .push_clip = [](void* c, ReplayClip const* clip) {
            auto& p = *static_cast<Impl*>(c);
            p.group();
            if (clip->mode == ClipMode::Intersect)
                p.clip(rounded_rect(Gfx::to_skia_rect(clip->rect), clip->corner_radii));
            else
                p.state.hidden = true; },
        .push_clip_path = [](void* c, Gfx::FFI::PathGeometryData const* geometry, u64, Gfx::WindingRule rule) {
            auto& p = *static_cast<Impl*>(c);
            p.group();
            auto path = Gfx::to_skia_path(Gfx::PathData::from_ffi(*geometry));
            path.setFillType(Gfx::to_skia_path_fill_type(rule));
            p.clip(path); },
        .push_layer = [](void* c, ReplayLayer const* layer) { static_cast<Impl*>(c)->group(layer->opacity, blend_method(layer->blend_mode)); },
        .push_mask = [](void* c, ReplayMask const*, ReplayLayer const*) { static_cast<Impl*>(c)->scope(true); },
        .begin_mask_content = [](void*, ReplayMask const*) { },
        .pop_mask = [](void* c) { static_cast<Impl*>(c)->pop(); },
        .pop = [](void* c) { static_cast<Impl*>(c)->pop(); },
        .push_device_space_plane_clip = [](void* c, Gfx::FloatVector3 const*, size_t) { static_cast<Impl*>(c)->scope(true); },
        .push_transform = [](void* c, Gfx::AffineTransform const* t) {
            auto& p = *static_cast<Impl*>(c);
            p.scope();
            p.state.matrix.preConcat(SkM44(Gfx::to_skia_matrix(*t))); },
        .push_clip_path_bytes = [](void* c, u8 const* bytes, size_t n, Gfx::WindingRule rule, ClipMode mode) {
            auto& p = *static_cast<Impl*>(c);
            p.group();
            if (mode == ClipMode::Intersect) {
                auto path = Gfx::skia_path_from_serialized_bytes({ bytes, n });
                path.setFillType(Gfx::to_skia_path_fill_type(rule));
                p.clip(path);
            } else
                p.state.hidden = true; },
        .play_command = [](void* c, DisplayListCommandType type, u8 const* bytes, u8 const* payload, size_t n) { static_cast<Impl*>(c)->command(type, bytes, { payload, n }); },
        .fill_rect = [](void* c, Replay::ResolvedFillRect const* fill) { static_cast<Impl*>(c)->rect(fill->rect, fill->color, blend_method(fill->compositing_and_blending_operator)); },
        .fill_rect_with_rounded_corners = [](void* c, Replay::ResolvedFillRectWithRoundedCorners const* fill) {
            auto& p = *static_cast<Impl*>(c);
            p.path(rounded_rect(Gfx::to_skia_rect(fill->rect), fill->corner_radii), fill->color, p.state.matrix.asM33()); },
        .paint_scrollbar = [](void* c, Replay::PresentedScrollbar const* bar) {
            auto& p = *static_cast<Impl*>(c);
            p.group(bar->opacity);
            p.rect(bar->gutter_rect, bar->track_color);
            auto r = Gfx::to_skia_rect(bar->thumb_rect);
            p.path(SkPath::RRect(r, r.width() / 2, r.width() / 2), bar->thumb_color, p.state.matrix.asM33());
            p.pop(); },
        .draw_composited_context = [](void* c, Replay::CompositedContextDraw const* d) { static_cast<Impl*>(c)->surface(*static_cast<Gfx::PaintingSurface*>(d->surface), Gfx::to_skia_rect(d->src_rect), Gfx::to_skia_rect(d->dst_rect)); },
        .begin_isolated_group = [](void* c, Replay::IsolatedGroup const* g) {
            auto& p = *static_cast<Impl*>(c);
            p.group(g->opacity, blend_method(g->compositing_and_blending_operator));
            p.clip(SkPath::Rect(Gfx::to_skia_rect(g->clip_rect))); },
        .begin_isolated_group_mask = [](void* c, Gfx::MaskKind) {
            auto& p = *static_cast<Impl*>(c);
            if (!p.state.hidden)
                p.state.scene->opacity(0);
            p.scope(true); },
        .end_isolated_group_mask = [](void* c) { static_cast<Impl*>(c)->pop(); },
        .end_isolated_group = [](void* c) { static_cast<Impl*>(c)->pop(); },
        .begin_tile_raster = [](void*, Replay::TileRaster const*) { return false; },
        .end_tile_raster = [](void*) { },
        .begin_vector_image = [](void* c, Replay::VectorImagePlacement const* v, bool) {
            auto& p = *static_cast<Impl*>(c);
            if (p.state.hidden || v->rect.is_empty() || v->recorded_size.is_empty())
                return Replay::VectorImageReplay::Skipped;
            p.group();
            p.clip(SkPath::Rect(Gfx::to_skia_rect(v->rect)));
            p.state.matrix.preConcat(SkM44(SkMatrix::RectToRect(SkRect::MakeIWH(v->recorded_size.width(), v->recorded_size.height()), Gfx::to_skia_rect(v->rect))));
            return Replay::VectorImageReplay::Directly; },
        .end_vector_image = [](void* c) { static_cast<Impl*>(c)->pop(); },
        .draw_surface = [](void* c, Replay::SurfaceDraw const* d) { static_cast<Impl*>(c)->surface(*static_cast<Gfx::PaintingSurface*>(d->surface), Gfx::to_skia_rect(d->source_rect), Gfx::to_skia_rect(d->target_rect)); },
    };
}

DisplayListPlayerThorVG::DisplayListPlayerThorVG()
    : m_impl(make<Impl>())
{
}
DisplayListPlayerThorVG::~DisplayListPlayerThorVG() = default;

Replay::FfiRasterPainter DisplayListPlayerThorVG::begin(Gfx::PaintingSurface& surface, DrawnFonts const& fonts, DrawnImageFrames const* images)
{
    m_impl->open(surface);
    m_impl->fonts = &fonts;
    m_impl->images = images;
    return m_impl->callbacks();
}

void DisplayListPlayerThorVG::end()
{
    m_impl->finish();
    if (m_impl->glyphs_painted && !m_impl->reported_text) {
        dbgln("ThorVG painted {} positioned glyphs into the page bitmap", m_impl->glyphs_painted);
        m_impl->reported_text = true;
    }
    for (auto const& [type, count] : m_impl->skipped) {
        if (m_impl->reported_skips.set(type) != AK::HashSetResult::InsertedNewEntry)
            continue;
        switch (static_cast<DisplayListCommandType>(type)) {
#define REPORT_SKIP(name)                           \
    case DisplayListCommandType::name:              \
        dbgln("ThorVG skipped {} x " #name, count); \
        break;
            ENUMERATE_DISPLAY_LIST_COMMANDS_PLAYED_AS_RECORDED(REPORT_SKIP)
#undef REPORT_SKIP
        default:
            break;
        }
    }
    m_impl->skipped.clear();
    m_impl->glyphs_painted = 0;
}

void DisplayListPlayerThorVG::clear(Gfx::PaintingSurface& surface, Gfx::Color color)
{
    auto& bitmap = *surface.backing_bitmap();
    auto area = surface.canvas().getDeviceClipBounds();
    if (!area.intersect(SkIRect::MakeWH(bitmap.width(), bitmap.height())))
        return;
    surface.notify_content_will_change();
    color.set_alpha(color.alpha(), bitmap.alpha_type());
    bitmap.set_pixel(area.left(), area.top(), color);
    auto value = bitmap.scanline(area.top())[area.left()];
    for (int y = area.top(); y < area.bottom(); ++y)
        std::fill_n(bitmap.scanline(y) + area.left(), area.width(), value);
}

void DisplayListPlayerThorVG::blit(Gfx::PaintingSurface& target, Gfx::PaintingSurface const& source, Gfx::IntRect src, Gfx::IntRect dst, bool replace)
{
    Impl painter;
    painter.open(target);
    painter.surface(source, Gfx::to_skia_rect(src), Gfx::to_skia_rect(dst));
    if (replace) {
        auto& canvas = target.canvas();
        SkAutoCanvasRestore save(&canvas, true);
        canvas.clipRect(Gfx::to_skia_rect(dst));
        clear(target, Gfx::Color::Transparent);
    }
    painter.finish();
}

}
