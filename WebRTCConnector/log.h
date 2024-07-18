#pragma once

#include <stdio.h>
#include <string>
#include <stdio.h>
#include <sstream>
#include "framework.h"

extern "C"
{
	typedef void(*FuncCallBack)(const char* message, int console_level, int color, int size);
	static FuncCallBack callbackInstance = nullptr;
	DLLExport void RegisterDebugCallback(FuncCallBack cb);
}

enum class Color { Red, Green, Blue, Black, White, Yellow, Orange };

class Log {

public:
	static void log(const char* message, int console_level, Color color = Color::Black);
	static void log(const std::string message, int console_level, Color color = Color::Black);
	static void log(const int message, int console_level, Color color = Color::Black);
	static void log(const char message, int console_level, Color color = Color::Black);
	static void log(const float message, int console_level, Color color = Color::Black);
	static void log(const double message, int console_level, Color color = Color::Black);
	static void log(const bool message, int console_level, Color color = Color::Black);

private:
	static void send_log(const std::stringstream& ss, int console_level, const Color& color);
};
