QT += core gui widgets network testlib
CONFIG += console testcase c++11
TEMPLATE = app
TARGET = filetransfer_test
INCLUDEPATH += ..
SOURCES += filetransfer_test.cpp ../book.cpp ../protocol.cpp
HEADERS += ../book.h ../protocol.h ../pduframer.h
