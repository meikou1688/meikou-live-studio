#pragma once

#include "GraphicsRender.hpp"

#include <obs.h>
#include <graphics/vec2.h>
#include <util/platform.h>

#include <atomic>
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
	bool isScene = false; // 畫面組合：wrap 裡放多個訊號源圖層（子母畫面、並排等）
	uint32_t channel = 0;
};

// 預覽畫面裡可以直接拖曳、縮放的東西：疊加素材，或預覽畫面裡的圖層
struct EditTarget {
	std::string overlayUid;
	int64_t layerId = 0;
	bool Valid() const { return !overlayUid.empty() || layerId != 0; }
	bool operator==(const EditTarget &o) const { return overlayUid == o.overlayUid && layerId == o.layerId; }
	bool operator!=(const EditTarget &o) const { return !(*this == o); }
};

// 場景裡的一層（給「場景內容」面板用），由上到下排列
struct LayerInfo {
	int64_t id = 0;
	std::string uid; // 訊號源或疊加素材的 uid
	std::string name;
	bool visible = true;
	bool isOverlay = false;
};

struct CanvasRect {
	float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

enum class OverlayState { Off, Preview, Program };

// 疊加素材（台標、人名條、跑馬燈、時鐘、圖片、文字）。
// 同一個來源同時放在「預覽疊加場景」與「播出疊加場景」，用顯示／隱藏切換。
struct Overlay : GraphicSpec {
	std::string uid;
	std::string name;
	obs_source_t *source = nullptr; // 疊加本體：內建圖卡（meikou_graphic）或圖片
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
	// 從按下開始到 libobs 通知「已停止」之前都算使用中（連線中 libobs 還不算 active）
	std::shared_ptr<std::atomic<bool>> running = std::make_shared<std::atomic<bool>>(false);
	uint64_t stopRequestNs = 0; // 按下停止的時間，太久沒停就強制停止
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
	std::vector<Input *> VideoInputs() const; // 單一影像訊號源（不含場景）
	std::vector<Input *> Scenes() const;
	bool HasAudio(const Input *in) const;

	// ---------- 畫面組合（多個訊號源疊在同一個畫面） ----------
	Input *CreateScene(const std::string &name, const std::string &uid = std::string());
	// 把訊號源疊到目前的預覽畫面上；預覽是單一訊號源時，自動建立新的畫面組合
	Input *ComposeOnPreview(Input *src);
	std::string NextSceneName() const;
	Input *DuplicateScene(const std::string &uid);
	// 把疊加素材加進場景（場景送出時一起上播出、撤回時一起撤回）
	obs_sceneitem_t *AddOverlayToScene(Input *scene, Overlay *ov);
	std::vector<LayerInfo> SceneLayers(Input *scene) const;
	void SetLayerVisible(Input *scene, int64_t layerId, bool visible);
	bool LayerVisible(Input *scene, int64_t layerId) const;
	obs_sceneitem_t *AddSourceToScene(Input *scene, Input *src);
	void LayerOrderIn(Input *scene, int64_t layerId, obs_order_movement movement);
	void LayerResetIn(Input *scene, int64_t layerId); // 訊號源＝填滿畫面；疊加＝回到預設位置
	void LayerRemoveIn(Input *scene, int64_t layerId);
	Overlay *OverlayBySource(obs_source_t *source) const;

	// ---------- 切換 ----------
	void SetPreview(Input *in);
	Input *Preview() const { return pvw_; }
	Input *Program() const { return pgm_; }
	void Cut();  // 直切送出／撤回
	void Auto(); // 轉場送出／撤回
	bool WillWithdraw() const { return !pvw_ && pgm_; } // 下一次按送出是「撤回」
	bool SetTransitionType(const std::string &typeId);
	const std::string &TransitionType() const { return transitionType_; }
	void SetTransitionDuration(int ms) { transitionMs_ = ms; }
	int TransitionDuration() const { return transitionMs_; }
	std::vector<Choice> TransitionTypes() const;

	// 預覽視窗的繪製（在繪圖執行緒）；handleSize 是控制點大小（1920×1080 座標）
	void RenderPreview(float handleSize);

	// ---------- 預覽畫面裡的編輯（座標都是 1920×1080） ----------
	EditTarget HitPreview(float x, float y);
	bool TargetRect(const EditTarget &t, CanvasRect &r);
	bool IsLocked(const EditTarget &t); // 填滿整個畫面的圖層不能直接拖動，避免誤觸
	void MoveTarget(const EditTarget &t, float dx, float dy);
	void ResizeTarget(const EditTarget &t, int corner, float mx, float my); // corner：0 左上、1 右上、2 右下、3 左下
	void ScaleTarget(const EditTarget &t, float factor);
	void SetSelection(const EditTarget &t);
	EditTarget Selection() const;
	std::string TargetName(const EditTarget &t);
	void LayerOrder(const EditTarget &t, obs_order_movement movement);
	void LayerFill(const EditTarget &t);
	void LayerRemove(const EditTarget &t);
	obs_source_t *PreviewOverlayScene() const { return pvwOverlay_ ? obs_scene_get_source(pvwOverlay_) : nullptr; }

	// ---------- 疊加 ----------
	Overlay *AddOverlay(const GraphicSpec &spec, const std::string &name, const std::string &uid = std::string());
	void UpdateOverlay(Overlay *ov);
	void RemoveOverlay(const std::string &uid);
	void ToggleOverlay(const std::string &uid, bool direct);
	Overlay *FindOverlay(const std::string &uid);
	const std::vector<std::unique_ptr<Overlay>> &Overlays() const { return overlays_; }
	void ResetOverlayPosition(Overlay *ov);
	void SetSelectedOverlay(const std::string &uid);

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
	void RefreshGraphic(Overlay *ov);
	uint32_t NextAudioChannel() const;
	std::vector<obs_sceneitem_t *> TargetItems(const EditTarget &t);
	obs_sceneitem_t *AddLayer(Input *scene, Input *src, bool fullscreen);
	void SwapPreviewProgram(bool animated);
	void RestartMediaOnSend(obs_scene_t *scene);
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
	EditTarget selection_;

	obs_encoder_t *videoEncoder_ = nullptr;
	obs_encoder_t *audioEncoder_ = nullptr;
	obs_output_t *recordOutput_ = nullptr;
	std::atomic<bool> recordRunning_{false};
	uint64_t recordStopRequestNs_ = 0;
	void ForceStopStuckOutputs();
	std::string recordPath_;
	uint64_t recordStart_ = 0;
	uint64_t streamStart_ = 0;

	os_cpu_usage_info_t *cpu_ = nullptr;
	uint64_t uidCounter_ = 0;
};

} // namespace meikou
