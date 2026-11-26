#pragma once

#include <QObject>
#include <QString>

#include <functional>

namespace meikou {

// YouTube、Facebook 等「網頁網址」不能直接播放，要先解析成影片串流網址。
// 解析用開源工具 yt-dlp（Unlicense），YouTube 另外需要 JavaScript 執行環境 Deno（MIT）。
// 兩個工具第一次使用時從官方 GitHub Releases 下載並核對 SHA-256，放在使用者資料夾的 tools 裡。

struct ResolvedMedia {
	QString url;   // 可以直接播放的串流網址
	QString title; // 影片標題
	bool live = false;
};

void SetToolsBaseDir(const QString &configDir);
QString ToolsDir();
QString YtDlpPath();
QString DenoPath();
bool ToolsReady();

// http(s) 網址但不是影片檔或串流清單（.m3u8、.mp4…）→ 需要解析
bool IsPageUrl(const QString &url);

// 同步版本（會等待；給自我檢查與背景執行緒用）
bool InstallToolsSync(QString &error, std::function<void(const QString &)> progress = nullptr);
bool ResolveSync(const QString &pageUrl, ResolvedMedia &out, QString &error, int timeoutMs = 90000);

// 介面用：需要時先問要不要下載工具，然後解析（顯示進度視窗）。成功回傳 true
bool ResolveWithUi(QWidget *parent, const QString &pageUrl, ResolvedMedia &out, QString &error);

// 背景解析（載入專案時，把舊的、已過期的串流網址換新）
void ResolveInBackground(QObject *context, const QString &pageUrl,
			 std::function<void(bool ok, const ResolvedMedia &media, const QString &error)> done);

} // namespace meikou
