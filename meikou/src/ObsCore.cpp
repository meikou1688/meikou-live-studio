#include "ObsCore.hpp"

#include <algorithm>
#include <cstring>

namespace meikou {

namespace {

// 只載入導播需要的 OBS 模組。依賴 OBS 介面的外掛（frontend-tools、websocket、
// 各種 *-output-ui）不在清單內，libobs 會略過它們。
const char *kSafeModules[] = {
	"image-source",  "obs-ffmpeg", "obs-filters", "obs-outputs", "obs-transitions",  "obs-x264",
	"obs-nvenc",     "obs-qsv11",  "rtmp-services", "obs-text",  "text-freetype2", "win-capture",
	"win-dshow",     "win-wasapi", "obs-webrtc",  "nv-filters",  "decklink",       "vlc-video",
	"coreaudio-encoder", "obs-libfdk",
};

// 編碼器優先順序：顯示卡優先，最後才用 CPU
const char *kEncoderPreference[] = {
	"obs_nvenc_h264_tex", "jim_nvenc", "obs_qsv11_v2", "obs_qsv11", "h264_texture_amf", "obs_x264",
};

int PreferenceIndex(const std::string &id)
{
	for (size_t i = 0; i < sizeof(kEncoderPreference) / sizeof(kEncoderPreference[0]); i++) {
		if (id == kEncoderPreference[i])
			return (int)i;
	}
	return 100;
}

std::string StopMessage(int code, const char *lastError)
{
	std::string msg;
	switch (code) {
	case OBS_OUTPUT_SUCCESS:
		msg = "推流已停止";
		break;
	case OBS_OUTPUT_BAD_PATH:
		msg = "伺服器網址或串流金鑰不正確";
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
		msg = "這個編碼器或格式不被支援";
		break;
	case OBS_OUTPUT_ENCODE_ERROR:
		msg = "編碼器發生錯誤，請改用其他編碼器";
		break;
	default:
		msg = "推流發生錯誤（代碼 " + std::to_string(code) + "）";
		break;
	}
	if (lastError && *lastError) {
		msg += "：";
		msg += lastError;
	}
	return msg;
}

void ApplyEncoderSettings(obs_data_t *s, const std::string &id, const StreamConfig &cfg)
{
	obs_data_set_string(s, "rate_control", "CBR");
	obs_data_set_int(s, "bitrate", cfg.videoKbps);
	obs_data_set_int(s, "keyint_sec", 2);

	if (id == "obs_x264") {
		obs_data_set_string(s, "preset", "veryfast");
		obs_data_set_string(s, "profile", "high");
		if (cfg.lowLatency)
			obs_data_set_string(s, "tune", "zerolatency");
	} else if (id == "obs_nvenc_h264_tex" || id == "jim_nvenc") {
		// 對齊 OBS 預設：p5 + 高畫質；低延遲時改 ll、不用 B 幀
		obs_data_set_string(s, "preset2", "p5");
		obs_data_set_string(s, "preset", "p5");
		obs_data_set_string(s, "profile", "high");
		obs_data_set_string(s, "tune", cfg.lowLatency ? "ll" : "hq");
		obs_data_set_string(s, "multipass", cfg.lowLatency ? "disabled" : "qres");
		obs_data_set_int(s, "bf", cfg.lowLatency ? 0 : 2);
		obs_data_set_bool(s, "psycho_aq", true);
		obs_data_set_bool(s, "lookahead", false);
	} else if (id == "obs_qsv11_v2" || id == "obs_qsv11") {
		obs_data_set_string(s, "profile", "high");
		obs_data_set_string(s, "latency", cfg.lowLatency ? "ultra-low" : "normal");
	} else if (id == "h264_texture_amf") {
		obs_data_set_string(s, "profile", "high");
		obs_data_set_string(s, "preset", "quality");
	}
}

} // namespace

ObsCore::~ObsCore()
{
	Shutdown();
}

bool ObsCore::Startup(const std::string &configDir, std::string &error)
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

	struct obs_video_info ovi = {};
	ovi.graphics_module = "libobs-d3d11";
	ovi.fps_num = 30;
	ovi.fps_den = 1;
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
			error = "顯示卡不支援 Direct3D 11，請更新顯示卡驅動程式";
		else
			error = "畫面引擎初始化失敗（代碼 " + std::to_string(rv) + "）";
		return false;
	}

	for (const char *m : kSafeModules)
		obs_add_safe_module(m);

	if (!obs_load_core_modules())
		blog(LOG_WARNING, "Some core modules failed to load");
	obs_log_loaded_modules();
	obs_post_load_modules();

	// 播出場景：深色底＋標題文字，之後加入的訊號源疊在上面
	scene_ = obs_scene_create("Meikou PGM");

	obs_data_t *bg = obs_data_create();
	obs_data_set_int(bg, "color", 0xFF281E1E); // ABGR
	obs_data_set_int(bg, "width", 1920);
	obs_data_set_int(bg, "height", 1080);
	obs_source_t *bgSrc = obs_source_create("color_source_v3", "背景", bg, nullptr);
	obs_data_release(bg);
	if (bgSrc) {
		obs_scene_add(scene_, bgSrc);
		obs_source_release(bgSrc);
	}

	obs_data_t *txt = obs_data_create();
	obs_data_t *font = obs_data_create();
	obs_data_set_string(font, "face", "Microsoft JhengHei");
	obs_data_set_int(font, "size", 72);
	obs_data_set_obj(txt, "font", font);
	obs_data_release(font);
	obs_data_set_string(txt, "text", "Meikou Live Studio");
	obs_data_set_int(txt, "color", 0xFFFFFFFF);
	obs_source_t *txtSrc = obs_source_create("text_gdiplus_v3", "標題", txt, nullptr);
	obs_data_release(txt);
	if (txtSrc) {
		obs_sceneitem_t *item = obs_scene_add(scene_, txtSrc);
		struct vec2 pos;
		vec2_set(&pos, 96.0f, 860.0f);
		obs_sceneitem_set_pos(item, &pos);
		obs_source_release(txtSrc);
	}

	obs_set_output_source(0, obs_scene_get_source(scene_));

	// 桌面音訊（系統聲音）與預設麥克風，開啟就載入
	obs_data_t *da = obs_data_create();
	obs_data_set_string(da, "device_id", "default");
	desktopAudio_ = obs_source_create("wasapi_output_capture", "桌面音訊", da, nullptr);
	obs_data_release(da);
	if (desktopAudio_)
		obs_set_output_source(1, desktopAudio_);

	obs_data_t *mic = obs_data_create();
	obs_data_set_string(mic, "device_id", "default");
	micAudio_ = obs_source_create("wasapi_input_capture", "麥克風", mic, nullptr);
	obs_data_release(mic);
	if (micAudio_) {
		obs_source_set_muted(micAudio_, true); // 預設靜音，避免一開就收到現場聲音
		obs_set_output_source(3, micAudio_);
	}

	cpu_ = os_cpu_usage_info_start();
	return true;
}

void ObsCore::Shutdown()
{
	if (!started_)
		return;

	if (output_ && obs_output_active(output_))
		obs_output_force_stop(output_);
	ReleaseStreamObjects();

	for (uint32_t i = 0; i < MAX_CHANNELS; i++)
		obs_set_output_source(i, nullptr);

	if (desktopAudio_) {
		obs_source_release(desktopAudio_);
		desktopAudio_ = nullptr;
	}
	if (micAudio_) {
		obs_source_release(micAudio_);
		micAudio_ = nullptr;
	}
	if (scene_) {
		obs_scene_release(scene_);
		scene_ = nullptr;
	}
	if (cpu_) {
		os_cpu_usage_info_destroy(cpu_);
		cpu_ = nullptr;
	}

	obs_shutdown();
	started_ = false;
}

std::vector<Choice> ObsCore::VideoEncoders() const
{
	std::vector<Choice> list;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		if (obs_get_encoder_type(id) != OBS_ENCODER_VIDEO)
			continue;
		const char *codec = obs_get_encoder_codec(id);
		if (!codec || strcmp(codec, "h264") != 0)
			continue;
		uint32_t caps = obs_get_encoder_caps(id);
		if (caps & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL))
			continue;
		const char *name = obs_encoder_get_display_name(id);
		list.push_back({id, name ? name : id});
	}
	std::stable_sort(list.begin(), list.end(), [](const Choice &a, const Choice &b) {
		return PreferenceIndex(a.id) < PreferenceIndex(b.id);
	});
	return list;
}

std::vector<Choice> ObsCore::ListProperty(const char *sourceId, const char *propName) const
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

obs_sceneitem_t *ObsCore::AddSource(const char *sourceId, const std::string &name, obs_data_t *settings)
{
	if (!scene_)
		return nullptr;
	obs_source_t *src = obs_source_create(sourceId, name.c_str(), settings, nullptr);
	if (!src)
		return nullptr;
	obs_sceneitem_t *item = obs_scene_add(scene_, src);
	obs_source_release(src);
	if (!item)
		return nullptr;

	// 不管來源多大，等比例縮放填進 1920×1080
	struct obs_video_info ovi;
	obs_get_video_info(&ovi);
	struct vec2 bounds, pos;
	vec2_set(&bounds, (float)ovi.base_width, (float)ovi.base_height);
	vec2_set(&pos, 0.0f, 0.0f);
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds(item, &bounds);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_pos(item, &pos);
	return item;
}

void ObsCore::RemoveItem(obs_sceneitem_t *item)
{
	if (item)
		obs_sceneitem_remove(item);
}

bool ObsCore::StartStream(const StreamConfig &cfg, std::string &error)
{
	if (Streaming()) {
		error = "已經在推流中";
		return false;
	}
	ReleaseStreamObjects();

	obs_data_t *ss = obs_data_create();
	obs_data_set_string(ss, "server", cfg.server.c_str());
	obs_data_set_string(ss, "key", cfg.key.c_str());
	service_ = obs_service_create("rtmp_custom", "meikou_service", ss, nullptr);
	obs_data_release(ss);

	obs_data_t *vs = obs_data_create();
	ApplyEncoderSettings(vs, cfg.encoderId, cfg);
	videoEncoder_ = obs_video_encoder_create(cfg.encoderId.c_str(), "meikou_video", vs, nullptr);
	obs_data_release(vs);

	obs_data_t *as = obs_data_create();
	obs_data_set_int(as, "bitrate", cfg.audioKbps);
	audioEncoder_ = obs_audio_encoder_create("ffmpeg_aac", "meikou_audio", as, 0, nullptr);
	obs_data_release(as);

	obs_data_t *os = obs_data_create();
	obs_data_set_bool(os, "dyn_bitrate", cfg.dynamicBitrate);
	obs_data_set_bool(os, "low_latency_mode_enabled", cfg.lowLatency);
	output_ = obs_output_create("rtmp_output", "meikou_stream", os, nullptr);
	obs_data_release(os);

	if (!service_ || !videoEncoder_ || !audioEncoder_ || !output_) {
		error = "無法建立推流元件（編碼器 " + cfg.encoderId + "）";
		ReleaseStreamObjects();
		return false;
	}

	obs_encoder_set_video(videoEncoder_, obs_get_video());
	obs_encoder_set_audio(audioEncoder_, obs_get_audio());
	obs_output_set_video_encoder(output_, videoEncoder_);
	obs_output_set_audio_encoder(output_, audioEncoder_, 0);
	obs_output_set_service(output_, service_);
	obs_output_set_reconnect_settings(output_, 20, 3);

	signal_handler_t *sh = obs_output_get_signal_handler(output_);
	signal_handler_connect(sh, "start", OnStart, this);
	signal_handler_connect(sh, "stop", OnStop, this);
	signal_handler_connect(sh, "reconnect", OnReconnect, this);
	signal_handler_connect(sh, "reconnect_success", OnReconnectSuccess, this);

	lastBytes_ = 0;
	lastBytesTime_ = os_gettime_ns();

	if (!obs_output_start(output_)) {
		const char *le = obs_output_get_last_error(output_);
		error = std::string("推流無法開始") + (le && *le ? std::string("：") + le : "");
		return false;
	}
	blog(LOG_INFO, "Stream starting: encoder=%s, %d kbps, low latency=%d", cfg.encoderId.c_str(), cfg.videoKbps,
	     cfg.lowLatency ? 1 : 0);
	return true;
}

void ObsCore::StopStream()
{
	if (output_ && obs_output_active(output_))
		obs_output_stop(output_);
}

bool ObsCore::Streaming() const
{
	return output_ && obs_output_active(output_);
}

Stats ObsCore::QueryStats()
{
	Stats s;
	if (cpu_)
		s.cpu = os_cpu_usage_info_query(cpu_);
	s.renderFps = obs_get_active_fps();
	s.laggedFrames = obs_get_lagged_frames();
	s.totalFrames = obs_get_total_frames();

	if (output_ && obs_output_active(output_)) {
		s.streaming = true;
		s.reconnecting = obs_output_reconnecting(output_);
		s.droppedFrames = obs_output_get_frames_dropped(output_);
		s.streamFrames = obs_output_get_total_frames(output_);
		s.congestion = obs_output_get_congestion(output_);

		uint64_t bytes = obs_output_get_total_bytes(output_);
		uint64_t now = os_gettime_ns();
		double secs = (double)(now - lastBytesTime_) / 1e9;
		if (secs > 0.0 && bytes >= lastBytes_)
			s.streamKbps = (double)(bytes - lastBytes_) * 8.0 / 1000.0 / secs;
		lastBytes_ = bytes;
		lastBytesTime_ = now;
	}
	return s;
}

void ObsCore::ReleaseStreamObjects()
{
	if (output_) {
		signal_handler_t *sh = obs_output_get_signal_handler(output_);
		signal_handler_disconnect(sh, "start", OnStart, this);
		signal_handler_disconnect(sh, "stop", OnStop, this);
		signal_handler_disconnect(sh, "reconnect", OnReconnect, this);
		signal_handler_disconnect(sh, "reconnect_success", OnReconnectSuccess, this);
		obs_output_release(output_);
		output_ = nullptr;
	}
	if (videoEncoder_) {
		obs_encoder_release(videoEncoder_);
		videoEncoder_ = nullptr;
	}
	if (audioEncoder_) {
		obs_encoder_release(audioEncoder_);
		audioEncoder_ = nullptr;
	}
	if (service_) {
		obs_service_release(service_);
		service_ = nullptr;
	}
}

void ObsCore::OnStart(void *data, calldata_t *)
{
	auto *self = static_cast<ObsCore *>(data);
	if (self->onStreamEvent)
		self->onStreamEvent("started", "推流中");
}

void ObsCore::OnStop(void *data, calldata_t *cd)
{
	auto *self = static_cast<ObsCore *>(data);
	int code = (int)calldata_int(cd, "code");
	const char *le = self->output_ ? obs_output_get_last_error(self->output_) : nullptr;
	std::string msg = StopMessage(code, le);
	blog(LOG_INFO, "Stream stopped: code=%d %s", code, le ? le : "");
	if (self->onStreamEvent)
		self->onStreamEvent("stopped", msg);
}

void ObsCore::OnReconnect(void *data, calldata_t *)
{
	auto *self = static_cast<ObsCore *>(data);
	if (self->onStreamEvent)
		self->onStreamEvent("reconnect", "連線中斷，正在自動重連…");
}

void ObsCore::OnReconnectSuccess(void *data, calldata_t *)
{
	auto *self = static_cast<ObsCore *>(data);
	if (self->onStreamEvent)
		self->onStreamEvent("reconnected", "已重新連線");
}

} // namespace meikou
