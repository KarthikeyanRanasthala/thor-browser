/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ThorVGRaster.h"
#include <core/SkPath.h>
#include <core/SkRRect.h>

namespace Compositor {

bool append_thorvg_path(tvg::Shape& shape, SkPath const& path)
{
    using enum tvg::Result;
    SkRect oval;
    if (path.isOval(&oval))
        return shape.appendCircle(oval.centerX(), oval.centerY(), oval.width() / 2, oval.height() / 2) == Success;

    SkRRect rounded;
    if (path.isRRect(&rounded) && rounded.isSimple()) {
        auto rect = rounded.rect();
        auto radii = rounded.radii(SkRRect::kUpperLeft_Corner);
        return shape.appendRect(rect.x(), rect.y(), rect.width(), rect.height(), radii.x(), radii.y()) == Success;
    }

    auto quad = [&](SkPoint start, SkPoint control, SkPoint end) {
        auto c1 = start + (control - start) * (2.f / 3);
        auto c2 = end + (control - end) * (2.f / 3);
        return shape.cubicTo(c1.x(), c1.y(), c2.x(), c2.y(), end.x(), end.y());
    };
    auto iter = path.iter();
    while (auto record = iter.next()) {
        auto p = record->fPoints;
        tvg::Result result;
        switch (record->fVerb) {
        case SkPathVerb::kMove:
            result = shape.moveTo(p[0].x(), p[0].y());
            break;
        case SkPathVerb::kLine:
            result = shape.lineTo(p[1].x(), p[1].y());
            break;
        case SkPathVerb::kQuad:
            result = quad(p[0], p[1], p[2]);
            break;
        case SkPathVerb::kCubic:
            result = shape.cubicTo(p[1].x(), p[1].y(), p[2].x(), p[2].y(), p[3].x(), p[3].y());
            break;
        case SkPathVerb::kClose:
            result = shape.close();
            break;
        case SkPathVerb::kConic: {
            SkPoint points[9];
            int count = SkPath::ConvertConicToQuads(p[0], p[1], p[2], record->fConicWeight, points, 2);
            result = Success;
            for (int i = 0; i < count && result == Success; ++i)
                result = quad(points[2 * i], points[2 * i + 1], points[2 * i + 2]);
            break;
        }
        }
        if (result != Success)
            return false;
    }
    return true;
}

}
