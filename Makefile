SRC=log.cpp pch.cpp plugin.cpp
INC=framework.h log.h pch.h plugin.h buffer.hpp data_parser.hpp packet_data.hpp received_control.hpp received_tile.hpp

UNAME=$(shell uname)

ifeq ($(UNAME), Darwin)
OUTPUT=libWebRTCConnector.dylib
CC=clang++
CFLAGS=-g -O0 --std=c++14 -arch x86_64 -arch arm64
LDFLAGS=-dynamiclib
endif
ifeq ($(UNAME), Linux)
OUTPUT=libWebRTCConnector.so
CC=g++
CFLAGS=--std=c++14
LDFLAGS=-shared
endif

$(OUTPUT): $(SRC) $(INC)
	$(CC) $(LDFLAGS) $(CFLAGS) $(SRC) -o $(OUTPUT)