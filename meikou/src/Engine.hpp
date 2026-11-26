#pragma once

#include <obs.h>
#include <graphics/vec2.h>
#include <util/platform.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace meikou {

struct Choice {
	std::string id;
	std::string name;
};

// 一個訊號源。影像來源包在自己的場景裡（等比例填滿 1920×1080），
// 純音訊來源掛在 libobs 的輸出聲道上，不產生畫面。
struct Input {
	std::string uid;
	std::string name;
	std::string typeId;
	obs_source_t *source = nullptr;
	obs_scene_t *wrap = nullptr;
	bool audioOnly = false;
	uint32_t channel = 0;
};

enum class OverlayState { Off, Preview, Program };

// 疊加素材（台標、人名條、跑馬燈、時鐘、圖片、文字）。
// 同一個來源同時放在「預覽疊加場景」與「播出疊加場景」，用顯示／隱藏切換。
struct Overlay {
	std::string uid;
	std::string name;
	std::string kind; // logo / lower / ticker / clock / image / text
	std::string text1, text2, file;
	uint32_t color = 0xFFD7263D; // 0xAARRGGBB（介面用）
	obs_source_t *source = nullptr; // 疊加本體（多數是巢狀場景）
	obs_source_t *t1 = nullptr, *t2 = nullptr, *bar = nullptr;
	obs_sceneitem_t *pvwItem = nullptr, *pgmItem = nullptr;
	OverlayState state = OverlayState::Off;
};

struct Target {
	std::string name;
	std::string server;
	std::string key;
	bool enabled = true;
	obs_output_t *output = nullptr;
	obs_service_t *service = nullptr;
	std::string status = "未推流";
	bool active = false;
	bool reconnecting = false;
	uint64_t lastBytes = 0;
	uint64_t lastTime = 0;
	double kbps = 0.0;
	int dropped = 0;
	int frames = 0;
};

struct EncodeConfig {
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
	int skippedFrames = 0;
	bool streaming = false;
	bool recording = false;
	uint64_t recordBytes = 0;
	uint64_t recordStartNs = 0;
	uint64_t streamStartNs = 0;
};

class Engine {
public:
	Engine() = default;
	~Engine();
	Engine(const Engine &) = delete;
	Engine &operator=(const Engine &) = delete;

	bool Startup(const std::string &configDir, int fpsNum, int fpsDen, std::string &error);
	void Shutdown();
	bool ResetVideo(int fpsNum, int fpsDen, std::string &error);
	bool OutputsActive() const;
	void Tick(); // 每秒由介面呼叫：更新時鐘疊加、碼率統計

	// ---------- 訊號源 ----------
	Input *AddInput(const std::string &typeId, const std::string &name, obs_data_t *settings,
			const std::string &uid = std::string());
	void RemoveInput(const std::string &uid);
	void RenameInput(const std::string &uid, const std::string &name);
	Input *FindInput(const std::string &uid);
	const std::vector<std::unique_ptr<Input>> &Inputs() const { return inputs_; }
	std::vector<Input *> VideoInputs() const;
	bool HasAudio(const Input *in) const;

	// ---------- 切換 ----------
	void SetPreview(Input *in);
	Input *Preview() const { return pvw_; }
	Input *Program() const { return pgm_; }
	void Cut();
	void Auto();
	bool SetTransitionType(const std::string &typeId);
	const std::string &TransitionType() const { return transitionType_; }
	void SetTransitionDuration(int ms) { transitionMs_ = ms; }
	int TransitionDuration() const { return transitionMs_; }
	std::vector<Choice> TransitionTypes() const;

	// 預覽視窗的繪製（在繪圖執行緒）
	void RenderPreview();
	obs_source_t *PreviewOverlayScene() const { return pvwOverlay_ ? obs_scene_get_source(pvwOverlay_) : nullptr; }

	// ---------- 疊加 ----------
	Overlay *AddOverlay(const std::string &kind, const std::string &name, const std::string &text1,
			    const std::string &text2, const std::string &file, uint32_t color,
			    const std::string &uid = std::string());
	void UpdateOverlay(Overlay *ov);
	void RemoveOverlay(const std::string &uid);
	void ToggleOverlay(const std::string &uid, bool direct);
	Overlay *FindOverlay(const std::string &uid);
	const std::vector<std::unique_ptr<Overlay>> &Overlays() const { return overlays_; }
	Overlay *HitOverlay(float x, float y); // 以 1920×1080 座標找預覽中的疊加
	void MoveOverlay(Overlay *ov, float dx, float dy);
	void ScaleOverlay(Overlay *ov, float factor);
	void ResetOverlayPosition(Overlay *ov);
	void SetSelectedOverlay(const std::string &uid);
	const std::string &SelectedOverlay() const { return selectedOverlay_; }

	// ---------- 輸出 ----------
	std::vector<Choice> VideoEncoders() const;
	std::vector<Target> &Targets() { return targets_; }
	bool StartStreaming(const EncodeConfig &cfg, std::string &error);
	void StopStreaming();
	bool Streaming() const;
	bool StartRecording(const EncodeConfig &cfg, const std::string &path, std::string &error);
	void StopRecording();
	bool Recording() const;
	std::string RecordingPath() const { return recordPath_; }
	Stats QueryStats();

	std::function<void(int targetIndex, const std::string &kind, const std::string &message)> onOutputEvent;

	// ---------- 專案 ----------
	std::string SaveProjectJson() const;
	void LoadProjectJson(const std::string &json);
	void CreateDefaultAudio();

	std::vector<Choice> ListProperty(const char *sourceId, const char *propName) const;

private:
	bool EnsureEncoders(const EncodeConfig &cfg, std::string &error);
	void ReleaseEncodersIfIdle();
	void ReleaseTarget(Target &t);
	void ApplyOverlayState(Overlay *ov);
	void BuildOverlaySource(Overlay *ov);
	uint32_t NextAudioChannel() const;
	std::string NewUid(const char *prefix);

	static void OnOutputStart(void *data, calldata_t *cd);
	static void OnOutputStop(void *data, calldata_t *cd);
	static void OnOutputReconnect(void *data, calldata_t *cd);
	static void OnOutputReconnected(void *data, calldata_t *cd);
	int TargetIndexOf(obs_output_t *out) const;

	bool started_ = false;
	std::vector<std::unique_ptr<Input>> inputs_;
	std::vector<std::unique_ptr<Overlay>> overlays_;
	std::vector<Target> targets_;

	mutable std::mutex previewMutex_;
	Input *pvw_ = nullptr;
	Input *pgm_ = nullptr;
	obs_scene_t *black_ = nullptr;
	obs_source_t *transition_ = nullptr;
	std::string transitionType_ = "fade_transition";
	int transitionMs_ = 500;

	obs_scene_t *pvwOverlay_ = nullptr;
	obs_scene_t *pgmOverlay_ = nullptr;
	obs_source_t *overlayFade_ = nullptr;
	std::string selectedOverlay_;

	obs_encoder_t *videoEncoder_ = nullptr;
	obs_encoder_t *audioEncoder_ = nullptr;
	obs_output_t *recordOutput_ = nullptr;
	std::string recordPath_;
	uint64_t recordStart_ = 0;
	uint64_t streamStart_ = 0;

	os_cpu_usage_info_t *cpu_ = nullptr;
	uint64_t uidCounter_ = 0;
};

} // namespace meikou
