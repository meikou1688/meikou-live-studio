#include "Engine.hpp"

#include "Platform.hpp"

#include <graphics/matrix4.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>

namespace meikou {

namespace {

const char *kSafeModules[] = {
	"image-source",  "obs-ffmpeg",  "obs-filters",   "obs-outputs", "obs-transitions", "obs-x264",
	"obs-nvenc",     "obs-qsv11",   "rtmp-services", "obs-text",    "text-freetype2",  "win-capture",
	"win-dshow",     "win-wasapi",  "obs-webrtc",    "nv-filters",  "decklink",        "vlc-video",
	"coreaudio-encoder", "obs-libfdk", "mac-capture", "mac-avcapture", "mac-videotoolbox", "mac-syphon",
	"linux-capture", "linux-pulseaudio", "linux-v4l2", "linux-pipewire", "obs-vst",
};

const char *kEncoderPreference[] = {
	"obs_nvenc_h264_tex", "jim_nvenc", "obs_qsv11_v2", "obs_qsv11", "h264_texture_amf",
	"com.apple.videotoolbox.videoencoder.ave.avc", "com.apple.videotoolbox.videoencoder.h264.gva", "obs_x264",
};

constexpr uint32_t kOverlayChannel = 10;

int PreferenceIndex(const std::string &id)
{
	for (size_t i = 0; i < sizeof(kEncoderPreference) / sizeof(kEncoderPreference[0]); i++)
		if (id == kEncoderPreference[i])
			return (int)i;
	return 100;
}

// 介面用 0xAARRGGBB；libobs 的色彩來源用 0xAABBGGRR
uint32_t ToAbgr(uint32_t argb)
{
	uint32_t a = (argb >> 24) & 0xFF, r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
	return (a << 24) | (b << 16) | (g << 8) | r;
}

std::string StopMessage(int code, const char *lastError)
{
	std::string msg;
	switch (code) {
	case OBS_OUTPUT_SUCCESS:
		msg = "已停止";
		break;
	case OBS_OUTPUT_BAD_PATH:
		msg = "伺服器網址、串流金鑰或錄影路徑不正確";
		break;
	case OBS_OUTPUT_CONNECT_FAILED:
		msg = "連不上伺服器，請檢查網路或伺服器網址";
		break;
	case OBS_OUTPUT_INVALID_STREAM:
		msg = "平台拒絕連線，請檢查串流金鑰";
		break;
	case OBS_OUTPUT_DISCONNECTED:
		msg = "連線中斷，自動重連失敗";
		break;
	case OBS_OUTPUT_UNSUPPORTED:
		msg = "編碼器或格式不被支援";
		break;
	case OBS_OUTPUT_NO_SPACE:
		msg = "磁碟空間不足";
		break;
	case OBS_OUTPUT_ENCODE_ERROR:
		msg = "編碼器發生錯誤，請改用其他編碼器";
		break;
	default:
		msg = "發生錯誤（代碼 " + std::to_string(code) + "）";
		break;
	}
	if (lastError && *lastError) {
		msg += "：";
		msg += lastError;
	}
	return msg;
}

void ApplyEncoderSettings(obs_data_t *s, const std::string &id, const EncodeConfig &cfg, const char *profile)
{
	obs_data_set_string(s, "rate_control", "CBR");
	obs_data_set_int(s, "bitrate", cfg.videoKbps);
	obs_data_set_int(s, "keyint_sec", 2);
	obs_data_set_string(s, "profile", profile);

	if (id == "obs_x264") {
		obs_data_set_string(s, "preset", "veryfast");
		if (cfg.lowLatency)
			obs_data_set_string(s, "tune", "zerolatency");
	} else if (id == "obs_nvenc_h264_tex" || id == "jim_nvenc") {
		obs_data_set_string(s, "preset", "p5");
		obs_data_set_string(s, "preset2", "p5");
		obs_data_set_string(s, "tune", cfg.lowLatency ? "ll" : "hq");
		obs_data_set_string(s, "multipass", cfg.lowLatency ? "disabled" : "qres");
		obs_data_set_int(s, "bf", cfg.lowLatency ? 0 : 2);
		obs_data_set_bool(s, "psycho_aq", true);
		obs_data_set_bool(s, "lookahead", false);
	} else if (id == "obs_qsv11_v2" || id == "obs_qsv11") {
		obs_data_set_string(s, "latency", cfg.lowLatency ? "ultra-low" : "normal");
	} else if (id == "h264_texture_amf") {
		obs_data_set_string(s, "preset", "quality");
	}
}

obs_source_t *CreateText(const std::string &name, const std::string &text, int size, bool bold, uint32_t argb,
			 int bkOpacity = 0)
{
	obs_data_t *s = obs_data_create();
	obs_data_t *font = obs_data_create();
#if defined(_WIN32)
	obs_data_set_string(font, "face", "Microsoft JhengHei");
#elif defined(__APPLE__)
	obs_data_set_string(font, "face", "PingFang TC");
#else
	obs_data_set_string(font, "face", "Noto Sans CJK TC");
#endif
	obs_data_set_int(font, "size", size);
	obs_data_set_int(font, "flags", bold ? 1 : 0);
	obs_data_set_string(font, "style", bold ? "Bold" : "Regular");
	obs_data_set_obj(s, "font", font);
	obs_data_release(font);
	obs_data_set_string(s, "text", text.c_str());
	if (platform::kTextIsGdi) {
		obs_data_set_int(s, "color", ToAbgr(argb) & 0xFFFFFF);
		obs_data_set_int(s, "opacity", (int)(((argb >> 24) & 0xFF) * 100 / 255));
		if (bkOpacity > 0) {
			obs_data_set_int(s, "bk_color", 0x201810);
			obs_data_set_int(s, "bk_opacity", bkOpacity);
		}
	} else {
		obs_data_set_int(s, "color1", ToAbgr(argb));
		obs_data_set_int(s, "color2", ToAbgr(argb));
	}
	obs_source_t *src = obs_source_create_private(platform::kText, name.c_str(), s);
	obs_data_release(s);
	return src;
}

void SetText(obs_source_t *src, const std::string &text)
{
	if (!src)
		return;
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "text", text.c_str());
	obs_source_update(src, s);
	obs_data_release(s);
}

obs_source_t *CreateColor(const std::string &name, uint32_t argb, int w, int h)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_int(s, "color", ToAbgr(argb));
	obs_data_set_int(s, "width", w);
	obs_data_set_int(s, "height", h);
	obs_source_t *src = obs_source_create_private("color_source_v3", name.c_str(), s);
	obs_data_release(s);
	return src;
}

obs_sceneitem_t *AddAt(obs_scene_t *scene, obs_source_t *src, float x, float y)
{
	if (!scene || !src)
		return nullptr;
	obs_sceneitem_t *item = obs_scene_add(scene, src);
	struct vec2 pos;
	vec2_set(&pos, x, y);
	obs_sceneitem_set_pos(item, &pos);
	return item;
}

void FitToCanvas(obs_sceneitem_t *item)
{
	struct obs_video_info ovi;
	obs_get_video_info(&ovi);
	struct vec2 bounds, pos;
	vec2_set(&bounds, (float)ovi.base_width, (float)ovi.base_height);
	vec2_set(&pos, 0.0f, 0.0f);
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds(item, &bounds);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
	obs_sceneitem_set_pos(item, &pos);
}

std::string ClockText(const std::string &prefix)
{
	std::time_t t = std::time(nullptr);
	std::tm tmv;
#if defined(_WIN32)
	localtime_s(&tmv, &t);
#else
	localtime_r(&t, &tmv);
#endif
	char buf[64];
	std::strftime(buf, sizeof(buf), "%H:%M:%S", &tmv);
	return prefix.empty() ? std::string(buf) : prefix + "  " + buf;
}

} // namespace

Engine::~Engine()
{
	Shutdown();
}

std::string Engine::NewUid(const char *prefix)
{
	auto now = std::chrono::system_clock::now().time_since_epoch();
	long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
	return std::string(prefix) + "-" + std::to_string(ms) + "-" + std::to_string(++uidCounter_);
}

bool Engine::Startup(const std::string &configDir, int fpsNum, int fpsDen, std::string &error)
{
	if (started_)
		return true;

	std::string moduleConfig = configDir + "/plugin_config";
	if (!obs_startup("zh-TW", moduleConfig.c_str(), nullptr)) {
		error = "libobs 啟動失敗";
		return false;
	}
	started_ = true;
	blog(LOG_INFO, "Meikou Live Studio %s, libobs %s", MEIKOU_VERSION, obs_get_version_string());

	struct obs_audio_info ai = {};
	ai.samples_per_sec = 48000;
	ai.speakers = SPEAKERS_STEREO;
	if (!obs_reset_audio(&ai)) {
		error = "音訊引擎初始化失敗";
		return false;
	}
	if (!ResetVideo(fpsNum, fpsDen, error))
		return false;

	for (const char *m : kSafeModules)
		obs_add_safe_module(m);
	if (!obs_load_core_modules())
		blog(LOG_WARNING, "Some core modules failed to load");
	obs_log_loaded_modules();
	obs_post_load_modules();

	obs_set_audio_monitoring_device("Default", "default");

	black_ = obs_scene_create_private("Meikou Black");
	transition_ = obs_source_create_private(transitionType_.c_str(), "Meikou Transition", nullptr);
	if (!transition_) {
		transitionType_ = "cut_transition";
		transition_ = obs_source_create_private(transitionType_.c_str(), "Meikou Transition", nullptr);
	}
	obs_transition_set(transition_, obs_scene_get_source(black_));
	obs_set_output_source(0, transition_);

	pvwOverlay_ = obs_scene_create_private("Meikou PVW Overlay");
	pgmOverlay_ = obs_scene_create_private("Meikou PGM Overlay");
	obs_set_output_source(kOverlayChannel, obs_scene_get_source(pgmOverlay_));
	// 預覽疊加場景不在輸出上，手動標成「顯示中」讓內容持續更新
	obs_source_inc_showing(obs_scene_get_source(pvwOverlay_));

	cpu_ = os_cpu_usage_info_start();
	return true;
}

bool Engine::ResetVideo(int fpsNum, int fpsDen, std::string &error)
{
	if (OutputsActive()) {
		error = "推流或錄影中不能變更幀率";
		return false;
	}
	struct obs_video_info ovi = {};
	ovi.graphics_module = platform::kGraphicsModule;
	ovi.fps_num = (uint32_t)fpsNum;
	ovi.fps_den = (uint32_t)fpsDen;
	ovi.base_width = 1920;
	ovi.base_height = 1080;
	ovi.output_width = 1920;
	ovi.output_height = 1080;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.adapter = 0;
	ovi.gpu_conversion = true;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.scale_type = OBS_SCALE_BICUBIC;

	int rv = obs_reset_video(&ovi);
	if (rv != OBS_VIDEO_SUCCESS) {
		if (rv == OBS_VIDEO_NOT_SUPPORTED)
			error = "顯示卡不支援所需的繪圖功能，請更新顯示卡驅動程式";
		else if (rv == OBS_VIDEO_CURRENTLY_ACTIVE)
			error = "推流或錄影中不能變更幀率";
		else
			error = "畫面引擎初始化失敗（代碼 " + std::to_string(rv) + "）";
		return false;
	}
	blog(LOG_INFO, "Video reset: 1920x1080 @ %d/%d", fpsNum, fpsDen);
	return true;
}

void Engine::Shutdown()
{
	if (!started_)
		return;

	for (auto &t : targets_) {
		if (t.output && obs_output_active(t.output))
			obs_output_force_stop(t.output);
		ReleaseTarget(t);
	}
	if (recordOutput_) {
		if (obs_output_active(recordOutput_))
			obs_output_force_stop(recordOutput_);
		obs_output_release(recordOutput_);
		recordOutput_ = nullptr;
	}
	if (videoEncoder_) {
		obs_encoder_release(videoEncoder_);
		videoEncoder_ = nullptr;
	}
	if (audioEncoder_) {
		obs_encoder_release(audioEncoder_);
		audioEncoder_ = nullptr;
	}

	for (uint32_t i = 0; i < MAX_CHANNELS; i++)
		obs_set_output_source(i, nullptr);

	{
		std::lock_guard<std::mutex> lock(previewMutex_);
		pvw_ = nullptr;
		selectedOverlay_.clear();
	}
	pgm_ = nullptr;

	std::vector<std::unique_ptr<Overlay>> ovs;
	{
		std::lock_guard<std::mutex> lock(previewMutex_);
		ovs.swap(overlays_);
	}
	for (auto &ov : ovs) {
		if (ov->source)
			obs_source_release(ov->source);
		if (ov->t1)
			obs_source_release(ov->t1);
		if (ov->t2)
			obs_source_release(ov->t2);
		if (ov->bar)
			obs_source_release(ov->bar);
	}

	for (auto &in : inputs_) {
		if (in->wrap)
			obs_scene_release(in->wrap);
		if (in->source)
			obs_source_release(in->source);
	}
	inputs_.clear();

	if (pvwOverlay_) {
		obs_source_dec_showing(obs_scene_get_source(pvwOverlay_));
		obs_scene_release(pvwOverlay_);
		pvwOverlay_ = nullptr;
	}
	if (pgmOverlay_) {
		obs_scene_release(pgmOverlay_);
		pgmOverlay_ = nullptr;
	}
	if (transition_) {
		obs_source_release(transition_);
		transition_ = nullptr;
	}
	if (black_) {
		obs_scene_release(black_);
		black_ = nullptr;
	}
	if (cpu_) {
		os_cpu_usage_info_destroy(cpu_);
		cpu_ = nullptr;
	}

	obs_shutdown();
	started_ = false;
}

bool Engine::OutputsActive() const
{
	if (recordOutput_ && obs_output_active(recordOutput_))
		return true;
	for (const auto &t : targets_)
		if (t.output && obs_output_active(t.output))
			return true;
	return false;
}

void Engine::Tick()
{
	for (auto &ov : overlays_) {
		if (ov->kind == "clock")
			SetText(ov->t1, ClockText(ov->text1));
	}
	ReleaseEncodersIfIdle();
}

// ---------------------------------------------------------------- 訊號源

uint32_t Engine::NextAudioChannel() const
{
	for (uint32_t c = 1; c < MAX_CHANNELS; c++) {
		if (c == kOverlayChannel)
			continue;
		bool used = false;
		for (const auto &in : inputs_)
			if (in->audioOnly && in->channel == c)
				used = true;
		if (!used)
			return c;
	}
	return 0;
}

Input *Engine::AddInput(const std::string &typeId, const std::string &name, obs_data_t *settings,
			const std::string &uid)
{
	uint32_t flags = obs_get_source_output_flags(typeId.c_str());
	obs_source_t *src = obs_source_create(typeId.c_str(), name.c_str(), settings, nullptr);
	if (!src)
		return nullptr;

	auto in = std::make_unique<Input>();
	in->uid = uid.empty() ? NewUid("in") : uid;
	in->name = name;
	in->typeId = typeId;
	in->source = src;
	in->audioOnly = !(flags & OBS_SOURCE_VIDEO) && (flags & OBS_SOURCE_AUDIO);

	if (in->audioOnly) {
		in->channel = NextAudioChannel();
		if (in->channel)
			obs_set_output_source(in->channel, src);
	} else {
		in->wrap = obs_scene_create_private(("IN:" + in->uid).c_str());
		obs_sceneitem_t *item = obs_scene_add(in->wrap, src);
		FitToCanvas(item);
	}

	Input *raw = in.get();
	inputs_.push_back(std::move(in));
	if (!raw->audioOnly && !pvw_)
		SetPreview(raw);
	return raw;
}

void Engine::RemoveInput(const std::string &uid)
{
	auto it = std::find_if(inputs_.begin(), inputs_.end(), [&](const auto &p) { return p->uid == uid; });
	if (it == inputs_.end())
		return;
	Input *in = it->get();

	if (pvw_ == in)
		SetPreview(nullptr);
	if (pgm_ == in) {
		obs_transition_set(transition_, obs_scene_get_source(black_));
		pgm_ = nullptr;
	}
	if (in->audioOnly && in->channel)
		obs_set_output_source(in->channel, nullptr);

	obs_source_remove(in->source);
	if (in->wrap)
		obs_scene_release(in->wrap);
	obs_source_release(in->source);
	inputs_.erase(it);
}

void Engine::RenameInput(const std::string &uid, const std::string &name)
{
	Input *in = FindInput(uid);
	if (!in)
		return;
	in->name = name;
	obs_source_set_name(in->source, name.c_str());
}

Input *Engine::FindInput(const std::string &uid)
{
	for (auto &in : inputs_)
		if (in->uid == uid)
			return in.get();
	return nullptr;
}

std::vector<Input *> Engine::VideoInputs() const
{
	std::vector<Input *> list;
	for (const auto &in : inputs_)
		if (!in->audioOnly)
			list.push_back(in.get());
	return list;
}

bool Engine::HasAudio(const Input *in) const
{
	return in && in->source && (obs_source_get_output_flags(in->source) & OBS_SOURCE_AUDIO);
}

// ---------------------------------------------------------------- 切換

void Engine::SetPreview(Input *in)
{
	if (in && in->audioOnly)
		return;
	Input *old = nullptr;
	{
		std::lock_guard<std::mutex> lock(previewMutex_);
		old = pvw_;
		pvw_ = in;
	}
	if (old == in)
		return;
	if (in && in->wrap)
		obs_source_inc_showing(obs_scene_get_source(in->wrap));
	if (old && old->wrap)
		obs_source_dec_showing(obs_scene_get_source(old->wrap));
}

void Engine::Cut()
{
	if (!pvw_)
		return;
	Input *next = pvw_, *old = pgm_;
	obs_transition_set(transition_, obs_scene_get_source(next->wrap));
	pgm_ = next;
	SetPreview(old);
	for (auto &ov : overlays_) {
		if (ov->state == OverlayState::Preview)
			ov->state = OverlayState::Program;
		else if (ov->state == OverlayState::Program)
			ov->state = OverlayState::Preview;
		ApplyOverlayState(ov.get());
	}
}

void Engine::Auto()
{
	if (!pvw_)
		return;
	Input *next = pvw_, *old = pgm_;
	if (!obs_transition_start(transition_, OBS_TRANSITION_MODE_AUTO, (uint32_t)transitionMs_,
				  obs_scene_get_source(next->wrap)))
		obs_transition_set(transition_, obs_scene_get_source(next->wrap));
	pgm_ = next;
	SetPreview(old);
	for (auto &ov : overlays_) {
		if (ov->state == OverlayState::Preview)
			ov->state = OverlayState::Program;
		else if (ov->state == OverlayState::Program)
			ov->state = OverlayState::Preview;
		ApplyOverlayState(ov.get());
	}
}

std::vector<Choice> Engine::TransitionTypes() const
{
	static const Choice wanted[] = {
		{"fade_transition", "淡入淡出"}, {"cut_transition", "直切"},       {"fade_to_color_transition", "淡出到黑"},
		{"swipe_transition", "推移"},    {"slide_transition", "滑動"},     {"wipe_transition", "擦除"},
	};
	std::vector<Choice> list;
	for (const auto &w : wanted) {
		const char *id = nullptr;
		for (size_t i = 0; obs_enum_transition_types(i, &id); i++) {
			if (id && w.id == id) {
				list.push_back(w);
				break;
			}
		}
	}
	return list;
}

bool Engine::SetTransitionType(const std::string &typeId)
{
	if (typeId == transitionType_)
		return true;
	obs_source_t *nt = obs_source_create_private(typeId.c_str(), "Meikou Transition", nullptr);
	if (!nt)
		return false;
	obs_source_t *cur = pgm_ ? obs_scene_get_source(pgm_->wrap) : obs_scene_get_source(black_);
	obs_transition_set(nt, cur);
	obs_set_output_source(0, nt);
	obs_source_release(transition_);
	transition_ = nt;
	transitionType_ = typeId;
	return true;
}

void Engine::RenderPreview()
{
	obs_source_t *src = nullptr;
	bool haveBox = false;
	struct matrix4 box;
	{
		std::lock_guard<std::mutex> lock(previewMutex_);
		if (pvw_ && pvw_->wrap)
			src = obs_source_get_ref(obs_scene_get_source(pvw_->wrap));
		if (!selectedOverlay_.empty()) {
			for (auto &ov : overlays_) {
				if (ov->uid == selectedOverlay_ && ov->state == OverlayState::Preview) {
					obs_sceneitem_get_box_transform(ov->pvwItem, &box);
					haveBox = true;
				}
			}
		}
	}
	if (src) {
		obs_source_video_render(src);
		obs_source_release(src);
	}
	if (pvwOverlay_)
		obs_source_video_render(obs_scene_get_source(pvwOverlay_));
	if (!haveBox)
		return;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	struct vec4 c;
	vec4_set(&c, 0.24f, 0.86f, 0.45f, 1.0f);
	gs_effect_set_vec4(color, &c);
	gs_matrix_push();
	gs_matrix_mul(&box);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(true);
		gs_vertex2f(0.0f, 0.0f);
		gs_vertex2f(1.0f, 0.0f);
		gs_vertex2f(1.0f, 1.0f);
		gs_vertex2f(0.0f, 1.0f);
		gs_vertex2f(0.0f, 0.0f);
		gs_render_stop(GS_LINESTRIP);
	}
	gs_matrix_pop();
}

// ---------------------------------------------------------------- 疊加

void Engine::BuildOverlaySource(Overlay *ov)
{
	const std::string base = ov->kind + ":" + ov->uid;
	if (ov->kind == "lower") {
		obs_scene_t *sc = obs_scene_create_private(base.c_str());
		ov->bar = CreateColor(base + ":bar", ov->color, 980, 150);
		ov->t1 = CreateText(base + ":name", ov->text1, 64, true, 0xFFFFFFFF);
		ov->t2 = CreateText(base + ":title", ov->text2, 34, false, 0xE6FFFFFF);
		AddAt(sc, ov->bar, 0, 0);
		AddAt(sc, ov->t1, 36, 12);
		AddAt(sc, ov->t2, 40, 96);
		ov->source = obs_source_get_ref(obs_scene_get_source(sc));
		obs_scene_release(sc);
	} else if (ov->kind == "ticker") {
		obs_scene_t *sc = obs_scene_create_private(base.c_str());
		ov->bar = CreateColor(base + ":bar", ov->color, 1920, 72);
		ov->t1 = CreateText(base + ":text", ov->text1, 40, false, 0xFFFFFFFF);
		obs_data_t *fs = obs_data_create();
		obs_data_set_double(fs, "speed_x", 140.0);
		obs_data_set_bool(fs, "limit_cx", true);
		obs_data_set_int(fs, "cx", 1920);
		obs_data_set_bool(fs, "loop", true);
		obs_source_t *scroll = obs_source_create_private("scroll_filter", (base + ":scroll").c_str(), fs);
		obs_data_release(fs);
		if (scroll) {
			obs_source_filter_add(ov->t1, scroll);
			obs_source_release(scroll);
		}
		AddAt(sc, ov->bar, 0, 0);
		AddAt(sc, ov->t1, 0, 10);
		ov->source = obs_source_get_ref(obs_scene_get_source(sc));
		obs_scene_release(sc);
	} else if (ov->kind == "clock") {
		ov->t1 = CreateText(base + ":clock", ClockText(ov->text1), 44, true, 0xFFFFFFFF, 55);
		ov->source = obs_source_get_ref(ov->t1);
	} else if (ov->kind == "text") {
		ov->t1 = CreateText(base + ":text", ov->text1, 56, true, ov->color | 0xFF000000);
		ov->source = obs_source_get_ref(ov->t1);
	} else { // logo / image
		obs_data_t *s = obs_data_create();
		obs_data_set_string(s, "file", ov->file.c_str());
		ov->source = obs_source_create_private("image_source", base.c_str(), s);
		obs_data_release(s);
	}
}

Overlay *Engine::AddOverlay(const std::string &kind, const std::string &name, const std::string &text1,
			    const std::string &text2, const std::string &file, uint32_t color, const std::string &uid)
{
	auto ov = std::make_unique<Overlay>();
	ov->uid = uid.empty() ? NewUid("ov") : uid;
	ov->kind = kind;
	ov->name = name;
	ov->text1 = text1;
	ov->text2 = text2;
	ov->file = file;
	ov->color = color;
	BuildOverlaySource(ov.get());
	if (!ov->source)
		return nullptr;

	ov->pvwItem = obs_scene_add(pvwOverlay_, ov->source);
	ov->pgmItem = obs_scene_add(pgmOverlay_, ov->source);

	// 播出端：出現／消失都淡入淡出 0.3 秒
	for (bool show : {true, false}) {
		obs_source_t *fade = obs_source_create_private("fade_transition", "overlay fade", nullptr);
		if (fade) {
			obs_sceneitem_set_transition(ov->pgmItem, show, fade);
			obs_sceneitem_set_transition_duration(ov->pgmItem, show, 300);
			obs_source_release(fade);
		}
	}

	Overlay *raw = ov.get();
	{
		std::lock_guard<std::mutex> lock(previewMutex_);
		overlays_.push_back(std::move(ov));
	}
	ResetOverlayPosition(raw);
	ApplyOverlayState(raw);
	return raw;
}

void Engine::UpdateOverlay(Overlay *ov)
{
	if (!ov)
		return;
	if (ov->kind == "lower") {
		SetText(ov->t1, ov->text1);
		SetText(ov->t2, ov->text2);
	} else if (ov->kind == "ticker" || ov->kind == "text") {
		SetText(ov->t1, ov->text1);
	} else if (ov->kind == "clock") {
		SetText(ov->t1, ClockText(ov->text1));
	} else if (ov->source) {
		obs_data_t *s = obs_data_create();
		obs_data_set_string(s, "file", ov->file.c_str());
		obs_source_update(ov->source, s);
		obs_data_release(s);
	}
	if (ov->bar) {
		obs_data_t *s = obs_data_create();
		obs_data_set_int(s, "color", ToAbgr(ov->color));
		obs_source_update(ov->bar, s);
		obs_data_release(s);
	}
}

void Engine::RemoveOverlay(const std::string &uid)
{
	auto it = std::find_if(overlays_.begin(), overlays_.end(), [&](const auto &p) { return p->uid == uid; });
	if (it == overlays_.end())
		return;
	std::unique_ptr<Overlay> owned;
	{
		std::lock_guard<std::mutex> lock(previewMutex_);
		if (selectedOverlay_ == uid)
			selectedOverlay_.clear();
		owned = std::move(*it);
		overlays_.erase(it);
	}
	Overlay *ov = owned.get();
	obs_sceneitem_remove(ov->pvwItem);
	obs_sceneitem_remove(ov->pgmItem);
	if (ov->source)
		obs_source_release(ov->source);
	if (ov->t1)
		obs_source_release(ov->t1);
	if (ov->t2)
		obs_source_release(ov->t2);
	if (ov->bar)
		obs_source_release(ov->bar);
}

void Engine::ApplyOverlayState(Overlay *ov)
{
	obs_sceneitem_set_visible(ov->pvwItem, ov->state == OverlayState::Preview);
	obs_sceneitem_set_visible(ov->pgmItem, ov->state == OverlayState::Program);
}

void Engine::ToggleOverlay(const std::string &uid, bool direct)
{
	Overlay *ov = FindOverlay(uid);
	if (!ov)
		return;
	// 規則：關閉 → 預覽（綠框）；預覽 → 關閉；播出中點一下直接下架。勾「直接送出」時關閉 → 播出
	if (ov->state == OverlayState::Off)
		ov->state = direct ? OverlayState::Program : OverlayState::Preview;
	else
		ov->state = OverlayState::Off;
	ApplyOverlayState(ov);
	SetSelectedOverlay(ov->state == OverlayState::Preview ? uid : std::string());
}

Overlay *Engine::FindOverlay(const std::string &uid)
{
	for (auto &ov : overlays_)
		if (ov->uid == uid)
			return ov.get();
	return nullptr;
}

Overlay *Engine::HitOverlay(float x, float y)
{
	for (auto it = overlays_.rbegin(); it != overlays_.rend(); ++it) {
		Overlay *ov = it->get();
		if (ov->state != OverlayState::Preview)
			continue;
		struct matrix4 box, inv;
		obs_sceneitem_get_box_transform(ov->pvwItem, &box);
		if (!matrix4_inv(&inv, &box))
			continue;
		struct vec3 p;
		vec3_set(&p, x, y, 0.0f);
		vec3_transform(&p, &p, &inv);
		if (p.x >= 0.0f && p.x <= 1.0f && p.y >= 0.0f && p.y <= 1.0f)
			return ov;
	}
	return nullptr;
}

void Engine::MoveOverlay(Overlay *ov, float dx, float dy)
{
	if (!ov)
		return;
	struct vec2 pos;
	obs_sceneitem_get_pos(ov->pvwItem, &pos);
	pos.x += dx;
	pos.y += dy;
	obs_sceneitem_set_pos(ov->pvwItem, &pos);
	obs_sceneitem_set_pos(ov->pgmItem, &pos);
}

void Engine::ScaleOverlay(Overlay *ov, float factor)
{
	if (!ov)
		return;
	struct vec2 sc;
	obs_sceneitem_get_scale(ov->pvwItem, &sc);
	float s = std::clamp(sc.x * factor, 0.05f, 10.0f);
	vec2_set(&sc, s, s);
	obs_sceneitem_set_scale(ov->pvwItem, &sc);
	obs_sceneitem_set_scale(ov->pgmItem, &sc);
}

void Engine::ResetOverlayPosition(Overlay *ov)
{
	if (!ov)
		return;
	uint32_t align = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
	float x = 0, y = 0, scale = 1.0f;
	if (ov->kind == "logo") {
		align = OBS_ALIGN_RIGHT | OBS_ALIGN_TOP;
		x = 1880;
		y = 40;
		uint32_t w = obs_source_get_width(ov->source);
		if (w > 300)
			scale = 300.0f / (float)w;
	} else if (ov->kind == "lower") {
		align = OBS_ALIGN_LEFT | OBS_ALIGN_BOTTOM;
		x = 100;
		y = 1000;
	} else if (ov->kind == "ticker") {
		align = OBS_ALIGN_LEFT | OBS_ALIGN_BOTTOM;
		x = 0;
		y = 1080;
	} else if (ov->kind == "clock") {
		x = 40;
		y = 40;
	} else {
		align = OBS_ALIGN_CENTER;
		x = 960;
		y = 540;
		uint32_t w = obs_source_get_width(ov->source);
		if (w > 1600)
			scale = 1600.0f / (float)w;
	}
	struct vec2 pos, sc;
	vec2_set(&pos, x, y);
	vec2_set(&sc, scale, scale);
	for (obs_sceneitem_t *item : {ov->pvwItem, ov->pgmItem}) {
		obs_sceneitem_set_alignment(item, align);
		obs_sceneitem_set_pos(item, &pos);
		obs_sceneitem_set_scale(item, &sc);
	}
}

void Engine::SetSelectedOverlay(const std::string &uid)
{
	std::lock_guard<std::mutex> lock(previewMutex_);
	selectedOverlay_ = uid;
}

// ---------------------------------------------------------------- 輸出

std::vector<Choice> Engine::VideoEncoders() const
{
	std::vector<Choice> list;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		if (obs_get_encoder_type(id) != OBS_ENCODER_VIDEO)
			continue;
		const char *codec = obs_get_encoder_codec(id);
		if (!codec || strcmp(codec, "h264") != 0)
			continue;
		if (obs_get_encoder_caps(id) & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL))
			continue;
		const char *name = obs_encoder_get_display_name(id);
		list.push_back({id, name ? name : id});
	}
	std::stable_sort(list.begin(), list.end(),
			 [](const Choice &a, const Choice &b) { return PreferenceIndex(a.id) < PreferenceIndex(b.id); });
	return list;
}

bool Engine::EnsureEncoders(const EncodeConfig &cfg, std::string &error)
{
	if (videoEncoder_ && audioEncoder_)
		return true;

	bool facebook = false;
	for (const auto &t : targets_)
		if (t.enabled && t.server.find("facebook") != std::string::npos)
			facebook = true;

	obs_data_t *vs = obs_data_create();
	ApplyEncoderSettings(vs, cfg.encoderId, cfg, facebook ? "main" : "high");
	videoEncoder_ = obs_video_encoder_create(cfg.encoderId.c_str(), "meikou_video", vs, nullptr);
	obs_data_release(vs);

	obs_data_t *as = obs_data_create();
	obs_data_set_int(as, "bitrate", facebook ? std::min(cfg.audioKbps, 128) : cfg.audioKbps);
	audioEncoder_ = obs_audio_encoder_create("ffmpeg_aac", "meikou_audio", as, 0, nullptr);
	obs_data_release(as);

	if (!videoEncoder_ || !audioEncoder_) {
		error = "無法建立編碼器（" + cfg.encoderId + "）";
		if (videoEncoder_)
			obs_encoder_release(videoEncoder_);
		if (audioEncoder_)
			obs_encoder_release(audioEncoder_);
		videoEncoder_ = audioEncoder_ = nullptr;
		return false;
	}
	obs_encoder_set_video(videoEncoder_, obs_get_video());
	obs_encoder_set_audio(audioEncoder_, obs_get_audio());
	blog(LOG_INFO, "Encoders created: %s %d kbps (low latency %d)", cfg.encoderId.c_str(), cfg.videoKbps,
	     cfg.lowLatency ? 1 : 0);
	return true;
}

void Engine::ReleaseTarget(Target &t)
{
	if (t.output) {
		signal_handler_t *sh = obs_output_get_signal_handler(t.output);
		signal_handler_disconnect(sh, "start", OnOutputStart, this);
		signal_handler_disconnect(sh, "stop", OnOutputStop, this);
		signal_handler_disconnect(sh, "reconnect", OnOutputReconnect, this);
		signal_handler_disconnect(sh, "reconnect_success", OnOutputReconnected, this);
		obs_output_release(t.output);
		t.output = nullptr;
	}
	if (t.service) {
		obs_service_release(t.service);
		t.service = nullptr;
	}
	t.active = false;
}

void Engine::ReleaseEncodersIfIdle()
{
	if (OutputsActive())
		return;
	for (auto &t : targets_)
		if (t.output)
			ReleaseTarget(t);
	if (recordOutput_) {
		signal_handler_t *sh = obs_output_get_signal_handler(recordOutput_);
		signal_handler_disconnect(sh, "start", OnOutputStart, this);
		signal_handler_disconnect(sh, "stop", OnOutputStop, this);
		obs_output_release(recordOutput_);
		recordOutput_ = nullptr;
	}
	if (videoEncoder_ && !obs_encoder_active(videoEncoder_)) {
		obs_encoder_release(videoEncoder_);
		videoEncoder_ = nullptr;
	}
	if (audioEncoder_ && !obs_encoder_active(audioEncoder_)) {
		obs_encoder_release(audioEncoder_);
		audioEncoder_ = nullptr;
	}
}

int Engine::TargetIndexOf(obs_output_t *out) const
{
	for (size_t i = 0; i < targets_.size(); i++)
		if (targets_[i].output == out)
			return (int)i;
	return -1;
}

bool Engine::StartStreaming(const EncodeConfig &cfgIn, std::string &error)
{
	if (Streaming()) {
		error = "已經在推流中";
		return false;
	}
	EncodeConfig cfg = cfgIn;
	bool facebook = false, twitch = false;
	int enabled = 0;
	for (const auto &t : targets_) {
		if (!t.enabled || t.server.empty())
			continue;
		enabled++;
		if (t.server.find("facebook") != std::string::npos)
			facebook = true;
		if (t.server.find("twitch") != std::string::npos)
			twitch = true;
	}
	if (!enabled) {
		error = "請先新增並勾選至少一個推流目標";
		return false;
	}

	// 平台上限（依 OBS rtmp-services）：Facebook 30fps 6000k／60fps 9000k，Twitch 6000k
	struct obs_video_info ovi;
	obs_get_video_info(&ovi);
	double fps = (double)ovi.fps_num / (double)ovi.fps_den;
	int cap = 100000;
	if (facebook)
		cap = std::min(cap, fps > 31 ? 9000 : 6000);
	if (twitch)
		cap = std::min(cap, 6000);
	if (cfg.videoKbps > cap) {
		blog(LOG_INFO, "Bitrate capped from %d to %d by platform limits", cfg.videoKbps, cap);
		cfg.videoKbps = cap;
	}

	if (!EnsureEncoders(cfg, error))
		return false;

	int started = 0;
	for (auto &t : targets_) {
		if (!t.enabled || t.server.empty())
			continue;
		ReleaseTarget(t);

		obs_data_t *ss = obs_data_create();
		obs_data_set_string(ss, "server", t.server.c_str());
		obs_data_set_string(ss, "key", t.key.c_str());
		t.service = obs_service_create("rtmp_custom", ("svc:" + t.name).c_str(), ss, nullptr);
		obs_data_release(ss);

		obs_data_t *os = obs_data_create();
		obs_data_set_bool(os, "dyn_bitrate", cfg.dynamicBitrate);
		obs_data_set_bool(os, "low_latency_mode_enabled", cfg.lowLatency);
		t.output = obs_output_create("rtmp_output", ("out:" + t.name).c_str(), os, nullptr);
		obs_data_release(os);

		if (!t.service || !t.output) {
			t.status = "無法建立推流元件";
			ReleaseTarget(t);
			continue;
		}
		obs_output_set_video_encoder(t.output, videoEncoder_);
		obs_output_set_audio_encoder(t.output, audioEncoder_, 0);
		obs_output_set_service(t.output, t.service);
		obs_output_set_reconnect_settings(t.output, 20, 3);

		signal_handler_t *sh = obs_output_get_signal_handler(t.output);
		signal_handler_connect(sh, "start", OnOutputStart, this);
		signal_handler_connect(sh, "stop", OnOutputStop, this);
		signal_handler_connect(sh, "reconnect", OnOutputReconnect, this);
		signal_handler_connect(sh, "reconnect_success", OnOutputReconnected, this);

		t.lastBytes = 0;
		t.lastTime = os_gettime_ns();
		t.kbps = 0;
		if (obs_output_start(t.output)) {
			t.status = "連線中…";
			started++;
		} else {
			const char *le = obs_output_get_last_error(t.output);
			t.status = std::string("無法開始") + (le && *le ? std::string("：") + le : "");
		}
	}
	if (!started) {
		error = "所有推流目標都無法開始，請看各目標的狀態";
		return false;
	}
	streamStart_ = os_gettime_ns();
	blog(LOG_INFO, "Streaming to %d target(s), %d kbps", started, cfg.videoKbps);
	return true;
}

void Engine::StopStreaming()
{
	for (auto &t : targets_)
		if (t.output && obs_output_active(t.output))
			obs_output_stop(t.output);
}

bool Engine::Streaming() const
{
	for (const auto &t : targets_)
		if (t.output && obs_output_active(t.output))
			return true;
	return false;
}

bool Engine::StartRecording(const EncodeConfig &cfg, const std::string &path, std::string &error)
{
	if (Recording()) {
		error = "已經在錄影中";
		return false;
	}
	if (!EnsureEncoders(cfg, error))
		return false;
	if (recordOutput_) {
		obs_output_release(recordOutput_);
		recordOutput_ = nullptr;
	}
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "path", path.c_str());
	recordOutput_ = obs_output_create("ffmpeg_muxer", "meikou_record", s, nullptr);
	obs_data_release(s);
	if (!recordOutput_) {
		error = "無法建立錄影輸出";
		return false;
	}
	obs_output_set_video_encoder(recordOutput_, videoEncoder_);
	obs_output_set_audio_encoder(recordOutput_, audioEncoder_, 0);
	signal_handler_t *sh = obs_output_get_signal_handler(recordOutput_);
	signal_handler_connect(sh, "start", OnOutputStart, this);
	signal_handler_connect(sh, "stop", OnOutputStop, this);
	if (!obs_output_start(recordOutput_)) {
		const char *le = obs_output_get_last_error(recordOutput_);
		error = std::string("錄影無法開始") + (le && *le ? std::string("：") + le : "");
		return false;
	}
	recordPath_ = path;
	recordStart_ = os_gettime_ns();
	blog(LOG_INFO, "Recording to %s", path.c_str());
	return true;
}

void Engine::StopRecording()
{
	if (recordOutput_ && obs_output_active(recordOutput_))
		obs_output_stop(recordOutput_);
}

bool Engine::Recording() const
{
	return recordOutput_ && obs_output_active(recordOutput_);
}

Stats Engine::QueryStats()
{
	Stats s;
	if (cpu_)
		s.cpu = os_cpu_usage_info_query(cpu_);
	s.renderFps = obs_get_active_fps();
	s.laggedFrames = obs_get_lagged_frames();
	s.totalFrames = obs_get_total_frames();
	if (video_t *v = obs_get_video())
		s.skippedFrames = (int)video_output_get_skipped_frames(v);
	s.streaming = Streaming();
	s.recording = Recording();
	s.streamStartNs = streamStart_;
	s.recordStartNs = recordStart_;
	if (s.recording)
		s.recordBytes = obs_output_get_total_bytes(recordOutput_);

	uint64_t now = os_gettime_ns();
	for (auto &t : targets_) {
		if (!t.output || !obs_output_active(t.output)) {
			t.kbps = 0;
			continue;
		}
		t.reconnecting = obs_output_reconnecting(t.output);
		t.dropped = obs_output_get_frames_dropped(t.output);
		t.frames = obs_output_get_total_frames(t.output);
		uint64_t bytes = obs_output_get_total_bytes(t.output);
		double secs = (double)(now - t.lastTime) / 1e9;
		if (secs > 0.2 && bytes >= t.lastBytes) {
			t.kbps = (double)(bytes - t.lastBytes) * 8.0 / 1000.0 / secs;
			t.lastBytes = bytes;
			t.lastTime = now;
		}
	}
	return s;
}

void Engine::OnOutputStart(void *data, calldata_t *cd)
{
	auto *self = static_cast<Engine *>(data);
	auto *out = static_cast<obs_output_t *>(calldata_ptr(cd, "output"));
	if (out == self->recordOutput_) {
		if (self->onOutputEvent)
			self->onOutputEvent(-1, "started", "錄影中");
		return;
	}
	int idx = self->TargetIndexOf(out);
	if (idx >= 0)
		self->targets_[idx].active = true;
	if (self->onOutputEvent)
		self->onOutputEvent(idx, "started", "推流中");
}

void Engine::OnOutputStop(void *data, calldata_t *cd)
{
	auto *self = static_cast<Engine *>(data);
	auto *out = static_cast<obs_output_t *>(calldata_ptr(cd, "output"));
	int code = (int)calldata_int(cd, "code");
	const char *le = out ? obs_output_get_last_error(out) : nullptr;
	std::string msg = StopMessage(code, le);
	blog(LOG_INFO, "Output '%s' stopped: code=%d %s", out ? obs_output_get_name(out) : "?", code, le ? le : "");
	if (out == self->recordOutput_) {
		if (self->onOutputEvent)
			self->onOutputEvent(-1, code == OBS_OUTPUT_SUCCESS ? "stopped" : "failed", msg);
		return;
	}
	int idx = self->TargetIndexOf(out);
	if (idx >= 0) {
		self->targets_[idx].active = false;
		self->targets_[idx].status = code == OBS_OUTPUT_SUCCESS ? "未推流" : msg;
	}
	if (self->onOutputEvent)
		self->onOutputEvent(idx, code == OBS_OUTPUT_SUCCESS ? "stopped" : "failed", msg);
}

void Engine::OnOutputReconnect(void *data, calldata_t *cd)
{
	auto *self = static_cast<Engine *>(data);
	int idx = self->TargetIndexOf(static_cast<obs_output_t *>(calldata_ptr(cd, "output")));
	if (idx >= 0)
		self->targets_[idx].status = "連線中斷，正在自動重連…";
	if (self->onOutputEvent)
		self->onOutputEvent(idx, "reconnect", "連線中斷，正在自動重連…");
}

void Engine::OnOutputReconnected(void *data, calldata_t *cd)
{
	auto *self = static_cast<Engine *>(data);
	int idx = self->TargetIndexOf(static_cast<obs_output_t *>(calldata_ptr(cd, "output")));
	if (idx >= 0)
		self->targets_[idx].status = "推流中";
	if (self->onOutputEvent)
		self->onOutputEvent(idx, "reconnected", "已重新連線");
}

// ---------------------------------------------------------------- 專案

void Engine::CreateDefaultAudio()
{
	obs_data_t *da = obs_data_create();
	obs_data_set_string(da, "device_id", "default");
	AddInput(platform::kAudioOut, "桌面音訊", da);
	obs_data_release(da);

	obs_data_t *mic = obs_data_create();
	obs_data_set_string(mic, "device_id", "default");
	if (Input *in = AddInput(platform::kAudioIn, "麥克風", mic))
		obs_source_set_muted(in->source, true);
	obs_data_release(mic);
}

std::string Engine::SaveProjectJson() const
{
	obs_data_t *root = obs_data_create();
	obs_data_set_int(root, "version", 1);

	obs_data_array_t *ins = obs_data_array_create();
	for (const auto &in : inputs_) {
		obs_data_t *d = obs_data_create();
		obs_data_set_string(d, "uid", in->uid.c_str());
		obs_data_set_string(d, "name", in->name.c_str());
		obs_data_set_string(d, "type", in->typeId.c_str());
		obs_data_t *st = obs_source_get_settings(in->source);
		obs_data_set_obj(d, "settings", st);
		obs_data_release(st);
		obs_data_set_bool(d, "muted", obs_source_muted(in->source));
		obs_data_set_double(d, "volume", obs_source_get_volume(in->source));
		obs_data_set_bool(d, "monitor",
				  obs_source_get_monitoring_type(in->source) != OBS_MONITORING_TYPE_NONE);
		obs_data_set_bool(d, "enabled", obs_source_enabled(in->source));
		obs_data_array_push_back(ins, d);
		obs_data_release(d);
	}
	obs_data_set_array(root, "inputs", ins);
	obs_data_array_release(ins);

	obs_data_array_t *ovs = obs_data_array_create();
	for (const auto &ov : overlays_) {
		obs_data_t *d = obs_data_create();
		obs_data_set_string(d, "uid", ov->uid.c_str());
		obs_data_set_string(d, "name", ov->name.c_str());
		obs_data_set_string(d, "kind", ov->kind.c_str());
		obs_data_set_string(d, "text1", ov->text1.c_str());
		obs_data_set_string(d, "text2", ov->text2.c_str());
		obs_data_set_string(d, "file", ov->file.c_str());
		obs_data_set_int(d, "color", ov->color);
		struct vec2 pos, sc;
		obs_sceneitem_get_pos(ov->pvwItem, &pos);
		obs_sceneitem_get_scale(ov->pvwItem, &sc);
		obs_data_set_double(d, "x", pos.x);
		obs_data_set_double(d, "y", pos.y);
		obs_data_set_double(d, "scale", sc.x);
		obs_data_array_push_back(ovs, d);
		obs_data_release(d);
	}
	obs_data_set_array(root, "overlays", ovs);
	obs_data_array_release(ovs);

	obs_data_array_t *tgs = obs_data_array_create();
	for (const auto &t : targets_) {
		obs_data_t *d = obs_data_create();
		obs_data_set_string(d, "name", t.name.c_str());
		obs_data_set_string(d, "server", t.server.c_str());
		obs_data_set_string(d, "key", t.key.c_str());
		obs_data_set_bool(d, "enabled", t.enabled);
		obs_data_array_push_back(tgs, d);
		obs_data_release(d);
	}
	obs_data_set_array(root, "targets", tgs);
	obs_data_array_release(tgs);

	obs_data_set_string(root, "pvw", pvw_ ? pvw_->uid.c_str() : "");
	obs_data_set_string(root, "pgm", pgm_ ? pgm_->uid.c_str() : "");
	obs_data_set_string(root, "transition", transitionType_.c_str());
	obs_data_set_int(root, "transition_ms", transitionMs_);

	std::string json = obs_data_get_json_pretty(root);
	obs_data_release(root);
	return json;
}

void Engine::LoadProjectJson(const std::string &json)
{
	obs_data_t *root = obs_data_create_from_json(json.c_str());
	if (!root)
		return;

	obs_data_array_t *ins = obs_data_get_array(root, "inputs");
	for (size_t i = 0; ins && i < obs_data_array_count(ins); i++) {
		obs_data_t *d = obs_data_array_item(ins, i);
		obs_data_t *st = obs_data_get_obj(d, "settings");
		Input *in = AddInput(obs_data_get_string(d, "type"), obs_data_get_string(d, "name"), st,
				     obs_data_get_string(d, "uid"));
		if (in) {
			obs_source_set_muted(in->source, obs_data_get_bool(d, "muted"));
			if (obs_data_has_user_value(d, "volume"))
				obs_source_set_volume(in->source, (float)obs_data_get_double(d, "volume"));
			if (obs_data_get_bool(d, "monitor"))
				obs_source_set_monitoring_type(in->source, OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
			if (obs_data_has_user_value(d, "enabled") && !obs_data_get_bool(d, "enabled"))
				obs_source_set_enabled(in->source, false);
		}
		obs_data_release(st);
		obs_data_release(d);
	}
	obs_data_array_release(ins);

	obs_data_array_t *ovs = obs_data_get_array(root, "overlays");
	for (size_t i = 0; ovs && i < obs_data_array_count(ovs); i++) {
		obs_data_t *d = obs_data_array_item(ovs, i);
		Overlay *ov = AddOverlay(obs_data_get_string(d, "kind"), obs_data_get_string(d, "name"),
					 obs_data_get_string(d, "text1"), obs_data_get_string(d, "text2"),
					 obs_data_get_string(d, "file"), (uint32_t)obs_data_get_int(d, "color"),
					 obs_data_get_string(d, "uid"));
		if (ov && obs_data_has_user_value(d, "x")) {
			struct vec2 pos, sc;
			vec2_set(&pos, (float)obs_data_get_double(d, "x"), (float)obs_data_get_double(d, "y"));
			float s = (float)obs_data_get_double(d, "scale");
			vec2_set(&sc, s > 0 ? s : 1.0f, s > 0 ? s : 1.0f);
			for (obs_sceneitem_t *item : {ov->pvwItem, ov->pgmItem}) {
				obs_sceneitem_set_pos(item, &pos);
				obs_sceneitem_set_scale(item, &sc);
			}
		}
		obs_data_release(d);
	}
	obs_data_array_release(ovs);

	targets_.clear();
	obs_data_array_t *tgs = obs_data_get_array(root, "targets");
	for (size_t i = 0; tgs && i < obs_data_array_count(tgs); i++) {
		obs_data_t *d = obs_data_array_item(tgs, i);
		Target t;
		t.name = obs_data_get_string(d, "name");
		t.server = obs_data_get_string(d, "server");
		t.key = obs_data_get_string(d, "key");
		t.enabled = obs_data_get_bool(d, "enabled");
		targets_.push_back(t);
		obs_data_release(d);
	}
	obs_data_array_release(tgs);

	const char *tr = obs_data_get_string(root, "transition");
	if (tr && *tr)
		SetTransitionType(tr);
	if (obs_data_has_user_value(root, "transition_ms"))
		transitionMs_ = (int)obs_data_get_int(root, "transition_ms");

	if (Input *pg = FindInput(obs_data_get_string(root, "pgm")); pg && pg->wrap) {
		obs_transition_set(transition_, obs_scene_get_source(pg->wrap));
		pgm_ = pg;
	}
	if (Input *pv = FindInput(obs_data_get_string(root, "pvw")); pv && pv->wrap)
		SetPreview(pv);
	else if (pvw_ == pgm_)
		SetPreview(nullptr);

	obs_data_release(root);
}

std::vector<Choice> Engine::ListProperty(const char *sourceId, const char *propName) const
{
	std::vector<Choice> list;
	obs_properties_t *props = obs_get_source_properties(sourceId);
	if (!props)
		return list;
	obs_property_t *p = obs_properties_get(props, propName);
	if (p && obs_property_list_format(p) == OBS_COMBO_FORMAT_STRING) {
		size_t count = obs_property_list_item_count(p);
		for (size_t i = 0; i < count; i++) {
			const char *name = obs_property_list_item_name(p, i);
			const char *value = obs_property_list_item_string(p, i);
			if (value && *value)
				list.push_back({value, name ? name : value});
		}
	}
	obs_properties_destroy(props);
	return list;
}

} // namespace meikou
