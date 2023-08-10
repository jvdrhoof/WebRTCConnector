#include "received_tile.hpp"

#include <cstdint>
#include <cstring>

class DataParser {

public:
	DataParser() {}

	void set_number_of_tiles(uint32_t number_of_tiles) {
		current_tiles = std::vector<ReceivedTile>(number_of_tiles);
	}

	int fill_data_array(void* d, uint32_t size, uint32_t tile_number) {
		uint32_t local_size = current_tiles[tile_number].get_tile_length();
		if (size != local_size) {
			return local_size;
		}
		char* p = current_tiles[tile_number].get_data();
		char* temp_d = reinterpret_cast<char*>(d);
		memcpy(temp_d, p, local_size);
		return 0;
	}

	void set_current_tile(ReceivedTile& r, uint32_t tile_number) {
		current_tiles[tile_number] = std::move(r);
	}

	uint32_t get_current_tile_size(uint32_t tile_number) {
		return current_tiles[tile_number].get_tile_length();
	}

private:
	std::vector<ReceivedTile> current_tiles;
};
