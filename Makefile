# Makefile for jt9_decode

CXX = g++
TARGET = jt9_decode
SOURCE = jt9_decode.cpp
INCLUDES = -I./wsjtx
CXXFLAGS = -std=c++11 -fPIC $(shell pkg-config --cflags Qt5Core)
LDFLAGS = $(shell pkg-config --libs Qt5Core) -lrt

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CXX) -o $(TARGET) $(SOURCE) $(INCLUDES) $(CXXFLAGS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

uninstall:
	rm -f /usr/local/bin/$(TARGET)
