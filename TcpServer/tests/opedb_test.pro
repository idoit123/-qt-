QT += core sql testlib
QT -= gui
CONFIG += console testcase c++11
TEMPLATE = app
TARGET = opedb_test

INCLUDEPATH += ..
SOURCES += opedb_test.cpp \
           ../opedb.cpp
HEADERS += ../opedb.h
