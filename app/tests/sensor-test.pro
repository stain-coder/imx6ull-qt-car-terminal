QT = core
CONFIG += console c++14
CONFIG -= app_bundle
TEMPLATE = app
TARGET = sensor-test
SOURCES += sensor-test.cpp ../src/sensors.cpp
HEADERS += ../src/sensors.h
INCLUDEPATH += ../src
QMAKE_LFLAGS += -Wl,--wrap=open,--wrap=read,--wrap=fstat,--wrap=close
