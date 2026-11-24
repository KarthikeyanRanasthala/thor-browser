/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <Compositor/DrawnResources.h>
#include <Compositor/Forward.h>
#include <LibCompositing/RustFFI.h>
#include <LibMedia/VideoSinkHandle.h>
#include <LibMediaCodecs/VideoFrame.h>

namespace Compositor {

class DisplayListPlayerThorVG {
public:
    AK_ALLOC_WITH_KMALLOC;
    DisplayListPlayerThorVG();
    ~DisplayListPlayerThorVG();
    static void set_enabled(bool);
    static bool enabled();
    using CurrentVideoFrame = Function<RefPtr<Media::VideoFrame>(Media::VideoSinkHandle)>;
    Compositing::RustFFI::FfiRasterPainter begin(Gfx::PaintingSurface&, DrawnFonts const&, DrawnImageFrames const*, CurrentVideoFrame = {});
    void end();

    static void clear(Gfx::PaintingSurface&, Gfx::Color);
    static void blit(Gfx::PaintingSurface&, Gfx::PaintingSurface const&, Gfx::IntRect src, Gfx::IntRect dst, bool replace);

private:
    struct Impl;
    NonnullOwnPtr<Impl> m_impl;
};

}
