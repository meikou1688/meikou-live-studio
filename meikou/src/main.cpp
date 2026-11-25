#include "Log.hpp"
#include "MainWindow.hpp"
#include "ObsCore.hpp"

#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QPalette>
#include <QStandardPaths>
#include <QStyleFactory>

#include <util/base.h>

#include <windows.h>

namespace {

void ApplyDarkTheme(QApplication &app)
{
	app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
	QPalette p;
	const QColor bg(24, 26, 32), panel(32, 35, 43), text(230, 232, 236), accent(46, 125, 230);
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
	QFont f(QStringLiteral("Microsoft JhengHei UI"), 10);
	app.setFont(f);
}

} // namespace

int main(int argc, char *argv[])
{
	// 同一台電腦只開一個，避免兩個程式搶同一張擷取卡
	HANDLE mutex = CreateMutexW(nullptr, TRUE, L"MeikouLiveStudio.SingleInstance");
	if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
		MessageBoxW(nullptr, L"Meikou Live Studio 已經在執行中。", L"Meikou Live Studio", MB_ICONINFORMATION);
		return 0;
	}

	QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
	QApplication app(argc, argv);
	QApplication::setOrganizationName(QStringLiteral("Meikou"));
	QApplication::setApplicationName(QStringLiteral("Meikou Live Studio"));
	QApplication::setApplicationVersion(QStringLiteral(MEIKOU_VERSION));
	ApplyDarkTheme(app);

	// libobs 以「程式所在資料夾」為基準找 ../../data 與 ../../obs-plugins
	QDir::setCurrent(QCoreApplication::applicationDirPath());

	const QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	QDir().mkpath(configDir);
	meikou::InitLogging((configDir + QStringLiteral("/logs")).toStdString());

	int ret = 0;
	{
		meikou::ObsCore core;
		std::string error;
		if (!core.Startup(configDir.toStdString(), error)) {
			blog(LOG_ERROR, "Startup failed: %s", error.c_str());
			QMessageBox::critical(nullptr, QStringLiteral("Meikou Live Studio"),
					      QStringLiteral("無法啟動：%1\n\n紀錄檔：%2")
						      .arg(QString::fromUtf8(error.c_str()),
							   QString::fromUtf8(meikou::CurrentLogPath().c_str())));
			core.Shutdown();
			meikou::ShutdownLogging();
			return 1;
		}

		{
			meikou::MainWindow window(core);
			window.show();
			ret = app.exec();
		}
		core.Shutdown();
	}

	blog(LOG_INFO, "Meikou Live Studio exited normally");
	meikou::ShutdownLogging();
	if (mutex)
		CloseHandle(mutex);
	return ret;
}
