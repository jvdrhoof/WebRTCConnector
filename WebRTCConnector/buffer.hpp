#pragma once

#include "received_tile.hpp"
#include "tile_queue.hpp"
#include <condition_variable>
#include <list>

class Buffer {

public:
	Buffer() {}
	~Buffer() {
		stop_waiting = true; 
		for (auto& tq : tile_queues) {
			tq.cv->notify_all();
		}
	}

	ReceivedTile next(uint32_t tile_number) {
		std::unique_lock<std::mutex> guard(m);

		// Dynamic size
		if (tile_number >= tile_queues.size()) {
			tile_queues.resize(tile_number + 1);
		}

		ReceivedTile next_tile = tile_queues[tile_number].prio_queue.top();
		tile_queues[tile_number].prio_queue.pop();
		while (!tile_queues[tile_number].prio_queue.empty()) {
			next_tile = tile_queues[tile_number].prio_queue.top();
			tile_queues[tile_number].prio_queue.pop();
		}

		tile_queues[tile_number].frame_number = next_tile.get_frame_number();
		guard.unlock();
		return std::move(next_tile);
	}

	bool insert_tile(ReceivedTile& tile, uint32_t tile_number) {
		std::unique_lock<std::mutex> guard(m);
		uint32_t frame_number = tile.get_frame_number();

		// Dynamic size
		if (tile_number >= tile_queues.size()) {
			tile_queues.resize(tile_number + 1);
		}

		if (frame_number < tile_queues[tile_number].frame_number) {
			guard.unlock();
			return false;
		}


		tile_queues[tile_number].prio_queue.push(std::move(tile));
		// frame_numbers[tile_number] = frame_number;
		guard.unlock();
		tile_queues[tile_number].cv->notify_one();
		return true;
	}

	size_t get_buffer_size(uint32_t tile_number) {
		std::unique_lock<std::mutex> guard(m);
		
		// Dynamic size
		if (tile_number >= tile_queues.size()) {
			tile_queues.resize(tile_number + 1);
		}

		size_t size = tile_queues[tile_number].prio_queue.size();
		guard.unlock();
		return size;
	}

	std::priority_queue<ReceivedTile>& get_queue(uint32_t tile_number) {
		std::unique_lock<std::mutex> guard(m);

		// Dynamic size
		if (tile_number >= tile_queues.size()) {
			tile_queues.resize(tile_number + 1);
		}

		std::priority_queue<ReceivedTile>& q = tile_queues[tile_number].prio_queue;
		guard.unlock();
		return q;
	}

	bool wait_for_tile(uint32_t tile_number) {
		std::unique_lock<std::mutex> guard(m);

		if (tile_number >= tile_queues.size()) {
			tile_queues.resize(tile_number + 1);
		}

		tile_queues[tile_number].cv->wait(guard, [this, tile_number] { return stop_waiting || tile_queues[tile_number].prio_queue.size() > 0; });
		guard.unlock();
		if (stop_waiting) {
			return false;
		}
		
		return true;
	}

private:
	std::vector<TileQueue> tile_queues;
	std::mutex m;

	bool stop_waiting = false;
};
