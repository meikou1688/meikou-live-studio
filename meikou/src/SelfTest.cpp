// 自動巡檢：meikou-live-studio --selftest --report <json> [--shots <資料夾>] [--rtmp <網址>]
// 不開主畫面也能逐項檢查引擎功能，最後再開主視窗截圖。結果寫成 JSON，結束代碼＝失敗項目數。
#include "SelfTest.hpp"

#include "Engine.hpp"
#include "GraphicSource.hpp"
#include "GraphicsRender.hpp"
#include "Log.hpp"
#include "MainWindow.hpp"
#include "OverlayDialog.hpp"
#include "Platform.hpp"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QPushButton>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>

#include <util/base.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <cstring>
#include <mutex>
#include <vector>

namespace meikou {

namespace {

struct Report {
	QJsonArray checks;
	int passed = 0, failed = 0, warnings = 0;
	QString section;

	void Section(const QString &s) { section = s; }
	bool Check(const QString &name, bool ok, const QString &detail = QString(), bool warnOnly = false)
	{
		QJsonObject o;
		o["section"] = section;
		o["name"] = name;
		o["result"] = ok ? QStringLiteral("pass") : (warnOnly ? QStringLiteral("warn") : QStringLiteral("fail"));
		if (!detail.isEmpty())
			o["detail"] = detail;
		checks.append(o);
		if (ok)
			passed++;
		else if (warnOnly)
			warnings++;
		else
			failed++;
		blog(ok ? LOG_INFO : LOG_WARNING, "[selftest] %s / %s: %s %s", section.toUtf8().constData(),
		     name.toUtf8().constData(), ok ? "PASS" : (warnOnly ? "WARN" : "FAIL"), detail.toUtf8().constData());
		return ok;
	}
};

// 從輸出端抓畫面（RGBA），用來確認播出畫面的顏色
struct FrameGrab {
	std::mutex mutex;
	std::vector<uint8_t> rgba;
	uint32_t w = 1920, h = 1080, linesize = 0;
	uint64_t frames = 0;

	static void Callback(void *param, struct video_data *frame)
	{
		auto *g = static_cast<FrameGrab *>(param);
		std::lock_guard<std::mutex> lock(g->mutex);
		g->linesize = frame->linesize[0];
		g->rgba.assign(frame->data[0], frame->data[0] + (size_t)g->linesize * g->h);
		g->frames++;
	}

	QColor Average(int x0, int y0, int x1, int y1)
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (rgba.empty())
			return QColor();
		double r = 0, gsum = 0, b = 0;
		int n = 0;
		for (int y = y0; y < y1; y += 4) {
			const uint8_t *row = rgba.data() + (size_t)y * linesize;
			for (int x = x0; x < x1; x += 4) {
				r += row[x * 4 + 0];
				gsum += row[x * 4 + 1];
				b += row[x * 4 + 2];
				n++;
			}
		}
		if (!n)
			return QColor();
		return QColor((int)(r / n), (int)(gsum / n), (int)(b / n));
	}

	QImage Image()
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (rgba.empty())
			return QImage();
		QImage img((int)w, (int)h, QImage::Format_RGBA8888);
		for (uint32_t y = 0; y < h; y++)
			std::memcpy(img.scanLine((int)y), rgba.data() + (size_t)y * linesize, (size_t)w * 4);
		return img.convertToFormat(QImage::Format_RGB32);
	}
};

QString Rgb(const QColor &c)
{
	return QStringLiteral("rgb(%1,%2,%3)").arg(c.red()).arg(c.green()).arg(c.blue());
}

bool Near(const QColor &c, int r, int g, int b, int tol = 60)
{
	return c.isValid() && std::abs(c.red() - r) <= tol && std::abs(c.green() - g) <= tol && std::abs(c.blue() - b) <= tol;
}

QString RectText(const CanvasRect &r)
{
	return QStringLiteral("(%1,%2)-(%3,%4)").arg(r.x0, 0, 'f', 0).arg(r.y0, 0, 'f', 0).arg(r.x1, 0, 'f', 0).arg(r.y1, 0, 'f', 0);
}

size_t LayerCount(Input *scene)
{
	size_t n = 0;
	if (!scene || !scene->wrap)
		return 0;
	obs_scene_enum_items(
		scene->wrap,
		[](obs_scene_t *, obs_sceneitem_t *, void *p) {
			(*static_cast<size_t *>(p))++;
			return true;
		},
		&n);
	return n;
}

Input *AddColor(Engine &e, const char *name, uint32_t abgr)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_int(s, "color", abgr);
	obs_data_set_int(s, "width", 1920);
	obs_data_set_int(s, "height", 1080);
	Input *in = e.AddInput("color_source_v3", name, s);
	obs_data_release(s);
	return in;
}

} // namespace

int RunSelfTest(QApplication &app, const QString &configDir, const QString &reportPath, const QString &shotsDir,
		const QString &rtmpUrl)
{
	Report rep;
	QDir().mkpath(shotsDir);
	QElapsedTimer clock;
	clock.start();

	Engine engine;
	std::mutex evMutex;
	std::vector<std::pair<int, std::string>> events;
	engine.onOutputEvent = [&](int index, const std::string &kind, const std::string &msg) {
		std::lock_guard<std::mutex> lock(evMutex);
		events.emplace_back(index, kind + (msg.empty() ? std::string() : ": " + msg));
	};
	auto eventSeen = [&](int index, const std::string &prefix) {
		std::lock_guard<std::mutex> lock(evMutex);
		for (const auto &ev : events)
			if (ev.first == index && ev.second.rfind(prefix, 0) == 0)
				return true;
		return false;
	};
	qint64 lastTick = 0;
	auto pump = [&](int ms) {
		QElapsedTimer t;
		t.start();
		while (t.elapsed() < ms) {
			app.processEvents(QEventLoop::AllEvents, 20);
			if (clock.elapsed() - lastTick >= 1000) {
				engine.Tick();
				engine.QueryStats();
				lastTick = clock.elapsed();
			}
			QThread::msleep(10);
		}
	};
	auto waitFor = [&](const std::function<bool()> &cond, int timeoutMs) {
		QElapsedTimer t;
		t.start();
		while (t.elapsed() < timeoutMs) {
			if (cond())
				return true;
			pump(50);
		}
		return cond();
	};

	// ------------------------------------------------ 啟動
	rep.Section(QStringLiteral("啟動"));
	std::string err;
	const bool started = engine.Startup(configDir.toUtf8().toStdString(), 30, 1, err);
	rep.Check(QStringLiteral("引擎啟動（1920×1080 30fps）"), started, QString::fromUtf8(err.c_str()));
	if (!started) {
		QJsonObject root;
		root["checks"] = rep.checks;
		root["failed"] = rep.failed;
		QFile f(reportPath);
		if (f.open(QIODevice::WriteOnly))
			f.write(QJsonDocument(root).toJson());
		return 1;
	}

	struct Need {
		const char *id;
		const char *label;
		bool required;
	};
	const Need needs[] = {
		{"color_source_v3", "純色", true},
		{"image_source", "圖片", true},
		{"ffmpeg_source", "影片／網路串流", true},
		{platform::kText, "文字", true},
		{platform::kCamera, "攝影機／擷取卡", true},
		{platform::kScreen, "螢幕擷取", true},
		{platform::kAudioIn, "麥克風", true},
		{platform::kAudioOut, "系統聲音", true},
		{kGraphicSourceId, "內建圖卡", true},
		{"fade_transition", "淡入淡出轉場", true},
		{"rtmp_output", "RTMP 推流", true},
		{"ffmpeg_muxer", "錄影", true},
	};
	for (const auto &n : needs) {
		const char *dn = obs_source_get_display_name(n.id);
		bool ok = dn != nullptr;
		if (!ok && (std::strcmp(n.id, "rtmp_output") == 0 || std::strcmp(n.id, "ffmpeg_muxer") == 0))
			ok = obs_output_get_display_name(n.id) != nullptr;
		rep.Check(QStringLiteral("模組：%1").arg(QString::fromUtf8(n.label)), ok, QString::fromUtf8(n.id));
	}
	QStringList enc;
	bool haveX264 = false;
	for (const auto &e : engine.VideoEncoders()) {
		enc << QString::fromUtf8(e.name.c_str());
		haveX264 = haveX264 || e.id == "obs_x264";
	}
	rep.Check(QStringLiteral("視訊編碼器"), !enc.isEmpty(), enc.join(QStringLiteral("、")));
	rep.Check(QStringLiteral("x264 軟體編碼器（沒有顯卡時的備援）"), haveX264);
	rep.Check(QStringLiteral("內建字型 思源黑體"),
		  QFontDatabase::families().contains(QStringLiteral("Noto Sans TC")),
		  QStringLiteral("字型資料夾要跟著程式安裝"));

	FrameGrab grab;
	struct video_scale_info conv = {};
	conv.format = VIDEO_FORMAT_RGBA;
	conv.width = 1920;
	conv.height = 1080;
	conv.range = VIDEO_RANGE_FULL;
	conv.colorspace = VIDEO_CS_DEFAULT;
	obs_add_raw_video_callback(&conv, FrameGrab::Callback, &grab);
	const bool framesOk = waitFor([&] { return grab.frames > 10; }, 5000);
	rep.Check(QStringLiteral("輸出畫面持續產生"), framesOk, QStringLiteral("%1 格").arg(grab.frames));

	// 等畫面變成指定顏色（測試機較慢時輸出會晚幾格）
	auto waitColor = [&](int x0, int y0, int x1, int y1, int r, int g, int b, int tol) {
		QColor c;
		waitFor([&] {
			c = grab.Average(x0, y0, x1, y1);
			return Near(c, r, g, b, tol);
		}, 3000);
		return c;
	};

	// ------------------------------------------------ 訊號源
	rep.Section(QStringLiteral("訊號源"));
	Input *red = AddColor(engine, "測試紅", 0xFF0000FF);   // ABGR
	Input *blue = AddColor(engine, "測試藍", 0xFFFF0000);
	rep.Check(QStringLiteral("新增純色訊號源"), red && blue);
	obs_data_t *ts = obs_data_create();
	obs_data_set_string(ts, "text", "Meikou 巡檢");
	Input *text = engine.AddInput(platform::kText, "測試文字", ts);
	obs_data_release(ts);
	rep.Check(QStringLiteral("新增文字訊號源"), text != nullptr);
	const QString imgPath = QDir(configDir).filePath(QStringLiteral("selftest-logo.png"));
	QFile::copy(QStringLiteral(":/meikou/round.png"), imgPath);
	obs_data_t *is = obs_data_create();
	obs_data_set_string(is, "file", imgPath.toUtf8().constData());
	Input *image = engine.AddInput("image_source", "測試圖片", is);
	obs_data_release(is);
	pump(300);
	rep.Check(QStringLiteral("新增圖片訊號源並讀到圖"), image && obs_source_get_width(image->source) > 0,
		  image ? QStringLiteral("%1×%2").arg(obs_source_get_width(image->source)).arg(obs_source_get_height(image->source))
			: QString());
	engine.CreateDefaultAudio();
	int audioCount = 0;
	for (const auto &in : engine.Inputs())
		if (in->audioOnly)
			audioCount++;
	rep.Check(QStringLiteral("開啟時自動加入系統聲音與麥克風"), audioCount >= 2, QStringLiteral("%1 軌").arg(audioCount));
	bool audioInBus = false;
	for (Input *in : engine.VideoInputs())
		audioInBus = audioInBus || in->audioOnly;
	rep.Check(QStringLiteral("純音訊來源不會出現在畫面切換列"), !audioInBus);

	// ------------------------------------------------ 場景與圖層
	rep.Section(QStringLiteral("場景"));
	engine.SetPreview(red);
	Input *base = engine.Preview();
	Input *scene = engine.CreateScene(engine.NextSceneName());
	engine.SetPreview(scene);
	engine.ComposeOnPreview(base); // 新增場景時拿預覽中的訊號源當底圖
	engine.ComposeOnPreview(blue); // 再疊一個子母畫面
	rep.Check(QStringLiteral("新增場景並放入兩個訊號源"), scene && LayerCount(scene) == 2,
		  QStringLiteral("%1 層").arg(LayerCount(scene)));
	const std::vector<Input *> sources = engine.VideoInputs();
	rep.Check(QStringLiteral("場景獨立於訊號源列表"),
		  engine.Scenes().size() == 1 && std::find(sources.begin(), sources.end(), scene) == sources.end());

	EditTarget pip = engine.HitPreview(1500, 860);
	CanvasRect r0;
	const bool pipOk = pip.layerId != 0 && engine.TargetRect(pip, r0) && (r0.x1 - r0.x0) < 1000;
	rep.Check(QStringLiteral("子母畫面在右下角、可以點選"), pipOk, RectText(r0));
	EditTarget baseLayer = engine.HitPreview(200, 200);
	rep.Check(QStringLiteral("填滿畫面的底圖鎖定，不會被誤拖"), baseLayer.Valid() && engine.IsLocked(baseLayer));

	engine.MoveTarget(pip, -300, -200);
	CanvasRect r1;
	engine.TargetRect(pip, r1);
	rep.Check(QStringLiteral("拖曳移動圖層"), std::fabs((r1.x0 - r0.x0) + 300) < 2 && std::fabs((r1.y0 - r0.y0) + 200) < 2,
		  RectText(r1));
	engine.ResizeTarget(pip, 2, r1.x1 + 160, r1.y1 + 90); // 拖右下角放大
	CanvasRect r2;
	engine.TargetRect(pip, r2);
	const double ratio1 = (r1.x1 - r1.x0) / std::max(1.0f, r1.y1 - r1.y0), ratio2 = (r2.x1 - r2.x0) / std::max(1.0f, r2.y1 - r2.y0);
	rep.Check(QStringLiteral("拖角落等比例縮放，對角固定"),
		  (r2.x1 - r2.x0) > (r1.x1 - r1.x0) + 100 && std::fabs(ratio1 - ratio2) < 0.02 && std::fabs(r2.x0 - r1.x0) < 2 &&
			  std::fabs(r2.y0 - r1.y0) < 2,
		  RectText(r2));
	engine.ScaleTarget(pip, 0.5f);
	CanvasRect r3;
	engine.TargetRect(pip, r3);
	rep.Check(QStringLiteral("滾輪縮放"), std::fabs((r3.x1 - r3.x0) - (r2.x1 - r2.x0) * 0.5) < 3, RectText(r3));
	engine.LayerOrder(pip, OBS_ORDER_MOVE_BOTTOM);
	rep.Check(QStringLiteral("圖層移到最下層"), engine.HitPreview((r3.x0 + r3.x1) / 2, (r3.y0 + r3.y1) / 2).layerId != pip.layerId);
	engine.LayerOrder(pip, OBS_ORDER_MOVE_TOP);
	rep.Check(QStringLiteral("圖層移到最上層"), engine.HitPreview((r3.x0 + r3.x1) / 2, (r3.y0 + r3.y1) / 2).layerId == pip.layerId);
	Input *dup = engine.DuplicateScene(scene->uid);
	rep.Check(QStringLiteral("複製場景"), dup && LayerCount(dup) == 2);
	engine.SetPreview(dup);
	EditTarget dupPip = engine.HitPreview((r3.x0 + r3.x1) / 2, (r3.y0 + r3.y1) / 2);
	engine.LayerRemove(dupPip);
	rep.Check(QStringLiteral("從場景移除圖層"), LayerCount(dup) == 1);
	EditTarget dupBase = engine.HitPreview(100, 100);
	engine.ScaleTarget(dupBase, 0.5f);
	engine.LayerFill(dupBase);
	CanvasRect rf;
	engine.TargetRect(dupBase, rf);
	rep.Check(QStringLiteral("填滿畫面"), rf.x0 <= 1 && rf.y0 <= 1 && rf.x1 >= 1919 && rf.y1 >= 1079, RectText(rf));
	rep.Check(QStringLiteral("複製出的場景不影響原場景"), LayerCount(scene) == 2);

	// ------------------------------------------------ 送出／撤回
	rep.Section(QStringLiteral("送出與撤回"));
	engine.SetPreview(scene);
	engine.Cut();
	rep.Check(QStringLiteral("CUT 送出：場景上播出"), engine.Program() == scene);
	rep.Check(QStringLiteral("送出後按鍵變成「撤回」"), engine.WillWithdraw());
	const QColor bg1 = waitColor(100, 100, 600, 400, 255, 0, 0, 60);
	const QColor pip1 = waitColor((int)r3.x0 + 10, (int)r3.y0 + 10, (int)r3.x1 - 10, (int)r3.y1 - 10, 0, 0, 255, 60);
	rep.Check(QStringLiteral("播出畫面：底圖是紅色"), Near(bg1, 255, 0, 0), Rgb(bg1));
	rep.Check(QStringLiteral("播出畫面：子母畫面是藍色、位置正確"), Near(pip1, 0, 0, 255), Rgb(pip1));
	engine.Cut();
	rep.Check(QStringLiteral("再按一次＝撤回到預覽"), engine.Program() == nullptr && engine.Preview() == scene);
	const QColor black = waitColor(100, 100, 1800, 1000, 0, 0, 0, 25);
	rep.Check(QStringLiteral("撤回後播出畫面變黑"), Near(black, 0, 0, 0, 25), Rgb(black));
	engine.SetTransitionDuration(400);
	engine.Auto();
	const QColor bg2 = waitColor(100, 100, 600, 400, 255, 0, 0, 60);
	rep.Check(QStringLiteral("AUTO 轉場送出"), engine.Program() == scene && Near(bg2, 255, 0, 0), Rgb(bg2));
	engine.SetPreview(text);
	engine.Auto();
	pump(900);
	rep.Check(QStringLiteral("預覽有新畫面時按送出＝互換"), engine.Program() == text && engine.Preview() == scene);
	engine.Auto();
	pump(900);
	rep.Check(QStringLiteral("再按一次換回來"), engine.Program() == scene && engine.Preview() == text);
	bool transitionsOk = true;
	for (const auto &t : engine.TransitionTypes())
		transitionsOk = transitionsOk && engine.SetTransitionType(t.id);
	engine.SetTransitionType("fade_transition");
	rep.Check(QStringLiteral("所有轉場效果都能切換"), transitionsOk,
		  QStringLiteral("%1 種").arg(engine.TransitionTypes().size()));

	// ------------------------------------------------ 圖卡
	rep.Section(QStringLiteral("疊加圖卡"));
	int styleCount = 0, badStyles = 0;
	QStringList bad;
	for (const char *kind : {"lower", "clock", "ticker", "logo", "text"}) {
		for (const auto &st : GraphicStyles(kind)) {
			for (bool vertical : {false, true}) {
				if (vertical && std::string(kind) != "lower")
					continue;
				for (bool italic : {false, true}) {
					GraphicSpec g;
					g.kind = kind;
					g.text1 = std::string(kind) == "ticker" ? "巡檢跑馬燈 Meikou Live Studio 123" : "王小明";
					g.text2 = "主持人 Host";
					g.file = std::string(kind) == "logo" && st.id != "text" ? imgPath.toUtf8().toStdString() : "";
					g.style = st.id;
					ApplyGraphicDefaults(g);
					g.style = st.id;
					ApplyStyleColors(g);
					g.vertical = vertical;
					g.italic = italic;
					Overlay *ov = engine.AddOverlay(g, "test");
					styleCount++;
					const uint32_t w = ov ? obs_source_get_width(ov->source) : 0, h = ov ? obs_source_get_height(ov->source) : 0;
					const bool ok = ov && w > 20 && h > 20 && h < 1000 && (std::string(kind) == "ticker" || w < 1900);
					if (!ok) {
						badStyles++;
						bad << QStringLiteral("%1/%2%3 %4×%5")
							       .arg(QString::fromUtf8(kind), QString::fromUtf8(st.id.c_str()),
								    vertical ? QStringLiteral("/直") : QString())
							       .arg(w)
							       .arg(h);
					}
					if (ov)
						engine.RemoveOverlay(ov->uid);
				}
			}
		}
	}
	rep.Check(QStringLiteral("所有版型（含直式、斜體）都畫得出來，大小等於圖卡本身"), badStyles == 0,
		  QStringLiteral("共 %1 種組合%2").arg(styleCount).arg(bad.isEmpty() ? QString() : QStringLiteral("；有問題：") + bad.join(u'、')));

	// 疊加屬於場景：每個場景有自己的訊號源與疊加
	rep.Section(QStringLiteral("場景裡的疊加"));
	GraphicSpec lower;
	lower.kind = "lower";
	lower.text1 = "王小明";
	lower.text2 = "Meikou Live Studio 主持人";
	ApplyGraphicDefaults(lower);
	Overlay *ovLower = engine.AddOverlay(lower, "姓名條");
	engine.SetPreview(dup);
	obs_sceneitem_t *lowerItem = engine.AddOverlayToScene(dup, ovLower);
	const int64_t lowerId = lowerItem ? obs_sceneitem_get_id(lowerItem) : 0;
	auto layersOf = [&](Input *sc, bool overlays) {
		int n = 0;
		for (const auto &l : engine.SceneLayers(sc))
			n += l.isOverlay == overlays ? 1 : 0;
		return n;
	};
	bool tagged = false;
	for (const auto &l : engine.SceneLayers(dup))
		tagged = tagged || (l.id == lowerId && l.isOverlay && l.uid == ovLower->uid);
	rep.Check(QStringLiteral("疊加加進場景，列表只顯示這個場景的內容"), lowerId != 0 && tagged && layersOf(dup, true) == 1 &&
										    layersOf(dup, false) == 1,
		  QStringLiteral("訊號源 %1、疊加 %2").arg(layersOf(dup, false)).arg(layersOf(dup, true)));
	rep.Check(QStringLiteral("其他場景不會出現這個疊加"), layersOf(scene, true) == 0);
	rep.Check(QStringLiteral("疊加不再全域疊在所有畫面上"), ovLower->state == OverlayState::Off);

	const EditTarget lowerT{std::string(), lowerId};
	CanvasRect lr;
	engine.TargetRect(lowerT, lr);
	rep.Check(QStringLiteral("姓名條預設在左下、大小合理"), lr.x0 > 50 && lr.x0 < 300 && lr.y1 > 900 && (lr.x1 - lr.x0) < 1000,
		  RectText(lr));
	CanvasRect lr2;
	engine.ResizeTarget(lowerT, 2, lr.x1 + 200, lr.y1 + 100);
	engine.TargetRect(lowerT, lr2);
	rep.Check(QStringLiteral("姓名條可以拖角落放大（不會變成整個畫面）"), (lr2.x1 - lr2.x0) > (lr.x1 - lr.x0) && (lr2.x1 - lr2.x0) < 1600,
		  RectText(lr2));
	engine.LayerResetIn(dup, lowerId);
	CanvasRect lr3;
	engine.TargetRect(lowerT, lr3);
	rep.Check(QStringLiteral("疊加回到預設位置"), std::fabs(lr3.x0 - lr.x0) < 2 && std::fabs((lr3.x1 - lr3.x0) - (lr.x1 - lr.x0)) < 2,
		  RectText(lr3));

	engine.SetLayerVisible(dup, lowerId, false);
	const bool hidden = !engine.LayerVisible(dup, lowerId);
	engine.SetLayerVisible(dup, lowerId, true);
	rep.Check(QStringLiteral("點按鈕在場景裡顯示／隱藏"), hidden && engine.LayerVisible(dup, lowerId));
	engine.Cut();
	rep.Check(QStringLiteral("送出場景時疊加一起上播出"), engine.Program() == dup && engine.LayerVisible(dup, lowerId));
	engine.SetLayerVisible(dup, lowerId, false);
	pump(500);
	const bool liveHidden = !engine.LayerVisible(dup, lowerId);
	engine.SetLayerVisible(dup, lowerId, true);
	rep.Check(QStringLiteral("播出中的場景可以直接開關疊加"), liveHidden && engine.LayerVisible(dup, lowerId));
	Input *dup2 = engine.DuplicateScene(dup->uid);
	rep.Check(QStringLiteral("複製場景時疊加一起複製"), dup2 && layersOf(dup2, true) == 1);
	engine.RemoveOverlay(ovLower->uid);
	rep.Check(QStringLiteral("刪除疊加素材會從所有場景移除"), layersOf(dup, true) == 0 && dup2 && layersOf(dup2, true) == 0);
	if (dup2)
		engine.RemoveInput(dup2->uid);

	// 截圖：播出畫面＋各種圖卡
	auto addShow = [&](const char *kind, const char *style, const char *t1, const char *t2, bool vertical = false) {
		GraphicSpec g;
		g.kind = kind;
		g.style = style;
		g.text1 = t1;
		g.text2 = t2;
		g.file = std::string(kind) == "logo" ? imgPath.toUtf8().toStdString() : "";
		ApplyGraphicDefaults(g);
		g.style = style;
		ApplyStyleColors(g);
		g.vertical = vertical;
		Overlay *ov = engine.AddOverlay(g, kind);
		engine.AddOverlayToScene(engine.Program(), ov); // 放進播出中的場景
		return ov;
	};
	Overlay *ovClock = addShow("clock", "dark", "LIVE", "");
	addShow("ticker", "news", "Meikou Live Studio 自動巡檢　推流、錄影、場景、圖卡逐項檢查", "");
	addShow("logo", "circle", "", "");
	addShow("lower", "pill", "直式測試", "主持人", true);
	pump(1500);
	const QImage program = grab.Image();
	program.save(QDir(shotsDir).filePath(QStringLiteral("program.png")));
	rep.Check(QStringLiteral("播出畫面截圖"), !program.isNull(), QStringLiteral("program.png"));
	CanvasRect cr;
	engine.TargetRect({ovClock->uid, 0}, cr);
	const QColor clockBg = grab.Average((int)cr.x0 + 30, (int)cr.y0 + 30, (int)cr.x1 - 30, (int)cr.y1 - 30);
	rep.Check(QStringLiteral("時鐘真的出現在播出畫面"), clockBg.isValid() && !Near(clockBg, 255, 0, 0, 40), Rgb(clockBg));

	// 繪圖效能（雲端測試機沒有實體顯示卡，只當參考）
	{
		const uint32_t total = obs_get_total_frames(), lagged = obs_get_lagged_frames();
		const double pct = total ? 100.0 * lagged / total : 0.0;
		rep.Check(QStringLiteral("繪圖跟得上幀率（掉格率 < 10%）"), pct < 10.0,
			  QStringLiteral("掉格 %1 / %2（%3%）").arg(lagged).arg(total).arg(pct, 0, 'f', 1), true);
	}

	// 截圖完成，拿掉抓畫面（每格都轉成 RGBA 很吃 CPU，會影響後面的編碼測試）
	obs_remove_raw_video_callback(FrameGrab::Callback, &grab);

	// ------------------------------------------------ 錄影
	rep.Section(QStringLiteral("錄影"));
	auto record = [&](const std::string &encoderId, const QString &label, bool warnOnly) {
		const QString path = QDir(configDir).filePath(QStringLiteral("selftest-%1.mkv").arg(QString::fromUtf8(encoderId.c_str())));
		QFile::remove(path);
		EncodeConfig cfg;
		cfg.encoderId = encoderId;
		cfg.videoKbps = 4000;
		{
			std::lock_guard<std::mutex> lock(evMutex);
			events.clear();
		}
		std::string e;
		const bool ok = engine.StartRecording(cfg, path.toUtf8().toStdString(), e);
		if (!rep.Check(QStringLiteral("開始錄影（%1）").arg(label), ok, QString::fromUtf8(e.c_str()), warnOnly))
			return;
		pump(4000);
		const uint64_t bytesWhileRecording = engine.QueryStats().recordBytes;
		rep.Check(QStringLiteral("錄影中有寫入資料（%1）").arg(label), bytesWhileRecording > 0,
			  QStringLiteral("%1 KB").arg(bytesWhileRecording / 1024), warnOnly);
		engine.StopRecording();
		rep.Check(QStringLiteral("停止錄影（%1）").arg(label), waitFor([&] { return !engine.Recording(); }, 15000),
			  QString(), warnOnly);
		pump(500);
		const qint64 size = QFileInfo(path).size();
		rep.Check(QStringLiteral("錄影檔案（%1）").arg(label), size > 200 * 1024, QStringLiteral("%1 KB").arg(size / 1024),
			  warnOnly);
	};
	record("obs_x264", QStringLiteral("x264"), false);
	const auto encoders = engine.VideoEncoders();
	if (!encoders.empty() && encoders.front().id != "obs_x264")
		record(encoders.front().id, QString::fromUtf8(encoders.front().name.c_str()), true); // 雲端測試機沒有顯示卡，只當參考

	// ------------------------------------------------ 推流
	rep.Section(QStringLiteral("推流"));
	if (!rtmpUrl.isEmpty()) {
		Target t;
		t.name = "巡檢";
		t.server = rtmpUrl.toUtf8().toStdString();
		t.key = "selftest-key";
		engine.Targets().clear();
		engine.Targets().push_back(t);
		EncodeConfig cfg;
		cfg.encoderId = "obs_x264";
		cfg.videoKbps = 3000;
		{
			std::lock_guard<std::mutex> lock(evMutex);
			events.clear();
		}
		std::string e;
		const bool ok = engine.StartStreaming(cfg, e);
		rep.Check(QStringLiteral("開始推流"), ok, QString::fromUtf8(e.c_str()));
		if (ok) {
			const bool connected = waitFor([&] { return eventSeen(0, "started"); }, 15000);
			rep.Check(QStringLiteral("連上 RTMP 伺服器"), connected);
			pump(6000);
			engine.QueryStats();
			const Target &tt = engine.Targets()[0];
			rep.Check(QStringLiteral("推流中有送出資料"), tt.active && tt.kbps > 100, // 沒有顯示卡的雲端機器畫面簡單時碼率較低
				  QStringLiteral("%1 kbps，網路掉格 %2").arg(tt.kbps, 0, 'f', 0).arg(tt.dropped));
			engine.StopStreaming();
			rep.Check(QStringLiteral("停止推流"), waitFor([&] { return !engine.Streaming(); }, 15000));
		}
		engine.Targets().clear();

		// 伺服器位址錯誤：要顯示失敗原因、不能當機、狀態要回到未推流
		Target badTarget;
		badTarget.name = "錯誤位址";
		badTarget.server = "rtmp://127.0.0.1:1/live";
		badTarget.key = "x";
		engine.Targets().push_back(badTarget);
		{
			std::lock_guard<std::mutex> lock(evMutex);
			events.clear();
		}
		std::string be;
		if (engine.StartStreaming(cfg, be)) {
			const bool failedReported = waitFor([&] { return eventSeen(0, "failed") || eventSeen(0, "stopped"); }, 20000);
			pump(1500);
			rep.Check(QStringLiteral("伺服器連不上時回報失敗、不當機"), failedReported && !engine.Streaming(),
				  QString::fromUtf8(engine.Targets()[0].status.c_str()));
		} else {
			rep.Check(QStringLiteral("伺服器連不上時回報失敗、不當機"), true, QString::fromUtf8(be.c_str()));
		}
		engine.Targets().clear();
	} else {
		rep.Check(QStringLiteral("推流（沒有指定測試伺服器，略過）"), false, QString(), true);
	}

	// ------------------------------------------------ 專案
	rep.Section(QStringLiteral("專案存檔"));
	const QByteArray json = QByteArray::fromStdString(engine.SaveProjectJson());
	QJsonParseError pe;
	const QJsonObject proj = QJsonDocument::fromJson(json, &pe).object();
	rep.Check(QStringLiteral("專案檔是正確的 JSON"), pe.error == QJsonParseError::NoError);
	int scenesSaved = 0, layersSaved = 0, overlayLayersSaved = 0;
	for (const auto &v : proj.value(QStringLiteral("inputs")).toArray()) {
		const QJsonObject o = v.toObject();
		if (o.value(QStringLiteral("scene")).toBool()) {
			scenesSaved++;
			for (const auto &l : o.value(QStringLiteral("layers")).toArray()) {
				if (l.toObject().contains(QStringLiteral("overlay")))
					overlayLayersSaved++;
				else
					layersSaved++;
			}
		}
	}
	rep.Check(QStringLiteral("場景與圖層有存進專案"), scenesSaved == 2 && layersSaved == 3,
		  QStringLiteral("%1 個場景、%2 層").arg(scenesSaved).arg(layersSaved));
	rep.Check(QStringLiteral("場景裡的疊加有存進專案"), overlayLayersSaved == 4, QStringLiteral("%1 個").arg(overlayLayersSaved));
	bool fieldsOk = false;
	for (const auto &v : proj.value(QStringLiteral("overlays")).toArray()) {
		const QJsonObject o = v.toObject();
		if (o.value(QStringLiteral("kind")).toString() == QLatin1String("lower") && o.contains(QStringLiteral("vertical")) &&
		    o.contains(QStringLiteral("italic")) && o.contains(QStringLiteral("style")) && o.contains(QStringLiteral("color2")))
			fieldsOk = true;
	}
	rep.Check(QStringLiteral("圖卡的版型、配色、直式、斜體有存進專案"), fieldsOk);
	rep.Check(QStringLiteral("專案格式版本 2"), proj.value(QStringLiteral("version")).toInt() == 2);

	// ------------------------------------------------ 幀率
	rep.Section(QStringLiteral("輸出設定"));
	pump(300);
	std::string fe;
	const bool fps60 = engine.ResetVideo(60, 1, fe);
	struct obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	rep.Check(QStringLiteral("切換到 60fps"), fps60 && ovi.fps_num == 60, QString::fromUtf8(fe.c_str()));
	const bool fps2997 = engine.ResetVideo(30000, 1001, fe);
	rep.Check(QStringLiteral("切換到 29.97fps"), fps2997);
	engine.ResetVideo(30, 1, fe);
	rep.Check(QStringLiteral("輸出固定 1920×1080"), ovi.base_width == 1920 && ovi.base_height == 1080 &&
								   ovi.output_width == 1920 && ovi.output_height == 1080);

	// ------------------------------------------------ 介面截圖
	rep.Section(QStringLiteral("介面"));
	{
		const QString projectPath = QDir(configDir).filePath(QStringLiteral("selftest-project.json"));
		auto *window = new MainWindow(engine, projectPath);
		window->resize(1600, 940);
		window->show();
		pump(2500);
		const QImage ui = window->grab().toImage();
		ui.save(QDir(shotsDir).filePath(QStringLiteral("ui.png")));
		rep.Check(QStringLiteral("主視窗開啟"), !ui.isNull() && ui.width() > 800, QStringLiteral("ui.png"));

		// 點不同場景，下面兩列只顯示該場景的訊號源與疊加
		auto countButtons = [window](const char *name) {
			int n = 0;
			for (QPushButton *b : window->findChildren<QPushButton *>())
				n += b->objectName() == QLatin1String(name) ? 1 : 0;
			return n;
		};
		auto *sceneList = window->findChild<QListWidget *>(QStringLiteral("sceneList"));
		auto clickScene = [&](Input *sc) {
			for (int i = 0; sceneList && i < sceneList->count(); i++) {
				QListWidgetItem *it = sceneList->item(i);
				if (it->data(Qt::UserRole).toString() == QString::fromUtf8(sc->uid.c_str()))
					emit sceneList->itemClicked(it);
			}
			pump(400);
		};
		clickScene(scene);
		const int aSrc = countButtons("busButton"), aOvl = countButtons("ovlButton");
		clickScene(dup);
		const int bSrc = countButtons("busButton"), bOvl = countButtons("ovlButton");
		window->grab().toImage().save(QDir(shotsDir).filePath(QStringLiteral("ui-scene2.png")));
		rep.Check(QStringLiteral("點場景只顯示該場景的訊號源與疊加"), aSrc == 2 && aOvl == 0 && bSrc == 1 && bOvl == 4,
			  QStringLiteral("場景一：%1 訊號源／%2 疊加；場景二：%3／%4").arg(aSrc).arg(aOvl).arg(bSrc).arg(bOvl));

		for (const char *kind : {"lower", "clock", "ticker", "logo", "text"}) {
			GraphicSpec g;
			g.kind = kind;
			g.text1 = "王小明";
			g.text2 = "主持人";
			ApplyGraphicDefaults(g);
			auto *dlg = new OverlayDialog(g, QStringLiteral("測試"), window);
			dlg->show();
			pump(300);
			const QImage di = dlg->grab().toImage();
			di.save(QDir(shotsDir).filePath(QStringLiteral("dialog-%1.png").arg(QString::fromUtf8(kind))));
			rep.Check(QStringLiteral("設計視窗：%1").arg(QString::fromUtf8(kind)), !di.isNull());
			dlg->close();
			delete dlg;
		}
		window->close();
		pump(300);
		delete window;
	}

	// ------------------------------------------------ 結束
	rep.Section(QStringLiteral("結束"));
	engine.Shutdown();
	rep.Check(QStringLiteral("正常關閉"), true);

	QJsonObject root;
	root["app_version"] = QString::fromUtf8(MEIKOU_VERSION);
	root["platform"] = QSysInfo::prettyProductName() + QStringLiteral(" ") + QSysInfo::currentCpuArchitecture();
	root["seconds"] = clock.elapsed() / 1000.0;
	root["passed"] = rep.passed;
	root["failed"] = rep.failed;
	root["warnings"] = rep.warnings;
	root["checks"] = rep.checks;
	root["log"] = QString::fromUtf8(CurrentLogPath().c_str());
	QFile f(reportPath);
	if (f.open(QIODevice::WriteOnly))
		f.write(QJsonDocument(root).toJson());
	return rep.failed;
}

} // namespace meikou
