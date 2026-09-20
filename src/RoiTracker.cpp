#include "RoiTracker.h"

#include <algorithm>

void RoiTracker::reset()
{
    filtered_ = {};
    valid_ = false;
}

void RoiTracker::update(const QPolygonF& decodedCorners)
{
    if (decodedCorners.size() != 4)
        return;
    const QRectF measured = decodedCorners.boundingRect();
    if (!measured.isValid())
        return;
    if (!valid_) {
        filtered_ = measured;
        valid_ = true;
        return;
    }
    constexpr double historyWeight = 0.65;
    filtered_.setX(historyWeight * filtered_.x() + (1.0 - historyWeight) * measured.x());
    filtered_.setY(historyWeight * filtered_.y() + (1.0 - historyWeight) * measured.y());
    filtered_.setWidth(historyWeight * filtered_.width()
                       + (1.0 - historyWeight) * measured.width());
    filtered_.setHeight(historyWeight * filtered_.height()
                        + (1.0 - historyWeight) * measured.height());
}

QRect RoiTracker::predict(const QSize& imageSize, double expansion) const
{
    if (!valid_ || imageSize.isEmpty())
        return {};
    expansion = std::clamp(expansion, 0.0, 1.0);
    const double dx = filtered_.width() * expansion * 0.5;
    const double dy = filtered_.height() * expansion * 0.5;
    return filtered_.adjusted(-dx, -dy, dx, dy).toAlignedRect()
        .intersected(QRect(QPoint(0, 0), imageSize));
}
