#include "Display.hpp"

#include "Engine.hpp"

#include <QMouseEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWheelEvent>

#include <algorithm>

namespace meikou {

Display::Display(Engine &engine, Mode mode, QWidget *parent) : QWidget(parent), engine_(engine), mode_(mode)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_NativeWindow);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setMinimumSize(320, 180);
	setMouseTracking(false);
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

void Display::mousePressEvent(QMouseEvent *event)
{
	if (mode_ != Mode::Preview || event->button() != Qt::LeftButton)
		return;
	float x, y;
	ToCanvas(event->position(), x, y);
	Overlay *ov = engine_.HitOverlay(x, y);
	dragUid_ = ov ? ov->uid : std::string();
	engine_.SetSelectedOverlay(dragUid_);
	lastX_ = x;
	lastY_ = y;
}

void Display::mouseMoveEvent(QMouseEvent *event)
{
	if (dragUid_.empty())
		return;
	float x, y;
	ToCanvas(event->position(), x, y);
	engine_.MoveOverlay(engine_.FindOverlay(dragUid_), x - lastX_, y - lastY_);
	lastX_ = x;
	lastY_ = y;
}

void Display::mouseReleaseEvent(QMouseEvent *)
{
	if (!dragUid_.empty() && onOverlayChanged)
		onOverlayChanged();
	dragUid_.clear();
}

void Display::wheelEvent(QWheelEvent *event)
{
	if (mode_ != Mode::Preview)
		return;
	float x, y;
	ToCanvas(event->position(), x, y);
	Overlay *ov = engine_.HitOverlay(x, y);
	if (!ov)
		return;
	engine_.SetSelectedOverlay(ov->uid);
	engine_.ScaleOverlay(ov, event->angleDelta().y() > 0 ? 1.05f : 1.0f / 1.05f);
	if (onOverlayChanged)
		onOverlayChanged();
	event->accept();
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
		self->engine_.RenderPreview();
	gs_projection_pop();
	gs_viewport_pop();
}

} // namespace meikou
