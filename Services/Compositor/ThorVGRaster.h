/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <memory>
#include <thorvg.h>

class SkPath;

namespace Compositor {

bool append_thorvg_path(tvg::Shape&, SkPath const&);

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
