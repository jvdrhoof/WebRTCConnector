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

uint32_t client_id;
uint32_t n_tiles;

static thread worker;
static bool keep_working = true;
static bool initialized = false;

mutex m_receivers;
mutex m_recv_data;
mutex m_send_data;
mutex m_recv_control;

static char* buf = NULL;
static char* buf_ori = NULL;

map<uint32_t, ClientReceiver*> client_receivers;
vector<uint32_t> frame_numbers;
queue<ReceivedControl> recv_controls;

static string log_file = "";
static bool verbose_mode = false;
mutex m_logging;


enum CONNECTION_SETUP_CODE : int {
	ConnectionSuccess = 0,
	StartUpError = 1,
	SocketCreationError = 2,
	SendToError = 3,
	AlreadyInitialized = 4
};

/*
	This function is used to get the current date/time in a predefined format, used by the custom_log function.
*/
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

/*
	This function is used to pass log messages to the user. Verbose logging can be enabled, and different colors can be
	used to inidicate a specific function (e.g., sending or receiving data).
*/
void custom_log(string message, bool verbose = true, Color color = Color::Black) {
	unique_lock<mutex> guard(m_logging);
	if (!verbose || verbose_mode) {
		Log::log(message, color);
	}
	if (log_file != "") {
		ofstream ofs(log_file.c_str(), ios_base::out | ios_base::app);
		ofs << get_current_date_time(false) << '\t' << message << '\n';
		ofs.close();
	}
	guard.unlock();
}

/*
	This function allows to specify a directory in which logs are created, and allows to specify if a verbose mode
	should be used. It should be called once per session from within Unity.
*/
void set_logging(char* log_directory, bool _verbose_mode) {
	Log::log("set_logging: Setting log directory to " + string(log_directory), Color::Orange);
	log_file = string(log_directory) + "\\" + get_current_date_time(true) + ".txt";
	verbose_mode = _verbose_mode;
	string str_verbose = verbose_mode ? "true" : "false";
	Log::log("set_logging: Setting verbose to " + str_verbose, Color::Orange);
}

/*
	This function enables the retrieval and (if needed) the creation of a client receiver. One receivere is required
	per WebRTC connection, so that frames and tiles corresponding to this peer can be stored appropriately.
*/
ClientReceiver* find_or_add_receiver(uint32_t client_id) {
	ClientReceiver* c;
	unique_lock<mutex> guard(m_receivers);
	auto it = client_receivers.find(client_id);
	if (it == client_receivers.end()) {
		custom_log("find_or_add_receiver: Client ID " + to_string(client_id) + " not yet registered, inserting now",
			false, Color::Orange);
		c = new ClientReceiver(client_id, n_tiles);
		client_receivers.insert({ client_id, c });
		custom_log("find_or_add_receiver: Client ID " + to_string(client_id) +
			" registered. Make sure you are receiving the correct client ID!", false, Color::Orange);
	} else {
		c = it->second;
	}
	guard.unlock();
	return c;
}                                                                                                                       

/*
	This function is responsible for initializing the DLL. It should be called once per session from within Unity,
	specifiying the required IP addresses and ports, the number of tiles that will be transmitted, and the client ID.
*/
int initialize(char* ip_send, uint32_t port_send, char* ip_recv, uint32_t port_recv, uint32_t _n_tiles,
	uint32_t _client_id) {
	custom_log("initialize: Attempting to connect to sender " + string(ip_send) + ":" + to_string(port_send) +
		" and receiver " + string(ip_recv) + ":" + to_string(port_recv) + ", using n_tiles " + to_string(_n_tiles) +
		" and client_id " + to_string(_client_id), false, Color::Orange);

	// Check if the DLL has already been initialized
	if (initialized) {
		custom_log("initialize: DLL already initialized, no changes are possible", false, Color::Orange);
		return AlreadyInitialized;
	}

	// Initialize parameters
	client_id = _client_id;
	n_tiles = _n_tiles;
	frame_numbers = vector<uint32_t>(n_tiles, 0);
	buf = (char*)malloc(BUFLEN);
	buf_ori = buf;
	keep_working = true;

#ifdef WIN32
	custom_log("initialize: Setting up socket to " + string(ip_send), false, Color::Orange);
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		return StartUpError;
	}
#endif

	// Generic parameters
	ULONG buf_size = 524288000;
	char t[BUFLEN] = {0};
	t[0] = (char)n_tiles;

	// Create send socket
	if ((s_send = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == SOCKET_ERROR) {
		custom_log("initialize: socket: ERROR: Failed to create socket", false, Color::Red);
		WSACleanup();
		return SocketCreationError;
	}

	// Binding ports
	sockaddr_in our_address;
	our_address.sin_family = AF_INET;
	our_address.sin_port = htons(port_send + 1);
	our_address.sin_addr.s_addr = htonl(INADDR_ANY);
	if (::bind(s_send, (struct sockaddr*)&our_address, sizeof(our_address)) < 0) {
		custom_log("initialize: bind: ERROR: " + std::to_string(WSAGetLastError()), false, Color::Red);
	}

	// Set socket options
	if (setsockopt(s_send, SOL_SOCKET, SO_RCVBUF, (char*)&buf_size, sizeof(ULONG)) < 0) {
		custom_log("initialize: setsockopt: ERROR: " + std::to_string(WSAGetLastError()), false, Color::Red);
	}
	si_send.sin_family = AF_INET;
	si_send.sin_port = htons(port_send);
#ifdef WIN32
    inet_pton(AF_INET, ip_send, &si_send.sin_addr.S_un.S_addr);
#else
    inet_pton(AF_INET, ip_send, &si_send.sin_addr.s_addr);
#endif

	// TODO: remove this?
	sockaddr_in our_addr;
	socklen_t our_addr_len = sizeof(our_addr);
	getsockname(s_send, (sockaddr*)&our_addr, &our_addr_len);
	custom_log("initialize: getsockname: Our port is " + to_string(ntohs(our_addr.sin_port)) + ", their port is " +
		to_string(ntohs(si_send.sin_port)), false, Color::Orange);

	// Send a message to the Golang peer, containing the number of tiles (<10 for now)
	if (sendto(s_send, t, BUFLEN, 0, (struct sockaddr*)&si_send, slen_send) == SOCKET_ERROR) {
		custom_log("initialize: sendto: ERROR: " + std::to_string(WSAGetLastError()), false, Color::Red);
		WSACleanup();
		return SendToError;
	}

	// In case a separate port is used to receive incoming data
	if (string(ip_send) != string(ip_recv) || port_send != port_recv) {

		// Make sure the listening thread is aware that a separate port is used
		one_socket = false;

		// Sleep for a while, so that conflicts in the WebRTC signaling can be avoided
		this_thread::sleep_for(chrono::milliseconds(2000));

		// Create receive socket
		if ((s_recv = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == SOCKET_ERROR) {
			custom_log("initialize: socket: ERROR: Failed to create socket", false, Color::Red);
			WSACleanup();
			return SocketCreationError;
		}

		// Set socket options
		if (setsockopt(s_recv, SOL_SOCKET, SO_RCVBUF, (char*)&buf_size, sizeof(ULONG)) < 0) {
			custom_log("initialize: setsockopt: ERROR: " + std::to_string(WSAGetLastError()), false, Color::Red);
		}
		si_recv.sin_family = AF_INET;
		si_recv.sin_port = htons(port_recv);
#ifdef WIN32
        inet_pton(AF_INET, ip_recv, &si_recv.sin_addr.S_un.S_addr);
#else
        inet_pton(AF_INET, ip_recv, &si_recv.sin_addr.s_addr);
#endif

		// TODO: remove this?
		sockaddr_in our_addr;
		socklen_t our_addr_len = sizeof(our_addr);
		getsockname(s_send, (sockaddr*)&our_addr, &our_addr_len);
		custom_log("initialize: getsockname: Our port is " + to_string(our_addr.sin_port) + ", their port is " +
			to_string(si_recv.sin_port), true, Color::Orange);

		// Send a message to the Golang peer, containing the number of tiles (<10 for now)
        if (sendto(s_recv, t, BUFLEN, 0, (struct sockaddr*)&si_recv, slen_recv) == SOCKET_ERROR) {
			custom_log("initialize: sendto: ERROR: " + std::to_string(WSAGetLastError()), false, Color::Red);
			WSACleanup();
			return SendToError;
		}
	}

	// Start a separate thread to listen to incoming data
	custom_log("initialize: Starting new listening thread", false, Color::Yellow);
	worker = thread(listen_for_data);

	// Make sure the clean_up function is aware
	initialized = true;

	return ConnectionSuccess;
}

/*
	This function is responsible for capturing incoming data. It is called from within a thread, which is started by the
	initialize function. No action is required from within Unity.
*/
void listen_for_data() {
	custom_log("listen_for_data: Starting to listen for incoming data", false, Color::Yellow);
	
	// Enable the listening thread to join
	while (keep_working) {

		// Make sure only one process is listening to the socket
		unique_lock<mutex> guard(m_recv_data);

		// Attempt to receive data from the Golang peer
		size_t size = 0;
		if (one_socket) {
			// Receive from the only available socket
			if ((size = recvfrom(s_send, buf, BUFLEN, 0, NULL, NULL)) == SOCKET_ERROR) {
				custom_log("listen_for_data: recvfrom: ERROR: " + std::to_string(WSAGetLastError()), false, Color::Red);
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				continue;
			}
		} else {
			// Receive from the additional socket
			if ((size = recvfrom(s_recv, buf, BUFLEN, 0, NULL, NULL)) == SOCKET_ERROR) {
				custom_log("listen_for_data: recvfrom: ERROR: " + std::to_string(WSAGetLastError()), false, Color::Red);
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				continue;
			}
		}
		custom_log("listen_for_data: recvfrom: got " + std::to_string(size) + " bytes");

		// Extract the packet type
		struct PacketType p_type(&buf, size);

		/*
			Distinguish between different packet types:
				1: point cloud frame
				2: control message
		*/
		if (p_type.type == 1) {

			// Extract the packet header
			struct PacketHeader p_header(&buf, size);

			// Get the receiver belonging to the connected peer
			ClientReceiver* c = find_or_add_receiver(p_header.client_id);

			// Retrieve tile iterator, creating a new instance if needed
			auto tile = c->recv_tiles.find(make_pair(p_header.frame_number, p_header.tile_id));
			if (tile == c->recv_tiles.end()) {
				auto e = c->recv_tiles.emplace(make_pair(p_header.frame_number, p_header.tile_id),
					ReceivedTile(p_header.file_length, p_header.frame_number, p_header.tile_id));
				tile = e.first;
				custom_log("listen_for_data: New tile: " + p_header.string_representation(), false, Color::Yellow);
			}

			// Insert the new data
			bool inserted = tile->second.insert(buf, p_header.file_offset, p_header.packet_length, size);
			if (!inserted) {
				custom_log("listen_for_data: Failed to insert: " + p_header.string_representation(), false, Color::Red);
			}

			// Check if all data corresponding to the frame has arrived
			if (tile->second.is_complete()) {
				custom_log("listen_for_data: Completed: " + p_header.string_representation(), false, Color::Yellow);
				c->tile_buffer.insert_tile(tile->second, p_header.tile_id);
				c->recv_tiles.erase(make_pair(p_header.frame_number, p_header.tile_id));
				custom_log("listen_for_data: New buffer size: " +
					to_string(c->tile_buffer.get_buffer_size(p_header.tile_id)), false, Color::Yellow);
			}

		} else if (p_type.type == 2) {

			// Push the message
			recv_controls.push(ReceivedControl(buf, (uint32_t)size));

		} else {
			custom_log("listen_for_data: ERROR: unknown packet type " + to_string(p_type.type), false, Color::Red);
			guard.unlock();
			exit(EXIT_FAILURE);
		}

		// Reset the buffer pointer
		buf = buf_ori;

		// Release the mutex
		guard.unlock();
	}

	// Stopped listening
	custom_log("listen_for_data: Stopped listening for incoming data", false, Color::Yellow);
}

/*
	This function is used to clean up threading and reset the required variables. It is called once per session from
	within Unity.
*/
void clean_up() {
    custom_log("clean_up: Attempting to clean up", false, Color::Orange);

	// Check if the DLL has already been initialized
	if (initialized) {

		// Halt sending/receiving operations
		keep_working = false;

		// Close sockets, using the mutex for sending data
		unique_lock<mutex> guard(m_send_data);
		closesocket(s_recv);
		closesocket(s_send);
		guard.unlock();

		// Join the listening thread
		if (worker.joinable())
			worker.join();

		// Free allocated memory
		if (buf != NULL) {
			free(buf);
			buf = NULL;
		}

		// Erase all client receivers
		for (auto it = client_receivers.cbegin(); it != client_receivers.cend(); ) {
			client_receivers.erase(it++);
		}

		// Clear frame numbers for all tiles
		frame_numbers.clear();

		// Reset the initialized flag
		initialized = false;

		custom_log("clean_up: Cleaned up", false, Color::Orange);
	} else {

		// No action is required
		custom_log("clean_up: Already cleaned up", false, Color::Orange);
	}
}

/*
	This function allows to send out a packet to the Golang peer. It returns the amount of bytes sent.
*/
int send_packet(char* data, uint32_t size, uint32_t _packet_type) {
	custom_log("send_packet: Sending out a packet with a size of " + to_string(size) + " bytes and packet type " +
		to_string(_packet_type), true, Color::Green);

	// Required parameters
	uint32_t packet_type = _packet_type;
	int size_sent = 0;

	// Insert all data into a buffer
	char buf_msg[BUFLEN] = {0};
	memcpy(buf_msg, &packet_type, size);
	memcpy(&buf_msg[sizeof(packet_type)], data, size);

	// Send the message to the Golang peer
	if ((size_sent = sendto(s_send, buf_msg, BUFLEN, 0, (struct sockaddr*)&si_send, slen_send)) == SOCKET_ERROR) {
		custom_log("send_packet: sendto: ERROR: " + std::to_string(WSAGetLastError()), false, Color::Red);
		return -1;
	}

	// Return the amount of bytes sent
	return size_sent;
}

/*
	This function allows to send out a frame of a tile to the Golang peer. It returns the amount of bytes sent.
*/
int send_tile(void* data, uint32_t size, uint32_t tile_id) {
	custom_log("send_tile: Tile " + to_string(tile_id) + " with size " + to_string(size), false, Color::Green);

	// Required parameters
	uint32_t buflen_nheader = BUFLEN - sizeof(PacketType) - sizeof(PacketHeader);
	buflen_nheader = 1148;
	uint32_t current_offset = 0;
	uint32_t remaining = size;
	int full_size_sent = 0;
	char* temp_d = reinterpret_cast<char*>(data);

	// Make sure only one process is sending out packets
	unique_lock<mutex> guard(m_send_data);

	// Send out packets as long as needed
	while (remaining > 0 && keep_working) {

		// Determine the amount of bytes to send out
		uint32_t next_size = 0;
		if (remaining >= buflen_nheader) {
			next_size = buflen_nheader;
		} else {
			next_size = remaining;
		}
		
		// Create a new packet header
		struct PacketHeader p_header {
			client_id, frame_numbers[tile_id], tile_id, size, current_offset, next_size
		};
		custom_log("send_tile: Sending packet: " + p_header.string_representation());

		// Insert all data into a buffer
		char buf_msg[BUFLEN];
		memcpy(buf_msg, &p_header, sizeof(p_header));
		memcpy(buf_msg + sizeof(p_header), reinterpret_cast<char*>(data) + current_offset, next_size);

		// Send out the packet
		int size_sent = send_packet(buf_msg, next_size + sizeof(PacketHeader), 1);
		if (size_sent < 0) {
			guard.unlock();
			custom_log("send_tile: ERROR: the return value of send_packet should not be negative!", false, Color::Red);
			return -1;
		}

		// Update parameters
		full_size_sent += size_sent;
		current_offset += next_size;
		remaining -= next_size;
	}

	custom_log("send_tile: Sent out frame " + to_string(frame_numbers[tile_id]) + " of tile " +
		to_string(tile_id) + ", using " + to_string(full_size_sent) + " bytes");

	// Increase frame number by one
	frame_numbers[tile_id] += 1;

	// Release the mutex
	guard.unlock();

	// Return the amount of bytes sent
	return full_size_sent;
}

/*
	This function returns the size of the next frame corresponding to a given tile. The function should be called from
	within the Unity reader every time a new frame is desired. The resulting return value should be used to allocate the
	required memory and call the retrieve_tile function.
*/
int get_tile_size(uint32_t client_id, uint32_t tile_id) {
	custom_log("get_tile_size: " + to_string(client_id) + ", " + to_string(tile_id), false, Color::Yellow);

	// Get the receiver belonging to the connected peer
	ClientReceiver* c = find_or_add_receiver(client_id);

	// Wait until a new frame is available
	while (c->tile_buffer.get_buffer_size(tile_id) == 0) {
		this_thread::sleep_for(chrono::milliseconds(1));
		if (!keep_working) {
			return 0;
		}
	}

	// Retrieve the next frame and forward it to the data parser
	ReceivedTile t = c->tile_buffer.next(tile_id);
	c->data_parser.set_current_tile(t, tile_id);

	// Retrieve the tile size
	int tile_size = c->data_parser.get_current_tile_size(tile_id);
	custom_log("get_tile_size: return " + to_string(tile_size), false, Color::Yellow);

	// Return the tile size
	return tile_size;
}

/*
	This function allows to retrieve the next frame corresponding to a tile in memory. The function should be called
	from within the Unity reader once get_tile_size has returned the tile size and the required memory has been
	allocated.
*/
void retrieve_tile(void* d, uint32_t size, uint32_t client_id, uint32_t tile_id) {
	custom_log("retrieve_tile: " + to_string(client_id) + ", " + to_string(tile_id), false, Color::Yellow);

	// Get the receiver belonging to the connected peer
	ClientReceiver* c = find_or_add_receiver(client_id);

	// Fill the allocated memory with the requested data, if possible
	int local_size = c->data_parser.fill_data_array(d, size, tile_id);
	if (local_size == 0) {
		custom_log("retrieve_tile: ERROR: the tile could not be retrieved", false, Color::Red);
	} else if (local_size != size) {
		custom_log("retrieve_tile: ERROR: retrieve_tile parameter size " + to_string(size) +
			" does not match the registered data length" + to_string(local_size), false, Color::Red);
	} else {
		custom_log("retrieve_tile: Tile " + to_string(tile_id) + " from client " + to_string(client_id) +
			" successfully retrieved", false, Color::Yellow);
	}
}

/* 
	The functions below can be used to send and retrieve control messages, yet this feature is currently not supported
	by the Golang peers and the SFU.
*/

int send_control(void* data, uint32_t size) {
	custom_log("send_control: called");
	if (size > BUFLEN - sizeof(PacketType)) {
		custom_log("send_control: ERROR: send_control returns -1 since the message size is too large", false, Color::Red);
		return -1;
	}
	char* temp_d = reinterpret_cast<char*>(data);
	int size_sent = send_packet(temp_d, size, 2);
	custom_log("send_control: return " + to_string(size_sent));
	return size_sent;
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
