#pragma once

#include <QImage>

class ImagePreprocessor
{
public:
    static QImage toGrayscale(const QImage& image);
    static QImage fitForDecode(const QImage& grayImage, int maximumDimension = 1920);
    static QImage normalizeContrast(const QImage& grayImage);
    static QImage unsharpMask(const QImage& grayImage, double amount = 1.25);
    static QImage localThreshold(const QImage& grayImage, int windowSize = 0,
                                 double bias = 0.12);
};

