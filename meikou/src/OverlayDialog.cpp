#include "OverlayDialog.hpp"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace meikou {

namespace {

QString Q(const std::string &s)
{
	return QString::fromUtf8(s.c_str());
}

std::string S(const QString &s)
{
	return s.toUtf8().toStdString();
}

QString KindTitle(const std::string &kind)
{
	if (kind == "lower")
		return QStringLiteral("姓名條");
	if (kind == "ticker")
		return QStringLiteral("跑馬燈");
	if (kind == "clock")
		return QStringLiteral("時鐘");
	if (kind == "logo")
		return QStringLiteral("台標");
	if (kind == "text")
		return QStringLiteral("文字");
	return QStringLiteral("圖片");
}

} // namespace

OverlayDialog::OverlayDialog(const GraphicSpec &spec, const QString &name, QWidget *parent)
	: QDialog(parent),
	  spec_(spec)
{
	setWindowTitle(QStringLiteral("設計%1").arg(KindTitle(spec_.kind)));
	setMinimumWidth(980);

	auto *root = new QHBoxLayout(this);
	auto *left = new QVBoxLayout;
	auto *form = new QFormLayout;
	left->addLayout(form);
	root->addLayout(left, 0);

	name_ = new QLineEdit(name);
	form->addRow(QStringLiteral("按鈕名稱"), name_);

	const std::string &k = spec_.kind;
	if (k == "lower") {
		text1_ = new QLineEdit(Q(spec_.text1));
		text2_ = new QLineEdit(Q(spec_.text2));
		form->addRow(QStringLiteral("姓名"), text1_);
		form->addRow(QStringLiteral("職稱／說明"), text2_);
	} else if (k == "ticker") {
		label_ = new QLineEdit(Q(spec_.label));
		label_->setPlaceholderText(QStringLiteral("留空就不顯示標籤"));
		text1_ = new QLineEdit(Q(spec_.text1));
		form->addRow(QStringLiteral("標籤"), label_);
		form->addRow(QStringLiteral("內容"), text1_);
		speed_ = new QSlider(Qt::Horizontal);
		speed_->setRange(30, 400);
		speed_->setValue(spec_.speed);
		form->addRow(QStringLiteral("捲動速度"), speed_);
	} else if (k == "clock") {
		text1_ = new QLineEdit(Q(spec_.text1));
		text1_->setPlaceholderText(QStringLiteral("例如 LIVE、台北"));
		form->addRow(QStringLiteral("前置文字"), text1_);
		seconds_ = new QCheckBox(QStringLiteral("顯示秒數"));
		seconds_->setChecked(spec_.showSeconds);
		h12_ = new QCheckBox(QStringLiteral("12 小時制"));
		h12_->setChecked(spec_.h12);
		date_ = new QCheckBox(QStringLiteral("顯示日期"));
		date_->setChecked(spec_.showDate);
		auto *row = new QHBoxLayout;
		row->addWidget(seconds_);
		row->addWidget(h12_);
		row->addWidget(date_);
		form->addRow(QString(), row);
	} else if (k == "text") {
		text1_ = new QLineEdit(Q(spec_.text1));
		form->addRow(QStringLiteral("文字"), text1_);
	}
	if (k == "logo" || k == "image") {
		file_ = new QLineEdit(Q(spec_.file));
		auto *browse = new QPushButton(QStringLiteral("瀏覽…"));
		auto *row = new QHBoxLayout;
		row->addWidget(file_, 1);
		row->addWidget(browse);
		connect(browse, &QPushButton::clicked, this, [this] {
			const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("選擇圖片"), file_->text(),
								       QStringLiteral("圖片 (*.png *.jpg *.jpeg *.gif *.webp *.bmp)"));
			if (f.isEmpty())
				return;
			file_->setText(f);
			if (spec_.kind == "logo" && spec_.style == "text") {
				style_->setCurrentIndex(0); // 選了圖片就改用「原圖」
			}
			UpdatePreview();
		});
		form->addRow(QStringLiteral("圖片"), row);
	}
	if (k == "logo") {
		text1_ = new QLineEdit(Q(spec_.text1));
		text2_ = new QLineEdit(Q(spec_.text2));
		form->addRow(QStringLiteral("台標文字"), text1_);
		form->addRow(QStringLiteral("副標"), text2_);
	}

	const auto styles = GraphicStyles(k);
	if (!styles.empty()) {
		style_ = new QComboBox;
		for (const auto &st : styles)
			style_->addItem(Q(st.name), Q(st.id));
		style_->setCurrentIndex(std::max(0, style_->findData(Q(spec_.style))));
		form->addRow(QStringLiteral("版型"), style_);
		connect(style_, &QComboBox::activated, this, [this](int) {
			Collect();
			spec_.style = S(style_->currentData().toString());
			ApplyStyleColors(spec_); // 換版型時套用建議配色，之後仍可自己改
			if (opacity_)
				opacity_->setValue(spec_.opacity);
			UpdateColorButtons();
			UpdatePreview();
		});
	}

	if (k != "image") {
		c1_ = new QPushButton;
		c2_ = new QPushButton;
		tc_ = new QPushButton;
		connect(c1_, &QPushButton::clicked, this, [this] { PickColor(spec_.color); });
		connect(c2_, &QPushButton::clicked, this, [this] { PickColor(spec_.color2); });
		connect(tc_, &QPushButton::clicked, this, [this] { PickColor(spec_.textColor); });
		auto *colors = new QHBoxLayout;
		colors->addWidget(c1_);
		colors->addWidget(c2_);
		colors->addWidget(tc_);
		form->addRow(QStringLiteral("主色／輔色／文字"), colors);
		UpdateColorButtons();

		size_ = new QSpinBox;
		size_->setRange(40, 300);
		size_->setSingleStep(5);
		size_->setSuffix(QStringLiteral(" %"));
		size_->setValue(spec_.size);
		form->addRow(QStringLiteral("字級"), size_);
	}
	opacity_ = new QSpinBox;
	opacity_->setRange(5, 100);
	opacity_->setSingleStep(5);
	opacity_->setSuffix(QStringLiteral(" %"));
	opacity_->setValue(spec_.opacity);
	if (k != "image")
		form->addRow(QStringLiteral("不透明度"), opacity_);
	else
		opacity_->hide();

	auto *hint = new QLabel(QStringLiteral("文字都使用思源黑體（粗）。位置與大小可以在預覽畫面裡直接拖曳、拖四個角縮放。"));
	hint->setWordWrap(true);
	hint->setStyleSheet(QStringLiteral("color:#8a90a0"));
	left->addWidget(hint);
	left->addStretch(1);
	auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	bb->button(QDialogButtonBox::Ok)->setText(QStringLiteral("確定"));
	bb->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
	connect(bb, &QDialogButtonBox::accepted, this, [this] {
		Collect();
		accept();
	});
	connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
	left->addWidget(bb);

	auto *pg = new QGroupBox(QStringLiteral("預覽"));
	auto *pl = new QVBoxLayout(pg);
	preview_ = new QLabel;
	preview_->setFixedSize(600, 340);
	preview_->setAlignment(Qt::AlignCenter);
	pl->addWidget(preview_);
	root->addWidget(pg, 1);

	// 任何欄位改變就更新預覽
	auto refresh = [this] { UpdatePreview(); };
	for (QLineEdit *e : {name_, text1_, text2_, label_, file_})
		if (e)
			connect(e, &QLineEdit::textChanged, this, refresh);
	for (QSpinBox *b : {size_, opacity_})
		if (b)
			connect(b, &QSpinBox::valueChanged, this, refresh);
	for (QCheckBox *c : {seconds_, h12_, date_})
		if (c)
			connect(c, &QCheckBox::toggled, this, refresh);
	if (speed_)
		connect(speed_, &QSlider::valueChanged, this, refresh);
	if (k == "clock") {
		clockTimer_ = new QTimer(this);
		connect(clockTimer_, &QTimer::timeout, this, refresh);
		clockTimer_->start(1000);
	}
	UpdatePreview();
}

QString OverlayDialog::Name() const
{
	const QString n = name_->text().trimmed();
	return n.isEmpty() ? KindTitle(spec_.kind) : n;
}

void OverlayDialog::Collect()
{
	if (text1_)
		spec_.text1 = S(text1_->text());
	if (text2_)
		spec_.text2 = S(text2_->text());
	if (label_)
		spec_.label = S(label_->text());
	if (file_)
		spec_.file = S(file_->text().trimmed());
	if (style_)
		spec_.style = S(style_->currentData().toString());
	if (size_)
		spec_.size = size_->value();
	if (opacity_)
		spec_.opacity = opacity_->value();
	if (speed_)
		spec_.speed = speed_->value();
	if (seconds_)
		spec_.showSeconds = seconds_->isChecked();
	if (h12_)
		spec_.h12 = h12_->isChecked();
	if (date_)
		spec_.showDate = date_->isChecked();
}

void OverlayDialog::UpdateColorButtons()
{
	auto paint = [](QPushButton *b, uint32_t argb, const QString &label) {
		if (!b)
			return;
		const QColor c = QColor::fromRgba(argb);
		b->setText(label);
		b->setStyleSheet(QStringLiteral("background:%1; color:%2; border:1px solid #555; min-width:70px;")
					 .arg(c.name(), c.lightness() > 140 ? QStringLiteral("#000") : QStringLiteral("#fff")));
	};
	paint(c1_, spec_.color, QStringLiteral("主色"));
	paint(c2_, spec_.color2, QStringLiteral("輔色"));
	paint(tc_, spec_.textColor, QStringLiteral("文字"));
}

void OverlayDialog::PickColor(uint32_t &target)
{
	const QColor c = QColorDialog::getColor(QColor::fromRgba(target), this, QString(), QColorDialog::ShowAlphaChannel);
	if (!c.isValid())
		return;
	target = c.rgba();
	UpdateColorButtons();
	UpdatePreview();
}

void OverlayDialog::UpdatePreview()
{
	Collect();
	QImage art;
	if (spec_.kind == "image") {
		art.load(Q(spec_.file));
	} else {
		art = RenderPreviewImage(spec_);
	}
	const QSize box(600, 340); // 固定大小，避免預覽圖把視窗越撐越大
	QImage bg(box, QImage::Format_ARGB32_Premultiplied);
	QPainter p(&bg);
	QLinearGradient lg(0, 0, box.width(), box.height());
	lg.setColorAt(0, QColor(58, 74, 104));
	lg.setColorAt(1, QColor(120, 98, 84));
	p.fillRect(bg.rect(), lg);
	if (!art.isNull()) {
		QImage scaled = art;
		if (art.width() > box.width() - 20 || art.height() > box.height() - 20)
			scaled = art.scaled(box.width() - 20, box.height() - 20, Qt::KeepAspectRatio, Qt::SmoothTransformation);
		p.drawImage((box.width() - scaled.width()) / 2, (box.height() - scaled.height()) / 2, scaled);
	}
	p.setPen(QColor(255, 255, 255, 160));
	p.drawText(QRect(8, box.height() - 24, box.width() - 16, 20), Qt::AlignRight,
		   art.isNull() ? QString() : QStringLiteral("實際大小 %1×%2").arg(art.width()).arg(art.height()));
	p.end();
	preview_->setPixmap(QPixmap::fromImage(bg));
}

} // namespace meikou
