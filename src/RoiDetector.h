#pragma once

#include "DecodeTypes.h"

#include <QImage>
#include <QPolygonF>
#include <QVector>

struct RoiCandidate
{
    QImage rectifiedImage;
    QPolygonF corners;
    double score = 0.0;
};

class RoiDetector
{
public:
    QVector<RoiCandidate> detect(const QImage& grayImage,
                                 const DecodeRecipe& recipe) const;
    RoiCandidate rectify(const QImage& grayImage, const QPolygonF& corners,
                         double score = 1.0) const;
};
