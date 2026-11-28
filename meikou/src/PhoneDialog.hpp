#pragma once

#include <QDialog>
#include <QImage>

#include <functional>
#include <string>

class QComboBox;
class QLabel;
class QTimer;

namespace meikou {

class Engine;
class PhoneLink;

// 把文字畫成 QR Code（白底黑點、四邊留白），每一格 scale 像素
QImage RenderQrCode(const QString &text, int scale);

// 「手機攝影機」視窗：QR Code、連線狀態、Tally、操作說明
class PhoneDialog : public QDialog {
	Q_OBJECT

public:
	// addCam：新增一個手機攝影機訊號源，回傳它的 uid（失敗回傳空字串）
	PhoneDialog(Engine &engine, PhoneLink &link, std::function<std::string()> addCam, const std::string &focusUid,
		    QWidget *parent = nullptr);

private:
	void Rebuild();
	void RefreshStatus();
	std::string CurrentUid() const;

	Engine &engine_;
	PhoneLink &link_;
	std::function<std::string()> addCam_;
	std::string focus_;
	QComboBox *camBox_ = nullptr;
	QLabel *qr_ = nullptr;
	QLabel *status_ = nullptr;
	QLabel *info_ = nullptr;
	QLabel *error_ = nullptr;
	QTimer *timer_ = nullptr;
};

} // namespace meikou
