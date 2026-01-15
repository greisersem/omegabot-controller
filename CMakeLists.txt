cmake_minimum_required(VERSION 3.16)
project(server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)       
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt5 COMPONENTS Widgets REQUIRED)
find_package(OpenCV REQUIRED)

add_executable(server raspberry_send_cam.cpp)

target_link_libraries(server
    Qt5::Widgets
    ${OpenCV_LIBS}
)