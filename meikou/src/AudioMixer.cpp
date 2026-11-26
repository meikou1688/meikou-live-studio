#include "AudioMixer.hpp"

#include "Engine.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace meikou {

namespace {

constexpr float kMinDb = -60.0f;

QString Q(const std::string &s)
{
	return QString::fromUtf8(s.c_str());
}

float Norm(float db)
{
	if (!std::isfinite(db))
		return 0.0f;
	return std::clamp((db - kMinDb) / -kMinDb, 0.0f, 1.0f);
}

} // namespace

LevelMeter::LevelMeter(QWidget *parent) : QWidget(parent)
{
	setMinimumHeight(14);
	setMaximumHeight(14);
}

void LevelMeter::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.fillRect(rect(), QColor(14, 16, 22));
	const int h = (height() - 3) / 2;
	auto bar = [&](float db, int y) {
		const int w = (int)(width() * Norm(db));
		const int g = (int)(width() * Norm(-20.0f)), yl = (int)(width() * Norm(-9.0f));
		p.fillRect(0, y, std::min(w, g), h, QColor(52, 199, 89));
		if (w > g)
			p.fillRect(g, y, std::min(w, yl) - g, h, QColor(255, 196, 0));
		if (w > yl)
			p.fillRect(yl, y, w - yl, h, QColor(255, 59, 78));
	};
	bar(l_, 1);
	bar(r_, 2 + h);
}

MixerStrip::MixerStrip(obs_source_t *source, const QString &name, bool followsVideo, QWidget *parent)
	: QWidget(parent),
	  source_(obs_source_get_ref(source))
{
	auto *v = new QVBoxLayout(this);
	v->setContentsMargins(8, 6, 8, 6);
	v->setSpacing(4);

	auto *title = new QLabel(name + (followsVideo ? QStringLiteral("　<span style='color:#8a90a0'>（跟隨畫面）</span>")
						      : QString()));
	title->setTextFormat(Qt::RichText);
	v->addWidget(title);

	levelWidget_ = new LevelMeter;
	v->addWidget(levelWidget_);

	auto *row = new QHBoxLayout;
	auto *slider = new QSlider(Qt::Horizontal);
	slider->setRange(0, 1000);
	auto *db = new QLabel;
	db->setMinimumWidth(56);
	db->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

	auto mkBtn = [](const QString &text, const QString &tip) {
		auto *b = new QPushButton(text);
		b->setCheckable(true);
		b->setToolTip(tip);
		b->setFixedWidth(48);
		b->setObjectName(QStringLiteral("mixBtn"));
		return b;
	};
	auto *mute = mkBtn(QStringLiteral("靜音"), QStringLiteral("不送出聲音（推流、錄影都聽不到）"));
	auto *mon = mkBtn(QStringLiteral("監聽"), QStringLiteral("用這台電腦的耳機聽這一軌"));
	auto *off = mkBtn(QStringLiteral("關閉"), QStringLiteral("完全停用這一軌"));

	row->addWidget(slider, 1);
	row->addWidget(db);
	row->addWidget(mute);
	row->addWidget(mon);
	row->addWidget(off);
	v->addLayout(row);

	fader_ = obs_fader_create(OBS_FADER_LOG);
	obs_fader_attach_source(fader_, source_);
	slider->setValue((int)(obs_fader_get_deflection(fader_) * 1000.0f));
	auto updateDb = [this, db] {
		float d = obs_fader_get_db(fader_);
		db->setText(std::isfinite(d) && d > -96.0f ? QStringLiteral("%1 dB").arg(d, 0, 'f', 1)
							   : QStringLiteral("-∞ dB"));
	};
	updateDb();
	connect(slider, &QSlider::valueChanged, this, [this, updateDb](int v) {
		obs_fader_set_deflection(fader_, (float)v / 1000.0f);
		updateDb();
	});

	mute->setChecked(obs_source_muted(source_));
	mon->setChecked(obs_source_get_monitoring_type(source_) != OBS_MONITORING_TYPE_NONE);
	off->setChecked(!obs_source_enabled(source_));
	connect(mute, &QPushButton::toggled, this, [this](bool on) { obs_source_set_muted(source_, on); });
	connect(mon, &QPushButton::toggled, this, [this](bool on) {
		obs_source_set_monitoring_type(source_, on ? OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT
							   : OBS_MONITORING_TYPE_NONE);
	});
	connect(off, &QPushButton::toggled, this, [this, slider, mute, mon](bool on) {
		obs_source_set_enabled(source_, !on);
		slider->setEnabled(!on);
		mute->setEnabled(!on);
		mon->setEnabled(!on);
	});
	if (off->isChecked()) {
		slider->setEnabled(false);
		mute->setEnabled(false);
		mon->setEnabled(false);
	}

	meter_ = obs_volmeter_create(OBS_FADER_LOG);
	obs_volmeter_attach_source(meter_, source_);
	obs_volmeter_add_callback(meter_, OnLevels, this);
}

MixerStrip::~MixerStrip()
{
	if (meter_) {
		obs_volmeter_remove_callback(meter_, OnLevels, this);
		obs_volmeter_destroy(meter_);
	}
	if (fader_)
		obs_fader_destroy(fader_);
	obs_source_release(source_);
}

void MixerStrip::OnLevels(void *param, const float[MAX_AUDIO_CHANNELS], const float peak[MAX_AUDIO_CHANNELS],
			  const float[MAX_AUDIO_CHANNELS])
{
	auto *self = static_cast<MixerStrip *>(param);
	self->peakL_.store(peak[0]);
	self->peakR_.store(std::isfinite(peak[1]) ? peak[1] : peak[0]);
}

void MixerStrip::Refresh()
{
	// 峰值保持並緩降，看起來比較順
	float l = peakL_.load(), r = peakR_.load();
	if (!std::isfinite(l))
		l = kMinDb;
	if (!std::isfinite(r))
		r = kMinDb;
	holdL_ = std::max(l, holdL_ - 1.5f);
	holdR_ = std::max(r, holdR_ - 1.5f);
	if (!obs_source_enabled(source_) || obs_source_muted(source_))
		holdL_ = holdR_ = kMinDb;
	levelWidget_->SetLevels(holdL_, holdR_);
	levelWidget_->update();
}

AudioMixer::AudioMixer(Engine &engine, QWidget *parent) : QWidget(parent), engine_(engine)
{
	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	auto *scroll = new QScrollArea;
	scroll->setWidgetResizable(true);
	auto *inner = new QWidget;
	list_ = new QVBoxLayout(inner);
	list_->setContentsMargins(4, 4, 4, 4);
	list_->setSpacing(2);
	list_->addStretch(1);
	scroll->setWidget(inner);
	outer->addWidget(scroll);

	timer_ = new QTimer(this);
	connect(timer_, &QTimer::timeout, this, [this] {
		for (auto *s : strips_)
			s->Refresh();
	});
	timer_->start(33);
}

void AudioMixer::Rebuild()
{
	for (auto *s : strips_)
		delete s;
	strips_.clear();

	int pos = 0;
	for (const auto &in : engine_.Inputs()) {
		if (!engine_.HasAudio(in.get()))
			continue;
		auto *strip = new MixerStrip(in->source, Q(in->name), !in->audioOnly);
		list_->insertWidget(pos++, strip);
		strips_.push_back(strip);
	}
}

} // namespace meikou
