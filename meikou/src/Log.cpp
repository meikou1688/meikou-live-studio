#include <cstring>
#include <atomic>
#include "Log.hpp"

#include <util/base.h>
#include <util/platform.h>

#ifdef _WIN32
#include <windows.h>
#endif
#include <chrono>

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

struct LocalTime {
	int year, month, day, hour, minute, second, ms;
};

LocalTime Now()
{
	using namespace std::chrono;
	const auto now = system_clock::now();
	const std::time_t t = system_clock::to_time_t(now);
	std::tm tm{};
#ifdef _WIN32
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	const int ms = (int)(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
	return {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms};
}

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

std::atomic<int> g_deprecationWarnings{0};

void LogHandler(int level, const char *format, va_list args, void *)
{
	char message[8192];
	va_list copy;
	va_copy(copy, args);
	vsnprintf(message, sizeof(message), format, copy);
	// OBS 提醒「即將淘汰」或「場景沒指定畫布」：自我檢查會確認這個數字是 0
	if (std::strstr(message, " is deprecated") || std::strstr(message, "without specifying a canvas"))
		g_deprecationWarnings.fetch_add(1);
	va_end(copy);

#ifndef _DEBUG
	if (level > LOG_INFO)
		return;
#endif

	const LocalTime st = Now();
	char line[8400];
	snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [%s] %s\n", st.hour, st.minute, st.second, st.ms,
		 LevelName(level), message);

#ifdef _WIN32
	OutputDebugStringA(line);
#else
	fputs(line, stderr);
#endif

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

	const LocalTime st = Now();
	char name[64];
	snprintf(name, sizeof(name), "meikou-%04d-%02d-%02d_%02d-%02d-%02d.txt", st.year, st.month, st.day,
		 st.hour, st.minute, st.second);

	std::filesystem::path file = dir / name;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_file.open(file, std::ios::out | std::ios::trunc);
		const auto u8 = file.u8string();
		g_path.assign(reinterpret_cast<const char *>(u8.data()), u8.size());
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

int DeprecationWarningCount()
{
	return g_deprecationWarnings.load();
}

std::string CurrentLogPath()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_path;
}

} // namespace meikou
