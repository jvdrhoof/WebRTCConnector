#include "pch.h"
#include "buffer.hpp"
#include "audio_buffer.hpp"
#include "client_receiver.hpp"
#include "data_parser.hpp"
#include "framework.h"
#include "log.h"
#include "packet_data.hpp"
#include "plugin.h"
#include "received_control.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <sys/stat.h>
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

static thread worker;
static bool keep_working = true;
static bool initialized = false;
static bool peer_ready = false;

mutex m_receivers;
mutex m_recv_data;
mutex m_send_data;
mutex m_recv_control;

static char* buf = NULL;
static char* buf_ori = NULL;


map<uint32_t, ClientReceiver*> client_receivers;

vector<uint32_t> frame_numbers;
uint32_t audio_frame_number;
queue<ReceivedControl> recv_controls;

static string log_file = "";
static int debug_level = 0;
mutex m_logging;

string api_version = "1.0";

enum CONNECTION_SETUP_CODE : int {
	ConnectionSuccess = 0,
	StartUpError = 1,
	SocketCreationError = 2,
	SendToError = 3,
	AlreadyInitialized = 4,
	WrongAPIVersion = 5
};

enum DEBUG_LEVEL : int {
	Default = 0,
	Verbose = 1,
	Debug = 2
};

enum CONSOLE_LEVEL : int
{
	Log = 0,
	Warning = 1,
	Error = 2
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
	localtime_r(&now, &tstruct);
#endif
	if (date_only) {
		strftime(buf, sizeof(buf), "%Y-%m-%d", &tstruct);
	}
	else {
		strftime(buf, sizeof(buf), "%Y-%m-%d %X", &tstruct);
	}
	return string(buf);
};

/*
	This function is used to pass log messages to the user. Verbose logging can be enabled, and different colors can be
	used to inidicate a specific function (e.g., sending or receiving data).
*/
void custom_log(string message, int _debug_level = 0, int console_level = 0, Color color = Color::Black) {
	unique_lock<mutex> guard(m_logging);
	if (_debug_level <= debug_level) {
		Log::log(message, console_level, color);
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
void set_logging(char* log_directory, int _debug_level) {
	debug_level = _debug_level;
	Log::log("set_logging: Debug level set to " + to_string(debug_level), Log, Color::Orange);
	if (log_directory == NULL) {
		Log::log("set_logging: Log directory is NULL, so logs will not be written to file", Log, Color::Orange);
		return;
	}
	if (strlen(log_directory) == 0) {
		Log::log("set_logging: Log directory is an empty string, so logs will not be written to file", Log, Color::Orange);
		return;
	}
	struct stat sb;
	if (stat(log_directory, &sb) != 0) {
		Log::log("set_logging: Log directory " + string(log_directory) + " does not exist!", Log, Color::Red);
		return;
	}
	std::filesystem::path log_file_path = log_directory;
	log_file_path /= get_current_date_time(true) + ".txt";
	log_file = log_file_path.string();
	Log::log("set_logging: Logs will be written to " + log_file, Log, Color::Orange);
}

/*
	This function enables the retrieval and (if needed) the creation of a client receiver. One receiver is required
	per WebRTC connection, so that frames and tiles corresponding to this peer can be stored appropriately.
*/
ClientReceiver* find_or_add_receiver(uint32_t client_id, bool is_audio = false) {
	unique_lock<mutex> guard(m_receivers);
	ClientReceiver* c = nullptr;
	auto it = client_receivers.find(client_id);
	if (it == client_receivers.end()) {
		custom_log("find_or_add_receiver: Client ID " + to_string(client_id) + " not yet registered. Registering now...",
			Verbose, Log, Color::Orange);
		c = new ClientReceiver(client_id);
		// We need to do it like this because else bad stuff happens when we use both audio + video
		auto itt = client_receivers.insert({ client_id, c });
		c = itt.first->second;
		custom_log("find_or_add_receiver: Client ID " + to_string(client_id) +
			" registered. Make sure you are receiving the correct client ID!", Verbose, Log, Color::Orange);
	}
	else {
		c = it->second;
	}
	guard.unlock();
	return c;
}

/*
	This function is responsible for initializing the DLL. It should be called once per session from within Unity,
	specifiying the required IP addresses and ports, the number of tiles that will be transmitted, and the client ID.
*/
int initialize(char* ip_send, uint32_t port_send, char* ip_recv, uint32_t port_recv, uint32_t n_tiles,
	uint32_t _client_id, char* api_version) {

	if (string(api_version) != api_version) {
		custom_log("intialize: ERROR: An incorrect API version of the WebRTCConnector is used!", Default, Log, Color::Red);
		return WrongAPIVersion;
	}

	custom_log("initialize: Attempting to connect to sender " + string(ip_send) + ":" + to_string(port_send) +
		" and receiver " + string(ip_recv) + ":" + to_string(port_recv) + ", using n_tiles " + to_string(n_tiles) +
		" and client_id " + to_string(_client_id), Verbose, Log, Color::Orange);

	// Check if the DLL has already been initialized
	if (initialized) {
		custom_log("initialize: DLL already initialized, no changes are possible", Verbose, Log, Color::Orange);
		return AlreadyInitialized;
	}

	// Initialize parameters
	client_id = _client_id;
	frame_numbers = vector<uint32_t>(n_tiles, 0);
	buf = (char*)malloc(BUFLEN);
	buf_ori = buf;
	keep_working = true;
	
	client_receivers = map<uint32_t, ClientReceiver*>();
	client_receivers.clear();

#ifdef WIN32
	custom_log("initialize: Setting up socket to " + string(ip_send), Verbose, Log, Color::Orange);
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		return StartUpError;
	}
#endif

	// Generic parameters
	ULONG buf_size = 524288000;

	// Create send socket
	if ((s_send = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == SOCKET_ERROR) {
		custom_log("initialize: socket: ERROR: Failed to create socket", Default, Log, Color::Red);
		custom_log("No data can be retrieved through WebRTC, so no content consumption is possible", Default, Error, Color::Red);
		WSACleanup();
		return SocketCreationError;
	}

	// Binding ports
	sockaddr_in our_address;
	our_address.sin_family = AF_INET;
	our_address.sin_port = htons(port_send + 1);
	our_address.sin_addr.s_addr = htonl(INADDR_ANY);
	if (::bind(s_send, (struct sockaddr*)&our_address, sizeof(our_address)) < 0) {
		custom_log("initialize: bind: ERROR: " + std::to_string(WSAGetLastError()), Default, Log, Color::Red);
	}

	// Set socket options
	if (setsockopt(s_send, SOL_SOCKET, SO_RCVBUF, (char*)&buf_size, sizeof(ULONG)) < 0) {
		custom_log("initialize: setsockopt: ERROR: " + std::to_string(WSAGetLastError()), Default, Log, Color::Red);
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
	custom_log("initialize: getsockname: Our port is " + to_string(ntohs(our_addr.sin_port)) + ", their port is " +
		to_string(ntohs(si_send.sin_port)), Verbose, Log, Color::Orange);

	// In case a separate port is used to receive incoming data
	if (string(ip_send) != string(ip_recv) || port_send != port_recv) {

		// Make sure the listening thread is aware that a separate port is used
		one_socket = false;

		// Sleep for a while, so that conflicts in the WebRTC signaling can be avoided
		this_thread::sleep_for(chrono::milliseconds(2000));

		// Create receive socket
		if ((s_recv = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == SOCKET_ERROR) {
			custom_log("initialize: socket: ERROR: Failed to create socket", Default, Log, Color::Red);
			custom_log("No data can be retrieved through WebRTC, so no content consumption is possible", Default, Error, Color::Red);
			WSACleanup();
			return SocketCreationError;
		}

		// Set socket options
		if (setsockopt(s_recv, SOL_SOCKET, SO_RCVBUF, (char*)&buf_size, sizeof(ULONG)) < 0) {
			custom_log("initialize: setsockopt: ERROR: " + std::to_string(WSAGetLastError()), Default, Log, Color::Red);
		}
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
		custom_log("initialize: getsockname: Our port is " + to_string(our_addr.sin_port) + ", their port is " +
			to_string(si_recv.sin_port), Verbose, Log, Color::Orange);
	}

	// Start a separate thread to listen to incoming data
	custom_log("initialize: Starting a thread to listen for incoming data", Verbose, Log, Color::Yellow);
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
	custom_log("listen_for_data: Starting to listen for incoming data", Verbose, Log, Color::Yellow);

	// Enable the listening thread to join
	while (keep_working) {

		// Make sure only one process is listening to the socket
		unique_lock<mutex> guard(m_recv_data);

		// Attempt to receive data from the Golang peer
		size_t size = 0;
		if (one_socket) {
			// Receive from the only available socket
			if ((size = recvfrom(s_send, buf, BUFLEN, 0, NULL, NULL)) == SOCKET_ERROR) {
				custom_log("listen_for_data: recvfrom: ERROR: " + std::to_string(WSAGetLastError()), Default,
					Log, Color::Red);
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				continue;
			}
		}
		else {
			// Receive from the additional socket
			if ((size = recvfrom(s_recv, buf, BUFLEN, 0, NULL, NULL)) == SOCKET_ERROR) {
				custom_log("listen_for_data: recvfrom: ERROR: " + std::to_string(WSAGetLastError()), Default,
					Log, Color::Red);
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				continue;
			}
		}
		custom_log("listen_for_data: recvfrom: Received " + std::to_string(size) + " bytes, processing now...", Debug, Log, Color::Yellow);

		// Extract the packet type
		struct PacketType p_type(&buf, size);

		/*
			Distinguish between different packet types:
			    0: peer is ready to receive / send data
				1: point cloud frame
				2: audio frame
				3: control message
		*/
		switch (p_type.type) {
			case(PacketType::PeerReadyPacket): {
				// Peer is now ready to receive data
				peer_ready = true;
				char t[BUFLEN] = { 0 };
				t[0] = (char)0;
				custom_log("listen_for_data: Connected to Go peer", Verbose, Log, Color::Orange);
				if (sendto(s_send, t, BUFLEN, 0, (struct sockaddr*)&si_send, slen_send) == SOCKET_ERROR) {
					custom_log("initialize: sendto: ERROR: " + std::to_string(WSAGetLastError()), Default, Log, Color::Red);
					WSACleanup();
					return;
				}
				break;
			};
			case(PacketType::TilePacket): {
				// Extract the packet header
				struct PacketHeader p_header(&buf, size);

				// Get the receiver belonging to the connected peer
				ClientReceiver* c = find_or_add_receiver(p_header.client_id);

				// Retrieve tile iterator, creating a new instance if needed
				auto tile = c->recv_tiles.find(make_pair(p_header.frame_number, p_header.tile_id));
				if (tile == c->recv_tiles.end()) {
					auto e = c->recv_tiles.emplace(make_pair(p_header.frame_number, p_header.tile_id),
						ReceivedTile(p_header.file_length, p_header.frame_number, p_header.tile_id, p_header.quality));
					tile = e.first;
					custom_log("listen_for_data: New tile frame arrived: " + p_header.string_representation(), Debug, Log, Color::Yellow);
				}
				else if (tile->second.get_quality() != p_header.quality) {
					custom_log("listen_for_data: A packet arrived with a different quality than expected: " + p_header.string_representation(), Debug, Log, Color::Yellow);
					custom_log("listen_for_data: Previous packets belonging to quality " + std::to_string(tile->second.get_quality()) + " will be erased", Debug, Log, Color::Yellow);
					custom_log("listen_for_data: Changing quality for tile " + to_string(p_header.tile_id) + " from " + to_string(tile->second.get_quality()) + " to " + to_string(p_header.quality), Verbose, Log, Color::Yellow);
					c->recv_tiles.erase(tile);
					auto e = c->recv_tiles.emplace(make_pair(p_header.frame_number, p_header.tile_id),
						ReceivedTile(p_header.file_length, p_header.frame_number, p_header.tile_id, p_header.quality));
					tile = e.first;
				}

				// Insert the new data
				bool inserted = tile->second.insert(buf, p_header.file_offset, p_header.packet_length, size);
				if (!inserted) {
					custom_log("listen_for_data: Failed to insert the received tile frame in the buffer: " + p_header.string_representation(), Default,
						Log, Color::Red);
				}

				// Check if all data corresponding to the frame has arrived
				if (tile->second.is_complete()) {
					custom_log("listen_for_data: Completed the reception of tile frame: " + p_header.string_representation(), Debug, Log, Color::Yellow);
					c->tile_buffer.insert_tile(tile->second, p_header.tile_id);
					c->recv_tiles.erase(make_pair(p_header.frame_number, p_header.tile_id));
					custom_log("listen_for_data: The new tile buffer size equals: " +
						to_string(c->tile_buffer.get_buffer_size(p_header.tile_id)), Debug, Log, Color::Yellow);
				}
				break;
			};
			case(PacketType::AudioPacket): {
				// Extract the packet header
				struct AudioPacketHeader p_header(&buf, size);

				// Get the receiver belonging to the connected peer
				ClientReceiver* c = find_or_add_receiver(p_header.client_id);

				// Retrieve audio iterator, creating a new instance if needed
				auto audio_frame = c->recv_audio.find(p_header.frame_number);
				if (audio_frame == c->recv_audio.end()) {
					auto e = c->recv_audio.emplace(p_header.frame_number,
						ReceivedAudio(p_header.file_length, p_header.frame_number));
					audio_frame = e.first;
					if (p_header.frame_number % 100 == 0) {
						custom_log("listen_for_data: New audio packet arrived: " + std::to_string(p_header.file_offset) + " " +
							std::to_string(p_header.packet_length) + std::to_string(p_header.file_length),
							Debug, Log, Color::Yellow);
					}
				}

				// Insert the new data
				bool inserted = audio_frame->second.insert(buf, p_header.file_offset, p_header.packet_length, size);
				if (!inserted) {
					custom_log("listen_for_data: Failed to insert the received audio packet in the buffer: " + p_header.string_representation(), Default,
						Log, Color::Red);
				}
				// Check if all data corresponding to the frame has arrived
				if (audio_frame->second.is_complete()) {
					custom_log("listen_for_data: Completed the reception of audio packet: " + p_header.string_representation(), Debug, Log, Color::Yellow);
					c->audio_buffer.insert_audio_frame(audio_frame->second);
					c->recv_audio.erase(p_header.frame_number);
					custom_log("listen_for_data: The new audio buffer size equals: " +
						to_string(c->audio_buffer.get_buffer_size()), Debug, Log, Color::Yellow);
				}
				break;
			};
			case(PacketType::ControlPacket): {
				custom_log("listen_for_data: Received a control packet, pushing...", Debug, Log, Color::Red);

				// Push the message
				recv_controls.push(ReceivedControl(buf, (uint32_t)size));
				break;
			};
			default: {
				custom_log("listen_for_data: ERROR: unknown packet type " + to_string(p_type.type), Default, Log, Color::Red);
				custom_log("Data can no longer be retrieved through WebRTC, so no content consumption is possible", Default, Error, Color::Red);
				guard.unlock();
				exit(EXIT_FAILURE);
			};
		}

		// Reset the buffer pointer
		buf = buf_ori;

		// Release the mutex
		guard.unlock();
	}

	// Stopped listening
	custom_log("listen_for_data: Stopped listening for incoming data", Verbose, Log, Color::Yellow);
}

/*
	This function is used to clean up threading and reset the required variables. It is called once per session from
	within Unity.
*/
void clean_up() {
	custom_log("clean_up: Attempting to clean up threading and resetting all variables", Verbose, Log, Color::Orange);

	// Check if the DLL has already been initialized
	if (initialized) {

		// Halt sending/receiving operations
		keep_working = false;

		// Close sockets, using the mutex for sending data
		unique_lock<mutex> guard(m_send_data);
		closesocket(s_recv);
		closesocket(s_send);
		frame_numbers.clear();
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

		// Reset the initialized flag
		initialized = false;
		peer_ready = false;
		custom_log("clean_up: Cleaned up", Verbose, Log, Color::Orange);
	}
	else {

		// No action is required
		custom_log("clean_up: Already cleaned up", Verbose, Log, Color::Orange);
	}
}

/*
	This function allows to send out a packet to the Golang peer. It returns the amount of bytes sent.
*/
int send_packet(char* data, uint32_t size, uint32_t _packet_type) {
	// Required parameters
	uint32_t packet_type = _packet_type;
	int size_sent = 0;

	// Insert all data into a buffer
	char buf_msg[BUFLEN] = { 0 };
	memcpy(buf_msg, &packet_type, sizeof(packet_type));
	memcpy(&buf_msg[sizeof(packet_type)], data, size);

	// Send the message to the Golang peer
	if ((size_sent = sendto(s_send, buf_msg, BUFLEN, 0, (struct sockaddr*)&si_send, slen_send)) == SOCKET_ERROR) {
		custom_log("send_packet: sendto: ERROR: Failed to send data to the Golang peer because of the following issue: " + std::to_string(WSAGetLastError()), Default, Log, Color::Red);
		return -1;
	}

	// Return the amount of bytes sent
	return size_sent;
}

/*
	This function allows to send out a frame of a tile to the Golang peer. It returns the amount of bytes sent.
*/
int send_tile(void* data, uint32_t size, uint32_t tile_id, uint32_t quality) {
	custom_log("send_tile: Trying to send out tile " + to_string(tile_id) + " at quality " + to_string(quality) + " with size " + to_string(size), Debug, Log, Color::Green);

	if (!peer_ready) {
		custom_log("send_tile: The Golang peer is not yet ready, so no data can be sent at the moment", Verbose, Log, Color::Red);
		return -1;
	}

	if (!initialized) {
		custom_log("send_tile: ERROR: The DLL has not yet been initialized!", Default, Log, Color::Red);
		return -1;
	}

	// Required parameters
	uint32_t buflen_nheader = BUFLEN - sizeof(PacketType) - sizeof(PacketHeader);
	// TODO: Calculate values using constants
	buflen_nheader = 1144;
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
		}
		else {
			next_size = remaining;
		}

		// Create a new packet header
		struct PacketHeader p_header {
			client_id, frame_numbers[tile_id], size, current_offset, next_size, tile_id, quality
		};

		// Insert all data into a buffer
		char buf_msg[BUFLEN];
		memcpy(buf_msg, &p_header, sizeof(p_header));

		memcpy(buf_msg + sizeof(p_header), reinterpret_cast<char*>(data) + current_offset, next_size);

		// Send out the packet
		int size_sent = send_packet(buf_msg, next_size + sizeof(PacketHeader), PacketType::TilePacket);
		if (size_sent < 0) {
			guard.unlock();
			custom_log("send_tile: ERROR: the return value of send_packet should not be negative!", Default,
				Log, Color::Red);
			return -1;
		}

		// Update parameters
		full_size_sent += size_sent;
		current_offset += next_size;
		remaining -= next_size;
	}

	custom_log("send_tile: Sent out frame " + to_string(frame_numbers[tile_id]) + " of tile " +
		to_string(tile_id) + " at quality " + to_string(quality) + ", using " + to_string(full_size_sent) + " bytes", Debug, Log, Color::Green);

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
	custom_log("get_tile_size: Determining tile size for client " + to_string(client_id) + " and tile " + to_string(tile_id), Debug, Log, Color::Yellow);

	// Get the receiver belonging to the connected peer
	ClientReceiver* c = find_or_add_receiver(client_id);

	// Wait until a new frame is available
	auto tile_status = c->tile_buffer.wait_for_tile(tile_id);
	if (!tile_status) {
		return 0;
	}
	/*while (c->tile_buffer.get_buffer_size(tile_id) == 0) {
		this_thread::sleep_for(chrono::milliseconds(1));
		if (!keep_working) {
			return 0;
		}
	}*/

	// Retrieve the next frame and forward it to the data parser
	ReceivedTile t = c->tile_buffer.next(tile_id);
	c->data_parser.set_current_tile(t, tile_id);

	// Retrieve the tile size
	int tile_size = c->data_parser.get_current_tile_size(tile_id);
	custom_log("get_tile_size: The tile size equals: " + to_string(tile_size), Debug, Log, Color::Yellow);

	// Return the tile size
	return tile_size;
}


/*
	This function allows to retrieve the next frame corresponding to a tile in memory. The function should be called
	from within the Unity reader once get_tile_size has returned the tile size and the required memory has been
	allocated.
*/
void retrieve_tile(void* d, uint32_t size, uint32_t client_id, uint32_t tile_id) {
	custom_log("retrieve_tile: Retrieving tile for client " + to_string(client_id) + " and tile " + to_string(tile_id), Debug, Log, Color::Yellow);

	// Get the receiver belonging to the connected peer
	ClientReceiver* c = find_or_add_receiver(client_id);

	// Fill the allocated memory with the requested data, if possible
	int local_size = c->data_parser.fill_data_array(d, size, tile_id);
	if (local_size == 0) {
		custom_log("retrieve_tile: ERROR: The tile could not be retrieved", Default, Log, Color::Red);
	}
	else if (local_size != size) {
		custom_log("retrieve_tile: ERROR: retrieve_tile parameter size " + to_string(size) +
			" does not match the registered data length" + to_string(local_size), Default, Log, Color::Red);
	}
	else {
		custom_log("retrieve_tile: Tile " + to_string(tile_id) + " from client " + to_string(client_id) +
			" successfully retrieved", Debug, Log, Color::Yellow);
	}
}

/*
	This function allows to send out an audio frame to the Golang peer. It returns the amount of bytes sent.
*/
int send_audio(void* data, uint32_t size) {
	custom_log("send_audio: Trying to send out audio packet with size " + to_string(size), Debug, Log, Color::Green);

	if (!peer_ready) {
		custom_log("send_audio: The Golang peer is not yet ready, so no data can be sent at the moment", Verbose, Log, Color::Red);
		return -1;
	}

	if (!initialized) {
		custom_log("send_audio: ERROR: The DLL has not yet been initialized!", Default, Log, Color::Red);
		return -1;
	}

	// Required parameters
	uint32_t buflen_nheader = BUFLEN - sizeof(PacketType) - sizeof(PacketHeader);
	// TODO: Calculate values using constants
	buflen_nheader = 1152;
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
		}
		else {
			next_size = remaining;
		}

		// Create a new packet header
		struct AudioPacketHeader p_header {
			client_id, audio_frame_number, size, current_offset, next_size
		};

		// Insert all data into a buffer
		char buf_msg[BUFLEN];
		memcpy(buf_msg, &p_header, sizeof(p_header));
		memcpy(buf_msg + sizeof(p_header), reinterpret_cast<char*>(data) + current_offset, next_size);

		// Send out the packet
		int size_sent = send_packet(buf_msg, next_size + sizeof(AudioPacketHeader), PacketType::AudioPacket);
		if (size_sent < 0) {
			guard.unlock();
			custom_log("send_audio: ERROR: The return value of send_packet should not be negative!", Default,
				Log, Color::Red);
			return -1;
		}

		// Update parameters
		full_size_sent += size_sent;
		current_offset += next_size;
		remaining -= next_size;
	}

	custom_log("send_audio: Sent out audio frame " + to_string(audio_frame_number) + ", using " + to_string(full_size_sent) + " bytes", Debug, Log, Color::Green);

	// Increase frame number by one
	audio_frame_number += 1;

	// Release the mutex
	guard.unlock();

	// Return the amount of bytes sent
	return full_size_sent;
}

/*
	This function returns the size of the next audio frame corresponding. The function should be called from
	within the Unity reader every time a new audio frame is desired. The resulting return value should be used to allocate the
	required memory and call the retrieve_audio function.
*/
int get_audio_size(uint32_t client_id) {
	custom_log("get_audio_size: Determining audio packet size for client " + to_string(client_id), Debug, Log, Color::Yellow);
	
	// Get the receiver belonging to the connected peer
	ClientReceiver* c = find_or_add_receiver(client_id);
	
	// Wait until a new frame is available
	//timeBeginPeriod(1);
	bool audio_ready = c->audio_buffer.wait_for_audio();
	if (!audio_ready) {
		return 0;
	}
	/*while (c->audio_buffer.get_buffer_size() == 0) {
		this_thread::sleep_for(chrono::milliseconds(1)); // TODO: remove all of this pull logic and replace it with callbacks
		c = find_or_add_receiver(client_id);
		if (!keep_working) {
			return 0;
		}
	}*/
	//timeEndPeriod(1);
	// Retrieve the next frame and forward it to the data parser
	ReceivedAudio t = c->audio_buffer.next();
	c->data_parser.set_current_audio(t);

	// Retrieve the tile size
	int audio_size = c->data_parser.get_current_audio_size();
	custom_log("get_audio_size: The audio packet size equals: " + to_string(audio_size), Debug, Log, Color::Yellow);

	// Return the tile size
	return audio_size;
}

/*
	This function allows to retrieve the next audio frame from memory. The function should be called
	from within the Unity reader once get_audio_size has returned the audio size and the required memory has been
	allocated.
*/
void retrieve_audio(void* d, uint32_t size, uint32_t client_id) {
	custom_log("retrieve_audio: Retrieving audio packet for client " + to_string(client_id), Debug, Log, Color::Yellow);

	// Get the receiver belonging to the connected peer
	ClientReceiver* c = find_or_add_receiver(client_id);

	// Fill the allocated memory with the requested data, if possible
	int local_size = c->data_parser.fill_data_array(d, size);
	if (local_size == 0) {
		custom_log("retrieve_audio: ERROR: The audio could not be retrieved", Default, Log, Color::Red);
	}
	else if (local_size != size) {
		custom_log("retrieve_audio: ERROR: retrieve_audio parameter size " + to_string(size) +
			" does not match the registered data length " + to_string(local_size), Default, Log, Color::Red);
	}
	else {
		custom_log("retrieve_audio: Audio frame from client " + to_string(client_id) +
			" successfully retrieved", Debug, Log, Color::Yellow);
	}
}

/*
	The functions below can be used to send and retrieve control messages, yet this feature is currently not supported
	by the Golang peers and the SFU.
*/

/*
	This function allows to send out an audio frame to the Golang peer. It returns the amount of bytes sent.
*/
int send_control_packet(void* data, uint32_t size) {
	custom_log("send_control_packet: Trying to send out control packet with size " + to_string(size), Debug, Log, Color::Green);

	if (size > BUFLEN - sizeof(PacketType)) {
		custom_log("send_control_packet: ERROR: The message size " + to_string(size) + " is larger than " + to_string(BUFLEN - sizeof(PacketType)) + ", so the control packet cannot be processed", Default, Log, Color::Red);
		return -1;
	}

	if (!peer_ready) {
		custom_log("send_control_packet: The Golang peer is not yet ready, so no data can be sent at the moment", Verbose, Log, Color::Red);
		return -1;
	}

	if (!initialized) {
		custom_log("send_control_packet: ERROR: The DLL has not yet been initialized!", Default, Log, Color::Red);
		return -1;
	}

	// Required parameters
	char* temp_d = reinterpret_cast<char*>(data);

	// Make sure only one process is sending out packets
	unique_lock<mutex> guard(m_send_data);

	// Insert all data into a buffer
	char buf_msg[BUFLEN];
	memcpy(buf_msg, reinterpret_cast<char*>(data), size);

	// Send out the packet
	int size_sent = send_packet(buf_msg, size, PacketType::ControlPacket);
	if (size_sent < 0) {
		guard.unlock();
		custom_log("send_control_packet: ERROR: The return value of send_packet should not be negative!", Default, Log, Color::Red);
		return -1;
	}

	custom_log("send_control_packet: Sent out control frame  " + to_string(size_sent) + " bytes", Debug, Log, Color::Green);
	// Release the mutex
	guard.unlock();

	// Return the amount of bytes sent
	return size_sent;
}
