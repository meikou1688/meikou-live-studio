#include "Updater.hpp"

#include "Log.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTextBrowser>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <obs.h>

#include <algorithm>

namespace meikou {

const char *kRepoUrl = "https://github.com/meikou1688/meikou-live-studio";
const char *kReleasesApi = "https://api.github.com/repos/meikou1688/meikou-live-studio/releases?per_page=50";

namespace {

QString g_pendingInstaller;

QString Ver()
{
	return QString::fromUtf8(MEIKOU_VERSION);
}

// "2.0.0-m2.23" → base {2,0,0}、pre {2,23}；沒有 -m… 的是正式版
void SplitVersion(const QString &v, QList<int> &base, QList<int> &pre, bool &hasPre)
{
	QString s = v.trimmed();
	if (s.startsWith(u'v') || s.startsWith(u'V'))
		s.remove(0, 1);
	const int dash = s.indexOf(u'-');
	const QString b = dash >= 0 ? s.left(dash) : s, p = dash >= 0 ? s.mid(dash + 1) : QString();
	static const QRegularExpression num(QStringLiteral("\\d+"));
	for (auto it = num.globalMatch(b); it.hasNext();)
		base << it.next().captured().toInt();
	for (auto it = num.globalMatch(p); it.hasNext();)
		pre << it.next().captured().toInt();
	hasPre = !p.isEmpty();
}

int CompareLists(const QList<int> &a, const QList<int> &b)
{
	for (int i = 0; i < std::max(a.size(), b.size()); i++) {
		const int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
		if (x != y)
			return x < y ? -1 : 1;
	}
	return 0;
}

QString Human(qint64 bytes)
{
	return QStringLiteral("%1 MB").arg(bytes / 1048576.0, 0, 'f', 1);
}

} // namespace

int CompareVersions(const QString &a, const QString &b)
{
	QList<int> ab, ap, bb, bp;
	bool aPre = false, bPre = false;
	SplitVersion(a, ab, ap, aPre);
	SplitVersion(b, bb, bp, bPre);
	if (int c = CompareLists(ab, bb))
		return c;
	if (aPre != bPre)
		return aPre ? -1 : 1; // 同一版號：正式版比測試版新
	return CompareLists(ap, bp);
}

QString PlatformAssetSuffix()
{
#if defined(_WIN32)
	return QStringLiteral("-Windows-x64-Setup.exe");
#elif defined(__APPLE__)
	return QStringLiteral("-macOS-AppleSilicon.pkg");
#else
	return QString();
#endif
}

bool ParseLatestRelease(const QByteArray &json, const QString &suffix, ReleaseInfo &out, QString &error)
{
	QJsonParseError pe;
	const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
	if (pe.error != QJsonParseError::NoError || !doc.isArray()) {
		error = QStringLiteral("伺服器回傳的資料看不懂");
		return false;
	}
	bool found = false;
	for (const auto &v : doc.array()) {
		const QJsonObject r = v.toObject();
		if (r.value(QStringLiteral("draft")).toBool())
			continue;
		QString tag = r.value(QStringLiteral("tag_name")).toString();
		const QString version = tag.startsWith(u'v') ? tag.mid(1) : tag;
		for (const auto &av : r.value(QStringLiteral("assets")).toArray()) {
			const QJsonObject a = av.toObject();
			const QString name = a.value(QStringLiteral("name")).toString();
			if (suffix.isEmpty() || !name.endsWith(suffix))
				continue;
			if (found && CompareVersions(version, out.version) <= 0)
				continue;
			out.version = version;
			out.name = r.value(QStringLiteral("name")).toString();
			out.notes = r.value(QStringLiteral("body")).toString();
			out.page = r.value(QStringLiteral("html_url")).toString();
			out.assetName = name;
			out.url = a.value(QStringLiteral("browser_download_url")).toString();
			out.size = (qint64)a.value(QStringLiteral("size")).toDouble();
			QString digest = a.value(QStringLiteral("digest")).toString();
			out.sha256 = digest.startsWith(QStringLiteral("sha256:")) ? digest.mid(7).toLower() : QString();
			found = true;
		}
	}
	if (!found)
		error = QStringLiteral("找不到這個平台的安裝檔");
	return found;
}

QString CurlPath()
{
	QString p = QStandardPaths::findExecutable(QStringLiteral("curl"));
#ifdef _WIN32
	if (p.isEmpty() && QFileInfo::exists(QStringLiteral("C:/Windows/System32/curl.exe")))
		p = QStringLiteral("C:/Windows/System32/curl.exe");
#else
	if (p.isEmpty() && QFileInfo::exists(QStringLiteral("/usr/bin/curl")))
		p = QStringLiteral("/usr/bin/curl");
#endif
	return p;
}

QStringList ApiCurlArgs()
{
	return {QStringLiteral("-sS"),
		QStringLiteral("-L"),
		QStringLiteral("--fail"),
		QStringLiteral("--max-time"),
		QStringLiteral("25"),
		QStringLiteral("-H"),
		QStringLiteral("Accept: application/vnd.github+json"),
		QStringLiteral("-A"),
		QStringLiteral("MeikouLiveStudio/") + Ver(),
		QString::fromLatin1(kReleasesApi)};
}

QStringList DownloadCurlArgs(const QString &url, const QString &path)
{
	return {QStringLiteral("-sS"),        QStringLiteral("-L"),         QStringLiteral("--fail"),
		QStringLiteral("--retry"),    QStringLiteral("3"),          QStringLiteral("--connect-timeout"),
		QStringLiteral("20"),         QStringLiteral("-A"),         QStringLiteral("MeikouLiveStudio/") + Ver(),
		QStringLiteral("-o"),         QDir::toNativeSeparators(path), url};
}

bool FetchReleasesSync(QByteArray &json, QString &error, int timeoutMs)
{
	const QString curl = CurlPath();
	if (curl.isEmpty()) {
		error = QStringLiteral("這台電腦找不到 curl，無法連網檢查");
		return false;
	}
	QProcess p;
	p.start(curl, ApiCurlArgs());
	if (!p.waitForFinished(timeoutMs) || p.exitCode() != 0) {
		error = QStringLiteral("連不上更新伺服器：%1").arg(QString::fromLocal8Bit(p.readAllStandardError()).trimmed());
		p.kill();
		return false;
	}
	json = p.readAllStandardOutput();
	return true;
}

bool DownloadSync(const QString &url, const QString &path, QString &error, int timeoutMs)
{
	const QString curl = CurlPath();
	if (curl.isEmpty()) {
		error = QStringLiteral("這台電腦找不到 curl");
		return false;
	}
	QDir().mkpath(QFileInfo(path).absolutePath());
	QFile::remove(path);
	QProcess p;
	p.start(curl, DownloadCurlArgs(url, path));
	if (!p.waitForFinished(timeoutMs) || p.exitCode() != 0) {
		error = QStringLiteral("下載失敗：%1").arg(QString::fromLocal8Bit(p.readAllStandardError()).trimmed());
		p.kill();
		return false;
	}
	return true;
}

bool VerifySha256(const QString &path, const QString &expected, QString &error)
{
	if (expected.isEmpty())
		return true; // 舊的 Release 沒有雜湊資料就只看檔案大小
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		error = QStringLiteral("讀不到下載的檔案");
		return false;
	}
	QCryptographicHash h(QCryptographicHash::Sha256);
	h.addData(&f);
	const QString got = QString::fromLatin1(h.result().toHex());
	if (got != expected.toLower()) {
		error = QStringLiteral("檔案核對不符（可能下載不完整），請再試一次");
		return false;
	}
	return true;
}

QString UpdateDownloadPath(const ReleaseInfo &rel)
{
	const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
				    .filePath(QStringLiteral("MeikouLiveStudioUpdate"));
	return QDir(dir).filePath(rel.assetName);
}

void SetPendingInstaller(const QString &path)
{
	g_pendingInstaller = path;
}

QString PendingInstaller()
{
	return g_pendingInstaller;
}

bool LaunchInstaller(const QString &path)
{
	if (path.isEmpty() || !QFileInfo::exists(path))
		return false;
	blog(LOG_INFO, "Launching installer %s", QFileInfo(path).fileName().toUtf8().constData());
#ifdef __APPLE__
	// 用系統「安裝程式」打開 .pkg
	return QProcess::startDetached(QStringLiteral("/usr/bin/open"), {path});
#else
	// 一鍵安裝：裝好會自動重新開啟程式
	return QProcess::startDetached(path, {});
#endif
}

QString ChangelogText()
{
	QFile f(QStringLiteral(":/meikou/CHANGELOG.md"));
	if (!f.open(QIODevice::ReadOnly))
		return QString();
	return QString::fromUtf8(f.readAll());
}

void ShowChangelogDialog(QWidget *parent)
{
	QDialog dlg(parent);
	dlg.setWindowTitle(QStringLiteral("更新歷程"));
	dlg.resize(720, 640);
	auto *v = new QVBoxLayout(&dlg);
	auto *tb = new QTextBrowser;
	tb->setOpenExternalLinks(true);
	tb->setMarkdown(ChangelogText());
	v->addWidget(tb, 1);
	auto *row = new QHBoxLayout;
	auto *web = new QPushButton(QStringLiteral("在網頁上看所有版本"));
	auto *close = new QPushButton(QStringLiteral("關閉"));
	QObject::connect(web, &QPushButton::clicked, &dlg,
			 [] { QDesktopServices::openUrl(QUrl(QString::fromLatin1(kRepoUrl) + QStringLiteral("/releases"))); });
	QObject::connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);
	row->addWidget(web);
	row->addStretch(1);
	row->addWidget(close);
	v->addLayout(row);
	dlg.exec();
}

void ShowAboutDialog(QWidget *parent)
{
	QMessageBox box(parent);
	box.setWindowTitle(QStringLiteral("關於 Meikou Live Studio"));
	box.setIconPixmap(QPixmap(QStringLiteral(":/meikou/round.png"))
				  .scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	box.setTextFormat(Qt::RichText);
	box.setText(QStringLiteral("<h3>Meikou Live Studio</h3>"
				   "<p>版本 %1（測試版）<br>OBS 核心 %2<br>%3 %4</p>"
				   "<p>免費、開放原始碼（GPL-2.0-or-later）。本程式以 OBS Studio 的 libobs 為核心，"
				   "不是 OBS 官方產品。</p>"
				   "<p><a href=\"%5\">%5</a></p>")
			    .arg(Ver(), QString::fromUtf8(obs_get_version_string()), QSysInfo::prettyProductName(),
				 QSysInfo::currentCpuArchitecture(), QString::fromLatin1(kRepoUrl)));
	box.exec();
}

QString ExportProblemReport(const QString &configDir, QString &error, const QString &targetParent)
{
	QString parent = targetParent;
	if (parent.isEmpty())
		parent = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
	if (parent.isEmpty())
		parent = QDir::homePath();
	const QString dir = QDir(parent).filePath(QStringLiteral("Meikou 問題報告 %1")
							  .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
	if (!QDir().mkpath(dir)) {
		error = QStringLiteral("無法建立資料夾：%1").arg(dir);
		return QString();
	}
	// 最近 3 份紀錄（包含上次當機前的紀錄）；紀錄裡的串流金鑰本來就遮蔽了，專案檔有金鑰所以不附
	QDir logs(QDir(configDir).filePath(QStringLiteral("logs")));
	const QFileInfoList files =
		logs.entryInfoList({QStringLiteral("*.txt")}, QDir::Files, QDir::Time); // 新的在前
	int copied = 0;
	for (const QFileInfo &fi : files) {
		if (copied >= 3)
			break;
		if (QFile::copy(fi.absoluteFilePath(), QDir(dir).filePath(fi.fileName())))
			copied++;
	}
	QFile info(QDir(dir).filePath(QStringLiteral("系統資訊.txt")));
	if (info.open(QIODevice::WriteOnly | QIODevice::Text)) {
		QString t;
		t += QStringLiteral("Meikou Live Studio %1\n").arg(Ver());
		t += QStringLiteral("OBS 核心 %1\n").arg(QString::fromUtf8(obs_get_version_string()));
		t += QStringLiteral("作業系統 %1（%2）\n").arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion());
		t += QStringLiteral("CPU 架構 %1\n").arg(QSysInfo::currentCpuArchitecture());
		t += QStringLiteral("Qt %1\n").arg(QString::fromLatin1(qVersion()));
		t += QStringLiteral("匯出時間 %1\n").arg(QDateTime::currentDateTime().toString(Qt::ISODate));
		t += QStringLiteral("附上的紀錄檔 %1 份\n\n").arg(copied);
		t += QStringLiteral("請描述發生了什麼事（例如：直播約 40 分鐘時切換場景後畫面卡住）：\n\n");
		info.write(t.toUtf8());
	}
	blog(LOG_INFO, "Problem report exported (%d log files)", copied);
	return dir;
}

// ------------------------------------------------------------ 更新視窗

UpdateDialog::UpdateDialog(QWidget *parent, bool busy) : QDialog(parent), busy_(busy)
{
	setWindowTitle(QStringLiteral("檢查更新"));
	resize(640, 520);
	auto *v = new QVBoxLayout(this);
	title_ = new QLabel;
	title_->setWordWrap(true);
	QFont f = title_->font();
	f.setPointSizeF(f.pointSizeF() * 1.25);
	f.setBold(true);
	title_->setFont(f);
	v->addWidget(title_);
	auto *cur = new QLabel(QStringLiteral("目前版本：%1").arg(Ver()));
	cur->setObjectName(QStringLiteral("hint"));
	v->addWidget(cur);
	notes_ = new QTextBrowser;
	notes_->setOpenExternalLinks(true);
	v->addWidget(notes_, 1);
	progress_ = new QProgressBar;
	progress_->setVisible(false);
	v->addWidget(progress_);
	auto *row = new QHBoxLayout;
	page_ = new QPushButton(QStringLiteral("在網頁上查看"));
	page_->setVisible(false);
	connect(page_, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl(rel_.page)); });
	action_ = new QPushButton;
	action_->setObjectName(QStringLiteral("accent"));
	action_->setVisible(false);
	auto *close = new QPushButton(QStringLiteral("關閉"));
	connect(close, &QPushButton::clicked, this, &QDialog::reject);
	row->addWidget(page_);
	row->addStretch(1);
	row->addWidget(action_);
	row->addWidget(close);
	v->addLayout(row);
	connect(action_, &QPushButton::clicked, this, [this] {
		if (stage_ == Stage::Available || stage_ == Stage::Failed)
			StartDownload();
		else if (stage_ == Stage::Ready)
			emit installRequested();
	});
	poll_ = new QTimer(this);
	poll_->setInterval(300);
	connect(poll_, &QTimer::timeout, this, [this] {
		const qint64 got = QFileInfo(downloadPath_).size();
		if (rel_.size > 0) {
			progress_->setValue((int)std::min<qint64>(1000, got * 1000 / rel_.size));
			progress_->setFormat(QStringLiteral("%1 / %2").arg(Human(got), Human(rel_.size)));
		}
	});
}

UpdateDialog::~UpdateDialog()
{
	if (proc_ && proc_->state() != QProcess::NotRunning) {
		proc_->kill();
		proc_->waitForFinished(2000);
	}
}

void UpdateDialog::StartCheck()
{
	stage_ = Stage::Checking;
	title_->setText(QStringLiteral("正在檢查有沒有新版本…"));
	notes_->setMarkdown(QString());
	action_->setVisible(false);
	const QString curl = CurlPath();
	if (curl.isEmpty()) {
		OnChecked({}, QStringLiteral("這台電腦找不到 curl，無法連網檢查"));
		return;
	}
	auto *p = new QProcess(this);
	connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus) {
		const QByteArray out = p->readAllStandardOutput();
		const QString err = QString::fromLocal8Bit(p->readAllStandardError()).trimmed();
		p->deleteLater();
		OnChecked(code == 0 ? out : QByteArray(), code == 0 ? QString() : QStringLiteral("連不上更新伺服器：%1").arg(err));
	});
	p->start(curl, ApiCurlArgs());
}

void UpdateDialog::OnChecked(const QByteArray &json, const QString &error)
{
	if (!error.isEmpty()) {
		stage_ = Stage::Failed;
		title_->setText(QStringLiteral("無法檢查更新"));
		notes_->setPlainText(error + QStringLiteral("\n\n請確認網路連線，或到網頁上下載：\n") + QString::fromLatin1(kRepoUrl) +
				     QStringLiteral("/releases"));
		action_->setVisible(false);
		return;
	}
	ReleaseInfo rel;
	QString err;
	if (!ParseLatestRelease(json, PlatformAssetSuffix(), rel, err)) {
		OnChecked({}, err);
		return;
	}
	if (CompareVersions(rel.version, Ver()) <= 0) {
		stage_ = Stage::UpToDate;
		rel_ = rel;
		title_->setText(QStringLiteral("已經是最新版本 ✓"));
		notes_->setMarkdown(QStringLiteral("線上最新版本：**%1**\n\n").arg(rel.version) + rel.notes);
		page_->setVisible(!rel.page.isEmpty());
		return;
	}
	ShowRelease(rel);
}

void UpdateDialog::ShowRelease(const ReleaseInfo &rel)
{
	rel_ = rel;
	stage_ = Stage::Available;
	title_->setText(QStringLiteral("有新版本：%1").arg(rel.version));
	notes_->setMarkdown(rel.notes);
	page_->setVisible(!rel.page.isEmpty());
	action_->setText(QStringLiteral("下載並安裝（%1）").arg(Human(rel.size)));
	action_->setVisible(true);
	action_->setEnabled(!busy_);
	if (busy_)
		action_->setToolTip(QStringLiteral("直播或錄影中不能更新，結束後再按"));
}

void UpdateDialog::StartDownload()
{
	const QString curl = CurlPath();
	if (curl.isEmpty() || rel_.url.isEmpty()) {
		OnDownloaded(false, QStringLiteral("這台電腦找不到 curl"));
		return;
	}
	stage_ = Stage::Downloading;
	downloadPath_ = UpdateDownloadPath(rel_);
	QDir().mkpath(QFileInfo(downloadPath_).absolutePath());
	QFile::remove(downloadPath_);
	title_->setText(QStringLiteral("正在下載 %1…").arg(rel_.version));
	action_->setEnabled(false);
	progress_->setVisible(true);
	progress_->setRange(0, 1000);
	progress_->setValue(0);
	poll_->start();
	proc_ = new QProcess(this);
	connect(proc_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
		poll_->stop();
		const QString err = QString::fromLocal8Bit(proc_->readAllStandardError()).trimmed();
		proc_->deleteLater();
		proc_ = nullptr;
		if (code != 0) {
			OnDownloaded(false, QStringLiteral("下載失敗：%1").arg(err));
			return;
		}
		QString verr;
		if (rel_.size > 0 && QFileInfo(downloadPath_).size() != rel_.size)
			OnDownloaded(false, QStringLiteral("檔案大小不符（可能下載不完整），請再試一次"));
		else if (!VerifySha256(downloadPath_, rel_.sha256, verr))
			OnDownloaded(false, verr);
		else
			OnDownloaded(true, QString());
	});
	blog(LOG_INFO, "Downloading update %s", rel_.version.toUtf8().constData());
	proc_->start(curl, DownloadCurlArgs(rel_.url, downloadPath_));
}

void UpdateDialog::OnDownloaded(bool ok, const QString &error)
{
	if (!ok) {
		stage_ = Stage::Failed;
		blog(LOG_WARNING, "Update download failed: %s", error.toUtf8().constData());
		QFile::remove(downloadPath_);
		title_->setText(QStringLiteral("下載沒有成功"));
		notes_->setPlainText(error);
		progress_->setVisible(false);
		action_->setText(QStringLiteral("再試一次"));
		action_->setEnabled(true);
		return;
	}
	stage_ = Stage::Ready;
	progress_->setValue(1000);
	progress_->setFormat(QStringLiteral("下載完成，檔案核對無誤"));
	title_->setText(QStringLiteral("%1 已下載完成").arg(rel_.version));
#ifdef __APPLE__
	notes_->setMarkdown(QStringLiteral("按「關閉程式並安裝」後，程式會先存檔再關閉，接著打開 macOS 安裝程式，"
					   "照畫面按「繼續」完成即可，裝好後從「應用程式」重新開啟。\n\n") +
			    rel_.notes);
#else
	notes_->setMarkdown(QStringLiteral("按「關閉程式並安裝」後，程式會先存檔再關閉，接著自動安裝新版，"
					   "裝好會自動重新開啟。專案、場景、推流設定都會保留。\n\n") +
			    rel_.notes);
#endif
	SetPendingInstaller(downloadPath_);
	action_->setText(QStringLiteral("關閉程式並安裝"));
	action_->setEnabled(!busy_);
}

void CheckForUpdateInBackground(QObject *context, std::function<void(const ReleaseInfo &)> onFound)
{
	const QString curl = CurlPath();
	if (curl.isEmpty())
		return;
	auto *p = new QProcess(context);
	QObject::connect(p, &QProcess::finished, context, [p, onFound](int code, QProcess::ExitStatus) {
		const QByteArray out = p->readAllStandardOutput();
		p->deleteLater();
		if (code != 0)
			return;
		ReleaseInfo rel;
		QString err;
		if (ParseLatestRelease(out, PlatformAssetSuffix(), rel, err) &&
		    CompareVersions(rel.version, QString::fromUtf8(MEIKOU_VERSION)) > 0) {
			blog(LOG_INFO, "Update available: %s", rel.version.toUtf8().constData());
			onFound(rel);
		}
	});
	p->start(curl, ApiCurlArgs());
}

} // namespace meikou
