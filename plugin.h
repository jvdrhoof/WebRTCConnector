#pragma once

#define DLLExport __declspec(dllexport)

// All exported functions should be declared here
extern "C"
{
	DLLExport void set_logging(char* _log_directory, bool _debug_mode);
	DLLExport int connect_to_proxy(char* ip_send, uint32_t port_send, char* ip_recv, uint32_t port_recv, uint32_t number_of_tiles);
	DLLExport void listen_for_data();
	DLLExport void start_listening();
	DLLExport void clean_up();
	DLLExport int send_tile(void* data, uint32_t size, uint32_t tile_number);
	DLLExport int get_tile_size(uint32_t tile_number);
	DLLExport void retrieve_tile(void* buff, uint32_t size, uint32_t tile_number);
	DLLExport int send_control(void* data, uint32_t size);
	DLLExport int get_control_size();
	DLLExport void retrieve_control(void* buff, uint32_t size);
}
