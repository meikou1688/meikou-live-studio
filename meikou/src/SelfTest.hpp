#pragma once

#include <QString>

class QApplication;

namespace meikou {

// 自動巡檢：逐項檢查引擎、場景、圖卡、錄影、推流、專案與介面，結果寫成 JSON。回傳失敗項目數。
int RunSelfTest(QApplication &app, const QString &configDir, const QString &reportPath, const QString &shotsDir,
		const QString &rtmpUrl);

} // namespace meikou
