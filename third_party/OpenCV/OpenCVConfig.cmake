# Minimal package configuration for the locally supplied OpenCV 4.6.0 SDK.
# The SDK contains the combined opencv_world import library instead of the
# upstream CMake package files.
get_filename_component(_opencv_root "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)

set(OpenCV_FOUND TRUE)
set(OpenCV_VERSION "4.6.0")
set(OpenCV_VERSION_MAJOR 4)
set(OpenCV_VERSION_MINOR 6)
set(OpenCV_VERSION_PATCH 0)
set(OpenCV_INCLUDE_DIRS "${_opencv_root}/include")
set(OpenCV_LIBS
    optimized "${_opencv_root}/lib/release/opencv_world460.lib"
    debug "${_opencv_root}/lib/debug/opencv_world460d.lib")

foreach(_component IN LISTS OpenCV_FIND_COMPONENTS)
    set(OpenCV_${_component}_FOUND TRUE)
endforeach()

