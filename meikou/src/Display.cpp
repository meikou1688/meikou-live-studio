#include "Display.hpp"

#include "Engine.hpp"

#include <graphics/vec4.h>

#include <QMenu>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace meikou {

Display::Display(Engine &engine, Mode mode, QWidget *parent) : QWidget(parent), engine_(engine), mode_(mode)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_NativeWindow);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setMinimumSize(320, 180);
	setMouseTracking(mode == Mode::Preview);
	setFocusPolicy(Qt::NoFocus);
}

Display::~Display()
{
	DestroyDisplay();
}

QSize Display::PixelSize() const
{
	const qreal ratio = devicePixelRatioF();
	return QSize(std::max(1, (int)(width() * ratio)), std::max(1, (int)(height() * ratio)));
}

void Display::CreateDisplay()
{
	if (display_ || !windowHandle())
		return;

	QSize px = PixelSize();
	struct gs_init_data info = {};
	info.cx = (uint32_t)px.width();
	info.cy = (uint32_t)px.height();
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
#if defined(_WIN32)
	info.window.hwnd = (void *)winId();
#elif defined(__APPLE__)
	info.window.view = (id)winId();
#else
	info.window.id = (uint32_t)winId();
#endif

	display_ = obs_display_create(&info, 0x2A2622); // 畫面外的區域用深灰（0xBBGGRR），和黑色畫面區分
	if (display_)
		obs_display_add_draw_callback(display_, Draw, this);
}

void Display::DestroyDisplay()
{
	if (display_) {
		obs_display_remove_draw_callback(display_, Draw, this);
		obs_display_destroy(display_);
		display_ = nullptr;
	}
}

void Display::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	CreateDisplay();
}

void Display::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	if (!display_) {
		CreateDisplay();
		return;
	}
	QSize px = PixelSize();
	obs_display_resize(display_, (uint32_t)px.width(), (uint32_t)px.height());
}

void Display::paintEvent(QPaintEvent *) {}

QRectF Display::ComputeView() const
{
	const QRectF canvas(0, 0, 1920, 1080);
	if (mode_ != Mode::Preview)
		return canvas;
	const EditTarget sel = engine_.Selection();
	CanvasRect r;
	if (!sel.Valid() || !engine_.TargetRect(sel, r))
		return canvas;
	const QRectF item(QPointF(std::min(r.x0, r.x1), std::min(r.y0, r.y1)),
			  QPointF(std::max(r.x0, r.x1), std::max(r.y0, r.y1)));
	if (canvas.contains(item))
		return canvas;
	// 圖層有一部分在畫面外：把畫面和圖層一起放進來，四周留一點空間
	QRectF u = canvas.united(item);
	const double maxSpan = 1920.0 * 12; // 太遠的話最多縮到 12 倍
	if (u.width() > maxSpan || u.height() > maxSpan * 9 / 16)
		u = u.intersected(QRectF(-maxSpan / 2, -maxSpan * 9 / 32, maxSpan + 1920, maxSpan * 9 / 16 + 1080));
	const double m = std::max(u.width(), u.height()) * 0.04;
	return u.adjusted(-m, -m, m, m);
}

QRectF Display::ViewRect() const
{
	std::lock_guard<std::mutex> lock(viewMutex_);
	return viewFrozen_ ? frozenView_ : ComputeView();
}

void Display::Mapping(float pixelW, float pixelH, float &scale, float &left, float &top) const
{
	const QRectF v = ViewRect();
	scale = std::min(pixelW / (float)v.width(), pixelH / (float)v.height());
	if (scale <= 0.0f)
		scale = 1.0f;
	left = (float)v.x() - (pixelW - (float)v.width() * scale) / 2.0f / scale;
	top = (float)v.y() - (pixelH - (float)v.height() * scale) / 2.0f / scale;
}

bool Display::ToCanvas(const QPointF &p, float &x, float &y) const
{
	float scale, left, top;
	Mapping((float)width(), (float)height(), scale, left, top);
	x = left + (float)p.x() / scale;
	y = top + (float)p.y() / scale;
	return x >= 0 && y >= 0 && x <= 1920.0f && y <= 1080.0f;
}

float Display::CanvasPerPixel() const
{
	float scale, left, top;
	Mapping((float)width(), (float)height(), scale, left, top);
	return 1.0f / scale;
}

int Display::HitHandle(float x, float y) const
{
	const EditTarget sel = engine_.Selection();
	CanvasRect r;
	if (!sel.Valid() || !engine_.TargetRect(sel, r))
		return -1;
	const float tol = 9.0f * CanvasPerPixel();
	const float cx[4] = {r.x0, r.x1, r.x1, r.x0};
	const float cy[4] = {r.y0, r.y0, r.y1, r.y1};
	for (int i = 0; i < 4; i++)
		if (std::fabs(x - cx[i]) <= tol && std::fabs(y - cy[i]) <= tol)
			return i;
	return -1;
}

void Display::mousePressEvent(QMouseEvent *event)
{
	if (mode_ != Mode::Preview)
		return;
	float x, y;
	ToCanvas(event->position(), x, y);

	if (event->button() == Qt::RightButton) {
		EditTarget t = engine_.HitPreview(x, y);
		engine_.SetSelection(t);
		if (t.Valid())
			ShowContextMenu(event->globalPosition().toPoint(), t);
		return;
	}
	if (event->button() != Qt::LeftButton)
		return;

	lastX_ = x;
	lastY_ = y;
	pressX_ = x;
	pressY_ = y;
	dragged_ = false;
	cycleOnRelease_ = false;
	{
		// 拖曳期間固定顯示範圍
		std::lock_guard<std::mutex> lock(viewMutex_);
		frozenView_ = ComputeView();
		viewFrozen_ = true;
	}
	dragCorner_ = HitHandle(x, y);
	if (dragCorner_ >= 0) {
		drag_ = engine_.Selection();
		return;
	}
	// 點在目前選取的圖層上：先保留（可以直接拖曳）；放開時沒拖動，就改選它下面的那一層
	const std::vector<EditTarget> hits = engine_.HitPreviewAll(x, y);
	const EditTarget cur = engine_.Selection();
	EditTarget t;
	if (cur.Valid() && std::find(hits.begin(), hits.end(), cur) != hits.end()) {
		t = cur;
		cycleOnRelease_ = hits.size() > 1;
	} else if (!hits.empty()) {
		t = hits.front();
	}
	engine_.SetSelection(t);
	// 填滿整個畫面的圖層只選取、不拖動，避免不小心把主畫面拖歪；要縮小請拖角落
	drag_ = (t.Valid() && !engine_.IsLocked(t)) ? t : EditTarget();
	if (onSelectionChanged)
		onSelectionChanged();
}

void Display::mouseMoveEvent(QMouseEvent *event)
{
	float x, y;
	ToCanvas(event->position(), x, y);
	if (!drag_.Valid()) {
		if (mode_ == Mode::Preview && !(event->buttons() & Qt::LeftButton)) {
			const int corner = HitHandle(x, y);
			if (corner == 0 || corner == 2)
				setCursor(Qt::SizeFDiagCursor);
			else if (corner == 1 || corner == 3)
				setCursor(Qt::SizeBDiagCursor);
			else {
				EditTarget t = engine_.HitPreview(x, y);
				setCursor(t.Valid() && !engine_.IsLocked(t) ? Qt::SizeAllCursor : Qt::ArrowCursor);
			}
		}
		return;
	}
	if (dragCorner_ >= 0)
		engine_.ResizeTarget(drag_, dragCorner_, x, y);
	else
		engine_.MoveTarget(drag_, x - lastX_, y - lastY_);
	dragged_ = true;
	lastX_ = x;
	lastY_ = y;
}

void Display::mouseReleaseEvent(QMouseEvent *)
{
	if (drag_.Valid() && dragged_ && onOverlayChanged)
		onOverlayChanged();
	if (cycleOnRelease_ && !dragged_) {
		// 同一點再點一次：選下一層（到最底層後回到最上層）
		const std::vector<EditTarget> hits = engine_.HitPreviewAll(pressX_, pressY_);
		const EditTarget cur = engine_.Selection();
		auto it = std::find(hits.begin(), hits.end(), cur);
		if (it != hits.end() && hits.size() > 1) {
			++it;
			engine_.SetSelection(it == hits.end() ? hits.front() : *it);
			if (onSelectionChanged)
				onSelectionChanged();
		}
	}
	cycleOnRelease_ = false;
	drag_ = EditTarget();
	dragCorner_ = -1;
	dragged_ = false;
	std::lock_guard<std::mutex> lock(viewMutex_);
	viewFrozen_ = false;
}

void Display::wheelEvent(QWheelEvent *event)
{
	if (mode_ != Mode::Preview)
		return;
	float x, y;
	ToCanvas(event->position(), x, y);
	EditTarget t = engine_.HitPreview(x, y);
	if (!t.Valid())
		return;
	engine_.SetSelection(t);
	engine_.ScaleTarget(t, event->angleDelta().y() > 0 ? 1.05f : 1.0f / 1.05f);
	if (onOverlayChanged)
		onOverlayChanged();
	event->accept();
}

void Display::ShowContextMenu(const QPoint &globalPos, const EditTarget &t)
{
	QMenu menu(this);
	const QString name = QString::fromUtf8(engine_.TargetName(t).c_str());
	menu.addSection(name.isEmpty() ? QStringLiteral("圖層") : name);
	QAction *fill = nullptr, *top = nullptr, *up = nullptr, *down = nullptr, *bottom = nullptr, *remove = nullptr,
		*reset = nullptr;
	if (t.layerId) {
		fill = menu.addAction(QStringLiteral("填滿畫面"));
		menu.addSeparator();
		top = menu.addAction(QStringLiteral("移到最上層"));
		up = menu.addAction(QStringLiteral("上移一層"));
		down = menu.addAction(QStringLiteral("下移一層"));
		bottom = menu.addAction(QStringLiteral("移到最下層"));
		Input *pvw = engine_.Preview();
		if (pvw && pvw->isScene) {
			menu.addSeparator();
			remove = menu.addAction(QStringLiteral("從這個畫面移除"));
		}
	} else {
		reset = menu.addAction(QStringLiteral("重設位置與大小"));
	}
	QAction *chosen = menu.exec(globalPos);
	if (!chosen)
		return;
	if (chosen == fill || chosen == reset)
		engine_.LayerFill(t);
	else if (chosen == top)
		engine_.LayerOrder(t, OBS_ORDER_MOVE_TOP);
	else if (chosen == up)
		engine_.LayerOrder(t, OBS_ORDER_MOVE_UP);
	else if (chosen == down)
		engine_.LayerOrder(t, OBS_ORDER_MOVE_DOWN);
	else if (chosen == bottom)
		engine_.LayerOrder(t, OBS_ORDER_MOVE_BOTTOM);
	else if (chosen == remove) {
		engine_.LayerRemove(t);
		if (onLayersChanged)
			onLayersChanged();
	}
	if (onOverlayChanged)
		onOverlayChanged();
}

void Display::Draw(void *param, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<Display *>(param);
	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi) || !ovi.base_width || !ovi.base_height)
		return;

	// 顯示範圍平常就是 1920×1080 畫面；選到畫面外的圖層時會自動放大範圍
	float scale, left, top;
	self->Mapping((float)cx, (float)cy, scale, left, top);

	gs_viewport_push();
	gs_projection_push();
	gs_ortho(left, left + (float)cx / scale, top, top + (float)cy / scale, -100.0f, 100.0f);
	gs_set_viewport(0, 0, (int)cx, (int)cy);
	if (self->mode_ == Mode::Program)
		obs_render_main_texture();
	else
		self->engine_.RenderPreview(10.0f / scale); // 控制點約 10 個實際像素

	// 蒂芬尼藍邊框：標出 1920×1080 畫面邊界，只畫在監看視窗上，不會進到輸出
	{
		const float t = 2.0f / scale; // 約 2 個實際像素
		const float W = (float)ovi.base_width, H = (float)ovi.base_height;
		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
		struct vec4 c;
		vec4_set(&c, 129.0f / 255.0f, 216.0f / 255.0f, 208.0f / 255.0f, 1.0f); // #81D8D0
		gs_effect_set_vec4(color, &c);
		while (gs_effect_loop(solid, "Solid")) {
			const float rects[4][4] = {{0, 0, W, t}, {0, H - t, W, H}, {0, 0, t, H}, {W - t, 0, W, H}};
			for (const auto &r : rects) {
				gs_render_start(true);
				gs_vertex2f(r[0], r[1]);
				gs_vertex2f(r[2], r[1]);
				gs_vertex2f(r[0], r[3]);
				gs_vertex2f(r[2], r[3]);
				gs_render_stop(GS_TRISTRIP);
			}
		}
	}
	gs_projection_pop();
	gs_viewport_pop();
}

} // namespace meikou
