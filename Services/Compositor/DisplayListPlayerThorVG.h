/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <Compositor/DrawnResources.h>
#include <Compositor/Forward.h>
#include <LibCompositing/RustFFI.h>

namespace Compositor {

class DisplayListPlayerThorVG {
public:
    AK_ALLOC_WITH_KMALLOC;
    DisplayListPlayerThorVG();
    ~DisplayListPlayerThorVG();
    static void set_enabled(bool);
    static bool enabled();
    Compositing::RustFFI::FfiRasterPainter begin(Gfx::PaintingSurface&, DrawnFonts const&, DrawnImageFrames const*);
    void end();

    static void clear(Gfx::PaintingSurface&, Gfx::Color);
    static void blit(Gfx::PaintingSurface&, Gfx::PaintingSurface const&, Gfx::IntRect src, Gfx::IntRect dst, bool replace);

private:
    struct Impl;
    NonnullOwnPtr<Impl> m_impl;
};

}
