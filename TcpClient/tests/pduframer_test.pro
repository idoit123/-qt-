QT += core testlib
QT -= gui
CONFIG += console testcase c++11
TEMPLATE = app
TARGET = pduframer_test

INCLUDEPATH += ..
SOURCES += pduframer_test.cpp \
           ../protocol.cpp
HEADERS += ../pduframer.h \
           ../protocol.h
