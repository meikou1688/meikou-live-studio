#include "MainWindow.hpp"

#include "Log.hpp"
#include "PreviewWidget.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace meikou {

namespace {

struct Platform {
	const char *name;
	const char *server;
};

const Platform kPlatforms[] = {
	{"YouTube", "rtmp://a.rtmp.youtube.com/live2"},
	{"YouTube（RTMPS 加密）", "rtmps://a.rtmps.youtube.com:443/live2"},
	{"Facebook（RTMPS）", "rtmps://live-api-s.facebook.com:443/rtmp/"},
	{"Twitch", "rtmp://live.twitch.tv/app"},
	{"自訂 RTMP／RTMPS", ""},
};

constexpr int kItemRole = Qt::UserRole + 1;

QString Q(const std::string &s)
{
	return QString::fromUtf8(s.c_str());
}

} // namespace

MainWindow::MainWindow(ObsCore &core, QWidget *parent) : QMainWindow(parent), core_(core)
{
	setWindowTitle(QStringLiteral("Meikou Live Studio %1").arg(QStringLiteral(MEIKOU_VERSION)));
	resize(1440, 860);

	auto *splitter = new QSplitter(Qt::Horizontal, this);
	preview_ = new PreviewWidget(splitter);
	splitter->addWidget(preview_);

	auto *side = new QWidget;
	auto *sideLayout = new QVBoxLayout(side);
	sideLayout->addWidget(BuildSourcesPanel());
	sideLayout->addWidget(BuildAudioPanel());
	sideLayout->addWidget(BuildStreamPanel());
	sideLayout->addStretch(1);

	auto *scroll = new QScrollArea;
	scroll->setWidget(side);
	scroll->setWidgetResizable(true);
	scroll->setMinimumWidth(380);
	splitter->addWidget(scroll);
	splitter->setStretchFactor(0, 1);
	splitter->setStretchFactor(1, 0);
	setCentralWidget(splitter);

	statsLabel_ = new QLabel;
	statusBar()->addWidget(statsLabel_, 1);
	auto *logButton = new QPushButton(QStringLiteral("開啟紀錄資料夾"));
	connect(logButton, &QPushButton::clicked, this, [] {
		QFileInfo fi(QString::fromUtf8(CurrentLogPath().c_str()));
		QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
	});
	statusBar()->addPermanentWidget(logButton);

	core_.onStreamEvent = [this](const std::string &kind, const std::string &message) {
		QString k = Q(kind), m = Q(message);
		QMetaObject::invokeMethod(this, [this, k, m] { OnStreamEvent(k, m); }, Qt::QueuedConnection);
	};

	statsTimer_ = new QTimer(this);
	connect(statsTimer_, &QTimer::timeout, this, &MainWindow::UpdateStats);
	statsTimer_->start(1000);

	LoadSettings();
	RefreshCameras();
}

MainWindow::~MainWindow()
{
	core_.onStreamEvent = nullptr;
}

QWidget *MainWindow::BuildSourcesPanel()
{
	auto *box = new QGroupBox(QStringLiteral("訊號源"));
	auto *v = new QVBoxLayout(box);

	auto *camRow = new QHBoxLayout;
	cameraBox_ = new QComboBox;
	cameraBox_->setMinimumWidth(180);
	auto *refresh = new QPushButton(QStringLiteral("重新整理"));
	auto *addCam = new QPushButton(QStringLiteral("加入"));
	camRow->addWidget(cameraBox_, 1);
	camRow->addWidget(refresh);
	camRow->addWidget(addCam);
	v->addWidget(new QLabel(QStringLiteral("攝影機／擷取卡／導播機 USB")));
	v->addLayout(camRow);

	connect(refresh, &QPushButton::clicked, this, &MainWindow::RefreshCameras);
	connect(addCam, &QPushButton::clicked, this, [this] {
		if (cameraBox_->currentIndex() < 0 || cameraBox_->currentData().toString().isEmpty())
			return;
		obs_data_t *s = obs_data_create();
		obs_data_set_string(s, "video_device_id", cameraBox_->currentData().toString().toUtf8().constData());
		obs_data_set_bool(s, "active", true);
		obs_data_set_int(s, "res_type", 0); // 使用裝置預設解析度
		obs_data_set_int(s, "audio_output_mode", 0); // 擷取卡聲音送進混音
		std::string name = cameraBox_->currentText().toStdString();
		obs_sceneitem_t *item = core_.AddSource("dshow_input", name, s);
		obs_data_release(s);
		AddItem(item, cameraBox_->currentText());
	});

	auto *addScreen = new QPushButton(QStringLiteral("加入螢幕擷取（主螢幕）"));
	v->addWidget(addScreen);
	connect(addScreen, &QPushButton::clicked, this, [this] {
		auto monitors = core_.ListProperty("monitor_capture", "monitor_id");
		obs_data_t *s = obs_data_create();
		if (!monitors.empty())
			obs_data_set_string(s, "monitor_id", monitors.front().id.c_str());
		obs_data_set_bool(s, "capture_cursor", true);
		obs_sceneitem_t *item = core_.AddSource("monitor_capture", "螢幕擷取", s);
		obs_data_release(s);
		AddItem(item, QStringLiteral("螢幕擷取"));
	});

	auto *urlRow = new QHBoxLayout;
	urlEdit_ = new QLineEdit;
	urlEdit_->setPlaceholderText(QStringLiteral("rtmp:// srt:// rtsp:// https://…m3u8 或影片網址"));
	auto *addUrl = new QPushButton(QStringLiteral("加入"));
	urlRow->addWidget(urlEdit_, 1);
	urlRow->addWidget(addUrl);
	v->addWidget(new QLabel(QStringLiteral("網路影片（RTMP／SRT／RTSP／HLS）")));
	v->addLayout(urlRow);
	connect(addUrl, &QPushButton::clicked, this, [this] {
		QString url = urlEdit_->text().trimmed();
		if (url.isEmpty())
			return;
		obs_data_t *s = obs_data_create();
		obs_data_set_bool(s, "is_local_file", false);
		obs_data_set_string(s, "input", url.toUtf8().constData());
		obs_data_set_bool(s, "restart_on_activate", true);
		obs_data_set_int(s, "reconnect_delay_sec", 3);
		obs_data_set_int(s, "buffering_mb", 1);
		obs_data_set_bool(s, "hw_decode", true);
		obs_data_set_bool(s, "close_when_inactive", false);
		obs_sceneitem_t *item = core_.AddSource("ffmpeg_source", "網路影片", s);
		obs_data_release(s);
		AddItem(item, QStringLiteral("網路影片：") + url);
	});

	sourceList_ = new QListWidget;
	sourceList_->setMinimumHeight(140);
	v->addWidget(new QLabel(QStringLiteral("播出畫面中的訊號源（勾選＝顯示，最上面＝最上層）")));
	v->addWidget(sourceList_);
	connect(sourceList_, &QListWidget::itemChanged, this, [](QListWidgetItem *it) {
		auto *item = reinterpret_cast<obs_sceneitem_t *>(it->data(kItemRole).value<quintptr>());
		if (item)
			obs_sceneitem_set_visible(item, it->checkState() == Qt::Checked);
	});

	auto *remove = new QPushButton(QStringLiteral("移除選取的訊號源"));
	v->addWidget(remove);
	connect(remove, &QPushButton::clicked, this, &MainWindow::RemoveSelected);
	return box;
}

QWidget *MainWindow::BuildAudioPanel()
{
	auto *box = new QGroupBox(QStringLiteral("音訊"));
	auto *v = new QVBoxLayout(box);

	auto addMute = [this, v](obs_source_t *src, const QString &label) {
		auto *cb = new QCheckBox(label + QStringLiteral("　靜音"));
		if (!src) {
			cb->setEnabled(false);
			cb->setText(label + QStringLiteral("（找不到裝置）"));
		} else {
			cb->setChecked(obs_source_muted(src));
			connect(cb, &QCheckBox::toggled, this, [src](bool on) { obs_source_set_muted(src, on); });
		}
		v->addWidget(cb);
	};
	addMute(core_.DesktopAudio(), QStringLiteral("桌面音訊（電腦播放的聲音）"));
	addMute(core_.MicAudio(), QStringLiteral("麥克風"));
	return box;
}

QWidget *MainWindow::BuildStreamPanel()
{
	auto *box = new QGroupBox(QStringLiteral("推流"));
	auto *form = new QFormLayout(box);

	platformBox_ = new QComboBox;
	for (const auto &p : kPlatforms)
		platformBox_->addItem(QString::fromUtf8(p.name), QString::fromUtf8(p.server));
	serverEdit_ = new QLineEdit;
	keyEdit_ = new QLineEdit;
	keyEdit_->setEchoMode(QLineEdit::Password);
	keyEdit_->setPlaceholderText(QStringLiteral("從平台後台複製串流金鑰"));
	auto *showKey = new QCheckBox(QStringLiteral("顯示金鑰"));
	connect(showKey, &QCheckBox::toggled, this,
		[this](bool on) { keyEdit_->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password); });

	connect(platformBox_, &QComboBox::currentIndexChanged, this, [this](int) {
		QString server = platformBox_->currentData().toString();
		if (!server.isEmpty())
			serverEdit_->setText(server);
	});

	encoderBox_ = new QComboBox;
	for (const auto &e : core_.VideoEncoders())
		encoderBox_->addItem(Q(e.name), Q(e.id));

	bitrateBox_ = new QSpinBox;
	bitrateBox_->setRange(1000, 20000);
	bitrateBox_->setSingleStep(500);
	bitrateBox_->setSuffix(QStringLiteral(" kbps"));
	bitrateBox_->setValue(6000);

	lowLatencyBox_ = new QCheckBox(QStringLiteral("低延遲（互動直播建議開啟）"));
	lowLatencyBox_->setChecked(true);
	dynBitrateBox_ = new QCheckBox(QStringLiteral("網路壅塞時自動降碼率"));

	streamButton_ = new QPushButton(QStringLiteral("開始推流"));
	streamButton_->setMinimumHeight(40);
	streamStatus_ = new QLabel(QStringLiteral("未推流"));
	streamStatus_->setWordWrap(true);
	connect(streamButton_, &QPushButton::clicked, this, &MainWindow::ToggleStream);

	form->addRow(QStringLiteral("平台"), platformBox_);
	form->addRow(QStringLiteral("伺服器"), serverEdit_);
	form->addRow(QStringLiteral("串流金鑰"), keyEdit_);
	form->addRow(QString(), showKey);
	form->addRow(QStringLiteral("編碼器"), encoderBox_);
	form->addRow(QStringLiteral("視訊碼率"), bitrateBox_);
	form->addRow(QString(), lowLatencyBox_);
	form->addRow(QString(), dynBitrateBox_);
	form->addRow(streamButton_);
	form->addRow(streamStatus_);
	return box;
}

void MainWindow::RefreshCameras()
{
	QString current = cameraBox_->currentData().toString();
	cameraBox_->clear();
	for (const auto &c : core_.ListProperty("dshow_input", "video_device_id"))
		cameraBox_->addItem(Q(c.name), Q(c.id));
	if (cameraBox_->count() == 0)
		cameraBox_->addItem(QStringLiteral("（沒有偵測到影像裝置）"), QString());
	int idx = cameraBox_->findData(current);
	if (idx >= 0)
		cameraBox_->setCurrentIndex(idx);
}

void MainWindow::AddItem(obs_sceneitem_t *item, const QString &label)
{
	if (!item) {
		QMessageBox::warning(this, windowTitle(), QStringLiteral("無法加入：") + label);
		return;
	}
	auto *it = new QListWidgetItem(label);
	it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
	it->setCheckState(Qt::Checked);
	it->setData(kItemRole, QVariant::fromValue(reinterpret_cast<quintptr>(item)));
	sourceList_->blockSignals(true);
	sourceList_->insertItem(0, it);
	sourceList_->blockSignals(false);
}

void MainWindow::RemoveSelected()
{
	QListWidgetItem *it = sourceList_->currentItem();
	if (!it)
		return;
	auto *item = reinterpret_cast<obs_sceneitem_t *>(it->data(kItemRole).value<quintptr>());
	core_.RemoveItem(item);
	delete it;
}

void MainWindow::SetStreamUi(bool streaming)
{
	streamButton_->setText(streaming ? QStringLiteral("停止推流") : QStringLiteral("開始推流"));
	streamButton_->setStyleSheet(streaming ? QStringLiteral("background:#7a1d1d;color:white;font-weight:600")
					       : QStringLiteral("background:#1b6b3a;color:white;font-weight:600"));
	for (QWidget *w : {(QWidget *)platformBox_, (QWidget *)serverEdit_, (QWidget *)keyEdit_,
			   (QWidget *)encoderBox_, (QWidget *)bitrateBox_, (QWidget *)lowLatencyBox_,
			   (QWidget *)dynBitrateBox_})
		w->setEnabled(!streaming);
}

void MainWindow::ToggleStream()
{
	if (core_.Streaming()) {
		stopping_ = true;
		streamButton_->setEnabled(false);
		streamStatus_->setText(QStringLiteral("正在停止…"));
		core_.StopStream();
		return;
	}

	StreamConfig cfg;
	cfg.server = serverEdit_->text().trimmed().toStdString();
	cfg.key = keyEdit_->text().trimmed().toStdString();
	cfg.encoderId = encoderBox_->currentData().toString().toStdString();
	cfg.videoKbps = bitrateBox_->value();
	cfg.lowLatency = lowLatencyBox_->isChecked();
	cfg.dynamicBitrate = dynBitrateBox_->isChecked();

	if (cfg.server.empty()) {
		QMessageBox::warning(this, windowTitle(), QStringLiteral("請先填伺服器網址"));
		return;
	}
	if (cfg.key.empty() && platformBox_->currentIndex() != platformBox_->count() - 1) {
		QMessageBox::warning(this, windowTitle(), QStringLiteral("請先貼上串流金鑰"));
		return;
	}
	if (cfg.encoderId.empty()) {
		QMessageBox::warning(this, windowTitle(), QStringLiteral("找不到可用的編碼器"));
		return;
	}

	SaveSettings();
	std::string err;
	if (!core_.StartStream(cfg, err)) {
		streamStatus_->setText(Q(err));
		QMessageBox::warning(this, windowTitle(), Q(err));
		return;
	}
	stopping_ = false;
	SetStreamUi(true);
	streamStatus_->setText(QStringLiteral("連線中…"));
}

void MainWindow::OnStreamEvent(const QString &kind, const QString &message)
{
	if (kind == QLatin1String("stopped")) {
		SetStreamUi(false);
		streamButton_->setEnabled(true);
		streamStatus_->setText(message);
		streamStatus_->setStyleSheet(stopping_ ? QString() : QStringLiteral("color:#e0a030"));
		stopping_ = false;
	} else if (kind == QLatin1String("reconnect")) {
		streamStatus_->setText(message);
		streamStatus_->setStyleSheet(QStringLiteral("color:#e0a030"));
	} else {
		streamStatus_->setText(message);
		streamStatus_->setStyleSheet(QString());
	}
}

void MainWindow::UpdateStats()
{
	Stats s = core_.QueryStats();
	QString text = QStringLiteral("CPU %1%　畫面 %2 fps　渲染延遲掉格 %3／%4")
			       .arg(s.cpu, 0, 'f', 1)
			       .arg(s.renderFps, 0, 'f', 1)
			       .arg(s.laggedFrames)
			       .arg(s.totalFrames);
	if (s.streaming) {
		double dropPct = s.streamFrames > 0 ? 100.0 * s.droppedFrames / s.streamFrames : 0.0;
		text += QStringLiteral("　推流 %1 kbps　網路掉格 %2（%3%）")
				.arg(s.streamKbps, 0, 'f', 0)
				.arg(s.droppedFrames)
				.arg(dropPct, 0, 'f', 1);
		if (!s.reconnecting) {
			QString st = QStringLiteral("推流中 · %1 kbps").arg(s.streamKbps, 0, 'f', 0);
			if (s.congestion > 0.3)
				st += QStringLiteral(" · 網路壅塞，建議調降碼率");
			streamStatus_->setText(st);
			streamStatus_->setStyleSheet(s.congestion > 0.3 ? QStringLiteral("color:#e0a030") : QString());
		}
	}
	statsLabel_->setText(text);
}

void MainWindow::LoadSettings()
{
	QSettings st;
	platformBox_->setCurrentIndex(std::clamp(st.value("stream/platform", 0).toInt(), 0, platformBox_->count() - 1));
	serverEdit_->setText(st.value("stream/server", platformBox_->currentData()).toString());
	keyEdit_->setText(st.value("stream/key").toString());
	int enc = encoderBox_->findData(st.value("stream/encoder").toString());
	if (enc >= 0)
		encoderBox_->setCurrentIndex(enc);
	bitrateBox_->setValue(st.value("stream/kbps", 6000).toInt());
	lowLatencyBox_->setChecked(st.value("stream/lowLatency", true).toBool());
	dynBitrateBox_->setChecked(st.value("stream/dynBitrate", false).toBool());
	SetStreamUi(false);
	restoreGeometry(st.value("window/geometry").toByteArray());
}

void MainWindow::SaveSettings()
{
	QSettings st;
	st.setValue("stream/platform", platformBox_->currentIndex());
	st.setValue("stream/server", serverEdit_->text().trimmed());
	st.setValue("stream/key", keyEdit_->text().trimmed());
	st.setValue("stream/encoder", encoderBox_->currentData().toString());
	st.setValue("stream/kbps", bitrateBox_->value());
	st.setValue("stream/lowLatency", lowLatencyBox_->isChecked());
	st.setValue("stream/dynBitrate", dynBitrateBox_->isChecked());
}

void MainWindow::closeEvent(QCloseEvent *event)
{
	if (core_.Streaming()) {
		auto r = QMessageBox::question(this, windowTitle(), QStringLiteral("目前正在推流，確定要關閉嗎？"));
		if (r != QMessageBox::Yes) {
			event->ignore();
			return;
		}
	}
	SaveSettings();
	QSettings().setValue("window/geometry", saveGeometry());
	statsTimer_->stop();
	preview_->DestroyDisplay();
	event->accept();
}

} // namespace meikou
