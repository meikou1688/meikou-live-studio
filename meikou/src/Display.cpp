#include "Display.hpp"

#include "Engine.hpp"

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

	display_ = obs_display_create(&info, 0x0B0C10);
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

bool Display::ToCanvas(const QPointF &p, float &x, float &y) const
{
	const float cw = 1920.0f, ch = 1080.0f;
	const float w = (float)width(), h = (float)height();
	const float scale = std::min(w / cw, h / ch);
	const float ox = (w - cw * scale) / 2.0f, oy = (h - ch * scale) / 2.0f;
	x = ((float)p.x() - ox) / scale;
	y = ((float)p.y() - oy) / scale;
	return x >= 0 && y >= 0 && x <= cw && y <= ch;
}

float Display::CanvasPerPixel() const
{
	const float scale = std::min((float)width() / 1920.0f, (float)height() / 1080.0f);
	return scale > 0.0f ? 1.0f / scale : 1.0f;
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
	dragged_ = false;
	dragCorner_ = HitHandle(x, y);
	if (dragCorner_ >= 0) {
		drag_ = engine_.Selection();
		return;
	}
	EditTarget t = engine_.HitPreview(x, y);
	engine_.SetSelection(t);
	// 填滿整個畫面的圖層只選取、不拖動，避免不小心把主畫面拖歪；要縮小請拖角落
	drag_ = (t.Valid() && !engine_.IsLocked(t)) ? t : EditTarget();
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
	drag_ = EditTarget();
	dragCorner_ = -1;
	dragged_ = false;
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

	const float scale = std::min((float)cx / (float)ovi.base_width, (float)cy / (float)ovi.base_height);
	const int w = (int)(ovi.base_width * scale);
	const int h = (int)(ovi.base_height * scale);
	const int x = ((int)cx - w) / 2;
	const int y = ((int)cy - h) / 2;

	gs_viewport_push();
	gs_projection_push();
	gs_ortho(0.0f, (float)ovi.base_width, 0.0f, (float)ovi.base_height, -100.0f, 100.0f);
	gs_set_viewport(x, y, w, h);
	if (self->mode_ == Mode::Program)
		obs_render_main_texture();
	else
		self->engine_.RenderPreview(10.0f / scale); // 控制點約 10 個實際像素
	gs_projection_pop();
	gs_viewport_pop();
}

} // namespace meikou
