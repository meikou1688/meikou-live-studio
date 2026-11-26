#include "Engine.hpp"
#include "GraphicsRender.hpp"
#include "Log.hpp"
#include "MainWindow.hpp"
#include "SelfTest.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QLockFile>
#include <QMessageBox>
#include <QPalette>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleFactory>

#include <util/base.h>

namespace {

const char *kStyle = R"(
QFrame#topBar { background: #12131a; border-bottom: 1px solid #2a2d38; }
QLabel#brand { font-size: 13pt; font-weight: 700; letter-spacing: 2px; color: #ffffff; }
QLabel#stats { color: #8a90a0; }
QLabel#clock { font-size: 13pt; font-weight: 700; color: #e6e8ec; }
QLabel#hint { color: #8a90a0; padding: 0 6px; }
QLabel#pill { padding: 3px 10px; border-radius: 10px; background: #2a2d38; color: #8a90a0; font-weight: 600; }
QLabel#pill[state="live"] { background: #d7263d; color: white; }
QLabel#pill[state="warn"] { background: #e0a100; color: black; }
QLabel#pvwTitle { color: #2ecc71; font-weight: 700; }
QLabel#pgmTitle { color: #ff4d5e; font-weight: 700; }
QFrame#pvwFrame { background: #000; border: 2px solid #2ecc71; border-radius: 4px; }
QFrame#pgmFrame { background: #000; border: 2px solid #ff4d5e; border-radius: 4px; }
QGroupBox { border: 1px solid #2a2d38; border-radius: 6px; margin-top: 14px; padding-top: 6px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; color: #b8bdc9; }
QPushButton { background: #2a2d38; border: 1px solid #3a3e4c; border-radius: 6px; padding: 6px 12px; color: #e6e8ec; }
QPushButton:hover { background: #343846; }
QPushButton:pressed { background: #1f2129; }
QPushButton:disabled { color: #6a6f7c; }
QPushButton#accent { background: #00e5c0; border-color: #00e5c0; color: #06121a; font-weight: 700; }
QPushButton#accent:hover { background: #3ff0d2; }
QPushButton#flat { background: transparent; border: none; color: #8a90a0; }
QPushButton#cut { background: #3a3e4c; font-size: 13pt; font-weight: 800; }
QPushButton#take { background: #d7263d; border-color: #d7263d; color: white; font-size: 13pt; font-weight: 800; }
QPushButton#take:hover { background: #e83a51; }
QPushButton#busButton, QPushButton#ovlButton { font-weight: 600; }
QPushButton#busButton[state="pvw"], QPushButton#ovlButton[state="pvw"] { background: #1e8e4e; border-color: #2ecc71; color: white; }
QPushButton#busButton[state="pgm"], QPushButton#ovlButton[state="pgm"] { background: #c0392b; border-color: #ff4d5e; color: white; }
QPushButton#stream { background: #d7263d; border-color: #d7263d; color: white; font-size: 12pt; font-weight: 700; }
QPushButton#stream[state="on"] { background: #3a3e4c; border-color: #ff4d5e; }
QPushButton#record { background: #3a3e4c; font-size: 12pt; font-weight: 700; }
QPushButton#record[state="on"] { border-color: #ff4d5e; color: #ff4d5e; }
QTabWidget::pane { border: 1px solid #2a2d38; }
QTabBar::tab { background: #1c1e26; padding: 8px 14px; border: 1px solid #2a2d38; border-bottom: none; }
QTabBar::tab:selected { background: #2a2d38; color: white; border-bottom: 2px solid #00e5c0; }
QListWidget, QTableWidget, QLineEdit, QSpinBox, QComboBox { background: #20222b; border: 1px solid #3a3e4c; border-radius: 4px; padding: 3px; }
QHeaderView::section { background: #1c1e26; border: none; padding: 4px; color: #b8bdc9; }
QFrame#targetCard { background: #1c1e26; border: 1px solid #2a2d38; border-radius: 6px; }
QPushButton#targetSummary { text-align: left; background: #20222b; border: 1px dashed #3a3e4c; padding: 6px 10px; color: #b8bdc9; }
QPushButton#targetSummary:hover { border-color: #00e5c0; color: #e6e8ec; }
QLineEdit { padding: 5px; }
)";

void ApplyDarkTheme(QApplication &app)
{
	app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
	QPalette p;
	const QColor bg(24, 26, 32), panel(32, 35, 43), text(230, 232, 236), accent(0, 200, 170);
	p.setColor(QPalette::Window, bg);
	p.setColor(QPalette::WindowText, text);
	p.setColor(QPalette::Base, panel);
	p.setColor(QPalette::AlternateBase, bg);
	p.setColor(QPalette::ToolTipBase, panel);
	p.setColor(QPalette::ToolTipText, text);
	p.setColor(QPalette::Text, text);
	p.setColor(QPalette::Button, panel);
	p.setColor(QPalette::ButtonText, text);
	p.setColor(QPalette::Highlight, accent);
	p.setColor(QPalette::HighlightedText, Qt::white);
	p.setColor(QPalette::Disabled, QPalette::Text, QColor(120, 124, 132));
	p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(120, 124, 132));
	app.setPalette(p);
#if defined(_WIN32)
	app.setFont(QFont(QStringLiteral("Microsoft JhengHei UI"), 10));
#elif defined(__APPLE__)
	app.setFont(QFont(QStringLiteral("PingFang TC"), 13));
#endif
	app.setStyleSheet(QString::fromUtf8(kStyle));
}

struct FpsChoice {
	int num, den;
};
// 與 MainWindow 的幀率選單順序一致：24 / 25 / 29.97 / 30 / 60
const FpsChoice kFps[] = {{24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {60, 1}};

} // namespace

int main(int argc, char *argv[])
{
	QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
	QApplication app(argc, argv);
	QApplication::setOrganizationName(QStringLiteral("Meikou"));
	QApplication::setApplicationName(QStringLiteral("Meikou Live Studio"));
	QApplication::setApplicationVersion(QString::fromUtf8(MEIKOU_VERSION));
	ApplyDarkTheme(app);
	meikou::LoadBrandFonts(); // 內建思源黑體，疊加圖卡用
	QApplication::setWindowIcon(QIcon(QStringLiteral(":/meikou/round.png")));

	// 自動巡檢模式：用獨立的設定與暫存資料夾，不碰使用者的專案
	const QStringList args = QCoreApplication::arguments();
	if (args.contains(QStringLiteral("--selftest"))) {
		auto arg = [&](const QString &name, const QString &def) {
			const int i = args.indexOf(name);
			return i >= 0 && i + 1 < args.size() ? args.at(i + 1) : def;
		};
		QApplication::setApplicationName(QStringLiteral("Meikou Live Studio SelfTest"));
		const QString dir = QDir(QDir::tempPath()).filePath(QStringLiteral("meikou-selftest"));
		QDir(dir).removeRecursively();
		QDir().mkpath(dir);
#ifndef __APPLE__
		QDir::setCurrent(QCoreApplication::applicationDirPath());
#endif
		meikou::InitLogging((dir + QStringLiteral("/logs")).toUtf8().toStdString());
		blog(LOG_INFO, "Meikou Live Studio %s self test", MEIKOU_VERSION);
		const int failed = meikou::RunSelfTest(app, dir, arg(QStringLiteral("--report"), dir + QStringLiteral("/report.json")),
						       arg(QStringLiteral("--shots"), dir + QStringLiteral("/shots")),
						       arg(QStringLiteral("--rtmp"), QString()));
		meikou::ShutdownLogging();
		return failed;
	}

	const QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	QDir().mkpath(configDir);

	// 同一台電腦只開一個，避免兩個程式搶同一張擷取卡
	QLockFile lock(configDir + QStringLiteral("/running.lock"));
	lock.setStaleLockTime(0);
	if (!lock.tryLock(100)) {
		QMessageBox::information(nullptr, QStringLiteral("Meikou Live Studio"),
					 QStringLiteral("Meikou Live Studio 已經在執行中。"));
		return 0;
	}

#ifndef __APPLE__
	// libobs 以目前工作資料夾為基準找 ../../data 與 ../../obs-plugins
	QDir::setCurrent(QCoreApplication::applicationDirPath());
#endif

	meikou::InitLogging((configDir + QStringLiteral("/logs")).toUtf8().toStdString());
	blog(LOG_INFO, "Meikou Live Studio %s starting", MEIKOU_VERSION);

	QSettings settings;
	int fpsIndex = settings.value("output/fpsIndex", 3).toInt();
	if (fpsIndex < 0 || fpsIndex >= (int)(sizeof(kFps) / sizeof(kFps[0])))
		fpsIndex = 3;

	int ret = 0;
	{
		meikou::Engine engine;
		std::string error;
		if (!engine.Startup(configDir.toUtf8().toStdString(), kFps[fpsIndex].num, kFps[fpsIndex].den, error)) {
			blog(LOG_ERROR, "Startup failed: %s", error.c_str());
			QMessageBox::critical(nullptr, QStringLiteral("Meikou Live Studio"),
					      QStringLiteral("無法啟動：%1\n\n紀錄檔：%2")
						      .arg(QString::fromUtf8(error.c_str()),
							   QString::fromUtf8(meikou::CurrentLogPath().c_str())));
			engine.Shutdown();
			meikou::ShutdownLogging();
			return 1;
		}

		// 載入上次的專案；第一次開啟時自動加入系統聲音與麥克風
		const QString projectPath = configDir + QStringLiteral("/project.json");
		QFile pf(projectPath);
		if (pf.open(QIODevice::ReadOnly)) {
			engine.LoadProjectJson(pf.readAll().toStdString());
			pf.close();
		} else {
			engine.CreateDefaultAudio();
		}

		{
			meikou::MainWindow window(engine, projectPath);
			window.show();
			ret = app.exec();
		}
		engine.Shutdown();
	}

	blog(LOG_INFO, "Meikou Live Studio exited normally");
	meikou::ShutdownLogging();
	return ret;
}
