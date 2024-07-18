#pragma once

#include "received_tile.hpp"
#include <condition_variable>
#include <list>

class TileQueue {

public:
	TileQueue() : cv(std::make_unique<std::condition_variable>()), frame_number(0) { }

	std::priority_queue<ReceivedTile> prio_queue;
	std::unique_ptr<std::condition_variable> cv;
	uint32_t frame_number;
};
