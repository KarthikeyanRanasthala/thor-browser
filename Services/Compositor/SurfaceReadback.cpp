/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/CanvasHost.h>
#include <LibGfx/Bitmap.h>

namespace Compositor {

Gfx::ShareableBitmap CanvasHost::read_back_surface(Gfx::PaintingSurface& surface, Gfx::IntRect rect, Gfx::AlphaType alpha_type, Gfx::ColorSpace const& color_space)
{
    auto clipped_rect = rect.intersected(surface.rect());
    if (clipped_rect.is_empty())
        return {};

    // Unpremultiplied pixels are for ImageData, which stores them as RGBA8888. Reading into that format lets the
    // surface do the conversion.
    auto format = alpha_type == Gfx::AlphaType::Unpremultiplied ? Gfx::BitmapFormat::RGBA8888 : Gfx::BitmapFormat::BGRA8888;
    auto bitmap_or_error = Gfx::Bitmap::create_shareable(format, alpha_type, clipped_rect.size());
    if (bitmap_or_error.is_error())
        return {};

    auto bitmap = bitmap_or_error.release_value();
    surface.flush();
    surface.read_into_bitmap(*bitmap, clipped_rect.location(), color_space);
    return Gfx::ShareableBitmap { move(bitmap), Gfx::ShareableBitmap::ConstructWithKnownGoodBitmap };
}

}
