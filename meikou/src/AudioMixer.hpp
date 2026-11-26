#pragma once

#include <QWidget>

#include <obs.h>

#include <atomic>
#include <string>
#include <vector>

class QVBoxLayout;
class QTimer;

namespace meikou {

class Engine;

// 一條音量表（左右聲道），數值由 libobs 的音訊執行緒寫入，畫面計時器讀取。
class LevelMeter : public QWidget {
	Q_OBJECT
public:
	explicit LevelMeter(QWidget *parent = nullptr);
	void SetLevels(float l, float r) { l_ = l; r_ = r; }

protected:
	void paintEvent(QPaintEvent *) override;

private:
	float l_ = -60.0f, r_ = -60.0f;
};

class MixerStrip : public QWidget {
	Q_OBJECT
public:
	MixerStrip(obs_source_t *source, const QString &name, bool followsVideo, QWidget *parent = nullptr);
	~MixerStrip() override;
	void Refresh();
	obs_source_t *Source() const { return source_; }

private:
	static void OnLevels(void *param, const float magnitude[MAX_AUDIO_CHANNELS],
			     const float peak[MAX_AUDIO_CHANNELS], const float inputPeak[MAX_AUDIO_CHANNELS]);

	obs_source_t *source_ = nullptr;
	obs_volmeter_t *meter_ = nullptr;
	obs_fader_t *fader_ = nullptr;
	LevelMeter *levelWidget_ = nullptr;
	std::atomic<float> peakL_{-60.0f}, peakR_{-60.0f};
	float holdL_ = -60.0f, holdR_ = -60.0f;
};

class AudioMixer : public QWidget {
	Q_OBJECT
public:
	explicit AudioMixer(Engine &engine, QWidget *parent = nullptr);
	void Rebuild();

private:
	Engine &engine_;
	QVBoxLayout *list_ = nullptr;
	std::vector<MixerStrip *> strips_;
	QTimer *timer_ = nullptr;
};

} // namespace meikou
