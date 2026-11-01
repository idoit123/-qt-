QT += core gui widgets network multimedia testlib
CONFIG += console testcase c++11
TEMPLATE = app
TARGET = monitor_test
SOURCES += monitor_test.cpp ../monitor/monitorwindow.cpp ../monitor/videoworker.cpp \
           ../monitor/camerasurface.cpp ../protocol.cpp
HEADERS += ../monitor/monitorwindow.h ../monitor/videoworker.h ../monitor/camerasurface.h ../pduframer.h
