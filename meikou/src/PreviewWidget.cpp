#include "PreviewWidget.hpp"

#include <QResizeEvent>
#include <QShowEvent>

#include <algorithm>

namespace meikou {

PreviewWidget::PreviewWidget(QWidget *parent) : QWidget(parent)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_NativeWindow);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setMinimumSize(320, 180);
}

PreviewWidget::~PreviewWidget()
{
	DestroyDisplay();
}

QSize PreviewWidget::PixelSize() const
{
	const qreal ratio = devicePixelRatioF();
	return QSize(std::max(1, (int)(width() * ratio)), std::max(1, (int)(height() * ratio)));
}

void PreviewWidget::CreateDisplay()
{
	if (display_ || !windowHandle())
		return;

	QSize px = PixelSize();
	struct gs_init_data info = {};
	info.cx = (uint32_t)px.width();
	info.cy = (uint32_t)px.height();
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
	info.window.hwnd = (void *)winId();

	display_ = obs_display_create(&info, 0x101014);
	if (display_)
		obs_display_add_draw_callback(display_, Draw, this);
}

void PreviewWidget::DestroyDisplay()
{
	if (display_) {
		obs_display_remove_draw_callback(display_, Draw, this);
		obs_display_destroy(display_);
		display_ = nullptr;
	}
}

void PreviewWidget::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	CreateDisplay();
}

void PreviewWidget::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	if (!display_) {
		CreateDisplay();
		return;
	}
	QSize px = PixelSize();
	obs_display_resize(display_, (uint32_t)px.width(), (uint32_t)px.height());
}

void PreviewWidget::paintEvent(QPaintEvent *) {}

void PreviewWidget::Draw(void *, uint32_t cx, uint32_t cy)
{
	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi) || !ovi.base_width || !ovi.base_height)
		return;

	// 等比例置中，四周留黑邊
	const float scale = std::min((float)cx / (float)ovi.base_width, (float)cy / (float)ovi.base_height);
	const int w = (int)(ovi.base_width * scale);
	const int h = (int)(ovi.base_height * scale);
	const int x = ((int)cx - w) / 2;
	const int y = ((int)cy - h) / 2;

	gs_viewport_push();
	gs_projection_push();
	gs_ortho(0.0f, (float)ovi.base_width, 0.0f, (float)ovi.base_height, -100.0f, 100.0f);
	gs_set_viewport(x, y, w, h);
	obs_render_main_texture();
	gs_projection_pop();
	gs_viewport_pop();
}

} // namespace meikou
