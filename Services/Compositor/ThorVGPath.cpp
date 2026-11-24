/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <Compositor/ThorVGRaster.h>
#include <LibGfx/PathData.h>

namespace Compositor {

static void quadratic(tvg::Shape& shape, Gfx::FloatPoint a, Gfx::FloatPoint b, Gfx::FloatPoint c)
{
    auto first = a + (b - a) * (2.f / 3);
    auto second = c + (b - c) * (2.f / 3);
    shape.cubicTo(first.x(), first.y(), second.x(), second.y(), c.x(), c.y());
}

// Split a rational quadratic in homogeneous coordinates until polynomial quadratics
// approximate it to a fraction of a local pixel. Bound the work for extreme weights.
static void conic(tvg::Shape& shape, Gfx::FloatPoint a, Gfx::FloatPoint b, Gfx::FloatPoint c, float w, unsigned depth)
{
    auto t = w / (1 + w);
    auto ab = a + (b - a) * t;
    auto cb = c + (b - c) * t;
    auto middle = ab + (cb - ab) * .5f;
    auto polynomial_middle = (a + b * 2.f + c) * .25f;
    if (depth == 8 || middle.distance_from(polynomial_middle) <= .05f) {
        quadratic(shape, a, b, c);
        return;
    }
    auto split_weight = sqrtf(.5f + .5f * w);
    conic(shape, a, ab, middle, split_weight, depth + 1);
    conic(shape, middle, cb, c, split_weight, depth + 1);
}

tvg::Shape* thorvg_shape(Gfx::PathData const& path, Gfx::WindingRule rule)
{
    auto* shape = tvg::Shape::gen();
    shape->fillRule(rule == Gfx::WindingRule::EvenOdd ? tvg::FillRule::EvenOdd : tvg::FillRule::NonZero);
    size_t point = 0;
    size_t weight = 0;
    Gfx::FloatPoint current, start;
    for (auto verb : path.verbs) {
        if (verb == Gfx::PathVerb::Close) {
            shape->close();
            current = start;
            continue;
        }
        auto p = path.points[point++];
        switch (verb) {
        case Gfx::PathVerb::Move:
            shape->moveTo(p.x(), p.y());
            start = p;
            break;
        case Gfx::PathVerb::Line:
            shape->lineTo(p.x(), p.y());
            break;
        case Gfx::PathVerb::Quad:
        case Gfx::PathVerb::Conic: {
            auto end = path.points[point++];
            if (verb == Gfx::PathVerb::Quad)
                quadratic(*shape, current, p, end);
            else
                conic(*shape, current, p, end, path.conic_weights[weight++], 0);
            p = end;
            break;
        }
        case Gfx::PathVerb::Cubic: {
            auto control = path.points[point++];
            auto end = path.points[point++];
            shape->cubicTo(p.x(), p.y(), control.x(), control.y(), end.x(), end.y());
            p = end;
            break;
        }
        case Gfx::PathVerb::Close:
            VERIFY_NOT_REACHED();
        }
        current = p;
    }
    return shape;
}

}
