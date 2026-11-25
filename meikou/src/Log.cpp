#include "Log.hpp"

#include <util/base.h>
#include <util/platform.h>

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace meikou {

namespace {

std::mutex g_mutex;
std::ofstream g_file;
std::string g_path;

const char *LevelName(int level)
{
	switch (level) {
	case LOG_ERROR:
		return "error";
	case LOG_WARNING:
		return "warning";
	case LOG_INFO:
		return "info";
	default:
		return "debug";
	}
}

void LogHandler(int level, const char *format, va_list args, void *)
{
	char message[8192];
	va_list copy;
	va_copy(copy, args);
	vsnprintf(message, sizeof(message), format, copy);
	va_end(copy);

#ifndef _DEBUG
	if (level > LOG_INFO)
		return;
#endif

	SYSTEMTIME st;
	GetLocalTime(&st);
	char line[8400];
	snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [%s] %s\n", st.wHour, st.wMinute, st.wSecond,
		 st.wMilliseconds, LevelName(level), message);

	OutputDebugStringA(line);

	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_file.is_open()) {
		g_file << line;
		g_file.flush();
	}
}

} // namespace

void InitLogging(const std::string &logDir)
{
	std::error_code ec;
	std::filesystem::path dir = std::filesystem::u8path(logDir);
	std::filesystem::create_directories(dir, ec);

	SYSTEMTIME st;
	GetLocalTime(&st);
	char name[64];
	snprintf(name, sizeof(name), "meikou-%04d-%02d-%02d_%02d-%02d-%02d.txt", st.wYear, st.wMonth, st.wDay,
		 st.wHour, st.wMinute, st.wSecond);

	std::filesystem::path file = dir / name;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_file.open(file, std::ios::out | std::ios::trunc);
		g_path = file.u8string();
	}

	// 只保留最近 20 份紀錄
	std::vector<std::filesystem::path> logs;
	for (auto &entry : std::filesystem::directory_iterator(dir, ec)) {
		if (entry.path().extension() == ".txt")
			logs.push_back(entry.path());
	}
	if (logs.size() > 20) {
		std::sort(logs.begin(), logs.end());
		for (size_t i = 0; i + 20 < logs.size(); i++)
			std::filesystem::remove(logs[i], ec);
	}

	base_set_log_handler(LogHandler, nullptr);
}

void ShutdownLogging()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_file.is_open())
		g_file.close();
}

std::string CurrentLogPath()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_path;
}

} // namespace meikou
