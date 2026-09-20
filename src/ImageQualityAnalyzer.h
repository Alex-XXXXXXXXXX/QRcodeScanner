#pragma once

#include "DecodeTypes.h"

#include <QImage>

class ImageQualityAnalyzer
{
public:
    static ImageQuality analyze(const QImage& grayImage);
};

