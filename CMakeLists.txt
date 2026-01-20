cmake_minimum_required(VERSION 3.16)
project(omegabot_controller)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt5 COMPONENTS Widgets Core Gui REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(OPENCV4 REQUIRED opencv4)

message(STATUS "OpenCV version: ${OPENCV4_VERSION}")
message(STATUS "OpenCV include dirs: ${OPENCV4_INCLUDE_DIRS}")
message(STATUS "OpenCV libs: ${OPENCV4_LIBRARIES}")

include_directories(${OPENCV4_INCLUDE_DIRS})
link_directories(${OPENCV4_LIBRARY_DIRS})

add_executable(operator operator.cpp)

target_link_libraries(operator
    ${OPENCV4_LIBRARIES}
    Qt5::Widgets
    Qt5::Core
    Qt5::Gui
)
