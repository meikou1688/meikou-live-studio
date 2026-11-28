#include "PhoneDialog.hpp"
#include "Engine.hpp"
#include "PhoneLink.hpp"
#include "third_party/qrcodegen/qrcodegen.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace meikou {

namespace {

QString Q(const std::string &s)
{
	return QString::fromUtf8(s.c_str());
}

std::string S(const QString &s)
{
	return s.toUtf8().toStdString();
}

constexpr int kQrPixels = 300;

} // namespace

QImage RenderQrCode(const QString &text, int scale)
{
	using qrcodegen::QrCode;
	const QByteArray utf8 = text.toUtf8();
	const QrCode qr = QrCode::encodeText(utf8.constData(), QrCode::Ecc::MEDIUM);
	const int border = 4;
	const int n = qr.getSize() + border * 2;
	QImage img(n * scale, n * scale, QImage::Format_RGB32);
	img.fill(Qt::white);
	QPainter p(&img);
	p.setPen(Qt::NoPen);
	p.setBrush(Qt::black);
	for (int y = 0; y < qr.getSize(); y++)
		for (int x = 0; x < qr.getSize(); x++)
			if (qr.getModule(x, y))
				p.drawRect((x + border) * scale, (y + border) * scale, scale, scale);
	p.end();
	return img;
}

PhoneDialog::PhoneDialog(Engine &engine, PhoneLink &link, std::function<std::string()> addCam,
			 const std::string &focusUid, QWidget *parent)
	: QDialog(parent), engine_(engine), link_(link), addCam_(std::move(addCam)), focus_(focusUid)
{
	setWindowTitle(QStringLiteral("手機攝影機"));
	setMinimumWidth(720);

	auto *root = new QVBoxLayout(this);
	root->setSpacing(12);

	auto *top = new QHBoxLayout;
	top->addWidget(new QLabel(QStringLiteral("訊號源")));
	camBox_ = new QComboBox;
	camBox_->setMinimumWidth(220);
	top->addWidget(camBox_, 1);
	auto *addBtn = new QPushButton(QStringLiteral("＋ 新增手機攝影機"));
	addBtn->setObjectName(QStringLiteral("accent"));
	top->addWidget(addBtn);
	root->addLayout(top);

	auto *body = new QHBoxLayout;
	body->setSpacing(20);
	qr_ = new QLabel;
	qr_->setFixedSize(kQrPixels, kQrPixels);
	qr_->setAlignment(Qt::AlignCenter);
	body->addWidget(qr_, 0, Qt::AlignTop);

	auto *right = new QVBoxLayout;
	right->setSpacing(10);
	status_ = new QLabel;
	status_->setObjectName(QStringLiteral("pill"));
	status_->setTextFormat(Qt::RichText);
	right->addWidget(status_, 0, Qt::AlignLeft);
	info_ = new QLabel;
	info_->setWordWrap(true);
	info_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	right->addWidget(info_);
	auto *steps = new QLabel(QStringLiteral(
		"<b>連線步驟</b><ol style='margin-left:-20px'>"
		"<li>手機和這台電腦連同一個 Wi‑Fi／區網。</li>"
		"<li>手機開啟 <b>Meikou Live Mobile</b>，切到「導播機」模式，按「連線導播機」。</li>"
		"<li>按「掃描 QR Code」掃左邊的圖，或在「搜尋到的導播機」清單點這個攝影機。</li>"
		"</ol>"
		"連上後，這個訊號源被切到<b style='color:#E0263A'>播出</b>時手機會亮紅框，"
		"在<b style='color:#34C27A'>預覽</b>時亮綠框。<br><br>"
		"第一次使用時，Windows 防火牆或 macOS 的「區域網路」權限會詢問是否允許，請按「允許」。"));
	steps->setWordWrap(true);
	steps->setTextFormat(Qt::RichText);
	right->addWidget(steps);
	error_ = new QLabel;
	error_->setWordWrap(true);
	error_->setStyleSheet(QStringLiteral("color:#F2A33A"));
	right->addWidget(error_);
	right->addStretch(1);
	body->addLayout(right, 1);
	root->addLayout(body);

	auto *bottom = new QHBoxLayout;
	bottom->addStretch(1);
	auto *close = new QPushButton(QStringLiteral("關閉"));
	bottom->addWidget(close);
	root->addLayout(bottom);

	connect(close, &QPushButton::clicked, this, &QDialog::accept);
	connect(addBtn, &QPushButton::clicked, this, [this] {
		if (!addCam_)
			return;
		const std::string uid = addCam_();
		if (!uid.empty())
			focus_ = uid;
		Rebuild();
	});
	connect(camBox_, &QComboBox::currentIndexChanged, this, [this] {
		focus_ = CurrentUid();
		RefreshStatus();
	});
	connect(&link_, &PhoneLink::ClientsChanged, this, &PhoneDialog::RefreshStatus);
	timer_ = new QTimer(this);
	timer_->setInterval(500);
	connect(timer_, &QTimer::timeout, this, &PhoneDialog::RefreshStatus);
	timer_->start();

	Rebuild();
}

std::string PhoneDialog::CurrentUid() const
{
	return camBox_ ? S(camBox_->currentData().toString()) : std::string();
}

void PhoneDialog::Rebuild()
{
	const auto cams = PhoneCams(engine_);
	camBox_->blockSignals(true);
	camBox_->clear();
	int select = 0;
	for (size_t i = 0; i < cams.size(); i++) {
		camBox_->addItem(QStringLiteral("%1（SRT 埠 %2）").arg(Q(cams[i].name)).arg(cams[i].port), Q(cams[i].uid));
		if (cams[i].uid == focus_)
			select = (int)i;
	}
	if (!cams.empty())
		camBox_->setCurrentIndex(select);
	camBox_->blockSignals(false);
	camBox_->setEnabled(!cams.empty());
	focus_ = CurrentUid();
	RefreshStatus();
}

void PhoneDialog::RefreshStatus()
{
	const auto cams = PhoneCams(engine_);
	const auto it = std::find_if(cams.begin(), cams.end(), [this](const PhoneCam &c) { return c.uid == focus_; });
	const QStringList hosts = PhoneLink::LocalAddresses();

	if (!link_.Running() || !link_.Error().isEmpty())
		error_->setText(link_.Error().isEmpty() ? QStringLiteral("手機連線服務沒有啟動。") : link_.Error());
	else if (hosts.isEmpty())
		error_->setText(QStringLiteral("這台電腦目前沒有連上區網，手機找不到導播機。"));
	else
		error_->clear();

	if (it == cams.end()) {
		qr_->setPixmap(QPixmap());
		qr_->setText(QStringLiteral("還沒有手機攝影機\n按右上角「＋ 新增手機攝影機」"));
		status_->setText(QStringLiteral("● 尚未建立"));
		info_->clear();
		return;
	}

	const QString qrText = link_.QrText(*it);
	if (qr_->property("qrText").toString() != qrText) {
		const QImage img = RenderQrCode(qrText, 8);
		qr_->setPixmap(QPixmap::fromImage(img.scaled(kQrPixels, kQrPixels, Qt::KeepAspectRatio, Qt::FastTransformation)));
		qr_->setProperty("qrText", qrText);
	}

	const QString device = link_.DeviceFor(it->uid);
	if (device.isEmpty()) {
		status_->setText(QStringLiteral("<span style='color:#9BA1AC'>●</span> 等待手機連線"));
	} else {
		const Tally t = TallyOf(engine_, it->uid);
		const QString tally = t == Tally::Program   ? QStringLiteral("<b style='color:#E0263A'>播出中</b>")
				      : t == Tally::Preview ? QStringLiteral("<b style='color:#34C27A'>預覽中</b>")
							    : QStringLiteral("待命");
		status_->setText(QStringLiteral("<span style='color:#34C27A'>●</span> 已連線：%1 · %2")
					 .arg(device.toHtmlEscaped(), tally));
	}
	QStringList srt;
	for (const QString &h : hosts.mid(0, 3))
		srt << QStringLiteral("srt://%1:%2").arg(h).arg(it->port);
	info_->setText(QStringLiteral("導播機位址：%1\n手動輸入時填：%2")
			       .arg(hosts.isEmpty() ? QStringLiteral("（沒有區網）") : hosts.mid(0, 3).join(QStringLiteral("、")),
				    srt.isEmpty() ? QStringLiteral("—") : srt.first()));
}

} // namespace meikou
