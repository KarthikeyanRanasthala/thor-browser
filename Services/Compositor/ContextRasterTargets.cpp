/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf8View.h>
#include <Compositor/ContextRasterTargets.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>

#ifndef LADYBIRD_CPU_ONLY
#    include <Compositor/FontSkia.h>
#    include <core/SkFont.h>
#    include <core/SkFontMetrics.h>
#    include <core/SkFontTypes.h>
#endif

namespace Compositor {

ContextRasterTargets::ContextRasterTargets(OwnedCanvasSurfaces canvas_surfaces)
    : m_canvas_surfaces(canvas_surfaces)
{
}

void ContextRasterTargets::retain_only_fonts(ReadonlySpan<u64> font_ids)
{
#ifdef LADYBIRD_CPU_ONLY
    (void)m_fonts.retain_only(font_ids);
#else
    m_raster_cache.forget_fonts(m_fonts.retain_only(font_ids));
#endif
}

void ContextRasterTargets::retain_only_image_frames(ReadonlySpan<u64> image_frame_ids)
{
#ifdef LADYBIRD_CPU_ONLY
    (void)m_image_frames.retain_only(image_frame_ids);
#else
    m_raster_cache.forget_image_frames(m_image_frames.retain_only(image_frame_ids));
#endif
}

static RefPtr<Gfx::Font> fallback_font_of_overlay_labels(float font_size_in_points)
{
    static Optional<FlyString> s_system_sans_serif_family;
    auto& font_database = Gfx::FontDatabase::the();
    if (!s_system_sans_serif_family.has_value())
        s_system_sans_serif_family = font_database.resolve_generic_family("sans-serif"sv, 400, 0);
    if (s_system_sans_serif_family.has_value()) {
        if (auto font = font_database.get(*s_system_sans_serif_family, font_size_in_points, 400, Gfx::FontWidth::Normal, 0))
            return font;
    }
    return font_database.get("SerenitySans"_fly_string, font_size_in_points, 400, Gfx::FontWidth::Normal, 0);
}

Optional<OverlayLabel> ContextRasterTargets::shape_overlay_label(Optional<StringView> font_family, float font_size_in_points, double device_pixel_ratio, StringView text)
{
    RefPtr<Gfx::Font> font;
    if (font_family.has_value())
        font = Gfx::FontDatabase::the().get(MUST(FlyString::from_utf8(*font_family)), font_size_in_points, 400, Gfx::FontWidth::Normal, 0);
    if (!font)
        font = fallback_font_of_overlay_labels(font_size_in_points);
    if (!font)
        return {};
    OverlayLabel label {};
#ifdef LADYBIRD_CPU_ONLY
    for (auto code_point : Utf8View(text)) {
        label.glyph_ids.append(font->glyph_id_for_code_point(code_point));
        label.glyph_x_positions.append(label.width);
        label.width += font->horizontal_advance_of_code_point(code_point).value_or(0) * device_pixel_ratio;
    }
    label.ascent = -font->pixel_metrics().ascent * device_pixel_ratio;
    label.descent = font->pixel_metrics().descent * device_pixel_ratio;
#else
    auto skia_font = Compositor::skia_font(*font, static_cast<float>(device_pixel_ratio));
    if (!skia_font.has_value())
        return {};

    auto glyph_count = skia_font->countText(text.characters_without_null_termination(), text.length(), SkTextEncoding::kUTF8);
    Vector<SkGlyphID> glyphs;
    glyphs.resize(glyph_count);
    skia_font->textToGlyphs(text.characters_without_null_termination(), text.length(), SkTextEncoding::kUTF8, { glyphs.data(), glyphs.size() });
    label.glyph_x_positions.resize(glyph_count);
    skia_font->getXPos({ glyphs.data(), glyphs.size() }, { label.glyph_x_positions.data(), label.glyph_x_positions.size() });
    label.glyph_ids.ensure_capacity(glyph_count);
    for (auto glyph : glyphs)
        label.glyph_ids.unchecked_append(glyph);
    label.width = skia_font->measureText(text.characters_without_null_termination(), text.length(), SkTextEncoding::kUTF8);
    SkFontMetrics font_metrics;
    skia_font->getMetrics(&font_metrics);
    label.ascent = font_metrics.fAscent;
    label.descent = font_metrics.fDescent;
#endif
    label.font_ascent = font->pixel_metrics().ascent;
    label.font_id = m_overlay_fonts.add(*font).value();
    return label;
}

bool ContextRasterTargets::set_drawn_video_sinks(ReadonlySpan<u64> handles)
{
    HashTable<Media::VideoSinkHandle> drawn_video_sinks;
    for (auto handle : handles)
        drawn_video_sinks.set(Media::VideoSinkHandle { handle });
    bool changed = drawn_video_sinks.size() != m_drawn_video_sinks.size();
    for (auto handle : m_drawn_video_sinks) {
        if (drawn_video_sinks.contains(handle))
            continue;
#ifndef LADYBIRD_CPU_ONLY
        m_raster_cache.forget_video_sink(handle);
#endif
        changed = true;
    }
    if (!changed)
        return false;
    m_drawn_video_sinks = move(drawn_video_sinks);
    return true;
}

}
