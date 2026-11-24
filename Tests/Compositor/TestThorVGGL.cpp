/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/DisplayListPlayerThorVG.h>
#include <Compositor/PaintingSurface.h>
#include <Compositor/ThorVGGLTarget.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibTest/TestCase.h>

TEST_CASE(thorvg_gl_paints_and_copies_a_tile_into_a_shared_page)
{
    auto initialized = Compositor::ThorVGGLTarget::initialize();
    if (initialized.is_error()) {
        warnln("{}", initialized.error());
        FAIL("This test requires a working accelerated OpenGL context and a GL-enabled ThorVG");
        return;
    }

    auto backing = MUST(Gfx::SharedImageBuffer::try_create({ 32, 24 }));
    auto page = MUST(Gfx::PaintingSurface::wrap_shared_gl_buffer(backing));
    auto tile = MUST(Gfx::PaintingSurface::try_create_with_size({ 16, 16 }, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied));
    VERIFY(page->gl_target() && tile->gl_target());
    Compositor::DisplayListPlayerThorVG::clear(*page, Gfx::Color::Black);
    Compositor::DisplayListPlayerThorVG::clear(*tile, Gfx::Color::White);

    Compositor::DisplayListPlayerThorVG player;
    Compositor::DrawnFonts fonts;
    tile->clip_rect({ 0, 0, 10, 8 });
    auto passes_before = Compositor::ThorVGGLTarget::painted_passes();
    auto painter = player.begin(*tile, fonts, nullptr);
    Compositing::RustFFI::ResolvedFillRect fill { { 2, 1, 14, 12 }, Gfx::Color::Red, Gfx::CompositingAndBlendingOperator::Normal };
    painter.fill_rect(painter.context, &fill);
    player.end();
    EXPECT_EQ(Compositor::ThorVGGLTarget::painted_passes(), passes_before + 1);

    page->clip_rect({ 5, 2, 17, 18 });
    Compositor::DisplayListPlayerThorVG::blit(*page, *tile, tile->rect(), { 4, 3, 16, 16 }, true);
    // Inspect the exported surface independently, as the UI does. The unequal offsets catch a vertical flip.
    auto imported = MUST(Gfx::SharedImageBuffer::import_from_shared_image(backing.export_shared_image()));
    VERIFY(imported.iosurface_handle().lock_read_only());
    auto bitmap = imported.bitmap();
    EXPECT_EQ(bitmap->get_pixel(6, 4), Gfx::Color(Gfx::Color::Red));
    EXPECT_EQ(bitmap->get_pixel(13, 10), Gfx::Color(Gfx::Color::Red));
    EXPECT_EQ(bitmap->get_pixel(14, 10), Gfx::Color(Gfx::Color::White));
    EXPECT_EQ(bitmap->get_pixel(6, 11), Gfx::Color(Gfx::Color::White));
    EXPECT_EQ(bitmap->get_pixel(4, 5), Gfx::Color(Gfx::Color::Black));
    imported.iosurface_handle().unlock_read_only();
}
