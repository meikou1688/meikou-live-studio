#pragma once

#include <string>

namespace meikou {

// 把 libobs 與本程式的紀錄寫到 %APPDATA%\Meikou Live Studio\logs\，
// 出問題時把最新的紀錄檔傳給開發者即可。
void InitLogging(const std::string &logDir);
void ShutdownLogging();
std::string CurrentLogPath();
// 執行中出現幾次 OBS「即將淘汰」類的提醒（升級 OBS 前要是 0）
int DeprecationWarningCount();

} // namespace meikou
