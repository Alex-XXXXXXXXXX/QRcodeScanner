#include "MainWindow.h"

#include "DecodeEngine.h"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSplitter>
#include <QStatusBar>
#include <QStringList>
#include <QVBoxLayout>

namespace {

QPolygonF scaledPolygon(const QPolygonF& source, const QSize& sourceSize,
                        const QSize& targetSize)
{
    QPolygonF result;
    if (sourceSize.isEmpty())
        return result;
    const double scaleX = static_cast<double>(targetSize.width()) / sourceSize.width();
    const double scaleY = static_cast<double>(targetSize.height()) / sourceSize.height();
    result.reserve(source.size());
    for (const QPointF& point : source)
        result << QPointF(point.x() * scaleX, point.y() * scaleY);
    return result;
}

QPolygonF reportPrimaryPolygon(const DecodeReport& report)
{
    QPolygonF polygon;
    if (report.corners.size() == 4) {
        for (const QPointF& point : report.corners)
            polygon << point;
    } else if (!report.candidateCorners.isEmpty()
               && report.candidateCorners.first().size() == 4) {
        polygon = report.candidateCorners.first();
    }
    return polygon;
}

void drawPolygonWithVertices(QPainter& painter, const QPolygonF& polygon,
                             const QColor& color, int width,
                             const QString& label)
{
    if (polygon.size() != 4)
        return;
    QPen pen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPolygon(polygon);

    QFont vertexFont = painter.font();
    vertexFont.setBold(true);
    vertexFont.setPixelSize(13);
    painter.setFont(vertexFont);
    for (int index = 0; index < polygon.size(); ++index) {
        painter.setPen(QPen(Qt::white, 2));
        painter.setBrush(color);
        painter.drawEllipse(polygon[index], 6.0, 6.0);
        const QPointF textPoint = polygon[index] + QPointF(8.0, -8.0);
        painter.setPen(QPen(color.lighter(135), 1));
        painter.drawText(textPoint, QStringLiteral("P%1").arg(index + 1));
    }

    painter.setPen(QPen(color, 1));
    painter.drawText(polygon.boundingRect().topLeft() + QPointF(4.0, -8.0), label);
}

void drawReportBadge(QPainter& painter, const DecodeReport& report,
                     const QSize& canvasSize, bool hasPolygon)
{
    QStringList lines;
    lines << (report.success ? QStringLiteral("解码成功")
                             : QStringLiteral("未解码（候选区域）"));
    if (!report.format.isEmpty())
        lines << QStringLiteral("码制：%1").arg(report.format);
    else if (report.estimatedRows > 0)
        lines << QStringLiteral("码制：Data Matrix");
    if (report.estimatedRows > 0 && report.estimatedColumns > 0) {
        lines << QStringLiteral("矩阵：%1×%2")
                     .arg(report.estimatedRows).arg(report.estimatedColumns);
    }
    lines << QStringLiteral("置信度：%1").arg(report.confidence, 0, 'f', 3)
          << QStringLiteral("耗时：%1 ms")
                 .arg(report.totalMicroseconds / 1000.0, 0, 'f', 3);
    if (!hasPolygon)
        lines << QStringLiteral("未找到四角区域");
    if (report.success && !report.text.isEmpty()) {
        QString content = report.text;
        if (content.size() > 38)
            content = content.left(35) + QStringLiteral("...");
        lines << QStringLiteral("内容：%1").arg(content);
    }

    QFont badgeFont = painter.font();
    badgeFont.setPixelSize(14);
    badgeFont.setBold(true);
    painter.setFont(badgeFont);
    const QFontMetrics metrics(badgeFont);
    int textWidth = 0;
    for (const QString& line : lines)
        textWidth = std::max(textWidth, metrics.horizontalAdvance(line));
    const int padding = 10;
    const int lineHeight = metrics.height() + 2;
    const QSize badgeSize(std::min(canvasSize.width() - 16, textWidth + padding * 2),
                          lines.size() * lineHeight + padding * 2);
    const QRect badgeRect(QPoint(8, 8), badgeSize);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(10, 18, 28, 205));
    painter.drawRoundedRect(badgeRect, 7, 7);
    painter.setPen(Qt::white);
    int baseline = badgeRect.top() + padding + metrics.ascent();
    for (const QString& line : lines) {
        painter.drawText(badgeRect.left() + padding, baseline, line);
        baseline += lineHeight;
    }
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    qRegisterMetaType<DecodeReport>("DecodeReport");
    buildUi();
    connect(&decodeWatcher_, &QFutureWatcher<DecodeReport>::finished,
            this, &MainWindow::decodeFinished);
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("QRCodeScanner - 工业读码原型"));
    resize(1180, 760);

    auto* central = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(central);

    auto* toolbarLayout = new QHBoxLayout;
    openButton_ = new QPushButton(QStringLiteral("打开本地图片"), central);
    decodeButton_ = new QPushButton(QStringLiteral("重新解码"), central);
    decodeButton_->setEnabled(false);
    toolbarLayout->addWidget(openButton_);
    toolbarLayout->addWidget(decodeButton_);
    toolbarLayout->addStretch();
    rootLayout->addLayout(toolbarLayout);

    auto* splitter = new QSplitter(Qt::Horizontal, central);

    auto* previewFrame = new QFrame(splitter);
    previewFrame->setFrameShape(QFrame::StyledPanel);
    auto* previewLayout = new QVBoxLayout(previewFrame);
    fileLabel_ = new QLabel(QStringLiteral("尚未选择图片"), previewFrame);
    fileLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    previewLayout->addWidget(fileLabel_);

    auto* scrollArea = new QScrollArea(previewFrame);
    scrollArea->setWidgetResizable(true);
    scrollArea->setAlignment(Qt::AlignCenter);
    imageLabel_ = new QLabel(QStringLiteral("点击“打开本地图片”选择二维码图片"), scrollArea);
    imageLabel_->setAlignment(Qt::AlignCenter);
    imageLabel_->setMinimumSize(320, 320);
    scrollArea->setWidget(imageLabel_);
    previewLayout->addWidget(scrollArea, 1);

    auto* resultFrame = new QFrame(splitter);
    resultFrame->setFrameShape(QFrame::StyledPanel);
    auto* resultLayout = new QVBoxLayout(resultFrame);
    statusLabel_ = new QLabel(QStringLiteral("等待图片"), resultFrame);
    statusLabel_->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 600;"));
    qualityLabel_ = new QLabel(resultFrame);
    qualityLabel_->setWordWrap(true);
    resultEdit_ = new QPlainTextEdit(resultFrame);
    resultEdit_->setReadOnly(true);
    resultEdit_->setPlaceholderText(QStringLiteral("解码内容、执行路径和各阶段耗时将显示在这里。"));
    resultLayout->addWidget(statusLabel_);
    resultLayout->addWidget(qualityLabel_);
    resultLayout->addWidget(resultEdit_, 1);

    splitter->addWidget(previewFrame);
    splitter->addWidget(resultFrame);
    splitter->setSizes({700, 480});
    rootLayout->addWidget(splitter, 1);

    setCentralWidget(central);
    statusBar()->showMessage(QStringLiteral("支持 PNG、JPG、BMP、TIFF；当前支持 QR Code 和 Data Matrix"));

    connect(openButton_, &QPushButton::clicked, this, &MainWindow::openImage);
    connect(decodeButton_, &QPushButton::clicked, this, &MainWindow::decodeCurrentImage);
}

void MainWindow::openImage()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择二维码图片"), QString(),
        QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.webp);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;

    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    if (image.isNull()) {
        QMessageBox::warning(this, QStringLiteral("无法打开图片"),
                             QStringLiteral("读取失败：%1").arg(reader.errorString()));
        return;
    }

    currentImage_ = image;
    currentPath_ = path;
    hasReport_ = false;
    fileLabel_->setText(QFileInfo(path).fileName() + QStringLiteral("  —  ") + path);
    decodeButton_->setEnabled(true);
    updatePreview();
    decodeCurrentImage();
}

void MainWindow::decodeCurrentImage()
{
    if (currentImage_.isNull() || decodeWatcher_.isRunning())
        return;

    setBusy(true);
    resultEdit_->clear();
    const QImage image = currentImage_;
    decodeWatcher_.setFuture(QtConcurrent::run([image] {
        return DecodeEngine().decode(image);
    }));
}

void MainWindow::decodeFinished()
{
    setBusy(false);
    showReport(decodeWatcher_.result());
}

void MainWindow::showReport(const DecodeReport& report)
{
    lastReport_ = report;
    hasReport_ = true;
    updatePreview();
    qualityLabel_->setText(QStringLiteral("图像质量：") + report.quality.summary());

    QString details;
    if (report.success) {
        statusLabel_->setText(QStringLiteral("解码成功"));
        statusLabel_->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 600; color: #18794e;"));
        details += QStringLiteral("状态：%1\n码制：%2\n路径：%3\n置信度：%4\n总耗时：%5 ms\n\n内容：\n%6\n\n")
            .arg(toString(report.status), report.format, report.route)
            .arg(report.confidence, 0, 'f', 3)
            .arg(report.totalMicroseconds / 1000.0, 0, 'f', 3)
            .arg(report.text);
    } else {
        statusLabel_->setText(QStringLiteral("未识别"));
        statusLabel_->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 600; color: #b42318;"));
        details += QStringLiteral("状态：%1\n失败阶段：%2\n总耗时：%3 ms\n原因：%4\n\n")
            .arg(toString(report.status), toString(report.failureStage))
            .arg(report.totalMicroseconds / 1000.0, 0, 'f', 3)
            .arg(report.failureReason);
    }

    if (report.estimatedRows > 0) {
        details += QStringLiteral("估计矩阵：%1×%2\n").arg(report.estimatedRows)
            .arg(report.estimatedColumns);
    }

    const QPolygonF primaryPolygon = reportPrimaryPolygon(report);
    if (primaryPolygon.size() == 4) {
        details += report.corners.size() == 4
            ? QStringLiteral("区域：校验通过的码区\n四角坐标：\n")
            : QStringLiteral("区域：定位候选（尚未校验通过）\n四角坐标：\n");
        for (int index = 0; index < primaryPolygon.size(); ++index) {
            details += QStringLiteral("  P%1  (%2, %3)\n")
                .arg(index + 1)
                .arg(primaryPolygon[index].x(), 0, 'f', 1)
                .arg(primaryPolygon[index].y(), 0, 'f', 1);
        }
    } else {
        details += QStringLiteral("区域：未找到四角候选\n");
    }

    details += QStringLiteral("\n阶段耗时：\n");
    for (const StageTiming& timing : report.stageTimings) {
        details += QStringLiteral("  %1  %2 ms\n").arg(timing.stage)
            .arg(timing.elapsedMicroseconds / 1000.0, 0, 'f', 3);
    }

    details += QStringLiteral("\n解码尝试：\n");
    for (const DecodeAttempt& attempt : report.attempts) {
        details += QStringLiteral("  %1  %2  %3 ms%4\n")
            .arg(attempt.success ? QStringLiteral("✓") : QStringLiteral("×"), attempt.name)
            .arg(attempt.elapsedMicroseconds / 1000.0, 0, 'f', 3)
            .arg(attempt.diagnostic.isEmpty()
                ? QString() : QStringLiteral("  [%1]").arg(attempt.diagnostic));
    }
    resultEdit_->setPlainText(details);
    statusBar()->showMessage(report.success ? QStringLiteral("解码完成") : QStringLiteral("所有路径均未识别"));
}

void MainWindow::setBusy(bool busy)
{
    openButton_->setEnabled(!busy);
    decodeButton_->setEnabled(!busy && !currentImage_.isNull());
    if (busy) {
        statusLabel_->setText(QStringLiteral("正在解码…"));
        statusLabel_->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 600;"));
        statusBar()->showMessage(QStringLiteral("执行快速路径和困难码回退…"));
        QApplication::setOverrideCursor(Qt::WaitCursor);
    } else if (QApplication::overrideCursor()) {
        QApplication::restoreOverrideCursor();
    }
}

void MainWindow::updatePreview()
{
    if (currentImage_.isNull())
        return;
    const QSize target = imageLabel_->size() - QSize(16, 16);
    QPixmap preview = QPixmap::fromImage(currentImage_).scaled(
        target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (hasReport_) {
        QPainter painter(&preview);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QPolygonF primarySource = reportPrimaryPolygon(lastReport_);
        const QPolygonF primary = scaledPolygon(
            primarySource, currentImage_.size(), preview.size());
        const QColor primaryColor = lastReport_.success
            ? QColor(31, 191, 117) : QColor(255, 170, 32);

        for (int index = 0; index < lastReport_.candidateCorners.size(); ++index) {
            const QPolygonF& source = lastReport_.candidateCorners[index];
            if (source.size() != 4 || source == primarySource)
                continue;
            const QPolygonF candidate = scaledPolygon(
                source, currentImage_.size(), preview.size());
            QPen candidatePen(QColor(64, 190, 255, 165), 2, Qt::DashLine);
            painter.setPen(candidatePen);
            painter.setBrush(Qt::NoBrush);
            painter.drawPolygon(candidate);
        }

        drawPolygonWithVertices(
            painter, primary, primaryColor, 3,
            lastReport_.success ? QStringLiteral("已验证码区")
                                : QStringLiteral("候选码区"));
        drawReportBadge(painter, lastReport_, preview.size(), primary.size() == 4);
    }
    imageLabel_->setPixmap(preview);
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    updatePreview();
}
