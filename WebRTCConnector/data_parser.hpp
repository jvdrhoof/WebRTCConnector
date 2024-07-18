#pragma once

#include "received_tile.hpp"
#include "received_audio.hpp"
#include <cstdint>
#include <cstring>

// TODO: make audio parser
class DataParser {

public:
	DataParser() {}

	int fill_data_array(void* d, uint32_t size, uint32_t tile_number) {

		// Dynamic size
		if (tile_number >= current_tiles.size()) {
			current_tiles.resize(tile_number + 1);
		}

		uint32_t local_size = current_tiles[tile_number].get_tile_length();
		if (local_size != size) {
			return local_size;
		}
		char* p = current_tiles[tile_number].get_data();
		char* temp_d = reinterpret_cast<char*>(d);
		memcpy(temp_d, p, local_size);
		return size;
	}

	int fill_data_array(void* d, uint32_t size) {
		uint32_t local_size = current_audio.get_frame_length();
		if (local_size != size) {
			return local_size;
		}
		char* p = current_audio.get_data();
		char* temp_d = reinterpret_cast<char*>(d);
		memcpy(temp_d, p, local_size);
		return size;
	}

	void set_current_tile(ReceivedTile& r, uint32_t tile_number) {

		// Dynamic size
		if (tile_number >= current_tiles.size()) {
			current_tiles.resize(tile_number + 1);
		}

		current_tiles[tile_number] = std::move(r);
	}

	void set_current_audio(ReceivedAudio& r) {
		current_audio = std::move(r);
	}

	uint32_t get_current_tile_size(uint32_t tile_number) {

		// Dynamic size
		if (tile_number >= current_tiles.size()) {
			current_tiles.resize(tile_number + 1);
		}

		return current_tiles[tile_number].get_tile_length();
	}

	uint32_t get_current_audio_size() {
		return current_audio.get_frame_length();
	}

private:
	std::vector<ReceivedTile> current_tiles;
	ReceivedAudio current_audio;
};
