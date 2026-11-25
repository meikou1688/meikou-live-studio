#pragma once

#include <string>

namespace meikou {

// 把 libobs 與本程式的紀錄寫到 %APPDATA%\Meikou Live Studio\logs\，
// 出問題時把最新的紀錄檔傳給開發者即可。
void InitLogging(const std::string &logDir);
void ShutdownLogging();
std::string CurrentLogPath();

} // namespace meikou
