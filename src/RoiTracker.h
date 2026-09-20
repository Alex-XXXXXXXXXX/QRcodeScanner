#pragma once

#include <QPolygonF>
#include <QRect>
#include <QSize>

class RoiTracker
{
public:
    void reset();
    void update(const QPolygonF& decodedCorners);
    QRect predict(const QSize& imageSize, double expansion = 0.18) const;

private:
    QRectF filtered_;
    bool valid_ = false;
};
