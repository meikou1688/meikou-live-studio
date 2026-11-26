#include "FlowLayout.hpp"

#include <QWidget>

#include <algorithm>

namespace meikou {

FlowLayout::FlowLayout(QWidget *parent, int hSpacing, int vSpacing) : QLayout(parent), hSpace_(hSpacing), vSpace_(vSpacing)
{
	setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout()
{
	while (QLayoutItem *item = takeAt(0))
		delete item;
}

void FlowLayout::addItem(QLayoutItem *item)
{
	items_.append(item);
}

int FlowLayout::count() const
{
	return (int)items_.size();
}

QLayoutItem *FlowLayout::itemAt(int index) const
{
	return index >= 0 && index < items_.size() ? items_.at(index) : nullptr;
}

QLayoutItem *FlowLayout::takeAt(int index)
{
	return index >= 0 && index < items_.size() ? items_.takeAt(index) : nullptr;
}

Qt::Orientations FlowLayout::expandingDirections() const
{
	return {};
}

bool FlowLayout::hasHeightForWidth() const
{
	return true;
}

int FlowLayout::heightForWidth(int width) const
{
	return DoLayout(QRect(0, 0, width, 0), true);
}

void FlowLayout::setGeometry(const QRect &rect)
{
	QLayout::setGeometry(rect);
	DoLayout(rect, false);
}

QSize FlowLayout::sizeHint() const
{
	return minimumSize();
}

QSize FlowLayout::minimumSize() const
{
	QSize size;
	for (const QLayoutItem *item : items_)
		size = size.expandedTo(item->minimumSize());
	const QMargins m = contentsMargins();
	size += QSize(m.left() + m.right(), m.top() + m.bottom());
	return size;
}

int FlowLayout::DoLayout(const QRect &rect, bool testOnly) const
{
	const QMargins m = contentsMargins();
	const QRect area = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
	int x = area.x(), y = area.y(), lineHeight = 0;
	for (QLayoutItem *item : items_) {
		if (item->widget() && item->widget()->isHidden())
			continue;
		const QSize hint = item->sizeHint();
		int nextX = x + hint.width() + hSpace_;
		if (nextX - hSpace_ > area.right() + 1 && lineHeight > 0) {
			x = area.x();
			y += lineHeight + vSpace_;
			nextX = x + hint.width() + hSpace_;
			lineHeight = 0;
		}
		if (!testOnly)
			item->setGeometry(QRect(QPoint(x, y), hint));
		x = nextX;
		lineHeight = std::max(lineHeight, hint.height());
	}
	return y + lineHeight - rect.y() + m.bottom();
}

} // namespace meikou
