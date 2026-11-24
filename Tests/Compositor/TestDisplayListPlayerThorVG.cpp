/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/DisplayListPlayerThorVG.h>
#include <Compositor/PaintingSurface.h>
#include <LibCompositing/DisplayList/DisplayListCommand.h>
#include <LibCore/MappedFile.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Font/Font.h>
#include <LibTest/TestCase.h>
#include <core/SkCanvas.h>

TEST_CASE(thorvg_draws_into_existing_pixels_and_restores_nested_clips)
{
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 48, 32 }));
    auto surface = Gfx::PaintingSurface::wrap_bitmap(*bitmap);
    Compositor::DisplayListPlayerThorVG::clear(*surface, Gfx::Color::White);
    surface->canvas().clipIRect(SkIRect::MakeLTRB(3, 2, 45, 31));
    surface->canvas().translate(4, 0);
    Compositor::DrawnFonts fonts;
    Compositor::DisplayListPlayerThorVG player;
    auto painter = player.begin(*surface, fonts, nullptr);
    auto fill = [&](Gfx::IntRect rect, Gfx::Color color) {
        Compositing::RustFFI::ResolvedFillRect command { rect, color, Gfx::CompositingAndBlendingOperator::Normal };
        painter.fill_rect(painter.context, &command);
    };
    Compositing::ReplayClip clip { { 1, 1, 9, 10 }, {}, Compositing::ClipMode::Intersect };
    painter.push_clip(painter.context, &clip);
    fill({ 0, 0, 48, 32 }, Gfx::Color::Red);
    painter.pop(painter.context);
    fill({ 25, 6, 3, 3 }, Gfx::Color::Blue);
    player.end();
    EXPECT_EQ(bitmap->get_pixel(7, 4), Gfx::Color(Gfx::Color::Red));
    EXPECT_EQ(bitmap->get_pixel(30, 7), Gfx::Color(Gfx::Color::Blue));
    EXPECT_EQ(bitmap->get_pixel(3, 4), Gfx::Color(Gfx::Color::White));
    EXPECT_EQ(bitmap->get_pixel(7, 1), Gfx::Color(Gfx::Color::White));
    EXPECT_EQ(bitmap->get_pixel(20, 8), Gfx::Color(Gfx::Color::White));
}

TEST_CASE(thorvg_draws_both_positioned_glyphs_from_a_registered_font)
{
    auto file = MUST(Core::MappedFile::map("../LibGfx/test-inputs/fonts/text.ttf"sv));
    auto typeface = MUST(Gfx::Typeface::try_load_from_mapped_file(move(file), 0));
    auto font = typeface->font(16);
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 80, 40 }));
    auto surface = Gfx::PaintingSurface::wrap_bitmap(*bitmap);
    Compositor::DisplayListPlayerThorVG::clear(*surface, Gfx::Color::White);
    Compositor::DrawnFonts fonts;
    Compositing::DisplayListGlyph glyphs[] = {
        { { 0, 0 }, font->glyph_id_for_code_point('A') },
        { { 40, 0 }, font->glyph_id_for_code_point('A') }
    };
    EXPECT_NE(glyphs[0].glyph_id, 0u);
    Compositing::DrawGlyphRun run {};
    run.font_id = fonts.add(font);
    run.glyphs = { 0, sizeof(glyphs) };
    run.translation = { 4, 2 };
    run.color = Gfx::Color::Black;
    run.scale = 1;
    Compositor::DisplayListPlayerThorVG player;
    auto painter = player.begin(*surface, fonts, nullptr);
    painter.play_command(painter.context, run.command_type, reinterpret_cast<u8 const*>(&run), reinterpret_cast<u8 const*>(glyphs), sizeof(glyphs));
    player.end();
    size_t ink = 0;
    for (int y = 0; y < bitmap->height(); ++y) {
        for (int x = 0; x < 40; ++x) {
            auto pixel = bitmap->get_pixel(x, y);
            EXPECT_EQ(pixel, bitmap->get_pixel(x + 40, y));
            if (pixel.red() < 200)
                ++ink;
        }
    }
    EXPECT(ink > 10);
}

TEST_CASE(thorvg_composites_a_cropped_tile_into_a_shared_page)
{
    auto tile = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 12, 12 }));
    auto page = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 32, 24 }));
    auto source = Gfx::PaintingSurface::wrap_bitmap(*tile);
    auto target = Gfx::PaintingSurface::wrap_bitmap(*page);
    Compositor::DisplayListPlayerThorVG::clear(*target, Gfx::Color::White);
    Compositor::DisplayListPlayerThorVG::clear(*source, Gfx::Color::Transparent);
    tile->set_pixel(5, 6, Gfx::Color::Red);
    Compositor::DisplayListPlayerThorVG::blit(*target, *source, { 3, 4, 6, 6 }, { 10, 8, 6, 6 }, true);
    EXPECT_EQ(page->get_pixel(12, 10), Gfx::Color(Gfx::Color::Red));
    EXPECT_EQ(page->get_pixel(10, 8), Gfx::Color(Gfx::Color::Transparent));
    EXPECT_EQ(page->get_pixel(9, 8), Gfx::Color(Gfx::Color::White));
}
