#include "GraphicSource.hpp"

#include <graphics/graphics.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace meikou {

namespace {

struct Pixels {
	std::vector<uint8_t> data; // BGRA，預乘 alpha
	uint32_t w = 0, h = 0;
	bool dirty = false;
};

struct Graphic {
	obs_source_t *source = nullptr;
	std::mutex mutex;
	Pixels main, text;
	std::atomic<uint32_t> width{0}, height{0};

	// 跑馬燈
	bool ticker = false;
	int areaX = 0, areaY = 0, areaW = 0;
	float speed = 0.0f;
	double offset = 0.0;

	// 只在繪圖執行緒使用
	gs_texture_t *mainTex = nullptr;
	gs_texture_t *textTex = nullptr;
	uint32_t textW = 0, textH = 0;
};

void CopyImage(Pixels &px, const QImage &src)
{
	const QImage img = src.convertToFormat(QImage::Format_ARGB32_Premultiplied); // 記憶體排列就是 BGRA
	px.w = (uint32_t)img.width();
	px.h = (uint32_t)img.height();
	px.data.resize((size_t)px.w * px.h * 4);
	for (uint32_t y = 0; y < px.h; y++)
		std::memcpy(px.data.data() + (size_t)y * px.w * 4, img.constScanLine((int)y), (size_t)px.w * 4);
	px.dirty = true;
}

gs_texture_t *Upload(gs_texture_t *old, Pixels &px)
{
	if (old)
		gs_texture_destroy(old);
	px.dirty = false;
	if (!px.w || !px.h || px.data.empty())
		return nullptr;
	const uint8_t *planes[] = {px.data.data()};
	return gs_texture_create(px.w, px.h, GS_BGRA, 1, planes, 0);
}

const char *GetName(void *)
{
	return "Meikou Graphic";
}

void *Create(obs_data_t *, obs_source_t *source)
{
	auto *g = new Graphic();
	g->source = source;
	return g;
}

void Destroy(void *data)
{
	auto *g = static_cast<Graphic *>(data);
	obs_enter_graphics();
	if (g->mainTex)
		gs_texture_destroy(g->mainTex);
	if (g->textTex)
		gs_texture_destroy(g->textTex);
	obs_leave_graphics();
	delete g;
}

uint32_t GetWidth(void *data)
{
	return static_cast<Graphic *>(data)->width.load();
}

uint32_t GetHeight(void *data)
{
	return static_cast<Graphic *>(data)->height.load();
}

void Tick(void *data, float seconds)
{
	auto *g = static_cast<Graphic *>(data);
	std::lock_guard<std::mutex> lock(g->mutex);
	if (g->ticker && g->textW > 0) {
		g->offset += g->speed * seconds;
		g->offset = std::fmod(g->offset, (double)g->textW);
	}
}

void DrawTexture(gs_effect_t *effect, gs_texture_t *tex, uint32_t x, uint32_t y, uint32_t cx, uint32_t cy)
{
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture_srgb(image, tex);
	gs_draw_sprite_subregion(tex, 0, x, y, cx, cy);
}

void Render(void *data, gs_effect_t *effect)
{
	auto *g = static_cast<Graphic *>(data);
	bool ticker;
	int areaX, areaY, areaW;
	double offset;
	{
		std::lock_guard<std::mutex> lock(g->mutex);
		if (g->main.dirty)
			g->mainTex = Upload(g->mainTex, g->main);
		if (g->text.dirty) {
			g->textTex = Upload(g->textTex, g->text);
			g->textW = g->text.w;
			g->textH = g->text.h;
			if (g->textW)
				g->offset = std::fmod(g->offset, (double)g->textW);
		}
		ticker = g->ticker;
		areaX = g->areaX;
		areaY = g->areaY;
		areaW = g->areaW;
		offset = g->offset;
	}
	if (!g->mainTex)
		return;

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA); // 圖片是預乘 alpha

	DrawTexture(effect, g->mainTex, 0, 0, gs_texture_get_width(g->mainTex), gs_texture_get_height(g->mainTex));

	if (ticker && g->textTex && g->textW > 0 && areaW > 0) {
		const uint32_t tw = g->textW, th = g->textH;
		// 文字從右往左捲：把同一段文字重複接在一起，只畫落在文字區裡的部分
		double x = -offset;
		while (x < (double)areaW) {
			const double srcX = std::max(0.0, -x);
			const double dstX = std::max(0.0, x);
			const double w = std::min((double)tw - srcX, (double)areaW - dstX);
			if (w >= 1.0) {
				gs_matrix_push();
				gs_matrix_translate3f((float)(areaX + std::floor(dstX)), (float)areaY, 0.0f);
				DrawTexture(effect, g->textTex, (uint32_t)srcX, 0, (uint32_t)w, th);
				gs_matrix_pop();
			}
			x += (double)tw;
		}
	}

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);
}

Graphic *Get(obs_source_t *source)
{
	if (!source || std::strcmp(obs_source_get_id(source), kGraphicSourceId) != 0)
		return nullptr;
	return static_cast<Graphic *>(obs_obj_get_data(source));
}

} // namespace

void RegisterGraphicSource()
{
	static bool registered = false;
	if (registered)
		return;
	registered = true;
	struct obs_source_info info = {};
	info.id = kGraphicSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CAP_DISABLED; // 不出現在任何新增來源清單
	info.get_name = GetName;
	info.create = Create;
	info.destroy = Destroy;
	info.get_width = GetWidth;
	info.get_height = GetHeight;
	info.video_tick = Tick;
	info.video_render = Render;
	obs_register_source(&info);
}

void GraphicPush(obs_source_t *source, const QImage &image)
{
	Graphic *g = Get(source);
	if (!g)
		return;
	std::lock_guard<std::mutex> lock(g->mutex);
	CopyImage(g->main, image);
	g->ticker = false;
	g->width = g->main.w;
	g->height = g->main.h;
}

void GraphicPushTicker(obs_source_t *source, const TickerParts &parts, int speed)
{
	Graphic *g = Get(source);
	if (!g)
		return;
	std::lock_guard<std::mutex> lock(g->mutex);
	CopyImage(g->main, parts.bar);
	CopyImage(g->text, parts.text);
	g->ticker = true;
	g->areaX = parts.area.x();
	g->areaY = parts.area.y();
	g->areaW = parts.area.width();
	g->speed = (float)std::clamp(speed, 10, 1000);
	g->width = g->main.w;
	g->height = g->main.h;
}

} // namespace meikou
