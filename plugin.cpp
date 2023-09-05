#include "pch.h"
#include "buffer.hpp"
#include "data_parser.hpp"
#include "framework.h"
#include "log.h"
#include "packet_data.hpp"
#include "plugin.h"
#include "received_control.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>


// Maximum length of buffer
#define BUFLEN 1300

using namespace std;

bool one_socket = true;
#ifdef WIN32
static WSADATA wsa;
#endif
static struct sockaddr_in si_send;
static SOCKET s_send;
int slen_send = sizeof(si_send);
static struct sockaddr_in si_recv;
static SOCKET s_recv;
int slen_recv = sizeof(si_recv);

static std::thread worker;
static bool keep_working = true;
static bool initialized = false;
static bool cleaned = false;

std::mutex m_recv_data;
std::mutex m_send_data;
std::mutex m_recv_control;

static char* buf = (char*)malloc(BUFLEN);
static char* buf_ori = buf;
static std::map<std::pair<uint32_t, uint32_t>, ReceivedTile> recv_tiles;
DataParser data_parser;
Buffer tile_buffer;
std::vector<uint32_t> frame_numbers;
static std::queue<ReceivedControl> recv_controls;

static string log_file = "";
static bool debug_mode = false;
std::mutex m_logging;


enum CONNECTION_SETUP_CODE : int {
	ConnectionSuccess = 0,
	StartUpError = 1,
	SocketCreationError = 2,
	SendToError = 3,
	AlreadyInitialized = 4
};

inline string get_current_date_time(bool date_only) {
	time_t now = time(0);
	char buf[80];
	struct tm tstruct;
    // xxxjack I think the parameters were in the wrong order...
	localtime_r( &now, &tstruct);
	if (date_only) {
		strftime(buf, sizeof(buf), "%Y-%m-%d", &tstruct);
	} else {
		strftime(buf, sizeof(buf), "%Y-%m-%d %X", &tstruct);
	}
	return string(buf);
};

void custom_log(string message, bool debug = true, Color color = Color::Black) {
	std::unique_lock<std::mutex> guard(m_logging);
	if (!debug) {
		Log::log(message, color);
	}
	if (log_file != "" && (!debug || debug_mode)) {
		ofstream ofs(log_file.c_str(), std::ios_base::out | std::ios_base::app);
		ofs << get_current_date_time(false) << '\t' << message << '\n';
		ofs.close();
	}
	guard.unlock();
}

void set_logging(char* log_directory, bool debug) {
	Log::log("Setting log directory to " + string(log_directory), Color::Orange);
	log_file = string(log_directory) + "\\" + get_current_date_time(true) + ".txt";
	string debugging = debug ? "true" : "false";
	Log::log("Setting debug mode to " + debugging, Color::Orange);
	debug_mode = debug;
}

int connect_to_proxy(char* ip_send, uint32_t port_send, char* ip_recv, uint32_t port_recv, uint32_t number_of_tiles) {
	custom_log("Attempting to connect to sender " + string(ip_send) + ":" + to_string(port_send) +
		" and receiver " + string(ip_recv) + ":" + to_string(port_recv), false, Color::Orange);
	if (initialized) {
		custom_log("Proxy plugin already initialized, no changes are possible", false, Color::Orange);
		return AlreadyInitialized;
	}

	custom_log("Number of tiles: " + to_string(number_of_tiles));
	frame_numbers = std::vector<uint32_t>(number_of_tiles, 0);
	tile_buffer.set_number_of_tiles(number_of_tiles);
	data_parser.set_number_of_tiles(number_of_tiles);

#ifdef WIN32
	custom_log("Setting up socket to " + string(ip_send), false, Color::Orange);
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		return StartUpError;
	}
#endif
	// Generic parameters
	ULONG buf_size = 524288000;
	char t[BUFLEN] = { 0 };
	t[0] = (char)number_of_tiles;
	// Create send socket
	if ((s_send = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == SOCKET_ERROR) {
		custom_log("ERROR: failed to create socket", false, Color::Red);
		WSACleanup();
		return SocketCreationError;
	}
	setsockopt(s_send, SOL_SOCKET, SO_RCVBUF, (char*)&buf_size, sizeof(ULONG));
	si_send.sin_family = AF_INET;
	si_send.sin_port = htons(port_send);
#ifdef WIN32
    inet_pton(AF_INET, ip_send, &si_send.sin_addr.S_un.S_addr);
#else
    inet_pton(AF_INET, ip_send, &si_send.sin_addr.s_addr);
#endif
	if (sendto(s_send, t, BUFLEN, 0, (struct sockaddr*)&si_send, slen_send) == SOCKET_ERROR) {
		custom_log("ERROR: failed to send to socket on port " + to_string(port_send), false, Color::Red);
		WSACleanup();
		return SendToError;
	}
	if (string(ip_send) != string(ip_recv) || port_send != port_recv) {
		one_socket = false;
		// Sleep for a while, so that conflicts in the WebRTC signaling can be avoided
		std::this_thread::sleep_for(std::chrono::milliseconds(2000));
		// Create receive socket
		if ((s_recv = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == SOCKET_ERROR) {
			custom_log("ERROR: failed to create socket", false, Color::Red);
			WSACleanup();
			return SocketCreationError;
		}
		setsockopt(s_recv, SOL_SOCKET, SO_RCVBUF, (char*)&buf_size, sizeof(ULONG));
		si_recv.sin_family = AF_INET;
		si_recv.sin_port = htons(port_recv);
#ifdef WIN32
        inet_pton(AF_INET, ip_recv, &si_recv.sin_addr.S_un.S_addr);
#else
        inet_pton(AF_INET, ip_recv, &si_recv.sin_addr.s_addr);
#endif
        if (sendto(s_recv, t, BUFLEN, 0, (struct sockaddr*)&si_recv, slen_recv) == SOCKET_ERROR) {
			custom_log("ERROR: failed to send to socket on port " + to_string(port_recv), false, Color::Red);
			WSACleanup();
			return SendToError;
		}
	}
	initialized = true;
	custom_log("Successfully set up sockets", false, Color::Orange);
	return ConnectionSuccess;
}

void listen_for_data() {
	custom_log("Starting to listen for incoming data", false, Color::Yellow);
	while (keep_working) {
		size_t size = 0;
		custom_log("Attempting to receive data");

		// Strictly speaking, the use of a mutex should be unneccesary. However, when the start_listening function is
		// called multiple times, this might cause two processes reading from the socket, resulting in compromised data.
		std::unique_lock<std::mutex> guard(m_recv_data);

		if (one_socket) {
			custom_log("About to receive from s_send", false, Color::Yellow);
			if ((size = recvfrom(s_send, buf, BUFLEN, 0, NULL, NULL)) == SOCKET_ERROR) {
				custom_log("ERROR: recvfrom() failed with error code " + std::to_string(WSAGetLastError()), true, Color::Red);
				guard.unlock();
				return;
			}
			custom_log("Received packet from s_send wit size " + to_string(size), false, Color::Yellow);
		} else {
			custom_log("About to receive from s_recv", false, Color::Yellow);
			if ((size = recvfrom(s_recv, buf, BUFLEN, 0, NULL, NULL)) == SOCKET_ERROR) {
				custom_log("ERROR: recvfrom() failed with error code " + std::to_string(WSAGetLastError()), true, Color::Red);
				guard.unlock();
				return;
			}
			custom_log("Received packet from s_recv wit size " + to_string(size), false, Color::Yellow);
		}

		custom_log("Received packet", false, Color::Yellow);

		struct PacketType p_type(&buf, size);
		if (p_type.type == 1) {
			struct PacketHeader p_header(&buf, size);
			auto tile = recv_tiles.find(std::make_pair(p_header.frame_number, p_header.tile_number));
			if (tile == recv_tiles.end()) {
				auto e = recv_tiles.emplace(std::make_pair(p_header.frame_number, p_header.tile_number),
					ReceivedTile(p_header.file_length, p_header.frame_number, p_header.tile_number));
				tile = e.first;
				custom_log("Receiving new tile: " + p_header.string_representation(), false, Color::Yellow);
			}
			bool inserted = tile->second.insert(buf, p_header.file_offset, p_header.packet_length, size);
			if (tile->second.is_complete()) {
				custom_log("Completed: " + p_header.string_representation(), false, Color::Yellow);
				tile_buffer.insert_tile(tile->second, p_header.tile_number);
				recv_tiles.erase(std::make_pair(p_header.frame_number, p_header.tile_number));
			}
		} else if (p_type.type == 2) {
			recv_controls.push(ReceivedControl(buf, (uint32_t)size));
		} else {
			custom_log("ERROR: unknown packet type " + to_string(p_type.type), true, Color::Red);
			guard.unlock();
			exit(EXIT_FAILURE);
		}
		buf = buf_ori;
		guard.unlock();
	}
	custom_log("Stopped listening for incoming data", false, Color::Yellow);
}

void start_listening() {
	custom_log("Starting new listening thread", false, Color::Yellow);
	worker = std::thread(listen_for_data);
}

void clean_up() {
	custom_log("Attempting to clean up", false, Color::Orange);
	closesocket(s_recv);
	closesocket(s_send);
	if (!cleaned) {
		cleaned = true;
		keep_working = false;
		if (worker.joinable())
			worker.join();
		// TODO: check if socket should be closed
		// WSACleanup();
		free(buf);
		custom_log("Cleaned up", false, Color::Orange);
	} else {
		custom_log("Already cleaned up", false, Color::Orange);
	}
}

int send_packet(char* data, uint32_t size, uint32_t _packet_type) {
	uint32_t packet_type = _packet_type;
	int size_send = 0;
	char buf_msg[BUFLEN] = { 0 };
	memcpy(buf_msg, &packet_type, size);
	memcpy(&buf_msg[sizeof(packet_type)], data, size);
	if ((size_send = sendto(s_send, buf_msg, BUFLEN, 0, (struct sockaddr*)&si_send, slen_send)) == SOCKET_ERROR) {
		return -1;
	}
	return size_send;
}

int send_tile(void* data, uint32_t size, uint32_t tile_number) {
	custom_log("CALL: send_tile");
	uint32_t buflen_nheader = BUFLEN - sizeof(PacketType) - sizeof(PacketHeader);
	buflen_nheader = 1148;
	uint32_t current_offset = 0;
	uint32_t remaining = size;
	int full_size_send = 0;
	char* temp_d = reinterpret_cast<char*>(data);
	std::unique_lock<std::mutex> guard(m_send_data);
	while (remaining > 0) {
		uint32_t next_size = 0;
		if (remaining >= buflen_nheader) {
			next_size = buflen_nheader;
		} else {
			next_size = remaining;
		}
		char buf_msg[BUFLEN];
		struct PacketHeader p_header {
			frame_numbers[tile_number], tile_number, size, current_offset, next_size
		};
		memcpy(buf_msg, &p_header, sizeof(p_header));
		memcpy(buf_msg + sizeof(p_header), reinterpret_cast<char*>(data) + current_offset, next_size);
		int size_send = send_packet(buf_msg, next_size + sizeof(PacketHeader), 1);
		if (size_send == SOCKET_ERROR) {
			guard.unlock();
			custom_log("ERROR: the return value of send_packet should not be negative!", false, Color::Red);
			return size_send;
		}
		full_size_send += size_send;
		current_offset += next_size;
		remaining -= next_size;
	}
	custom_log("Sent out frame " + to_string(frame_numbers[tile_number]) + ", tile " + to_string(tile_number), false, Color::Green);
	frame_numbers[tile_number] += 1;
	guard.unlock();
	custom_log("RETURN: send_tile, " + to_string(full_size_send));
	return full_size_send;
}

int get_tile_size(uint32_t tile_number) {
	custom_log("CALL: next_tile, " + to_string(tile_number));
	while (tile_buffer.get_buffer_size(tile_number) == 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ReceivedTile t = tile_buffer.next(tile_number);
	data_parser.set_current_tile(t, tile_number);
	int tile_size = data_parser.get_current_tile_size(tile_number);
	custom_log("RETURN: next_tile, " + to_string(tile_size));
	return tile_size;
}

void retrieve_tile(void* d, uint32_t size, uint32_t tile_number) {
	custom_log("CALL: retrieve_tile");
	int local_size = data_parser.fill_data_array(d, size, tile_number);
	if (local_size > 0) {
		custom_log("ERROR: retrieve_tile parameter size " + to_string(size) + " does not match registered data length" + to_string(local_size), false, Color::Red);
	}
	custom_log("RETURN: retrieve_tile");
}

int send_control(void* data, uint32_t size) {
	custom_log("CALL: send_control");
	if (size > BUFLEN - sizeof(PacketType)) {
		custom_log("ERROR: send_control returns -1 since the message size is too large", false, Color::Red);
		return -1;
	}
	char* temp_d = reinterpret_cast<char*>(data);
	int size_send = send_packet(temp_d, size, 2);
	custom_log("RETURN: send_control, " + to_string(size_send));
	return size_send;
}

int get_control_size() {
	custom_log("CALL: next_control");
	std::unique_lock<std::mutex> guard(m_recv_control);
	if (recv_controls.empty()) {
		guard.unlock();
		custom_log("RETURN: next_frame, -1");
		return -1;
	}
	int size = (int)recv_controls.front().get_data_length();
	guard.unlock();
	custom_log("RETURN: next_frame, " + to_string(size));
	return size;
}

void retrieve_control(void* d, uint32_t size) {
	custom_log("CALL: retrieve_control");
	std::unique_lock<std::mutex> guard(m_recv_control);
	auto next_control_packet = std::move(recv_controls.front());
	recv_controls.pop();
	guard.unlock();
	uint32_t local_size = (uint32_t)next_control_packet.get_data_length();
	if (size != local_size) {
		custom_log("ERROR: retrieve_control parameter size " + to_string(size) + " does not match data length " + to_string(local_size), false, Color::Red);
		return;
	}
	char* p = next_control_packet.get_data();
	char* temp_d = reinterpret_cast<char*>(d);
	memcpy(temp_d, p, size);
	custom_log("RETURN: retrieve_control");
}
