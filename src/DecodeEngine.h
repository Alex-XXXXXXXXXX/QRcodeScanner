#pragma once

#include "DecodeTypes.h"

#include <QImage>

class DecodeEngine
{
public:
    DecodeReport decode(const QImage& image) const;
    DecodeReport decode(const DecodeRequest& request) const;

private:
    static bool tryDecode(const QImage& image, bool hardMode,
                          QString& decodedText, QString& decodedFormat,
                          QString& detectionDiagnostic,
                          QVector<QPointF>& corners,
                          const DecodeRecipe& recipe);
};
