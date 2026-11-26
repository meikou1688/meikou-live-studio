#pragma once

#include <QLayout>
#include <QList>

namespace meikou {

// 由左到右排列，放不下就換行（讓一個區塊裡放得下更多彩色按鈕）
class FlowLayout : public QLayout {
public:
	explicit FlowLayout(QWidget *parent = nullptr, int hSpacing = 6, int vSpacing = 6);
	~FlowLayout() override;

	void addItem(QLayoutItem *item) override;
	int count() const override;
	QLayoutItem *itemAt(int index) const override;
	QLayoutItem *takeAt(int index) override;
	Qt::Orientations expandingDirections() const override;
	bool hasHeightForWidth() const override;
	int heightForWidth(int width) const override;
	void setGeometry(const QRect &rect) override;
	QSize sizeHint() const override;
	QSize minimumSize() const override;

private:
	int DoLayout(const QRect &rect, bool testOnly) const;
	QList<QLayoutItem *> items_;
	int hSpace_, vSpace_;
};

} // namespace meikou
