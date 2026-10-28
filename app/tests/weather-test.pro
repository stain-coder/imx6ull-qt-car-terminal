QT += core gui widgets
CONFIG += console c++14
CONFIG -= app_bundle
TEMPLATE = app
TARGET = weather-test
SOURCES += weather-test.cpp ../src/weather.cpp
HEADERS += ../src/weather.h
