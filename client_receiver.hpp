#pragma once

#include "buffer.hpp"
#include "data_parser.hpp"

#include <fstream>
#include <iostream>
#include <map>
#include <string>

class ClientReceiver {

public:
	ClientReceiver(uint32_t client_number, uint32_t number_of_tiles) {
		this->client_number = client_number;
		tile_buffer.set_number_of_tiles(number_of_tiles);
		data_parser.set_number_of_tiles(number_of_tiles);
	}

	std::map<std::pair<uint32_t, uint32_t>, ReceivedTile> recv_tiles;
	DataParser data_parser;
	Buffer tile_buffer;

private:
	uint32_t client_number;
};
