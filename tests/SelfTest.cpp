#include "DecodeEngine.h"

#include <QCoreApplication>
#include <QImage>
#include <QImageReader>
#include <QTemporaryDir>

#include <ZXingCpp.h>

#include <algorithm>
#include <array>
#include <iostream>

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);

    const QStringList arguments = application.arguments();
    if (arguments.size() > 1) {
        const QString imagePath = arguments.at(1);
        QImageReader reader(imagePath);
        reader.setAutoTransform(true);
        const QImage localImage = reader.read();
        if (localImage.isNull()) {
            std::cerr << "File decode failed: could not read the image\n";
            return 5;
        }

        DecodeRequest request;
        request.image = localImage;
        request.recipe.allowMultipleSymbols = true;
        request.recipe.maximumSymbols = arguments.size() > 2
            ? std::clamp(arguments.at(2).toInt(), 1, 64)
            : 16;
        const DecodeReport report = DecodeEngine().decode(request);
        if (!report.success) {
            std::cerr << "File decode failed after "
                      << report.totalMicroseconds / 1000.0 << " ms\n";
            const QImage gray = localImage.convertToFormat(QImage::Format_Grayscale8);
            const ZXing::ImageView view(gray.constBits(), gray.width(), gray.height(),
                                        ZXing::ImageFormat::Lum, gray.bytesPerLine());
            const auto diagnostics = ZXing::ReadBarcodes(
                view,
                ZXing::ReaderOptions()
                    .formats(ZXing::BarcodeFormat::QRCode | ZXing::BarcodeFormat::DataMatrix)
                    .tryHarder(true)
                    .tryRotate(true)
                    .tryInvert(true)
                    .returnErrors(true));
            for (const auto& barcode : diagnostics) {
                std::cerr << "Detector candidate: "
                          << ZXing::ToString(barcode.format()) << " / "
                          << ZXing::ToString(barcode.error()) << " / "
                          << ZXing::ToString(barcode.position()) << "\n";
            }

            const auto pureBarcodes = ZXing::ReadBarcodes(
                view,
                ZXing::ReaderOptions()
                    .formats(ZXing::BarcodeFormat::DataMatrix)
                    .isPure(true)
                    .returnErrors(true));
            for (const auto& barcode : pureBarcodes) {
                std::cerr << "Pure candidate: "
                          << ZXing::ToString(barcode.format()) << " / "
                          << ZXing::ToString(barcode.error()) << "\n";
                if (barcode.isValid()) {
                    std::cout << barcode.text() << "\n";
                    return 0;
                }
            }
            return 6;
        }

        std::cout << "Decoded " << report.symbols.size() << " symbol(s)"
                  << " via " << report.route.toStdString()
                  << " in " << report.totalMicroseconds / 1000.0 << " ms\n";
        if (!report.symbols.isEmpty()) {
            for (int index = 0; index < report.symbols.size(); ++index) {
                const DecodedSymbol& symbol = report.symbols[index];
                std::cout << index + 1 << ": " << symbol.format.toStdString()
                          << " | " << symbol.text.toStdString() << "\n";
            }
        } else {
            std::cout << report.text.toStdString() << "\n";
        }
        return 0;
    }

    QTemporaryDir temporaryDirectory;
    if (!temporaryDirectory.isValid()) {
        std::cerr << "Self-test failed: could not create a temporary directory\n";
        return 3;
    }

    const std::array formats{
        ZXing::BarcodeFormat::QRCode,
        ZXing::BarcodeFormat::DataMatrix
    };
    for (const auto format : formats) {
        const QString expected = format == ZXing::BarcodeFormat::QRCode
            ? QStringLiteral("QRCodeScanner self-test 2026")
            : QStringLiteral("DM-INDUSTRIAL-48X48-2026");
        auto barcode = ZXing::CreateBarcodeFromText(expected.toStdString(), format);
        auto generated = ZXing::WriteBarcodeToImage(barcode,
            ZXing::WriterOptions().scale(8).addQuietZones(true));
        QImage image(generated.data(), generated.width(), generated.height(),
                     generated.width(), QImage::Format_Grayscale8);
        image = image.copy();

        const QString imagePath = temporaryDirectory.filePath(
            format == ZXing::BarcodeFormat::QRCode
                ? QStringLiteral("generated-qr.png")
                : QStringLiteral("generated-dm.png"));
        if (!image.save(imagePath, "PNG")) {
            std::cerr << "Self-test failed: could not write a local PNG file\n";
            return 4;
        }
        const QImage loadedImage(imagePath);
        DecodeRequest request;
        request.image = loadedImage;
        request.recipe.allowQrCode = format == ZXing::BarcodeFormat::QRCode;
        request.recipe.allowDataMatrix = format == ZXing::BarcodeFormat::DataMatrix;
        request.recipe.payloadRegularExpression = QStringLiteral("^[A-Za-z0-9 -]+$");
        const DecodeReport report = DecodeEngine().decode(request);
        if (!report.success || report.text != expected) {
            std::cerr << "Self-test failed for " << ZXing::ToString(format) << "\n";
            return 1;
        }
        std::cout << ZXing::ToString(format) << " passed via "
                  << report.route.toStdString() << " in "
                  << report.totalMicroseconds / 1000.0 << " ms\n";
    }

    DecodeRequest invalidRequest;
    const DecodeReport invalidReport = DecodeEngine().decode(invalidRequest);
    if (invalidReport.status != DecodeStatus::InvalidInput) {
        std::cerr << "Self-test failed: invalid input status mismatch\n";
        return 2;
    }
    return 0;
}
