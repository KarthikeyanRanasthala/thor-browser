/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <memory>
#include <thorvg.h>

#include <LibGfx/PathData.h>

namespace Compositor {

tvg::Shape* thorvg_shape(Gfx::PathData const&, Gfx::WindingRule);

inline bool thorvg_runtime_ready()
{
    // The compositor schedules the work. Keep ThorVG synchronous so it releases the
    // surface before replay continues, and pair init/term on the same thread.
    struct Runtime {
        bool ready { tvg::Initializer::init(0) == tvg::Result::Success };
        ~Runtime()
        {
            if (ready)
                tvg::Initializer::term();
        }
    };
    static thread_local Runtime runtime;
    return runtime.ready;
}

}
