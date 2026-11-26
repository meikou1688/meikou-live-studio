#include "NetMedia.hpp"

#include "Updater.hpp"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <obs.h>

#include <atomic>
#include <memory>

namespace meikou {

namespace {

QString g_base;

const char *kYtDlpBase = "https://github.com/yt-dlp/yt-dlp/releases/latest/download/";
const char *kDenoBase = "https://github.com/denoland/deno/releases/latest/download/";

#if defined(_WIN32)
const char *kYtDlpAsset = "yt-dlp.exe";
const char *kDenoAsset = "deno-x86_64-pc-windows-msvc.zip";
const char *kDenoExe = "deno.exe";
#elif defined(__APPLE__)
const char *kYtDlpAsset = "yt-dlp_macos";
const char *kDenoAsset = "deno-aarch64-apple-darwin.zip";
const char *kDenoExe = "deno";
#else
const char *kYtDlpAsset = "yt-dlp_linux";
const char *kDenoAsset = "deno-x86_64-unknown-linux-gnu.zip";
const char *kDenoExe = "deno";
#endif

// 雜湊檔格式不一（「hash  檔名」或 PowerShell 表格），直接找 64 位十六進位字串
QString FindHash(const QByteArray &text, const QString &fileName)
{
	static const QRegularExpression hex(QStringLiteral("\\b([0-9a-fA-F]{64})\\b"));
	const QStringList lines = QString::fromUtf8(text).split(u'\n');
	if (!fileName.isEmpty()) {
		for (const QString &line : lines) {
			const QString t = line.trimmed();
			if (t.endsWith(QStringLiteral(" ") + fileName) || t.endsWith(QStringLiteral("*") + fileName)) {
				const auto m = hex.match(t);
				if (m.hasMatch())
					return m.captured(1).toLower();
			}
		}
	}
	const auto m = hex.match(QString::fromUtf8(text));
	return m.hasMatch() ? m.captured(1).toLower() : QString();
}

bool FetchText(const QString &url, QByteArray &out, QString &error)
{
	const QString tmp = QDir(ToolsDir()).filePath(QStringLiteral("download.tmp"));
	if (!DownloadSync(url, tmp, error, 60000))
		return false;
	QFile f(tmp);
	if (!f.open(QIODevice::ReadOnly)) {
		error = QStringLiteral("讀不到下載的檔案");
		return false;
	}
	out = f.readAll();
	f.close();
	QFile::remove(tmp);
	return true;
}

bool RunTool(const QString &program, const QStringList &args, int timeoutMs, QByteArray &out, QByteArray &err)
{
	QProcess p;
	p.start(program, args);
	if (!p.waitForStarted(15000)) {
		err = QByteArray("無法執行 ") + QFileInfo(program).fileName().toUtf8();
		return false;
	}
	if (!p.waitForFinished(timeoutMs)) {
		p.kill();
		p.waitForFinished(3000);
		err = QByteArray("逾時（沒有回應）");
		return false;
	}
	out = p.readAllStandardOutput();
	err = p.readAllStandardError();
	return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

// yt-dlp 錯誤訊息只留最後一行 ERROR，翻成看得懂的說明
QString FriendlyError(const QByteArray &err)
{
	QString last;
	for (const QString &line : QString::fromUtf8(err).split(u'\n')) {
		if (line.contains(QStringLiteral("ERROR")))
			last = line.trimmed();
	}
	if (last.isEmpty())
		last = QString::fromUtf8(err).trimmed().split(u'\n').value(0);
	QString hint;
	if (last.contains(QStringLiteral("Sign in to confirm"), Qt::CaseInsensitive) ||
	    last.contains(QStringLiteral("bot"), Qt::CaseInsensitive))
		hint = QStringLiteral("YouTube 要求登入確認（這個網路被判定為機器人）。");
	else if (last.contains(QStringLiteral("Private"), Qt::CaseInsensitive) ||
		 last.contains(QStringLiteral("login"), Qt::CaseInsensitive))
		hint = QStringLiteral("這部影片是私人或需要登入才能看。");
	else if (last.contains(QStringLiteral("Unsupported URL"), Qt::CaseInsensitive))
		hint = QStringLiteral("這個網址不支援。");
	else if (last.contains(QStringLiteral("not available"), Qt::CaseInsensitive) ||
		 last.contains(QStringLiteral("removed"), Qt::CaseInsensitive))
		hint = QStringLiteral("影片不存在或已移除，或在這個地區無法觀看。");
	return hint.isEmpty() ? last : hint + QStringLiteral("\n（") + last + QStringLiteral("）");
}

bool ResolveOnce(const QString &pageUrl, ResolvedMedia &out, QString &error, int timeoutMs)
{
	QStringList args = {QStringLiteral("--ignore-config"),
			    QStringLiteral("--no-playlist"),
			    QStringLiteral("--no-warnings"),
			    QStringLiteral("--no-progress"),
			    QStringLiteral("--cache-dir"),
			    QDir(ToolsDir()).filePath(QStringLiteral("cache")),
			    // 要「影像＋聲音在同一個串流」才能直接播放：優先 HLS（YouTube 直播與大部分影片），最高 1080p
			    QStringLiteral("-f"),
			    QStringLiteral("b[protocol^=m3u8][height<=1080]/b[height<=1080]/b/best"),
			    QStringLiteral("-J")};
	if (QFileInfo::exists(DenoPath()))
		args << QStringLiteral("--js-runtimes") << QStringLiteral("deno:") + DenoPath();
	args << pageUrl;
	QByteArray json, err;
	if (!RunTool(YtDlpPath(), args, timeoutMs, json, err)) {
		error = FriendlyError(err);
		return false;
	}
	const QJsonObject o = QJsonDocument::fromJson(json).object();
	QString url = o.value(QStringLiteral("url")).toString();
	if (url.isEmpty()) {
		error = QStringLiteral("找不到可以直接播放的串流（這個影片的影像和聲音是分開的）");
		return false;
	}
	out.url = url;
	out.title = o.value(QStringLiteral("title")).toString().trimmed();
	out.live = o.value(QStringLiteral("is_live")).toBool();
	return true;
}

} // namespace

void SetToolsBaseDir(const QString &configDir)
{
	g_base = configDir;
}

QString ToolsDir()
{
	const QString dir = QDir(g_base.isEmpty() ? QDir::tempPath() : g_base).filePath(QStringLiteral("tools"));
	QDir().mkpath(dir);
	return dir;
}

QString YtDlpPath()
{
#ifdef _WIN32
	return QDir(ToolsDir()).filePath(QStringLiteral("yt-dlp.exe"));
#else
	return QDir(ToolsDir()).filePath(QStringLiteral("yt-dlp"));
#endif
}

QString DenoPath()
{
	return QDir(ToolsDir()).filePath(QString::fromLatin1(kDenoExe));
}

bool ToolsReady()
{
	return QFileInfo(YtDlpPath()).size() > 1000000 && QFileInfo(DenoPath()).size() > 1000000;
}

bool IsPageUrl(const QString &url)
{
	const QUrl u(url.trimmed());
	const QString scheme = u.scheme().toLower();
	if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
		return false;
	static const QStringList direct = {QStringLiteral(".m3u8"), QStringLiteral(".mpd"), QStringLiteral(".mp4"),
					   QStringLiteral(".m4v"),  QStringLiteral(".mov"), QStringLiteral(".mkv"),
					   QStringLiteral(".webm"), QStringLiteral(".flv"), QStringLiteral(".ts"),
					   QStringLiteral(".mp3"),  QStringLiteral(".aac"), QStringLiteral(".m4a")};
	const QString path = u.path().toLower();
	for (const QString &ext : direct)
		if (path.endsWith(ext))
			return false;
	return true;
}

bool InstallToolsSync(QString &error, std::function<void(const QString &)> progress)
{
	auto say = [&](const QString &t) {
		if (progress)
			progress(t);
	};
	const QString dir = ToolsDir();

	// yt-dlp：下載執行檔，核對官方 SHA2-256SUMS
	if (QFileInfo(YtDlpPath()).size() < 1000000) {
		say(QStringLiteral("下載 yt-dlp（影片網址解析工具）…"));
		QByteArray sums;
		if (!FetchText(QString::fromLatin1(kYtDlpBase) + QStringLiteral("SHA2-256SUMS"), sums, error))
			return false;
		const QString expect = FindHash(sums, QString::fromLatin1(kYtDlpAsset));
		const QString tmp = YtDlpPath() + QStringLiteral(".part");
		if (!DownloadSync(QString::fromLatin1(kYtDlpBase) + QString::fromLatin1(kYtDlpAsset), tmp, error, 600000))
			return false;
		if (expect.isEmpty() || !VerifySha256(tmp, expect, error)) {
			if (error.isEmpty())
				error = QStringLiteral("找不到 yt-dlp 的檔案雜湊，無法確認檔案完整");
			QFile::remove(tmp);
			return false;
		}
		QFile::remove(YtDlpPath());
		QFile::rename(tmp, YtDlpPath());
		QFile::setPermissions(YtDlpPath(), QFile::permissions(YtDlpPath()) | QFile::ExeOwner | QFile::ExeUser);
	}

	// Deno：下載 zip，核對官方 .sha256sum，再解壓縮
	if (QFileInfo(DenoPath()).size() < 1000000) {
		say(QStringLiteral("下載 Deno（YouTube 解析需要的 JavaScript 執行環境）…"));
		QByteArray sum;
		if (!FetchText(QString::fromLatin1(kDenoBase) + QString::fromLatin1(kDenoAsset) + QStringLiteral(".sha256sum"),
			       sum, error))
			return false;
		const QString expect = FindHash(sum, QString::fromLatin1(kDenoAsset));
		const QString zip = QDir(dir).filePath(QString::fromLatin1(kDenoAsset));
		if (!DownloadSync(QString::fromLatin1(kDenoBase) + QString::fromLatin1(kDenoAsset), zip, error, 900000))
			return false;
		if (expect.isEmpty() || !VerifySha256(zip, expect, error)) {
			if (error.isEmpty())
				error = QStringLiteral("找不到 Deno 的檔案雜湊，無法確認檔案完整");
			QFile::remove(zip);
			return false;
		}
		say(QStringLiteral("解壓縮 Deno…"));
		QByteArray out, err;
#ifdef _WIN32
		const QString tar = QFileInfo::exists(QStringLiteral("C:/Windows/System32/tar.exe"))
					    ? QStringLiteral("C:/Windows/System32/tar.exe")
					    : QStringLiteral("tar");
		const bool ok = RunTool(tar, {QStringLiteral("-xf"), QDir::toNativeSeparators(zip), QStringLiteral("-C"),
					      QDir::toNativeSeparators(dir)},
					120000, out, err);
#else
		const bool ok = RunTool(QStringLiteral("/usr/bin/unzip"), {QStringLiteral("-o"), zip, QStringLiteral("-d"), dir},
					120000, out, err);
#endif
		QFile::remove(zip);
		if (!ok || !QFileInfo::exists(DenoPath())) {
			error = QStringLiteral("Deno 解壓縮失敗：%1").arg(QString::fromLocal8Bit(err).trimmed());
			return false;
		}
		QFile::setPermissions(DenoPath(), QFile::permissions(DenoPath()) | QFile::ExeOwner | QFile::ExeUser);
	}
	blog(LOG_INFO, "Media tools ready in %s", dir.toUtf8().constData());
	return true;
}

bool ResolveSync(const QString &pageUrl, ResolvedMedia &out, QString &error, int timeoutMs)
{
	if (!ToolsReady() && !InstallToolsSync(error))
		return false;
	if (ResolveOnce(pageUrl, out, error, timeoutMs))
		return true;
	// 網站常常改版，失敗時先把 yt-dlp 更新到最新版再試一次
	const QString first = error;
	QByteArray o, e;
	blog(LOG_INFO, "Resolve failed, updating yt-dlp and retrying");
	RunTool(YtDlpPath(), {QStringLiteral("-U")}, 180000, o, e);
	if (ResolveOnce(pageUrl, out, error, timeoutMs))
		return true;
	if (error.isEmpty())
		error = first;
	return false;
}

namespace {

// 在背景執行緒做事，畫面不會卡住；回傳時已經完成
template<typename F> void RunInThreadWithDialog(QWidget *parent, const QString &title, F work, std::shared_ptr<QString> status)
{
	QProgressDialog dlg(title, QString(), 0, 0, parent);
	dlg.setWindowTitle(QStringLiteral("網路影片"));
	dlg.setWindowModality(Qt::WindowModal);
	dlg.setMinimumDuration(0);
	dlg.setCancelButton(nullptr);
	dlg.setMinimumWidth(420);
	dlg.show();
	QThread *t = QThread::create(work);
	QEventLoop loop;
	QObject::connect(t, &QThread::finished, &loop, &QEventLoop::quit);
	QTimer poll;
	QObject::connect(&poll, &QTimer::timeout, &dlg, [&] {
		if (status && !status->isEmpty())
			dlg.setLabelText(*status);
	});
	poll.start(250);
	t->start();
	loop.exec();
	t->wait();
	delete t;
}

} // namespace

bool ResolveWithUi(QWidget *parent, const QString &pageUrl, ResolvedMedia &out, QString &error)
{
	if (!ToolsReady()) {
		const auto r = QMessageBox::question(
			parent, QStringLiteral("網路影片"),
			QStringLiteral("播放 YouTube、Facebook 等網頁上的影片，需要先下載兩個免費的開源工具（只要下載一次）：\n\n"
				       "・yt-dlp：把網頁網址解析成影片串流\n"
				       "・Deno：YouTube 解析需要的 JavaScript 執行環境\n\n"
				       "合計約 70 MB，從兩個工具的官方 GitHub 下載並核對檔案完整性。要下載嗎？"));
		if (r != QMessageBox::Yes) {
			error = QStringLiteral("沒有下載解析工具");
			return false;
		}
		auto status = std::make_shared<QString>();
		auto ok = std::make_shared<bool>(false);
		auto err = std::make_shared<QString>();
		RunInThreadWithDialog(
			parent, QStringLiteral("準備下載…"),
			[status, ok, err] { *ok = InstallToolsSync(*err, [status](const QString &t) { *status = t; }); }, status);
		if (!*ok) {
			error = QStringLiteral("下載解析工具失敗：") + *err;
			return false;
		}
	}
	auto ok = std::make_shared<bool>(false);
	auto res = std::make_shared<ResolvedMedia>();
	auto err = std::make_shared<QString>();
	const QString url = pageUrl;
	RunInThreadWithDialog(
		parent, QStringLiteral("正在解析影片網址…（第一次可能要 10～30 秒）"),
		[ok, res, err, url] { *ok = ResolveSync(url, *res, *err); }, nullptr);
	out = *res;
	error = *err;
	return *ok;
}

void ResolveInBackground(QObject *context, const QString &pageUrl,
			 std::function<void(bool ok, const ResolvedMedia &media, const QString &error)> done)
{
	auto ok = std::make_shared<bool>(false);
	auto res = std::make_shared<ResolvedMedia>();
	auto err = std::make_shared<QString>();
	const QString url = pageUrl;
	QThread *t = QThread::create([ok, res, err, url] {
		if (ToolsReady())
			*ok = ResolveSync(url, *res, *err);
		else
			*err = QStringLiteral("還沒有下載解析工具");
	});
	QObject::connect(t, &QThread::finished, context, [t, ok, res, err, done] {
		t->deleteLater();
		done(*ok, *res, *err);
	});
	t->start();
}

} // namespace meikou
