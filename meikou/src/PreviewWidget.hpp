#pragma once

#include <QWidget>

#include <obs.h>

namespace meikou {

// 用 libobs 的 obs_display 直接在原生視窗上畫出播出畫面（GPU 繪製，不經過 Qt）。
class PreviewWidget : public QWidget {
	Q_OBJECT

public:
	explicit PreviewWidget(QWidget *parent = nullptr);
	~PreviewWidget() override;

	QPaintEngine *paintEngine() const override { return nullptr; }
	void DestroyDisplay();

protected:
	void showEvent(QShowEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void paintEvent(QPaintEvent *event) override;

private:
	void CreateDisplay();
	QSize PixelSize() const;
	static void Draw(void *param, uint32_t cx, uint32_t cy);

	obs_display_t *display_ = nullptr;
};

} // namespace meikou
