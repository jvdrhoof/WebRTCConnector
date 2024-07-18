#include "pch.h"
#include "log.h"

#include <stdio.h>
#include <string>
#include <stdio.h>
#include <sstream>

void Log::log(const char* message, int console_level, Color color) {
	if (callbackInstance != nullptr) {
		callbackInstance(message, console_level, (int)color, (int)strlen(message));
	}
}

void Log::log(const std::string message, int console_level, Color color) {
	const char* tmsg = message.c_str();
	if (callbackInstance != nullptr) {
		callbackInstance(tmsg, console_level, (int)color, (int)strlen(tmsg));
	}
}

void Log::log(const int message, int console_level, Color color) {
	std::stringstream ss;
	ss << message;
	send_log(ss, console_level, color);
}

void Log::log(const char message, int console_level, Color color) {
	std::stringstream ss;
	ss << message;
	send_log(ss, console_level, color);
}

void Log::log(const float message, int console_level, Color color) {
	std::stringstream ss;
	ss << message;
	send_log(ss, console_level, color);
}

void Log::log(const double message, int console_level, Color color) {
	std::stringstream ss;
	ss << message;
	send_log(ss, console_level, color);
}

void Log::log(const bool message, int console_level, Color color) {
	std::stringstream ss;
	if (message) {
		ss << "true";
	} else {
		ss << "false";
	}
	send_log(ss, console_level, color);
}

void Log::send_log(const std::stringstream& ss, int console_level, const Color& color) {
	const std::string tmp = ss.str();
	const char* tmsg = tmp.c_str();
	if (callbackInstance != nullptr) {
		callbackInstance(tmsg, console_level, (int)color, (int)strlen(tmsg));
	}
}

// Create a callback delegate
void RegisterDebugCallback(FuncCallBack cb) {
	callbackInstance = cb;
}
