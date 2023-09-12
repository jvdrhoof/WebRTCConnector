#include "pch.h"
#include "buffer.hpp"
#include "client_receiver.hpp"
#include "data_parser.hpp"
#include "framework.h"
#include "log.h"
#include "packet_data.hpp"
#include "plugin.h"
#include "received_control.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
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

uint32_t client_number;
uint32_t number_of_tiles;

static thread worker;
static bool keep_working = true;
static bool initialized = false;

mutex m_receivers;
mutex m_recv_data;
mutex m_send_data;
mutex m_recv_control;

static char* buf;
static char* buf_ori;

map<uint32_t, ClientReceiver*> client_receivers;
vector<uint32_t> frame_numbers;
queue<ReceivedControl> recv_controls;

static string log_file = "";
static bool debug_mode = false;
mutex m_logging;


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
#if defined(_WIN64) || defined(_WIN32)
	localtime_s(&tstruct, &now);
#else
	localtime_r( &now, &tstruct);
#endif
	if (date_only) {
		strftime(buf, sizeof(buf), "%Y-%m-%d", &tstruct);
	} else {
		strftime(buf, sizeof(buf), "%Y-%m-%d %X", &tstruct);
	}
	return string(buf);
};

void custom_log(string message, bool debug = true, Color color = Color::Black) {
	unique_lock<mutex> guard(m_logging);
	if (!debug) {
		Log::log(message, color);
	}
	if (log_file != "" && (!debug || debug_mode)) {
		ofstream ofs(log_file.c_str(), ios_base::out | ios_base::app);
		ofs << get_current_date_time(false) << '\t' << message << '\n';
		ofs.close();
	}
	guard.unlock();
}

void set_logging(char* log_directory, bool debug) {
	Log::log("set_logging: Setting log directory to " + string(log_directory), Color::Orange);
	log_file = string(log_directory) + "\\" + get_current_date_time(true) + ".txt";
	string debugging = debug ? "true" : "false";
	Log::log("set_logging: Setting debug mode to " + debugging, Color::Orange);
	debug_mode = debug;
}

ClientReceiver* find_or_add_receiver(uint32_t client_number) {
	ClientReceiver* c;
	unique_lock<mutex> guard(m_receivers);
	auto it = client_receivers.find(client_number);
	if (it == client_receivers.end()) {
		custom_log("find_or_add_receiver: Client number " + to_string(client_number) + " not yet registered, inserting now", false, Color::Yellow);
		c = new ClientReceiver(client_number, number_of_tiles);
		client_receivers.insert({ client_number, c });
		custom_log("find_or_add_receiver: Client number " + to_string(client_number) + " registered. Make sure you are receiving the correct client ID!", false, Color::Yellow);
	} else {
		c = it->second;
	}
	guard.unlock();
	return c;
}

int connect_to_proxy(char* ip_send, uint32_t port_send, char* ip_recv, uint32_t port_recv, uint32_t n_tiles, uint32_t client_id) {
	custom_log("connect_to_proxy: Attempting to connect to sender " + string(ip_send) + ":" + to_string(port_send) +
		" and receiver " + string(ip_recv) + ":" + to_string(port_recv), false, Color::Orange);
	if (initialized) {
		custom_log("connect_to_proxy: DLL already initialized, no changes are possible", false, Color::Orange);
		return AlreadyInitialized;
	}

	client_number = client_id;
	number_of_tiles = n_tiles;
	custom_log("connect_to_proxy: Number of tiles: " + to_string(number_of_tiles), false, Color::Orange);
	frame_numbers = vector<uint32_t>(number_of_tiles, 0);
	buf = (char*)malloc(BUFLEN);
	buf_ori = buf;

#ifdef WIN32
	custom_log("connect_to_proxy: Setting up socket to " + string(ip_send), false, Color::Orange);
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
		custom_log("connect_to_proxy: ERROR: failed to create socket", false, Color::Red);
		WSACleanup();
		return SocketCreationError;
	}

	sockaddr_in our_address;
	our_address.sin_family = AF_INET;
	our_address.sin_port = htons(port_send + 1);
	our_address.sin_addr.s_addr = htonl(INADDR_ANY);
	if (::bind(s_send, (struct sockaddr*)&our_address, sizeof(our_address)) < 0) {
		custom_log("connect_to_proxy: bind: ERROR: " + std::to_string(errno));
	}
	if (setsockopt(s_send, SOL_SOCKET, SO_RCVBUF, (char*)&buf_size, sizeof(ULONG)) < 0) {
		custom_log("connect_to_proxy: setsockopt: ERROR: " + std::to_string(errno));
	}

	si_send.sin_family = AF_INET;
	si_send.sin_port = htons(port_send);

#ifdef WIN32
    inet_pton(AF_INET, ip_send, &si_send.sin_addr.S_un.S_addr);
#else
    inet_pton(AF_INET, ip_send, &si_send.sin_addr.s_addr);
#endif

	sockaddr_in our_addr;
	socklen_t our_addr_len = sizeof(our_addr);
	getsockname(s_send, (sockaddr*)&our_addr, &our_addr_len);
	custom_log("connect_to_proxy: sendto: our port is " + to_string(ntohs(our_addr.sin_port)) + ", their port is " + to_string(ntohs(si_send.sin_port)), false, Color::Orange);

	if (sendto(s_send, t, BUFLEN, 0, (struct sockaddr*)&si_send, slen_send) == SOCKET_ERROR) {
		custom_log("connect_to_proxy: ERROR: failed to send to socket on port " + to_string(port_send), false, Color::Red);
		WSACleanup();
		return SendToError;
	}
	if (string(ip_send) != string(ip_recv) || port_send != port_recv) {
		one_socket = false;
		// Sleep for a while, so that conflicts in the WebRTC signaling can be avoided
		this_thread::sleep_for(chrono::milliseconds(2000));
		// Create receive socket
		if ((s_recv = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == SOCKET_ERROR) {
			custom_log("connect_to_proxy: ERROR: failed to create socket", false, Color::Red);
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

		sockaddr_in our_addr;
		socklen_t our_addr_len = sizeof(our_addr);
		getsockname(s_send, (sockaddr*)&our_addr, &our_addr_len);
		custom_log("connect_to_proxy: SHOULD NOT BE EXECUTED: sendto: our port is " + to_string(our_addr.sin_port) + ", their port is " + to_string(si_recv.sin_port), true, Color::Orange);

        if (sendto(s_recv, t, BUFLEN, 0, (struct sockaddr*)&si_recv, slen_recv) == SOCKET_ERROR) {
			custom_log("connect_to_proxy: ERROR: failed to send to socket on port " + to_string(port_recv), false, Color::Red);
			WSACleanup();
			return SendToError;
		}
	}
	initialized = true;
	custom_log("connect_to_proxy: Successfully set up sockets", false, Color::Orange);
	return ConnectionSuccess;
}

void listen_for_data() {
	custom_log("listen_for_data: Starting to listen for incoming data", false, Color::Yellow);
	while (keep_working) {
		size_t size = 0;

		// Strictly speaking, the use of a mutex should be unneccesary. However, when the start_listening function is
		// called multiple times, this might cause two processes reading from the socket, resulting in compromised data.
		unique_lock<mutex> guard(m_recv_data);

		sockaddr_in other_addr;
		memset(&other_addr, 0, sizeof(other_addr));
		socklen_t other_addr_len = sizeof(other_addr);
		if (one_socket) {
			sockaddr_in our_addr;
			socklen_t our_addr_len = sizeof(our_addr);
			if (getsockname(s_send, (sockaddr*)&our_addr, &our_addr_len) < 0) {

			}
			custom_log("listen_for_data: Attempting to receive data on port " + std::to_string(our_addr.sin_port));
			if ((size = recvfrom(s_send, buf, BUFLEN, 0, (struct sockaddr*)&other_addr, &other_addr_len)) == SOCKET_ERROR) {
				custom_log("listen_for_data: ERROR: recvfrom() failed with error code " + std::to_string(WSAGetLastError()), false, Color::Red);
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				continue;
			}
		} else {
			if ((size = recvfrom(s_recv, buf, BUFLEN, 0, (struct sockaddr*)&other_addr, &other_addr_len)) == SOCKET_ERROR) {
				custom_log("listen_for_data: ERROR: recvfrom() failed with error code " + std::to_string(WSAGetLastError()), false, Color::Red);
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				continue;
			}
		}

		custom_log("listen_for_data: recvfrom: got " + std::to_string(size) + " bytes from port " + std::to_string(other_addr.sin_port));

		struct PacketType p_type(&buf, size);
		if (p_type.type == 1) {
			struct PacketHeader p_header(&buf, size);
			ClientReceiver* c = find_or_add_receiver(p_header.client_number);
			auto tile = c->recv_tiles.find(make_pair(p_header.frame_number, p_header.tile_number));
			if (tile == c->recv_tiles.end()) {
				auto e = c->recv_tiles.emplace(make_pair(p_header.frame_number, p_header.tile_number),
					ReceivedTile(p_header.file_length, p_header.frame_number, p_header.tile_number));
				tile = e.first;
				custom_log("listen_for_data: Receiving new tile: " + p_header.string_representation(), false, Color::Yellow);
			}
			bool inserted = tile->second.insert(buf, p_header.file_offset, p_header.packet_length, size);
			if (tile->second.is_complete()) {
				custom_log("listen_for_data: Completed: " + p_header.string_representation(), false, Color::Yellow);
				c->tile_buffer.insert_tile(tile->second, p_header.tile_number);
				c->recv_tiles.erase(make_pair(p_header.frame_number, p_header.tile_number));
				custom_log("listen_for_data: New buffer size: " + to_string(c->tile_buffer.get_buffer_size(p_header.tile_number)), false, Color::Yellow);
			}
		} else if (p_type.type == 2) {
			recv_controls.push(ReceivedControl(buf, (uint32_t)size));
		} else {
			custom_log("listen_for_data: ERROR: unknown packet type " + to_string(p_type.type), false, Color::Red);
			guard.unlock();
			exit(EXIT_FAILURE);
		}
		buf = buf_ori;
		guard.unlock();
	}
	custom_log("listen_for_data: Stopped listening for incoming data", false, Color::Yellow);
}

void start_listening() {
	custom_log("start_listening: Starting new listening thread", false, Color::Yellow);
	worker = thread(listen_for_data);
}

void clean_up() {
    custom_log("clean_up: Attempting to clean up", false, Color::Orange);
	if (initialized) {
		keep_working = false;
		unique_lock<mutex> guard(m_send_data);
		closesocket(s_recv);
		closesocket(s_send);
		guard.unlock();
		if (worker.joinable())
			worker.join();
		free(buf);
		for (auto it = client_receivers.cbegin(); it != client_receivers.cend(); ) {
			client_receivers.erase(it++);
		}
		frame_numbers.clear();
		initialized = false;
		custom_log("clean_up: Cleaned up", false, Color::Orange);
	} else {
		custom_log("clean_up: Already cleaned up", false, Color::Orange);
	}
}

int send_packet(char* data, uint32_t size, uint32_t _packet_type) {
	uint32_t packet_type = _packet_type;
	int size_send = 0;
	char buf_msg[BUFLEN] = { 0 };
	memcpy(buf_msg, &packet_type, size);
	memcpy(&buf_msg[sizeof(packet_type)], data, size);

	sockaddr_in our_addr;
	socklen_t our_addr_len = sizeof(our_addr);
	getsockname(s_send, (sockaddr*)&our_addr, &our_addr_len);
	custom_log("send_packet: sendto: our port is " + to_string(ntohs(our_addr.sin_port)) + ", their port is " + to_string(ntohs(si_send.sin_port)), true, Color::Green);

	if ((size_send = sendto(s_send, buf_msg, BUFLEN, 0, (struct sockaddr*)&si_send, slen_send)) == SOCKET_ERROR) {
		custom_log("send_packet: ERROR: " + to_string(errno), false, Color::Red);
		return -1;
	}
	return size_send;
}

int send_tile(void* data, uint32_t size, uint32_t tile_number) {
	custom_log("send_tile: tile " + to_string(tile_number) + " with size " + to_string(size), false, Color::Green);
	uint32_t buflen_nheader = BUFLEN - sizeof(PacketType) - sizeof(PacketHeader);
	buflen_nheader = 1148;
	uint32_t current_offset = 0;
	uint32_t remaining = size;
	int full_size_send = 0;
	char* temp_d = reinterpret_cast<char*>(data);
	unique_lock<mutex> guard(m_send_data);
	while (remaining > 0 && keep_working) {
		uint32_t next_size = 0;
		if (remaining >= buflen_nheader) {
			next_size = buflen_nheader;
		} else {
			next_size = remaining;
		}
		char buf_msg[BUFLEN];
		struct PacketHeader p_header {
			client_number, frame_numbers[tile_number], tile_number, size, current_offset, next_size
		};
		custom_log("send_tile: Sending packet: " + p_header.string_representation());
		memcpy(buf_msg, &p_header, sizeof(p_header));
		memcpy(buf_msg + sizeof(p_header), reinterpret_cast<char*>(data) + current_offset, next_size);
		int size_send = send_packet(buf_msg, next_size + sizeof(PacketHeader), 1);
		if (size_send == SOCKET_ERROR) {
			guard.unlock();
			custom_log("send_tile: ERROR: the return value of send_packet should not be negative!", false, Color::Red);
			return size_send;
		}
		full_size_send += size_send;
		current_offset += next_size;
		remaining -= next_size;
	}
	custom_log("send_tile: Sent out frame " + to_string(frame_numbers[tile_number]) + ", tile " + to_string(tile_number));
	frame_numbers[tile_number] += 1;
	guard.unlock();
	custom_log("send_tile: return " + to_string(full_size_send), false, Color::Green);
	return full_size_send;
}

int get_tile_size(uint32_t client_number, uint32_t tile_number) {
	custom_log("get_tile_size: " + to_string(client_number) + ", " + to_string(tile_number), false, Color::Yellow);
	ClientReceiver* c = find_or_add_receiver(client_number);
	while (c->tile_buffer.get_buffer_size(tile_number) == 0) {
		this_thread::sleep_for(chrono::milliseconds(1));
		if (!keep_working) return 0;
	}
	ReceivedTile t = c->tile_buffer.next(tile_number);
	c->data_parser.set_current_tile(t, tile_number);
	int tile_size = c->data_parser.get_current_tile_size(tile_number);
	custom_log("get_tile_size: return " + to_string(tile_size), false, Color::Yellow);
	return tile_size;
}

void retrieve_tile(void* d, uint32_t size, uint32_t client_number, uint32_t tile_number) {
	custom_log("retrieve_tile: " + to_string(client_number) + ", " + to_string(tile_number), false, Color::Yellow);
	ClientReceiver* c = find_or_add_receiver(client_number);
	int local_size = c->data_parser.fill_data_array(d, size, tile_number);
	if (local_size >= 0) {
		custom_log("retrieve_tile: ERROR: retrieve_tile parameter size " + to_string(size) + " does not match registered data length" + to_string(local_size), false, Color::Red);
	}
	custom_log("retrieve_tile: return");
}

int send_control(void* data, uint32_t size) {
	custom_log("send_control: called");
	if (size > BUFLEN - sizeof(PacketType)) {
		custom_log("send_control: ERROR: send_control returns -1 since the message size is too large", false, Color::Red);
		return -1;
	}
	char* temp_d = reinterpret_cast<char*>(data);
	int size_send = send_packet(temp_d, size, 2);
	custom_log("send_control: return " + to_string(size_send));
	return size_send;
}

int get_control_size() {
	custom_log("get_control_size: called");
	unique_lock<mutex> guard(m_recv_control);
	if (recv_controls.empty()) {
		guard.unlock();
		custom_log("get_control_size: return -1");
		return -1;
	}
	int size = (int)recv_controls.front().get_data_length();
	guard.unlock();
	custom_log("get_control_size: return " + to_string(size));
	return size;
}

void retrieve_control(void* d, uint32_t size) {
	custom_log("retrieve_control: called");
	unique_lock<mutex> guard(m_recv_control);
	auto next_control_packet = std::move(recv_controls.front());
	recv_controls.pop();
	guard.unlock();
	uint32_t local_size = (uint32_t)next_control_packet.get_data_length();
	if (size != local_size) {
		custom_log("retrieve_control: ERROR: retrieve_control parameter size " + to_string(size) + " does not match data length " + to_string(local_size), false, Color::Red);
		return;
	}
	char* p = next_control_packet.get_data();
	char* temp_d = reinterpret_cast<char*>(d);
	memcpy(temp_d, p, size);
	custom_log("retrieve_control: return");
}
