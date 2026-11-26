#pragma once

#include <QWidget>

#include <obs.h>

#include <functional>

namespace meikou {

class Engine;

// 用 libobs 的 obs_display 直接在原生視窗上以 GPU 繪製畫面。
// Program：整個播出畫面（含疊加）；Preview：預覽訊號源＋預覽中的疊加，可拖曳疊加、滾輪縮放。
class Display : public QWidget {
	Q_OBJECT

public:
	enum class Mode { Program, Preview };

	Display(Engine &engine, Mode mode, QWidget *parent = nullptr);
	~Display() override;

	QPaintEngine *paintEngine() const override { return nullptr; }
	void DestroyDisplay();

	std::function<void()> onOverlayChanged; // 拖曳或縮放後通知介面存檔

protected:
	void showEvent(QShowEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;

private:
	void CreateDisplay();
	QSize PixelSize() const;
	bool ToCanvas(const QPointF &widgetPos, float &x, float &y) const;
	static void Draw(void *param, uint32_t cx, uint32_t cy);

	Engine &engine_;
	Mode mode_;
	obs_display_t *display_ = nullptr;

	std::string dragUid_;
	float lastX_ = 0, lastY_ = 0;
};

} // namespace meikou
