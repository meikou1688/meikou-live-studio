#include "GraphicsRender.hpp"

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QTime>

#include <algorithm>
#include <cmath>

namespace meikou {

namespace {

QString U(const std::string &s)
{
	return QString::fromUtf8(s.c_str());
}

QColor C(uint32_t argb)
{
	return QColor::fromRgba(argb);
}

QColor A(QColor c, double alpha)
{
	c.setAlphaF(std::clamp(c.alphaF() * alpha, 0.0, 1.0));
	return c;
}

QFont Font(double px, QFont::Weight weight, bool italic = false)
{
	QFont f;
	f.setFamilies({QStringLiteral("Noto Sans TC"), QStringLiteral("Microsoft JhengHei"),
		       QStringLiteral("PingFang TC"), QStringLiteral("Heiti TC")});
	f.setPixelSize(std::max(8, (int)std::lround(px)));
	f.setWeight(weight);
	f.setItalic(italic);
	f.setStyleStrategy(QFont::PreferAntialias);
	f.setHintingPreference(QFont::PreferNoHinting);
	return f;
}

double Scale(const GraphicSpec &g)
{
	return std::clamp(g.size / 100.0, 0.4, 3.0);
}

QImage Canvas(double w, double h)
{
	QImage img(std::max(1, (int)std::ceil(w)), std::max(1, (int)std::ceil(h)), QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);
	return img;
}

void Begin(QPainter &p, QImage &img, const GraphicSpec &g)
{
	p.begin(&img);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setRenderHint(QPainter::TextAntialiasing, true);
	p.setRenderHint(QPainter::SmoothPixmapTransform, true);
	p.setOpacity(std::clamp(g.opacity, 5, 100) / 100.0);
}

double TextW(const QFont &f, const QString &s)
{
	return s.isEmpty() ? 0.0 : QFontMetricsF(f).horizontalAdvance(s);
}

// 文字一律轉成路徑畫，描邊和填色才會對齊
void DrawText(QPainter &p, const QFont &f, double x, double top, const QString &s, const QColor &fill,
	      double stroke = 0.0, const QColor &strokeColor = Qt::black)
{
	if (s.isEmpty())
		return;
	QPainterPath path;
	path.setFillRule(Qt::WindingFill); // 中文字的筆畫會重疊，要用 Winding 才不會出現破洞
	path.addText(x, top + QFontMetricsF(f).ascent(), f, s);
	if (stroke > 0.0) {
		QPen pen(strokeColor, stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
		p.strokePath(path, pen);
	}
	p.fillPath(path, fill);
}

// 數字等寬排列：時鐘每秒變化時寬度不會跳動
double MonoWidth(const QFont &f, const QString &s)
{
	QFontMetricsF fm(f);
	double digit = 0;
	for (QChar c = u'0'; c <= u'9'; c = QChar(c.unicode() + 1))
		digit = std::max(digit, fm.horizontalAdvance(c));
	double w = 0;
	for (QChar c : s)
		w += c.isDigit() ? digit : fm.horizontalAdvance(c);
	return w;
}

void DrawMono(QPainter &p, const QFont &f, double x, double top, const QString &s, const QColor &fill,
	      double stroke = 0.0, const QColor &strokeColor = Qt::black)
{
	QFontMetricsF fm(f);
	double digit = 0;
	for (QChar c = u'0'; c <= u'9'; c = QChar(c.unicode() + 1))
		digit = std::max(digit, fm.horizontalAdvance(c));
	QPainterPath path;
	path.setFillRule(Qt::WindingFill);
	const double base = top + fm.ascent();
	for (QChar c : s) {
		const double adv = c.isDigit() ? digit : fm.horizontalAdvance(c);
		const double off = c.isDigit() ? (digit - fm.horizontalAdvance(c)) / 2.0 : 0.0;
		path.addText(x + off, base, f, QString(c));
		x += adv;
	}
	if (stroke > 0.0)
		p.strokePath(path, QPen(strokeColor, stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	p.fillPath(path, fill);
}

// 柔和陰影：用幾層放大的半透明圖形疊出來
void SoftShadow(QPainter &p, const QPainterPath &shape, double spread, double dy, double strength = 0.35)
{
	p.save();
	p.setPen(Qt::NoPen);
	const int layers = 6;
	for (int i = layers; i >= 1; i--) {
		const double grow = spread * i / layers;
		QPainterPathStroker st;
		st.setWidth(grow * 2.0);
		st.setJoinStyle(Qt::RoundJoin);
		QPainterPath expanded = st.createStroke(shape).united(shape);
		expanded.translate(0, dy);
		p.setBrush(QColor(0, 0, 0, (int)(255 * strength / layers)));
		p.drawPath(expanded);
	}
	p.restore();
}

QPainterPath Rounded(const QRectF &r, double radius)
{
	QPainterPath path;
	path.addRoundedRect(r, radius, radius);
	return path;
}

// 右側斜切的長條（slant > 0 右下往左縮）
QPainterPath Slanted(double x, double y, double w, double h, double slant, double leftSlant = 0.0)
{
	QPainterPath path;
	path.moveTo(x + leftSlant, y);
	path.lineTo(x + w, y);
	path.lineTo(x + w - slant, y + h);
	path.lineTo(x, y + h);
	path.closeSubpath();
	return path;
}

QString TimeText(const GraphicSpec &g)
{
	const QTime t = QTime::currentTime();
	QString s;
	if (g.h12) {
		int h = t.hour() % 12;
		if (h == 0)
			h = 12;
		s = QString::number(h) + t.toString(g.showSeconds ? QStringLiteral(":mm:ss") : QStringLiteral(":mm"));
		s += t.hour() < 12 ? QStringLiteral(" AM") : QStringLiteral(" PM");
	} else {
		s = t.toString(g.showSeconds ? QStringLiteral("HH:mm:ss") : QStringLiteral("HH:mm"));
	}
	return s;
}

QString DateText()
{
	static const char *week[] = {"", "週一", "週二", "週三", "週四", "週五", "週六", "週日"};
	const QDate d = QDate::currentDate();
	return d.toString(QStringLiteral("yyyy/MM/dd ")) + QString::fromUtf8(week[d.dayOfWeek()]);
}

// ------------------------------------------------------------ 姓名條

QImage RenderLower(const GraphicSpec &g)
{
	const double s = Scale(g);
	const QString name = U(g.text1), title = U(g.text2);
	const QColor c1 = C(g.color), c2 = C(g.color2), tc = C(g.textColor);
	const double pad = 24 * s;

	if (g.style == "clean") {
		const QFont nf = Font(58 * s, QFont::Black, g.italic), tf = Font(30 * s, QFont::Bold, g.italic);
		const double bar = 12 * s, inX = 34 * s, inY = 18 * s;
		const double contentW = std::max(TextW(nf, name), TextW(tf, title));
		const double nh = QFontMetricsF(nf).height(), th = title.isEmpty() ? 0 : QFontMetricsF(tf).height();
		const double w = bar + inX * 2 + contentW, h = inY * 2 + nh + th;
		QImage img = Canvas(w + pad * 2, h + pad * 2);
		QPainter p;
		Begin(p, img, g);
		const QRectF card(pad, pad, w, h);
		SoftShadow(p, Rounded(card, 16 * s), 14 * s, 6 * s, 0.30);
		p.fillPath(Rounded(card, 16 * s), c2);
		QPainterPath accent = Rounded(QRectF(pad, pad, bar + 16 * s, h), 16 * s)
					      .intersected(Rounded(card, 16 * s))
					      .intersected([&] {
						      QPainterPath r;
						      r.addRect(pad, pad, bar, h);
						      return r;
					      }());
		p.fillPath(accent, c1);
		DrawText(p, nf, pad + bar + inX, pad + inY, name, tc);
		DrawText(p, tf, pad + bar + inX + 2 * s, pad + inY + nh, title, A(c1.darker(115), 1.0));
		return img;
	}

	if (g.style == "sport") {
		const QFont nf = Font(60 * s, QFont::Black, g.italic), tf = Font(32 * s, QFont::Bold, g.italic);
		const double slant = 34 * s, nh = 92 * s, th = 54 * s;
		const double nw = TextW(nf, name) + 70 * s + slant, tw = TextW(tf, title) + 60 * s + slant;
		const double tOff = 46 * s;
		const double w = std::max(nw + 26 * s, tOff + tw), h = nh + (title.isEmpty() ? 0 : th);
		QImage img = Canvas(w + pad * 2 + slant, h + pad * 2);
		QPainter p;
		Begin(p, img, g);
		const double x = pad + 26 * s, y = pad;
		QPainterPath nameBar = Slanted(x, y, nw, nh, slant, slant);
		SoftShadow(p, nameBar, 12 * s, 6 * s);
		// 左側兩條速度線
		p.fillPath(Slanted(pad, y, 10 * s + slant, nh, slant, slant), A(tc, 0.9));
		p.fillPath(Slanted(pad + 14 * s, y, 6 * s + slant, nh, slant, slant), A(c1, 0.7));
		QLinearGradient lg(x, y, x + nw, y);
		lg.setColorAt(0, c1);
		lg.setColorAt(1, c1.darker(125));
		p.fillPath(nameBar, lg);
		DrawText(p, nf, x + slant + 26 * s, y + (nh - QFontMetricsF(nf).height()) / 2, name, tc);
		if (!title.isEmpty()) {
			QPainterPath tb = Slanted(x + tOff - slant * 0.6, y + nh, tw, th, slant * 0.6, slant * 0.6);
			p.fillPath(tb, c2);
			DrawText(p, tf, x + tOff + 18 * s, y + nh + (th - QFontMetricsF(tf).height()) / 2, title, A(tc, 0.95));
		}
		return img;
	}

	if (g.style == "pill") {
		const QFont nf = Font(50 * s, QFont::Black, g.italic), tf = Font(32 * s, QFont::Bold, g.italic);
		const double h = 92 * s, dot = 14 * s;
		const double nw = TextW(nf, name), tw = TextW(tf, title);
		const double gap = title.isEmpty() ? 0 : 40 * s;
		const double w = h * 0.5 + 30 * s + dot + 22 * s + nw + gap + tw + h * 0.5;
		QImage img = Canvas(w + pad * 2, h + pad * 2);
		QPainter p;
		Begin(p, img, g);
		const QRectF r(pad, pad, w, h);
		QPainterPath pill = Rounded(r, h / 2);
		SoftShadow(p, pill, 14 * s, 6 * s);
		QLinearGradient lg(r.topLeft(), r.topRight());
		lg.setColorAt(0, c1.lighter(112));
		lg.setColorAt(1, c1.darker(135));
		p.fillPath(pill, lg);
		// 內側高光
		p.setPen(QPen(A(Qt::white, 0.35), 2 * s));
		p.drawRoundedRect(r.adjusted(2 * s, 2 * s, -2 * s, -2 * s), h / 2 - 2 * s, h / 2 - 2 * s);
		double x = pad + h * 0.5;
		p.setPen(Qt::NoPen);
		p.setBrush(c2);
		p.drawEllipse(QPointF(x + dot / 2, pad + h / 2), dot / 2 + 4 * s, dot / 2 + 4 * s);
		p.setBrush(tc);
		p.drawEllipse(QPointF(x + dot / 2, pad + h / 2), dot / 2 - 1 * s, dot / 2 - 1 * s);
		x += dot + 22 * s;
		DrawText(p, nf, x, pad + (h - QFontMetricsF(nf).height()) / 2, name, tc);
		x += nw;
		if (!title.isEmpty()) {
			p.fillRect(QRectF(x + gap / 2 - 1.5 * s, pad + h * 0.28, 3 * s, h * 0.44), A(tc, 0.6));
			x += gap;
			DrawText(p, tf, x, pad + (h - QFontMetricsF(tf).height()) / 2, title, A(tc, 0.92));
		}
		return img;
	}

	if (g.style == "outline") {
		const QFont nf = Font(70 * s, QFont::Black, g.italic), tf = Font(36 * s, QFont::Black, g.italic);
		const double sw = 10 * s;
		const double nw = TextW(nf, name), tw = TextW(tf, title);
		const double nh = QFontMetricsF(nf).height(), th = title.isEmpty() ? 0 : QFontMetricsF(tf).height() + 10 * s;
		const double barH = 10 * s;
		const double w = std::max(nw, tw) + sw * 2, h = nh + barH + 8 * s + th + sw;
		QImage img = Canvas(w + pad * 2, h + pad * 2);
		QPainter p;
		Begin(p, img, g);
		double y = pad;
		DrawText(p, nf, pad + sw, y, name, tc, sw * 2, c2);
		y += nh;
		const double bw = std::max(nw * 0.55, 120 * s);
		QPainterPath bar = Rounded(QRectF(pad + sw, y, bw, barH), barH / 2);
		p.strokePath(bar, QPen(c2, sw, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		p.fillPath(bar, c1);
		y += barH + 8 * s;
		DrawText(p, tf, pad + sw, y, title, A(tc, 0.95), sw * 1.4, c2);
		return img;
	}

	if (g.style == "glass") {
		const QFont nf = Font(58 * s, QFont::Black, g.italic), tf = Font(32 * s, QFont::Bold, g.italic);
		const double inX = 36 * s, inY = 20 * s, accent = 8 * s;
		const double contentW = std::max(TextW(nf, name), TextW(tf, title));
		const double nh = QFontMetricsF(nf).height(), th = title.isEmpty() ? 0 : QFontMetricsF(tf).height();
		const double w = accent + inX + contentW + 160 * s, h = inY * 2 + nh + th;
		QImage img = Canvas(w + pad * 2, h + pad * 2);
		QPainter p;
		Begin(p, img, g);
		const QRectF r(pad, pad, w, h);
		QLinearGradient bg(r.topLeft(), r.topRight());
		bg.setColorAt(0, A(c2, 0.92));
		bg.setColorAt(0.65, A(c2, 0.78));
		bg.setColorAt(1, A(c2, 0.0));
		p.fillRect(r, bg);
		QLinearGradient line(r.topLeft(), r.topRight());
		line.setColorAt(0, c1);
		line.setColorAt(0.7, A(c1, 0.6));
		line.setColorAt(1, A(c1, 0.0));
		p.fillRect(QRectF(r.left(), r.top(), w, 4 * s), line);
		p.fillRect(QRectF(r.left(), r.top(), accent, h), c1);
		DrawText(p, nf, r.left() + accent + inX, r.top() + inY, name, tc);
		DrawText(p, tf, r.left() + accent + inX + 2 * s, r.top() + inY + nh, title, c1.lighter(125));
		return img;
	}

	// 預設：新聞台
	const QFont nf = Font(60 * s, QFont::Black, g.italic), tf = Font(32 * s, QFont::Bold, g.italic);
	const double accent = 18 * s, slant = 30 * s, nh = 90 * s, th = 54 * s;
	const double nameW = TextW(nf, name) + 64 * s + slant;
	const double titleW = TextW(tf, title) + 56 * s + slant;
	const double barW = std::max(nameW, title.isEmpty() ? 0.0 : titleW);
	const double h = nh + (title.isEmpty() ? 0 : th);
	QImage img = Canvas(pad * 2 + accent + barW, pad * 2 + h);
	QPainter p;
	Begin(p, img, g);
	const double x = pad + accent, y = pad;
	QPainterPath nameBar = Slanted(x, y, barW, nh, slant);
	QPainterPath all = nameBar;
	if (!title.isEmpty())
		all = all.united(Slanted(x, y + nh, titleW, th, slant));
	all.addRect(pad, y, accent, h);
	SoftShadow(p, all, 14 * s, 6 * s);
	QLinearGradient lg(x, y, x, y + nh);
	lg.setColorAt(0, c1.lighter(115));
	lg.setColorAt(1, c1.darker(110));
	p.fillPath(nameBar, lg);
	p.fillRect(QRectF(x, y, barW - slant, 3 * s), A(Qt::white, 0.25));
	if (!title.isEmpty())
		p.fillPath(Slanted(x, y + nh, titleW, th, slant), c2);
	p.fillRect(QRectF(pad, y, accent - 4 * s, h), c1.darker(140));
	DrawText(p, nf, x + 30 * s, y + (nh - QFontMetricsF(nf).height()) / 2, name, tc);
	DrawText(p, tf, x + 30 * s, y + nh + (th - QFontMetricsF(tf).height()) / 2, title, A(tc, 0.92));
	return img;
}

// ------------------------------------------------------------ 時鐘

QImage RenderClock(const GraphicSpec &g)
{
	const double s = Scale(g);
	const QString time = TimeText(g), prefix = U(g.text1), date = g.showDate ? DateText() : QString();
	const QColor c1 = C(g.color), c2 = C(g.color2), tc = C(g.textColor);
	const double pad = 20 * s;
	const QFont timeF = Font(54 * s, QFont::Black, g.italic), preF = Font(30 * s, QFont::Black, g.italic), dateF = Font(22 * s, QFont::Bold, g.italic);
	const double tw = MonoWidth(timeF, time), th = QFontMetricsF(timeF).height();
	const double dh = date.isEmpty() ? 0 : QFontMetricsF(dateF).height();
	const double dw = TextW(dateF, date);

	if (g.style == "live") {
		const QString tag = prefix.isEmpty() ? QStringLiteral("LIVE") : prefix;
		const double h = th + dh + 16 * s;
		const double tagW = TextW(preF, tag) + 44 * s;
		const double timeW = std::max(tw, dw) + 44 * s;
		QImage img = Canvas(pad * 2 + tagW + timeW, pad * 2 + h);
		QPainter p;
		Begin(p, img, g);
		QPainterPath all;
		all.addRect(pad, pad, tagW + timeW, h);
		SoftShadow(p, all, 10 * s, 5 * s);
		p.fillRect(QRectF(pad, pad, tagW, h), c1);
		p.fillRect(QRectF(pad + tagW, pad, timeW, h), c2);
		// 閃爍感的小圓點
		p.setPen(Qt::NoPen);
		p.setBrush(tc);
		p.drawEllipse(QPointF(pad + 16 * s, pad + h / 2), 6 * s, 6 * s);
		DrawText(p, preF, pad + 28 * s, pad + (h - QFontMetricsF(preF).height()) / 2, tag, tc);
		const double tx = pad + tagW + 22 * s;
		DrawMono(p, timeF, tx, pad + 8 * s, time, tc);
		DrawText(p, dateF, tx, pad + 8 * s + th, date, A(tc, 0.75));
		return img;
	}

	if (g.style == "outline") {
		const double sw = 8 * s;
		const double ph = prefix.isEmpty() ? 0 : QFontMetricsF(preF).height();
		const double w = std::max({tw, TextW(preF, prefix), dw}) + sw * 2;
		const double h = ph + th + dh + sw * 2;
		QImage img = Canvas(pad * 2 + w, pad * 2 + h);
		QPainter p;
		Begin(p, img, g);
		double y = pad + sw;
		DrawText(p, preF, pad + sw, y, prefix, c1, sw * 1.6, c2);
		y += ph;
		DrawMono(p, timeF, pad + sw, y, time, tc, sw * 2, c2);
		y += th;
		DrawText(p, dateF, pad + sw, y, date, tc, sw * 1.2, c2);
		return img;
	}

	if (g.style == "neon") {
		const double inX = 30 * s, inY = 14 * s;
		const double ph = prefix.isEmpty() ? 0 : QFontMetricsF(dateF).height();
		const double w = std::max({tw, dw, TextW(dateF, prefix)}) + inX * 2;
		const double h = ph + th + dh + inY * 2;
		QImage img = Canvas(pad * 2 + w, pad * 2 + h);
		QPainter p;
		Begin(p, img, g);
		const QRectF r(pad, pad, w, h);
		QPainterPath box = Rounded(r, 18 * s);
		// 外發光
		for (int i = 5; i >= 1; i--) {
			p.strokePath(box, QPen(A(c1, 0.08), (3 + i * 3) * s));
		}
		p.fillPath(box, A(c2, 0.9));
		p.strokePath(box, QPen(c1, 3 * s));
		double y = pad + inY;
		DrawText(p, dateF, pad + inX, y, prefix, A(tc, 0.85));
		y += ph;
		DrawMono(p, timeF, pad + inX, y, time, c1.lighter(110));
		y += th;
		DrawText(p, dateF, pad + inX, y, date, A(tc, 0.7));
		return img;
	}

	// 預設：深色圓角
	const double inX = 26 * s, inY = 12 * s, gap = 18 * s;
	const double chipW = prefix.isEmpty() ? 0 : TextW(preF, prefix) + 32 * s;
	const double contentW = std::max(tw, dw);
	const double h = std::max(th + dh, QFontMetricsF(preF).height() + 8 * s) + inY * 2;
	const double w = inX + (chipW > 0 ? chipW + gap : 0) + contentW + inX;
	QImage img = Canvas(pad * 2 + w, pad * 2 + h);
	QPainter p;
	Begin(p, img, g);
	const QRectF r(pad, pad, w, h);
	QPainterPath box = Rounded(r, 16 * s);
	SoftShadow(p, box, 12 * s, 5 * s);
	p.fillPath(box, A(c2, 0.9));
	p.strokePath(box, QPen(A(Qt::white, 0.10), 1.5 * s));
	double x = pad + inX;
	if (chipW > 0) {
		const double chH = QFontMetricsF(preF).height() + 4 * s;
		const QRectF chip(x, pad + (h - chH) / 2, chipW, chH);
		p.fillPath(Rounded(chip, 8 * s), c1);
		DrawText(p, preF, chip.left() + 16 * s, chip.top() + 2 * s, prefix, tc);
		x += chipW + gap;
	}
	const double ty = pad + (h - th - dh) / 2;
	DrawMono(p, timeF, x, ty, time, tc);
	DrawText(p, dateF, x + 2 * s, ty + th, date, A(tc, 0.7));
	return img;
}

// ------------------------------------------------------------ 台標

QImage FitImage(const QImage &src, double maxW, double maxH)
{
	if (src.isNull())
		return src;
	return src.scaled((int)maxW, (int)maxH, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QImage RenderLogo(const GraphicSpec &g)
{
	const double s = Scale(g);
	const QColor c1 = C(g.color), c2 = C(g.color2), tc = C(g.textColor);
	const double pad = 18 * s;
	QImage src;
	if (!g.file.empty())
		src.load(U(g.file));
	const std::string style = (src.isNull() && g.style != "text") ? std::string("text") : g.style;

	if (style == "text") {
		const QString t = U(g.text1).isEmpty() ? QStringLiteral("MEIKOU LIVE") : U(g.text1);
		const QString sub = U(g.text2);
		const QFont f = Font(46 * s, QFont::Black, g.italic), sf = Font(20 * s, QFont::Bold, g.italic);
		const double inX = 26 * s, inY = 12 * s;
		const double th = QFontMetricsF(f).height(), sh = sub.isEmpty() ? 0 : QFontMetricsF(sf).height();
		const double w = std::max(TextW(f, t), TextW(sf, sub)) + inX * 2, h = th + sh + inY * 2;
		QImage img = Canvas(pad * 2 + w, pad * 2 + h);
		QPainter p;
		Begin(p, img, g);
		const QRectF r(pad, pad, w, h);
		QPainterPath box = Rounded(r, 14 * s);
		SoftShadow(p, box, 10 * s, 4 * s, 0.3);
		QLinearGradient lg(r.topLeft(), r.bottomRight());
		lg.setColorAt(0, c1.lighter(110));
		lg.setColorAt(1, c1.darker(125));
		p.fillPath(box, lg);
		// 角落的小紅點（直播感）
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(255, 59, 59));
		p.drawEllipse(QPointF(r.right() - 10 * s, r.top() + 10 * s), 6 * s, 6 * s);
		DrawText(p, f, r.left() + inX, r.top() + inY, t, tc);
		DrawText(p, sf, r.left() + inX + 2 * s, r.top() + inY + th, sub, A(tc, 0.85));
		return img;
	}

	if (style == "circle") {
		const double d = 200 * s, ring = 8 * s;
		QImage img = Canvas(pad * 2 + d, pad * 2 + d);
		QPainter p;
		Begin(p, img, g);
		const QRectF r(pad, pad, d, d);
		QPainterPath circle;
		circle.addEllipse(r);
		SoftShadow(p, circle, 10 * s, 4 * s, 0.3);
		p.fillPath(circle, c2);
		QPainterPath inner;
		inner.addEllipse(r.adjusted(ring, ring, -ring, -ring));
		p.save();
		p.setClipPath(inner);
		const QImage cover = src.scaled((int)(d - ring * 2), (int)(d - ring * 2), Qt::KeepAspectRatioByExpanding,
						Qt::SmoothTransformation);
		const QRectF ir = inner.boundingRect();
		p.drawImage(QPointF(ir.center().x() - cover.width() / 2.0, ir.center().y() - cover.height() / 2.0), cover);
		p.restore();
		p.strokePath(circle, QPen(c1, ring));
		return img;
	}

	if (style == "card") {
		const QImage fit = FitImage(src, 320 * s, 150 * s);
		const double inP = 18 * s;
		const double w = fit.width() + inP * 2, h = fit.height() + inP * 2;
		QImage img = Canvas(pad * 2 + w, pad * 2 + h);
		QPainter p;
		Begin(p, img, g);
		const QRectF r(pad, pad, w, h);
		QPainterPath box = Rounded(r, 18 * s);
		SoftShadow(p, box, 10 * s, 4 * s, 0.3);
		p.fillPath(box, c2);
		p.fillRect(QRectF(r.left() + 18 * s, r.bottom() - 5 * s, w - 36 * s, 5 * s), c1);
		p.drawImage(QPointF(r.left() + inP, r.top() + inP), fit);
		return img;
	}

	// 原圖／浮水印
	const QImage fit = FitImage(src, 420 * s, 220 * s);
	QImage img = Canvas(fit.width(), fit.height());
	QPainter p;
	Begin(p, img, g);
	p.drawImage(0, 0, fit);
	return img;
}

// ------------------------------------------------------------ 文字

QImage RenderTextGraphic(const GraphicSpec &g)
{
	const double s = Scale(g);
	const QString t = U(g.text1);
	const QColor c1 = C(g.color), c2 = C(g.color2), tc = C(g.textColor);
	const QFont f = Font(72 * s, QFont::Black, g.italic);
	const double tw = TextW(f, t), th = QFontMetricsF(f).height();
	const double pad = 24 * s;

	if (g.style == "box") {
		const double inX = 30 * s, inY = 10 * s;
		QImage img = Canvas(pad * 2 + tw + inX * 2, pad * 2 + th + inY * 2);
		QPainter p;
		Begin(p, img, g);
		const QRectF r(pad, pad, tw + inX * 2, th + inY * 2);
		QPainterPath box = Rounded(r, 14 * s);
		SoftShadow(p, box, 10 * s, 4 * s, 0.3);
		p.fillPath(box, c1);
		DrawText(p, f, r.left() + inX, r.top() + inY, t, tc);
		return img;
	}
	if (g.style == "shadow") {
		QImage img = Canvas(pad * 2 + tw, pad * 2 + th);
		QPainter p;
		Begin(p, img, g);
		QPainterPath path;
		path.setFillRule(Qt::WindingFill);
		path.addText(pad, pad + QFontMetricsF(f).ascent(), f, t);
		SoftShadow(p, path, 8 * s, 5 * s, 0.6);
		p.fillPath(path, tc);
		return img;
	}
	const double sw = 10 * s;
	QImage img = Canvas(pad * 2 + tw + sw * 2, pad * 2 + th + sw * 2);
	QPainter p;
	Begin(p, img, g);
	DrawText(p, f, pad + sw, pad + sw, t, tc, sw * 2, c2);
	return img;
}

// ------------------------------------------------------------ 跑馬燈

struct TickerLayout {
	double h, s;
	QFont textF, labelF;
};

TickerLayout TickerMetrics(const GraphicSpec &g)
{
	TickerLayout L;
	L.s = Scale(g);
	L.h = std::max(44.0, 72 * L.s);
	L.textF = Font(L.h * 0.52, QFont::Black, g.italic);
	L.labelF = Font(L.h * 0.50, QFont::Black, g.italic);
	return L;
}

} // namespace

// ------------------------------------------------------------ 對外

std::vector<StyleChoice> GraphicStyles(const std::string &kind)
{
	if (kind == "lower")
		return {{"news", "新聞台"},  {"clean", "簡約白卡"},   {"sport", "斜切運動"},
			{"pill", "圓角膠囊"}, {"outline", "透明描邊"}, {"glass", "玻璃漸層"}};
	if (kind == "clock")
		return {{"dark", "深色圓角"}, {"live", "LIVE 紅標"}, {"outline", "透明描邊"}, {"neon", "霓虹"}};
	if (kind == "ticker")
		return {{"news", "新聞快訊"}, {"light", "白底"}, {"gradient", "漸層"}};
	if (kind == "logo")
		return {{"plain", "原圖"},          {"circle", "圓形徽章"}, {"card", "圓角卡片"},
			{"watermark", "半透明浮水印"}, {"text", "文字台標"}};
	if (kind == "text")
		return {{"stroke", "描邊"}, {"shadow", "陰影"}, {"box", "底框"}};
	return {};
}

void ApplyStyleColors(GraphicSpec &g)
{
	struct Preset {
		const char *kind, *style;
		uint32_t c1, c2, tc;
	};
	static const Preset presets[] = {
		{"lower", "news", 0xFFD7263D, 0xFF101826, 0xFFFFFFFF},
		{"lower", "clean", 0xFF0ABAB5, 0xFFFFFFFF, 0xFF111111},
		{"lower", "sport", 0xFFF59E0B, 0xFF111827, 0xFFFFFFFF},
		{"lower", "pill", 0xFF0ABAB5, 0xFF063B3A, 0xFFFFFFFF},
		{"lower", "outline", 0xFF0ABAB5, 0xFF000000, 0xFFFFFFFF},
		{"lower", "glass", 0xFF81D8D0, 0xFF0B1220, 0xFFFFFFFF},
		{"clock", "dark", 0xFFE11D48, 0xFF0B1220, 0xFFFFFFFF},
		{"clock", "live", 0xFFE11D48, 0xFF111827, 0xFFFFFFFF},
		{"clock", "outline", 0xFFE11D48, 0xFF000000, 0xFFFFFFFF},
		{"clock", "neon", 0xFF81D8D0, 0xFF06101A, 0xFFFFFFFF},
		{"ticker", "news", 0xFFD7263D, 0xF00B1220, 0xFFFFFFFF},
		{"ticker", "light", 0xFF0ABAB5, 0xFFFFFFFF, 0xFF111111},
		{"ticker", "gradient", 0xFF0ABAB5, 0xFF1E3A8A, 0xFFFFFFFF},
		{"logo", "text", 0xFF0ABAB5, 0xFFFFFFFF, 0xFFFFFFFF},
		{"text", "stroke", 0xFF0ABAB5, 0xFF000000, 0xFFFFFFFF},
		{"text", "shadow", 0xFF0ABAB5, 0xFF000000, 0xFFFFFFFF},
		{"text", "box", 0xFF0ABAB5, 0xFF000000, 0xFFFFFFFF},
	};
	if (g.kind == "lower")
		g.italic = g.style == "sport"; // 斜切運動預設斜體，其他預設正體（之後都能自己改）
	for (const auto &pr : presets) {
		if (g.kind == pr.kind && g.style == pr.style) {
			g.color = pr.c1;
			g.color2 = pr.c2;
			g.textColor = pr.tc;
		}
	}
	if (g.kind == "logo") {
		if (g.style != "text") {
			g.color = 0xFF0ABAB5;
			g.color2 = 0xFFFFFFFF;
		}
		g.opacity = g.style == "watermark" ? 45 : 100;
	}
}

void ApplyGraphicDefaults(GraphicSpec &g)
{
	auto styles = GraphicStyles(g.kind);
	if (g.style.empty() && !styles.empty())
		g.style = styles.front().id;
	if (g.kind == "logo" && g.file.empty())
		g.style = "text";
	if (g.kind == "ticker" && g.label.empty())
		g.label = "快訊";
	ApplyStyleColors(g);
}

void LoadBrandFonts()
{
	static bool loaded = false;
	if (loaded)
		return;
	loaded = true;
	const QString app = QCoreApplication::applicationDirPath();
	const QStringList dirs = {app + QStringLiteral("/fonts"), app + QStringLiteral("/../Resources/fonts"),
				  app + QStringLiteral("/../../fonts")};
	for (const QString &d : dirs) {
		QDir dir(d);
		if (!dir.exists())
			continue;
		for (const QFileInfo &fi : dir.entryInfoList({QStringLiteral("*.ttf"), QStringLiteral("*.otf")}, QDir::Files))
			QFontDatabase::addApplicationFont(fi.absoluteFilePath());
	}
}

QImage RenderGraphic(const GraphicSpec &g)
{
	if (g.kind == "lower")
		return RenderLower(g);
	if (g.kind == "clock")
		return RenderClock(g);
	if (g.kind == "logo")
		return RenderLogo(g);
	if (g.kind == "text")
		return RenderTextGraphic(g);
	return Canvas(1, 1);
}

TickerParts RenderTicker(const GraphicSpec &g)
{
	const TickerLayout L = TickerMetrics(g);
	const double s = L.s, h = L.h, W = 1920;
	const QColor c1 = C(g.color), c2 = C(g.color2), tc = C(g.textColor);
	const QString label = U(g.label), text = U(g.text1);

	TickerParts out;
	out.bar = Canvas(W, h);
	QPainter p;
	Begin(p, out.bar, g);
	double textX = 24 * s;

	if (g.style == "light") {
		p.fillRect(QRectF(0, 0, W, h), c2);
		p.fillRect(QRectF(0, 0, W, 4 * s), c1);
		if (!label.isEmpty()) {
			const double lw = TextW(L.labelF, label) + 48 * s;
			const QRectF chip(14 * s, h * 0.16, lw, h * 0.68);
			p.fillPath(Rounded(chip, chip.height() / 2), c1);
			DrawText(p, L.labelF, chip.left() + 24 * s, chip.top() + (chip.height() - QFontMetricsF(L.labelF).height()) / 2,
				 label, Qt::white);
			textX = chip.right() + 24 * s;
		}
	} else if (g.style == "gradient") {
		QLinearGradient lg(0, 0, W, 0);
		lg.setColorAt(0, c1);
		lg.setColorAt(1, c2);
		p.fillRect(QRectF(0, 0, W, h), lg);
		p.fillRect(QRectF(0, 0, W, 2 * s), A(Qt::white, 0.35));
		if (!label.isEmpty()) {
			const double lw = TextW(L.labelF, label) + 48 * s;
			const QRectF chip(14 * s, h * 0.16, lw, h * 0.68);
			p.fillPath(Rounded(chip, 10 * s), Qt::white);
			DrawText(p, L.labelF, chip.left() + 24 * s, chip.top() + (chip.height() - QFontMetricsF(L.labelF).height()) / 2,
				 label, c1.darker(120));
			textX = chip.right() + 24 * s;
		}
	} else { // news
		p.fillRect(QRectF(0, 0, W, h), c2);
		p.fillRect(QRectF(0, 0, W, 3 * s), c1);
		if (!label.isEmpty()) {
			const double slant = h * 0.35;
			const double lw = TextW(L.labelF, label) + 56 * s + slant;
			QLinearGradient lg(0, 0, 0, h);
			lg.setColorAt(0, c1.lighter(115));
			lg.setColorAt(1, c1.darker(115));
			p.fillPath(Slanted(0, 0, lw, h, slant), lg);
			p.fillPath(Slanted(lw - slant * 0.4, 0, 12 * s + slant, h, slant, slant), A(c1, 0.55));
			DrawText(p, L.labelF, 26 * s, (h - QFontMetricsF(L.labelF).height()) / 2, label, Qt::white);
			textX = lw + 30 * s;
		}
	}
	p.end();
	out.area = QRect((int)textX, 0, (int)(W - textX - 12 * s), (int)h);

	// 一段文字＋尾端分隔點，重複接續捲動
	const double gap = 120 * s;
	const double tw = std::max(1.0, TextW(L.textF, text));
	out.text = Canvas(tw + gap, h);
	QPainter q;
	Begin(q, out.text, g);
	const QColor textColor = tc;
	DrawText(q, L.textF, 0, (h - QFontMetricsF(L.textF).height()) / 2, text, textColor);
	q.setPen(Qt::NoPen);
	q.setBrush(g.style == "gradient" ? A(Qt::white, 0.8) : c1);
	q.drawEllipse(QPointF(tw + gap / 2, h / 2), 6 * s, 6 * s);
	return out;
}

QImage RenderPreviewImage(const GraphicSpec &g)
{
	if (g.kind != "ticker")
		return RenderGraphic(g);
	TickerParts t = RenderTicker(g);
	QImage img = t.bar.copy();
	QPainter p(&img);
	p.setClipRect(t.area);
	for (int x = t.area.left(); x < t.area.right(); x += t.text.width())
		p.drawImage(x, 0, t.text);
	return img;
}

} // namespace meikou
