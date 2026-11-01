QT += core gui multimedia testlib
CONFIG += console testcase c++11
TEMPLATE = app
TARGET = camerasurface_test
SOURCES += camerasurface_test.cpp ../monitor/camerasurface.cpp
HEADERS += ../monitor/camerasurface.h
