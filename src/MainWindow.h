#pragma once

#include "DecodeTypes.h"

#include <QFutureWatcher>
#include <QImage>
#include <QMainWindow>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void openImage();
    void decodeCurrentImage();
    void decodeFinished();

private:
    void buildUi();
    void updatePreview();
    void showReport(const DecodeReport& report);
    void setBusy(bool busy);

    QLabel* imageLabel_ = nullptr;
    QLabel* fileLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* qualityLabel_ = nullptr;
    QPlainTextEdit* resultEdit_ = nullptr;
    QPushButton* openButton_ = nullptr;
    QPushButton* decodeButton_ = nullptr;
    QSpinBox* maximumSymbolsSpinBox_ = nullptr;
    QImage currentImage_;
    QString currentPath_;
    DecodeReport lastReport_;
    bool hasReport_ = false;
    QFutureWatcher<DecodeReport> decodeWatcher_;
};
