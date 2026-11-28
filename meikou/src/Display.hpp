#pragma once

#include <QRectF>
#include <QWidget>

#include "Engine.hpp"

#include <obs.h>

#include <functional>
#include <mutex>

namespace meikou {

class Engine;

// 用 libobs 的 obs_display 直接在原生視窗上以 GPU 繪製畫面。
// Program：整個播出畫面（含疊加）；Preview：預覽訊號源＋預覽中的疊加，可拖曳疊加、滾輪縮放。
class Display : public QWidget {
	Q_OBJECT

public:
	// 給自我檢查用：目前顯示範圍（畫布座標）、是不是預覽視窗
	QRectF CurrentView() const { return ViewRect(); }
	bool IsPreview() const { return mode_ == Mode::Preview; }

	enum class Mode { Program, Preview };

	Display(Engine &engine, Mode mode, QWidget *parent = nullptr);
	~Display() override;

	QPaintEngine *paintEngine() const override { return nullptr; }
	void DestroyDisplay();

	std::function<void()> onOverlayChanged; // 拖曳、縮放、圖層變更後通知介面存檔
	std::function<void()> onLayersChanged;    // 圖層增刪後通知介面重建列表
	std::function<void()> onSelectionChanged; // 在預覽點選了別的圖層

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
	// 監看視窗顯示的範圍（畫布座標）。選到畫面外的圖層時自動放大範圍，把它一起顯示出來
	QRectF ViewRect() const;
	QRectF ComputeView() const;
	void Mapping(float pixelW, float pixelH, float &scale, float &left, float &top) const;
	float CanvasPerPixel() const;
	int HitHandle(float x, float y) const; // 目前選取物件的哪個角（-1 表示沒有）
	void ShowContextMenu(const QPoint &globalPos, const EditTarget &t);
	static void Draw(void *param, uint32_t cx, uint32_t cy);

	Engine &engine_;
	Mode mode_;
	obs_display_t *display_ = nullptr;

	EditTarget drag_;
	int dragCorner_ = -1; // >=0 表示正在拖角落縮放
	bool dragged_ = false;
	float lastX_ = 0, lastY_ = 0;
	// 拖曳時固定顯示範圍，避免一邊拖、畫面一邊縮放
	mutable std::mutex viewMutex_;
	bool viewFrozen_ = false;
	QRectF frozenView_;
	// 在同一點再點一次（沒有拖曳）＝選下一層
	bool cycleOnRelease_ = false;
	float pressX_ = 0, pressY_ = 0;
};

} // namespace meikou
