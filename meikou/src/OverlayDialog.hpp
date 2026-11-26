#pragma once

#include "GraphicsRender.hpp"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;

namespace meikou {

// 疊加素材的設計視窗：選版型、改配色與文字，右邊即時預覽
class OverlayDialog : public QDialog {
	Q_OBJECT

public:
	OverlayDialog(const GraphicSpec &spec, const QString &name, QWidget *parent = nullptr);

	GraphicSpec Spec() const { return spec_; }
	QString Name() const;

private:
	void Collect();
	void UpdatePreview();
	void UpdateColorButtons();
	void PickColor(uint32_t &target);

	GraphicSpec spec_;
	QLineEdit *name_ = nullptr;
	QLineEdit *text1_ = nullptr, *text2_ = nullptr, *label_ = nullptr, *file_ = nullptr;
	QComboBox *style_ = nullptr;
	QPushButton *c1_ = nullptr, *c2_ = nullptr, *tc_ = nullptr;
	QSpinBox *size_ = nullptr, *opacity_ = nullptr;
	QSlider *speed_ = nullptr;
	QCheckBox *seconds_ = nullptr, *h12_ = nullptr, *date_ = nullptr;
	QLabel *preview_ = nullptr;
	QTimer *clockTimer_ = nullptr;
};

} // namespace meikou
