cmake_minimum_required(VERSION 3.21)

project(QRCodeScanner VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Qt5 5.14 REQUIRED COMPONENTS Core Gui Widgets Concurrent)

option(QRSCANNER_ENABLE_OPENCV "Enable OpenCV ROI and Data Matrix grid recovery" ON)
if(QRSCANNER_ENABLE_OPENCV)
    find_package(OpenCV 4.14.0 EXACT QUIET COMPONENTS core imgproc calib3d)
    if(OpenCV_FOUND)
        message(STATUS "OpenCV ${OpenCV_VERSION} enabled from ${OpenCV_DIR}")
    else()
        message(WARNING "OpenCV was not found; building the ZXing-only fallback")
    endif()
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zxing-cpp/CMakeLists.txt")
    message(FATAL_ERROR
        "ZXing-C++ was not found. Run scripts/fetch_dependencies.ps1 first.")
endif()

set(ZXING_READERS ON CACHE BOOL "Build ZXing readers" FORCE)
set(ZXING_WRITERS ON CACHE BOOL "Build ZXing writers for self-tests" FORCE)
set(ZXING_EXAMPLES OFF CACHE BOOL "Disable ZXing examples" FORCE)
set(ZXING_UNIT_TESTS OFF CACHE BOOL "Disable ZXing unit tests" FORCE)
set(ZXING_BLACKBOX_TESTS OFF CACHE BOOL "Disable ZXing black-box tests" FORCE)
set(ZXING_C_API OFF CACHE BOOL "Disable ZXing C API" FORCE)
add_subdirectory(third_party/zxing-cpp EXCLUDE_FROM_ALL)

# Enable Qt code generation only for targets declared below. Enabling it before
# ZXing would make Qt 5's moc parse ZXing's optional Qt 6 compatibility header.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(QRSCANNER_CORE_SOURCES
    src/DataMatrixGridRecoverer.cpp
    src/DataMatrixGridRecoverer.h
    src/DecodeEngine.cpp
    src/DecodeEngine.h
    src/DecodeTypes.cpp
    src/DecodeTypes.h
    src/FrameSource.cpp
    src/FrameSource.h
    src/FrameDecodeController.cpp
    src/FrameDecodeController.h
    src/ImagePreprocessor.cpp
    src/ImagePreprocessor.h
    src/ImageQualityAnalyzer.cpp
    src/ImageQualityAnalyzer.h
    src/RoiDetector.cpp
    src/RoiDetector.h
    src/RoiTracker.cpp
    src/RoiTracker.h
    src/RoutePredictor.cpp
    src/RoutePredictor.h
    src/RouteModelParameters.h
)

add_library(QRCodeScannerCore STATIC ${QRSCANNER_CORE_SOURCES})
target_include_directories(QRCodeScannerCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(QRCodeScannerCore PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zxing-cpp/core/src")
target_link_libraries(QRCodeScannerCore PUBLIC Qt5::Core Qt5::Gui ZXing::Core)
target_compile_definitions(QRCodeScannerCore PRIVATE QT_NO_KEYWORDS)
if(OpenCV_FOUND)
    target_compile_definitions(QRCodeScannerCore PRIVATE QRSCANNER_WITH_OPENCV=1)
    target_include_directories(QRCodeScannerCore PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(QRCodeScannerCore PRIVATE ${OpenCV_LIBS})
endif()

add_executable(QRCodeScanner WIN32
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
)
target_link_libraries(QRCodeScanner PRIVATE
    QRCodeScannerCore
    Qt5::Widgets
    Qt5::Concurrent
)

enable_testing()
add_executable(QRCodeScannerSelfTest tests/SelfTest.cpp)
target_link_libraries(QRCodeScannerSelfTest PRIVATE QRCodeScannerCore ZXing::Core)
add_test(NAME QRCodeScannerSelfTest COMMAND QRCodeScannerSelfTest)

add_executable(QRCodeScannerPipelineTests tests/PipelineTests.cpp)
target_link_libraries(QRCodeScannerPipelineTests PRIVATE QRCodeScannerCore ZXing::Core)
add_test(NAME QRCodeScannerPipelineTests COMMAND QRCodeScannerPipelineTests)

add_executable(QRCodeScannerBenchmark tools/Benchmark.cpp)
target_link_libraries(QRCodeScannerBenchmark PRIVATE QRCodeScannerCore)

if(OpenCV_FOUND)
    add_executable(QRCodeScannerSyntheticSweep tools/SyntheticSweep.cpp)
    target_include_directories(QRCodeScannerSyntheticSweep PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(QRCodeScannerSyntheticSweep PRIVATE QRCodeScannerCore ZXing::Core ${OpenCV_LIBS})
endif()

if(MSVC)
    target_compile_options(QRCodeScannerCore PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(QRCodeScanner PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(QRCodeScannerSelfTest PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(QRCodeScannerPipelineTests PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(QRCodeScannerBenchmark PRIVATE /W4 /permissive- /utf-8)
    if(TARGET QRCodeScannerSyntheticSweep)
        target_compile_options(QRCodeScannerSyntheticSweep PRIVATE /W4 /permissive- /utf-8)
    endif()
endif()
