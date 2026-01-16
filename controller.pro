QT += widgets
CONFIG += c++17

SOURCES += raspberry_send_cam.cpp

INCLUDEPATH += /usr/include/opencv4

LIBS += `pkg-config --libs opencv4`