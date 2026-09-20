#pragma once

#include "DecodeTypes.h"

#include <QImage>
#include <QSize>
#include <QVector>

struct GridRecoveryResult
{
    bool success = false;
    QString text;
    int rows = 0;
    int columns = 0;
    double confidence = 0.0;
    QString diagnostic;
};

class DataMatrixGridRecoverer
{
public:
    static QVector<QSize> legalEcc200Dimensions();
    static bool isLegalEcc200Dimension(int rows, int columns);
    static bool expectedFinderModule(int rows, int columns, int row, int column,
                                     bool& expectedBlack);
    GridRecoveryResult recover(const QImage& rectifiedImage,
                               const DecodeRecipe& recipe,
                               qint64 timeBudgetMicroseconds) const;
};
