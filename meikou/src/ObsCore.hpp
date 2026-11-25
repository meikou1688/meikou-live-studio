#pragma once

#include <obs.h>
#include <util/platform.h>
#include <graphics/vec2.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace meikou {

struct Choice {
	std::string id;
	std::string name;
};

struct StreamConfig {
	std::string server;
	std::string key;
	std::string encoderId;
	int videoKbps = 6000;
	int audioKbps = 160;
	bool lowLatency = true;
	bool dynamicBitrate = false;
};

struct Stats {
	double cpu = 0.0;
	double renderFps = 0.0;
	uint32_t laggedFrames = 0;
	uint32_t totalFrames = 0;
	bool streaming = false;
	bool reconnecting = false;
	double streamKbps = 0.0;
	int droppedFrames = 0;
	int streamFrames = 0;
	double congestion = 0.0;
};

// libobs 的生命週期、場景與推流輸出。所有呼叫都在 UI 執行緒；
// libobs 的訊號（停止、重連）會在別的執行緒觸發，經由 onStreamEvent 回報。
class ObsCore {
public:
	ObsCore() = default;
	~ObsCore();
	ObsCore(const ObsCore &) = delete;
	ObsCore &operator=(const ObsCore &) = delete;

	bool Startup(const std::string &configDir, std::string &error);
	void Shutdown();

	obs_scene_t *Scene() const { return scene_; }
	obs_source_t *DesktopAudio() const { return desktopAudio_; }
	obs_source_t *MicAudio() const { return micAudio_; }

	std::vector<Choice> VideoEncoders() const;
	std::vector<Choice> ListProperty(const char *sourceId, const char *propName) const;

	obs_sceneitem_t *AddSource(const char *sourceId, const std::string &name, obs_data_t *settings);
	void RemoveItem(obs_sceneitem_t *item);

	bool StartStream(const StreamConfig &cfg, std::string &error);
	void StopStream();
	bool Streaming() const;
	Stats QueryStats();

	// kind: "started" / "stopped" / "reconnect" / "reconnected"；message 為中文說明
	std::function<void(const std::string &kind, const std::string &message)> onStreamEvent;

private:
	void ReleaseStreamObjects();
	static void OnStart(void *data, calldata_t *cd);
	static void OnStop(void *data, calldata_t *cd);
	static void OnReconnect(void *data, calldata_t *cd);
	static void OnReconnectSuccess(void *data, calldata_t *cd);

	bool started_ = false;
	obs_scene_t *scene_ = nullptr;
	obs_source_t *desktopAudio_ = nullptr;
	obs_source_t *micAudio_ = nullptr;

	obs_output_t *output_ = nullptr;
	obs_encoder_t *videoEncoder_ = nullptr;
	obs_encoder_t *audioEncoder_ = nullptr;
	obs_service_t *service_ = nullptr;

	os_cpu_usage_info_t *cpu_ = nullptr;
	uint64_t lastBytes_ = 0;
	uint64_t lastBytesTime_ = 0;
};

} // namespace meikou
