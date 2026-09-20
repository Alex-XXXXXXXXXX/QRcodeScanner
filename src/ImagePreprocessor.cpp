#include "ImagePreprocessor.h"

#include <QVector>

#include <algorithm>
#include <array>
#include <cmath>

QImage ImagePreprocessor::toGrayscale(const QImage& image)
{
    if (image.format() == QImage::Format_Grayscale8)
        return image;
    return image.convertToFormat(QImage::Format_Grayscale8);
}

QImage ImagePreprocessor::fitForDecode(const QImage& grayImage, int maximumDimension)
{
    if (grayImage.width() <= maximumDimension && grayImage.height() <= maximumDimension)
        return grayImage;
    return grayImage.scaled(maximumDimension, maximumDimension,
                            Qt::KeepAspectRatio, Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_Grayscale8);
}

QImage ImagePreprocessor::normalizeContrast(const QImage& input)
{
    const QImage gray = toGrayscale(input);
    if (gray.isNull())
        return {};

    std::array<quint64, 256> histogram{};
    quint64 count = 0;
    for (int y = 0; y < gray.height(); ++y) {
        const uchar* row = gray.constScanLine(y);
        for (int x = 0; x < gray.width(); ++x) {
            ++histogram[static_cast<size_t>(row[x])];
            ++count;
        }
    }

    auto percentile = [&](double fraction) {
        const quint64 target = static_cast<quint64>(fraction * count);
        quint64 sum = 0;
        for (int i = 0; i < 256; ++i) {
            sum += histogram[static_cast<size_t>(i)];
            if (sum >= target)
                return i;
        }
        return 255;
    };

    const int low = percentile(0.01);
    const int high = percentile(0.99);
    if (high <= low + 2)
        return gray;

    std::array<uchar, 256> lut{};
    for (int i = 0; i < 256; ++i) {
        const int mapped = (i - low) * 255 / (high - low);
        lut[static_cast<size_t>(i)] = static_cast<uchar>(std::clamp(mapped, 0, 255));
    }

    QImage output(gray.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < gray.height(); ++y) {
        const uchar* source = gray.constScanLine(y);
        uchar* destination = output.scanLine(y);
        for (int x = 0; x < gray.width(); ++x)
            destination[x] = lut[static_cast<size_t>(source[x])];
    }
    return output;
}

QImage ImagePreprocessor::unsharpMask(const QImage& input, double amount)
{
    const QImage gray = toGrayscale(input);
    if (gray.width() < 3 || gray.height() < 3)
        return gray;

    QImage output(gray.size(), QImage::Format_Grayscale8);
    output.fill(255);
    for (int y = 1; y < gray.height() - 1; ++y) {
        const uchar* previous = gray.constScanLine(y - 1);
        const uchar* current = gray.constScanLine(y);
        const uchar* next = gray.constScanLine(y + 1);
        uchar* destination = output.scanLine(y);
        for (int x = 1; x < gray.width() - 1; ++x) {
            const int blurred = previous[x - 1] + 2 * previous[x] + previous[x + 1]
                + 2 * current[x - 1] + 4 * current[x] + 2 * current[x + 1]
                + next[x - 1] + 2 * next[x] + next[x + 1];
            const double value = current[x] + amount * (current[x] - blurred / 16.0);
            destination[x] = static_cast<uchar>(std::clamp(
                static_cast<int>(std::lround(value)), 0, 255));
        }
    }

    if (gray.width() > 0) {
        for (int y = 0; y < gray.height(); ++y) {
            output.scanLine(y)[0] = gray.constScanLine(y)[0];
            output.scanLine(y)[gray.width() - 1] = gray.constScanLine(y)[gray.width() - 1];
        }
    }
    if (gray.height() > 0) {
        std::copy_n(gray.constScanLine(0), gray.width(), output.scanLine(0));
        std::copy_n(gray.constScanLine(gray.height() - 1), gray.width(),
                    output.scanLine(gray.height() - 1));
    }
    return output;
}

QImage ImagePreprocessor::localThreshold(const QImage& input, int windowSize, double bias)
{
    const QImage gray = toGrayscale(input);
    if (gray.isNull())
        return {};

    const int width = gray.width();
    const int height = gray.height();
    if (windowSize <= 0)
        windowSize = std::max(15, std::min(width, height) / 24);
    if ((windowSize & 1) == 0)
        ++windowSize;
    const int radius = windowSize / 2;

    const int integralWidth = width + 1;
    QVector<quint64> integral((width + 1) * (height + 1), 0);
    for (int y = 1; y <= height; ++y) {
        quint64 rowSum = 0;
        const uchar* row = gray.constScanLine(y - 1);
        for (int x = 1; x <= width; ++x) {
            rowSum += row[x - 1];
            integral[y * integralWidth + x] =
                integral[(y - 1) * integralWidth + x] + rowSum;
        }
    }

    QImage output(gray.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < height; ++y) {
        const uchar* source = gray.constScanLine(y);
        uchar* destination = output.scanLine(y);
        const int y0 = std::max(0, y - radius);
        const int y1 = std::min(height - 1, y + radius);
        for (int x = 0; x < width; ++x) {
            const int x0 = std::max(0, x - radius);
            const int x1 = std::min(width - 1, x + radius);
            const quint64 sum = integral[(y1 + 1) * integralWidth + (x1 + 1)]
                - integral[y0 * integralWidth + (x1 + 1)]
                - integral[(y1 + 1) * integralWidth + x0]
                + integral[y0 * integralWidth + x0];
            const int area = (x1 - x0 + 1) * (y1 - y0 + 1);
            const double threshold = (static_cast<double>(sum) / area) * (1.0 - bias);
            destination[x] = source[x] < threshold ? 0 : 255;
        }
    }
    return output;
}

