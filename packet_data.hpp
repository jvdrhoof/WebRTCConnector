#pragma once

#include <stdio.h>
#include <iostream>

struct PacketType {
	uint32_t type;

	static constexpr auto size() {
		return sizeof(struct PacketType);
	}

	PacketType(char** buf, size_t& avail) {
		std::memcpy(data(), *buf, size());
		*buf += size();
		avail -= size();
	}

	char* data() {
		return reinterpret_cast<char*>(this);
	}

	std::string string_representation() {
		return "Packet type: " + std::to_string(type);
	}
};

struct PacketHeader {
	uint32_t client_number;
	uint32_t frame_number;
	uint32_t tile_number;
	uint32_t file_length;
	uint32_t file_offset;
	uint32_t packet_length;

	static constexpr auto size() {
		return sizeof(struct PacketHeader);
	}

	PacketHeader(uint32_t client_number, uint32_t frame_number, uint32_t tile_number,
		uint32_t file_length, uint32_t file_offset, uint32_t packet_length) {
		this->client_number = client_number;
		this->frame_number = frame_number;
		this->tile_number = tile_number;
		this->file_length = file_length;
		this->file_offset = file_offset;
		this->packet_length = packet_length;
	}

	PacketHeader(char** buf, size_t& avail) {
		std::memcpy(data(), *buf, size());
		*buf += size();
		avail -= size();
	}

	char* data() {
		return reinterpret_cast<char*>(this);
	}

	std::string string_representation() {
		return "Client number: " +
			std::to_string(client_number) +
			", frame number : " +
			std::to_string(frame_number) +
			", tile number: " +
			std::to_string(tile_number) +
			", file length: " +
			std::to_string(file_length) +
			", file offset: " +
			std::to_string(file_offset) +
			", packet length: " +
			std::to_string(packet_length);
	}
};
