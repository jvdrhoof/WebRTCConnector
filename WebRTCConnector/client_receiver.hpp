#pragma once

#include "buffer.hpp"
#include "data_parser.hpp"

#include <fstream>
#include <iostream>
#include <map>
#include <string>

class ClientReceiver {

public:
	ClientReceiver(uint32_t client_number) {
		this->client_number = client_number;
	}

	std::map<std::pair<uint32_t, uint32_t>, ReceivedTile> recv_tiles;
	std::map<uint32_t, ReceivedAudio> recv_audio;
	DataParser data_parser;
	Buffer tile_buffer;
	AudioBuffer audio_buffer;

private:
	uint32_t client_number;
};
