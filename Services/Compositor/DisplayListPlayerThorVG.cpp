/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/DisplayListPlayerThorVG.h>
#include <Compositor/PaintingSurface.h>
#include <Compositor/ThorVGRaster.h>
#include <LibCompositing/DisplayList/DisplayListCommand.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/CornerRadii.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Gradients.h>
#include <LibGfx/Matrix4x4.h>
#include <LibGfx/PathData.h>
#include <algorithm>

namespace Compositor {

using namespace Compositing;
namespace Replay = Compositing::RustFFI;

#ifdef LADYBIRD_CPU_ONLY
void DisplayListPlayerThorVG::set_enabled(bool) { }
bool DisplayListPlayerThorVG::enabled() { return true; }
#else
static bool s_enabled;
void DisplayListPlayerThorVG::set_enabled(bool enabled) { s_enabled = enabled; }
bool DisplayListPlayerThorVG::enabled() { return s_enabled; }
#endif

static tvg::ColorSpace pixel_format(Gfx::BitmapFormat format, Gfx::AlphaType alpha)
{
    bool bgra = format == Gfx::BitmapFormat::BGRA8888 || format == Gfx::BitmapFormat::BGRx8888;
    if (alpha == Gfx::AlphaType::Unpremultiplied)
        return bgra ? tvg::ColorSpace::ARGB8888S : tvg::ColorSpace::ABGR8888S;
    return bgra ? tvg::ColorSpace::ARGB8888 : tvg::ColorSpace::ABGR8888;
}

static tvg::Matrix tvg_matrix(Gfx::FloatMatrix4x4 const& m)
{
    return { m[0, 0], m[0, 1], m[0, 3], m[1, 0], m[1, 1], m[1, 3], 0, 0, 1 };
}

static Gfx::FloatMatrix4x4 translate(float x, float y) { return Gfx::translation_matrix<float>({ x, y, 0 }); }
static Gfx::FloatMatrix4x4 scale_by(float x, float y) { return Gfx::scale_matrix<float>({ x, y, 1 }); }
static Gfx::FloatMatrix4x4 rotate_about(float degrees, float x, float y)
{
    return translate(x, y) * Gfx::rotation_matrix<float>({ 0, 0, 1 }, AK::to_radians(degrees)) * translate(-x, -y);
}
static Gfx::FloatMatrix4x4 map_rect(Gfx::FloatRect src, Gfx::FloatRect dst)
{
    return translate(dst.x(), dst.y()) * scale_by(dst.width() / src.width(), dst.height() / src.height()) * translate(-src.x(), -src.y());
}
static bool can_transform(Gfx::FloatMatrix4x4 const& m)
{
    for (size_t row = 0; row < 4; ++row)
        for (size_t column = 0; column < 4; ++column)
            if (!isfinite(m[row, column]))
                return false;
    return m[3, 0] == 0 && m[3, 1] == 0 && m[3, 3] == 1;
}
static void append_rounded_rect(tvg::Shape& shape, Gfx::FloatRect r, Gfx::CornerRadii corners)
{
    if (r.is_empty())
        return;
    if (!corners.has_any_radius()) {
        shape.appendRect(r.x(), r.y(), r.width(), r.height(), 0, 0);
        return;
    }
    auto tl = corners.top_left, tr = corners.top_right, br = corners.bottom_right, bl = corners.bottom_left;
    auto factor = 1.f;
    auto fit = [&](int a, int b, float extent) {
        if (static_cast<float>(a) + b > extent)
            factor = min(factor, extent / (static_cast<float>(a) + b));
    };
    fit(tl.horizontal_radius, tr.horizontal_radius, r.width());
    fit(bl.horizontal_radius, br.horizontal_radius, r.width());
    fit(tl.vertical_radius, bl.vertical_radius, r.height());
    fit(tr.vertical_radius, br.vertical_radius, r.height());
    constexpr float k = .55228475f;
    auto corner = [&](float x, float y, float ax, float ay, float bx, float by) {
        shape.lineTo(x + ax, y + ay);
        shape.cubicTo(x + ax * (1 - k), y + ay * (1 - k), x + bx * (1 - k), y + by * (1 - k), x + bx, y + by);
    };
    shape.moveTo(r.left() + factor * tl.horizontal_radius, r.top());
    corner(r.right(), r.top(), -factor * tr.horizontal_radius, 0, 0, factor * tr.vertical_radius);
    corner(r.right(), r.bottom(), 0, -factor * br.vertical_radius, -factor * br.horizontal_radius, 0);
    corner(r.left(), r.bottom(), factor * bl.horizontal_radius, 0, 0, -factor * bl.vertical_radius);
    corner(r.left(), r.top(), 0, factor * tl.vertical_radius, factor * tl.horizontal_radius, 0);
    shape.close();
}
static tvg::Shape* rounded_rect(Gfx::FloatRect rect, Gfx::CornerRadii corners = {})
{
    auto* shape = tvg::Shape::gen();
    append_rounded_rect(*shape, rect, corners);
    return shape;
}
static tvg::Shape* decoded_shape(ReadonlyBytes bytes, Gfx::WindingRule rule)
{
    tvg::Shape* shape = nullptr;
    Gfx::with_contours_of_serialized_path(bytes, [&](auto const& path) { shape = thorvg_shape(path, rule); });
    return shape;
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
        Gfx::FloatMatrix4x4 matrix;
        bool hidden {};
    };
    Vector<State> stack;
    State state;
    std::unique_ptr<tvg::SwCanvas> canvas;
    DrawnFonts const* fonts {};
    DrawnImageFrames const* images {};
    HashMap<unsigned, unsigned> skipped;
    CurrentVideoFrame current_video_frame;
    // Replays for several damaged tiles can request the same frame. Cache its converted pixels
    // by frame identity, releasing the decoder storage as soon as conversion is complete.
    std::unique_ptr<tvg::Picture, decltype(&tvg::Paint::rel)> video_picture { nullptr, &tvg::Paint::rel };
    u64 video_frame_id {};
    Gfx::FloatRect video_source_rect {};
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
        state = { tvg::Scene::gen(), surface.paint_matrix(), false };
        auto clip = surface.device_clip();
        if (clip.is_empty())
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
    // Replay emits the content first, then the paints that determine its coverage.
    // Keep those paints in a detached scene owned by the content's mask.
    void begin_mask_content(Gfx::MaskKind kind)
    {
        scope();
        if (state.hidden)
            return;
        auto* mask = tvg::Scene::gen();
        VERIFY(state.scene->mask(mask, kind == Gfx::MaskKind::Luminance ? tvg::MaskMethod::Luma : tvg::MaskMethod::Alpha) == tvg::Result::Success);
        state.scene = mask;
    }
    void clip(tvg::Shape* shape)
    {
        if (!shape || state.hidden || !can_transform(state.matrix)) {
            if (shape)
                tvg::Paint::rel(shape);
            state.hidden = true;
            return;
        }
        shape->transform(tvg_matrix(state.matrix));
        VERIFY(state.scene->clip(shape) == tvg::Result::Success);
    }

    bool add(tvg::Paint* paint, Gfx::FloatMatrix4x4 const& m)
    {
        if (state.hidden || !can_transform(m) || paint->transform(tvg_matrix(m)) != tvg::Result::Success || state.scene->add(paint) != tvg::Result::Success) {
            tvg::Paint::rel(paint);
            return false;
        }
        has_draws = true;
        return true;
    }

    bool path(tvg::Shape* shape, Gfx::Color color, Gfx::FloatMatrix4x4 const& matrix, float stroke = 0, tvg::StrokeCap cap = tvg::StrokeCap::Butt, tvg::StrokeJoin join = tvg::StrokeJoin::Miter, float miter = 4, tvg::BlendMethod blend = tvg::BlendMethod::Normal)
    {
        if (!shape)
            return false;
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
        }
        if (!add(shape, matrix))
            return false;
        return true;
    }

    void rect(auto const& r, Gfx::Color color, tvg::BlendMethod blend = tvg::BlendMethod::Normal)
    {
        path(rounded_rect(Gfx::FloatRect(r)), color, state.matrix, 0, tvg::StrokeCap::Butt, tvg::StrokeJoin::Miter, 4, blend);
    }

    static tvg::Picture* copy_picture(u32 const* data, size_t pitch, Gfx::IntRect area, tvg::ColorSpace format)
    {
        auto* copy = static_cast<u32*>(calloc(static_cast<size_t>(area.width()) * area.height(), sizeof(u32)));
        if (!copy)
            return nullptr;
        for (int y = 0; y < area.height(); ++y)
            memcpy(copy + static_cast<size_t>(y) * area.width(), data + (area.y() + y) * pitch + area.x(), area.width() * sizeof(u32));
        auto* image = tvg::Picture::gen();
        if (image->load(copy, area.width(), area.width(), area.height(), format, false) != tvg::Result::Success) {
            free(copy);
            tvg::Paint::rel(image);
            return nullptr;
        }
        return image;
    }

    void draw_picture(tvg::Paint* image, Gfx::FloatRect src, Gfx::FloatRect dst)
    {
        group();
        clip(rounded_rect(dst));
        auto m = state.matrix;
        m = m * map_rect(src, dst);
        add(image, m);
        pop();
    }

    void pixels(u32 const* data, size_t pitch, int w, int h, tvg::ColorSpace format, Gfx::FloatRect src, Gfx::FloatRect dst)
    {
        if (state.hidden || src.is_empty() || dst.is_empty() || !data || !Gfx::FloatRect(0, 0, w, h).contains(src))
            return;
        auto area = Gfx::enclosing_int_rect(src);
        if (auto* image = copy_picture(data, pitch, area, format)) {
            src.translate_by(-area.x(), -area.y());
            draw_picture(image, src, dst);
        }
    }

    void draw_video(DrawVideoFrame const& command)
    {
        if (!current_video_frame || command.dst_rect.is_empty())
            return;
        auto frame = current_video_frame(command.video_sink_handle);
        if (!frame)
            return;
        if (!video_picture || frame->id() != video_frame_id) {
            auto pixels = frame->to_bitmap();
            if (pixels.is_error())
                return;
            auto const& bitmap = *pixels.value();
            auto area = Gfx::IntRect(0, 0, min(frame->width(), static_cast<u32>(bitmap.width())), min(frame->height(), static_cast<u32>(bitmap.height())));
            if (area.is_empty())
                return;
            video_picture.reset(copy_picture(bitmap.begin(), bitmap.pitch() / 4, area, pixel_format(bitmap.format(), bitmap.alpha_type())));
            if (!video_picture)
                return;
            video_source_rect = Gfx::FloatRect(area);
            video_frame_id = frame->id();
        }
        video_picture->filter(command.scaling_mode == Gfx::ScalingMode::NearestNeighbor ? tvg::FilterMethod::Nearest : tvg::FilterMethod::Bilinear);
        draw_picture(video_picture->duplicate(), video_source_rect, Gfx::FloatRect(command.dst_rect));
    }

    void surface(Gfx::PaintingSurface const& surface, Gfx::FloatRect src, Gfx::FloatRect dst)
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
    auto m = state.matrix;
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
        auto rect = Gfx::FloatRect(c.gradient_rect);
        auto length = Gfx::calculate_gradient_length(rect.size(), c.gradient_angle);
        auto center = rect.center();
        auto y = center.y() + (.5f - c.first_stop_position) * length;
        auto fill = std::unique_ptr<tvg::LinearGradient>(tvg::LinearGradient::gen());
        fill->linear(center.x(), y, center.x(), y - length * c.repeat_length);
        auto rotation = rotate_about(c.gradient_angle, center.x(), center.y());
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
        auto scale = translate(center.x(), center.y()) * scale_by(static_cast<float>(c.size.width()) / c.size.height(), 1.f) * translate(-center.x(), -center.y());
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
            m = m * translate(ccw ? 0 : run.rect.width(), ccw ? run.rect.height() : 0);
            m = m * rotate_about(ccw ? -90 : 90, run.rect.x(), run.rect.y());
        }
        m = m * translate(run.translation.x(), run.translation.y());
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
            position = position * scale_by(run.scale, run.scale);
            position = position * translate(glyphs[i].position.x(), glyphs[i].position.y() + ascent);
            if (path(thorvg_shape(outline, outline.winding_rule), run.color, position, run.stroke_width / run.scale))
                ++glyphs_painted;
        });
        return;
    }
    case DisplayListCommandType::FillPath: {
        auto fill = read.operator()<FillPath>();
        if (fill.paint_kind != PathPaintKind::Color)
            break;
        auto* outline = decoded_shape(data(fill.path_data), fill.winding_rule);
        path(outline, fill.color, m, 0, tvg::StrokeCap::Butt, tvg::StrokeJoin::Miter, 4, blend_method(fill.compositing_and_blending_operator));
        return;
    }
    case DisplayListCommandType::StrokePath: {
        auto stroke = read.operator()<StrokePath>();
        if (stroke.paint_kind != PathPaintKind::Color || stroke.dash_array.size || stroke.thickness <= 0)
            break;
        auto* outline = decoded_shape(data(stroke.path_data), Gfx::WindingRule::Nonzero);
        constexpr tvg::StrokeCap caps[] = { tvg::StrokeCap::Butt, tvg::StrokeCap::Round, tvg::StrokeCap::Square };
        constexpr tvg::StrokeJoin joins[] = { tvg::StrokeJoin::Miter, tvg::StrokeJoin::Round, tvg::StrokeJoin::Bevel };
        path(outline, stroke.color, m, stroke.thickness, caps[to_underlying(stroke.cap_style)], joins[to_underlying(stroke.join_style)], stroke.miter_limit);
        return;
    }
    case DisplayListCommandType::PaintCaret: {
        auto c = read.operator()<PaintCaret>();
        rect(c.rect, c.color);
        return;
    }
    case DisplayListCommandType::DrawRect: {
        auto c = read.operator()<DrawRect>();
        path(rounded_rect(Gfx::FloatRect(c.rect)), c.color, m, 1);
        return;
    }
    case DisplayListCommandType::DrawEllipse: {
        auto c = read.operator()<DrawEllipse>();
        auto* ellipse = tvg::Shape::gen();
        auto rect = Gfx::FloatRect(c.rect);
        ellipse->appendCircle(rect.center().x(), rect.center().y(), rect.width() / 2, rect.height() / 2);
        path(ellipse, c.color, m, c.thickness);
        return;
    }
    case DisplayListCommandType::DrawLine: {
        auto c = read.operator()<DrawLine>();
        if (c.style != Gfx::LineStyle::Solid)
            break;
        auto* line = tvg::Shape::gen();
        line->moveTo(c.from.x(), c.from.y());
        line->lineTo(c.to.x(), c.to.y());
        path(line, c.color, m, c.thickness);
        return;
    }
    case DisplayListCommandType::FillRoundedRectRing: {
        auto c = read.operator()<FillRoundedRectRing>();
        auto outer = Gfx::FloatRect(c.rect);
        auto inner = Gfx::FloatRect(outer.x() + c.left_width, outer.y() + c.top_width, outer.width() - c.left_width - c.right_width, outer.height() - c.top_width - c.bottom_width);
        auto corners = c.corner_radii;
        auto inset = [](auto& r, int x, int y) {
            r.horizontal_radius = max(0, r.horizontal_radius - x);
            r.vertical_radius = max(0, r.vertical_radius - y);
        };
        inset(corners.top_left, c.left_width, c.top_width);
        inset(corners.top_right, c.right_width, c.top_width);
        inset(corners.bottom_left, c.left_width, c.bottom_width);
        inset(corners.bottom_right, c.right_width, c.bottom_width);
        auto* ring = rounded_rect(outer, c.corner_radii);
        append_rounded_rect(*ring, inner, corners);
        ring->fillRule(tvg::FillRule::EvenOdd);
        path(ring, c.color, m);
        return;
    }
    case DisplayListCommandType::DrawVideoFrame:
        draw_video(read.operator()<DrawVideoFrame>());
        return;
    case DisplayListCommandType::DrawScaledDecodedImageFrame: {
        if (!images)
            break;
        auto c = read.operator()<DrawScaledDecodedImageFrame>();
        auto const& frame = images->image_frame(c.frame_id).frame();
        pixels(reinterpret_cast<u32 const*>(frame.pixels().data()), frame.pitch() / 4, frame.width(), frame.height(),
            pixel_format(frame.format(), frame.alpha_type()), c.src_rect.has_value() ? Gfx::FloatRect(c.src_rect.value()) : Gfx::FloatRect(0, 0, frame.width(), frame.height()), Gfx::FloatRect(c.dst_rect));
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
            return static_cast<Impl*>(c)->state.matrix; },
        .set_matrix = [](void* c, Gfx::FloatMatrix4x4 const* m) { static_cast<Impl*>(c)->state.matrix = *m; },
        .would_be_fully_clipped_by_painter = [](void* c, Gfx::IntRect) { return static_cast<Impl*>(c)->state.hidden; },
        .push_clip = [](void* c, ReplayClip const* clip) {
            auto& p = *static_cast<Impl*>(c);
            p.group();
            if (clip->mode == ClipMode::Intersect)
                p.clip(rounded_rect(Gfx::FloatRect(clip->rect), clip->corner_radii));
            else
                p.state.hidden = true; },
        .push_clip_path = [](void* c, Gfx::FFI::PathGeometryData const* geometry, u64, Gfx::WindingRule rule) {
            auto& p = *static_cast<Impl*>(c);
            p.group();
            p.clip(thorvg_shape(Gfx::PathData::from_ffi(*geometry), rule)); },
        .push_layer = [](void* c, ReplayLayer const* layer) { static_cast<Impl*>(c)->group(layer->opacity, blend_method(layer->blend_mode)); },
        .push_mask = [](void* c, ReplayMask const* mask, ReplayLayer const* backdrop) {
            auto& p = *static_cast<Impl*>(c);
            if (backdrop && backdrop->backdrop_filter_bytes_size)
                p.group(backdrop->opacity, blend_method(backdrop->blend_mode));
            else
                p.group();
            p.clip(rounded_rect(Gfx::FloatRect(mask->rect))); },
        .begin_mask_content = [](void* c, ReplayMask const* mask) { static_cast<Impl*>(c)->begin_mask_content(mask->kind); },
        .pop_mask = [](void* c) {
            auto& p = *static_cast<Impl*>(c);
            p.pop();
            p.pop(); },
        .pop = [](void* c) { static_cast<Impl*>(c)->pop(); },
        .push_device_space_plane_clip = [](void* c, Gfx::FloatVector3 const*, size_t) { static_cast<Impl*>(c)->scope(true); },
        .push_transform = [](void* c, Gfx::AffineTransform const* t) {
            auto& p = *static_cast<Impl*>(c);
            p.scope();
            p.state.matrix = p.state.matrix * t->to_matrix(); },
        .push_clip_path_bytes = [](void* c, u8 const* bytes, size_t n, Gfx::WindingRule rule, ClipMode mode) {
            auto& p = *static_cast<Impl*>(c);
            p.group();
            if (mode == ClipMode::Intersect) {
                p.clip(decoded_shape({ bytes, n }, rule));
            } else
                p.state.hidden = true; },
        .play_command = [](void* c, DisplayListCommandType type, u8 const* bytes, u8 const* payload, size_t n) { static_cast<Impl*>(c)->command(type, bytes, { payload, n }); },
        .fill_rect = [](void* c, Replay::ResolvedFillRect const* fill) { static_cast<Impl*>(c)->rect(fill->rect, fill->color, blend_method(fill->compositing_and_blending_operator)); },
        .fill_rect_with_rounded_corners = [](void* c, Replay::ResolvedFillRectWithRoundedCorners const* fill) {
            auto& p = *static_cast<Impl*>(c);
            p.path(rounded_rect(Gfx::FloatRect(fill->rect), fill->corner_radii), fill->color, p.state.matrix); },
        .paint_scrollbar = [](void* c, Replay::PresentedScrollbar const* bar) {
            auto& p = *static_cast<Impl*>(c);
            p.group(bar->opacity);
            p.rect(bar->gutter_rect, bar->track_color);
            auto r = Gfx::FloatRect(bar->thumb_rect);
            auto* thumb = tvg::Shape::gen();
            thumb->appendRect(r.x(), r.y(), r.width(), r.height(), r.width() / 2, r.width() / 2);
            p.path(thumb, bar->thumb_color, p.state.matrix);
            p.pop(); },
        .draw_composited_context = [](void* c, Replay::CompositedContextDraw const* d) { static_cast<Impl*>(c)->surface(*static_cast<Gfx::PaintingSurface*>(d->surface), Gfx::FloatRect(d->src_rect), Gfx::FloatRect(d->dst_rect)); },
        .begin_isolated_group = [](void* c, Replay::IsolatedGroup const* g) {
            auto& p = *static_cast<Impl*>(c);
            p.group(g->opacity, blend_method(g->compositing_and_blending_operator));
            p.clip(rounded_rect(Gfx::FloatRect(g->clip_rect))); },
        .begin_isolated_group_mask = [](void* c, Gfx::MaskKind kind) { static_cast<Impl*>(c)->begin_mask_content(kind); },
        .end_isolated_group_mask = [](void* c) { static_cast<Impl*>(c)->pop(); },
        .end_isolated_group = [](void* c) { static_cast<Impl*>(c)->pop(); },
        .begin_tile_raster = [](void*, Replay::TileRaster const*) { return false; },
        .end_tile_raster = [](void*) { },
        .begin_vector_image = [](void* c, Replay::VectorImagePlacement const* v, bool) {
            auto& p = *static_cast<Impl*>(c);
            if (p.state.hidden || v->rect.is_empty() || v->recorded_size.is_empty())
                return Replay::VectorImageReplay::Skipped;
            p.group();
            p.clip(rounded_rect(Gfx::FloatRect(v->rect)));
            p.state.matrix = p.state.matrix * map_rect(Gfx::FloatRect(0, 0, v->recorded_size.width(), v->recorded_size.height()), Gfx::FloatRect(v->rect));
            return Replay::VectorImageReplay::Directly; },
        .end_vector_image = [](void* c) { static_cast<Impl*>(c)->pop(); },
        .draw_surface = [](void* c, Replay::SurfaceDraw const* d) { static_cast<Impl*>(c)->surface(*static_cast<Gfx::PaintingSurface*>(d->surface), Gfx::FloatRect(d->source_rect), Gfx::FloatRect(d->target_rect)); },
    };
}

DisplayListPlayerThorVG::DisplayListPlayerThorVG()
    : m_impl(make<Impl>())
{
}
DisplayListPlayerThorVG::~DisplayListPlayerThorVG() = default;

Replay::FfiRasterPainter DisplayListPlayerThorVG::begin(Gfx::PaintingSurface& surface, DrawnFonts const& fonts, DrawnImageFrames const* images, CurrentVideoFrame current_video_frame)
{
    m_impl->open(surface);
    m_impl->fonts = &fonts;
    m_impl->images = images;
    m_impl->current_video_frame = move(current_video_frame);
    return m_impl->callbacks();
}

void DisplayListPlayerThorVG::end()
{
    m_impl->finish();
    m_impl->current_video_frame = nullptr;
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
    auto area = surface.device_clip();
    area.intersect(bitmap.rect());
    if (area.is_empty())
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
    painter.surface(source, Gfx::FloatRect(src), Gfx::FloatRect(dst));
    if (replace) {
        auto count = target.save_state();
        target.clip_rect(dst);
        clear(target, Gfx::Color::Transparent);
        target.restore_state(count);
    }
    painter.finish();
}

}
