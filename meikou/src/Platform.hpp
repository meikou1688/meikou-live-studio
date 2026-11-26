#pragma once

// 各平台的 libobs 來源 ID。Windows 與 macOS 使用不同的擷取外掛，其餘程式碼共用。
namespace meikou::platform {

#if defined(_WIN32)
inline constexpr const char *kGraphicsModule = "libobs-d3d11";
inline constexpr const char *kCamera = "dshow_input";
inline constexpr const char *kScreen = "monitor_capture";
inline constexpr const char *kWindow = "window_capture";
inline constexpr const char *kGame = "game_capture";
inline constexpr const char *kText = "text_gdiplus_v3";
inline constexpr const char *kAudioIn = "wasapi_input_capture";
inline constexpr const char *kAudioOut = "wasapi_output_capture";
inline constexpr const char *kAppAudio = "wasapi_process_output_capture";
inline constexpr bool kTextIsGdi = true;
#elif defined(__APPLE__)
// macOS 要給完整檔名（libobs-opengl.dylib），只給名稱的話 libobs 會去找 .so 而啟動失敗
#ifndef MEIKOU_DL_OPENGL
#define MEIKOU_DL_OPENGL "libobs-opengl.dylib"
#endif
inline constexpr const char *kGraphicsModule = MEIKOU_DL_OPENGL;
inline constexpr const char *kCamera = "macos-avcapture";
inline constexpr const char *kScreen = "screen_capture";
inline constexpr const char *kWindow = "screen_capture";
inline constexpr const char *kGame = nullptr;
inline constexpr const char *kText = "text_ft2_source_v2";
inline constexpr const char *kAudioIn = "coreaudio_input_capture";
inline constexpr const char *kAudioOut = "sck_audio_capture";
inline constexpr const char *kAppAudio = "sck_audio_capture";
inline constexpr bool kTextIsGdi = false;
#else
inline constexpr const char *kGraphicsModule = "libobs-opengl";
inline constexpr const char *kCamera = "v4l2_input";
inline constexpr const char *kScreen = "xshm_input";
inline constexpr const char *kWindow = "xcomposite_input";
inline constexpr const char *kGame = nullptr;
inline constexpr const char *kText = "text_ft2_source_v2";
inline constexpr const char *kAudioIn = "pulse_input_capture";
inline constexpr const char *kAudioOut = "pulse_output_capture";
inline constexpr const char *kAppAudio = "pulse_output_capture";
inline constexpr bool kTextIsGdi = false;
#endif

} // namespace meikou::platform
