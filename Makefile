CC=clang++
CFLAGS=--std=c++14 -arch x86_64 -arch arm64
LDFLAGS=-dynamiclib
SRC=log.cpp pch.cpp plugin.cpp
INC=framework.h log.h pch.h plugin.h buffer.hpp data_parser.hpp packet_data.hpp received_control.hpp received_tile.hpp
OUTPUT=libWebRTCConnector.dylib

$(OUTPUT): $(SRC) $(INC)
	$(CC) $(LDFLAGS) $(CFLAGS) $(SRC) -o $(OUTPUT)