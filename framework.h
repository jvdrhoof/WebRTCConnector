#pragma once

// Exclude rarely-used stuff from Windows headers
#define WIN32_LEAN_AND_MEAN

// Include Windows header files
#include <windows.h>
#include <stdio.h>
#include <winsock2.h>
#include <Ws2tcpip.h>
#include <iostream>
#include <chrono>
#include <memory.h>
#include <map>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>

// Winsock Library
#pragma comment(lib,"ws2_32.lib") 
