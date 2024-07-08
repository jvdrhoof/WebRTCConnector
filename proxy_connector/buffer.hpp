#pragma once

#include "received_tile.hpp"
#include <list>

class Buffer {

public:
	Buffer() {}

	ReceivedTile next(uint32_t tile_number) {
		std::unique_lock<std::mutex> guard(m);

		// Dynamic size
		if (tile_number >= tile_queues.size()) {
			tile_queues.resize(tile_number + 1);
		}

		ReceivedTile next_tile = tile_queues[tile_number].top();
		tile_queues[tile_number].pop();
		while (!tile_queues[tile_number].empty()) {
			next_tile = tile_queues[tile_number].top();
			tile_queues[tile_number].pop();
		}

		// Dynamic size
		if (tile_number >= frame_numbers.size()) {
			frame_numbers.resize(tile_number + 1);
		}

		frame_numbers[tile_number] = next_tile.get_frame_number();
		guard.unlock();
		return std::move(next_tile);
	}

	bool insert_tile(ReceivedTile& tile, uint32_t tile_number) {
		std::unique_lock<std::mutex> guard(m);
		uint32_t frame_number = tile.get_frame_number();

		// Dynamic size
		if (tile_number >= frame_numbers.size()) {
			frame_numbers.resize(tile_number + 1);
		}

		if (frame_number < frame_numbers[tile_number]) {
			guard.unlock();
			return false;
		}

		// Dynamic size
		if (tile_number >= tile_queues.size()) {
			tile_queues.resize(tile_number + 1);
		}

		tile_queues[tile_number].push(std::move(tile));
		// frame_numbers[tile_number] = frame_number;
		guard.unlock();
		return true;
	}

	size_t get_buffer_size(uint32_t tile_number) {
		std::unique_lock<std::mutex> guard(m);
		
		// Dynamic size
		if (tile_number >= tile_queues.size()) {
			tile_queues.resize(tile_number + 1);
		}

		size_t size = tile_queues[tile_number].size();
		guard.unlock();
		return size;
	}

	std::priority_queue<ReceivedTile> get_queue(uint32_t tile_number) {
		std::unique_lock<std::mutex> guard(m);

		// Dynamic size
		if (tile_number >= tile_queues.size()) {
			tile_queues.resize(tile_number + 1);
		}

		std::priority_queue<ReceivedTile> q = tile_queues[tile_number];
		guard.unlock();
		return q;
	}

private:
	std::vector<std::priority_queue<ReceivedTile>> tile_queues;
	std::vector<uint32_t> frame_numbers;
	std::mutex m;
};
