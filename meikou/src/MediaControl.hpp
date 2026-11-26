#pragma once

#include <QObject>
#include <QTimer>

#include <obs.h>

namespace meikou {

// 從頭播放，並在 0.4 秒後確認真的在播。
// OBS 的影片播放器停止時是由背景執行緒稍後處理；停止後馬上按播放，
// 晚到的「停止」會把播放吃掉，所以發現又停了就再送一次（最多 tries 次）。
inline void MediaPlayFromStart(QObject *ctx, obs_source_t *src, int tries = 3)
{
	if (!src)
		return;
	obs_source_media_restart(src);
	if (tries <= 1 || !ctx)
		return;
	obs_weak_source_t *weak = obs_source_get_weak_source(src);
	QTimer::singleShot(400, ctx, [ctx, weak, tries] {
		obs_source_t *s = obs_weak_source_get_source(weak);
		obs_weak_source_release(weak);
		if (!s)
			return;
		const obs_media_state st = obs_source_media_get_state(s);
		if (st == OBS_MEDIA_STATE_STOPPED || st == OBS_MEDIA_STATE_ENDED || st == OBS_MEDIA_STATE_NONE)
			MediaPlayFromStart(ctx, s, tries - 1);
		obs_source_release(s);
	});
}

// 播放鍵：播放中→暫停；暫停中→繼續；停止或播完→從頭播放
inline void MediaTogglePlay(QObject *ctx, obs_source_t *src)
{
	if (!src)
		return;
	switch (obs_source_media_get_state(src)) {
	case OBS_MEDIA_STATE_PLAYING:
		obs_source_media_play_pause(src, true);
		break;
	case OBS_MEDIA_STATE_PAUSED:
		obs_source_media_play_pause(src, false);
		break;
	default:
		MediaPlayFromStart(ctx, src);
		break;
	}
}

} // namespace meikou
