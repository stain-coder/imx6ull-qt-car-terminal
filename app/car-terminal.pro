QT += core gui widgets
CONFIG += c++14
TEMPLATE = app
TARGET = car-terminal
SOURCES += src/main.cpp src/window.cpp src/sensors.cpp
HEADERS += src/window.h src/sensors.h

SOURCES += src/audio.cpp
HEADERS += src/audio.h
LIBS += -lasound

SOURCES += src/weather.cpp
HEADERS += src/weather.h
