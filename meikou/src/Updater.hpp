#pragma once

#include <QByteArray>
#include <QDialog>
#include <QString>
#include <QStringList>

#include <functional>

class QLabel;
class QProcess;
class QProgressBar;
class QPushButton;
class QTextBrowser;
class QTimer;

namespace meikou {

// 官方發布位置（公開 GitHub 儲存庫的 Releases）
extern const char *kRepoUrl;
extern const char *kReleasesApi;

struct ReleaseInfo {
	QString version;   // 2.0.0-m2.23
	QString name;      // Release 標題
	QString notes;     // Release 說明（Markdown）
	QString page;      // 網頁連結
	QString assetName; // 這個平台的安裝檔
	QString url;       // 下載網址
	QString sha256;    // GitHub 提供的檔案雜湊（核對下載是否完整）
	qint64 size = 0;
};

// 比較版本：2.0.0 > 2.0.0-m2.23 > 2.0.0-m2.9；回傳 >0 表示 a 較新
int CompareVersions(const QString &a, const QString &b);
// 這個平台要下載的安裝檔結尾（Windows：-Windows-x64-Setup.exe；Mac：-macOS-AppleSilicon.pkg）
QString PlatformAssetSuffix();
// 從 GitHub Releases API 的 JSON 找出有這個平台安裝檔的最新版本
bool ParseLatestRelease(const QByteArray &json, const QString &suffix, ReleaseInfo &out, QString &error);

// 用作業系統內建的 curl 連網（Windows 10／11 與 macOS 都有），憑證由系統處理
QString CurlPath();
QStringList ApiCurlArgs();
QStringList DownloadCurlArgs(const QString &url, const QString &path);
bool FetchReleasesSync(QByteArray &json, QString &error, int timeoutMs = 25000);
bool DownloadSync(const QString &url, const QString &path, QString &error, int timeoutMs = 300000);
bool VerifySha256(const QString &path, const QString &expected, QString &error);
QString UpdateDownloadPath(const ReleaseInfo &rel);

// 關閉程式後要執行的安裝檔（main() 在引擎關閉後啟動它）
void SetPendingInstaller(const QString &path);
QString PendingInstaller();
bool LaunchInstaller(const QString &path);

// 更新歷程（內建在程式裡的 CHANGELOG.md）
QString ChangelogText();
void ShowChangelogDialog(QWidget *parent);
void ShowAboutDialog(QWidget *parent);

// 問題報告：把最近的紀錄檔與電腦資訊複製到一個資料夾（預設放在桌面）
QString ExportProblemReport(const QString &configDir, QString &error, const QString &targetParent = QString());

// 檢查更新、下載、核對、安裝
class UpdateDialog : public QDialog {
	Q_OBJECT

public:
	explicit UpdateDialog(QWidget *parent, bool busy);
	~UpdateDialog() override;
	void StartCheck();
	void ShowRelease(const ReleaseInfo &rel); // 已經查到新版（例如啟動時的自動檢查）

signals:
	void installRequested(); // 下載核對完成，請主視窗存檔並關閉程式

private:
	void OnChecked(const QByteArray &json, const QString &error);
	void StartDownload();
	void OnDownloaded(bool ok, const QString &error);

	bool busy_ = false; // 直播或錄影中不能更新
	ReleaseInfo rel_;
	QLabel *title_ = nullptr;
	QTextBrowser *notes_ = nullptr;
	QProgressBar *progress_ = nullptr;
	QPushButton *action_ = nullptr;
	QPushButton *page_ = nullptr;
	QProcess *proc_ = nullptr;
	QTimer *poll_ = nullptr;
	QString downloadPath_;
	enum class Stage { Checking, UpToDate, Available, Downloading, Ready, Failed } stage_ = Stage::Checking;
};

// 背景檢查更新（不跳視窗），查到新版時呼叫 onFound
void CheckForUpdateInBackground(QObject *context, std::function<void(const ReleaseInfo &)> onFound);

} // namespace meikou
