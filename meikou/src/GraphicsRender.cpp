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

// 柔和陰影：只給「陰影」文字版型用（其他疊加一律不加底部陰影）
void DropShadow(QPainter &p, const QPainterPath &shape, double spread, double dy, double strength = 0.35)
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

// ------------------------------------------------------------ 直式文字

bool Upright(QChar c)
{
	// 中日韓文字與全形符號直立排列；英文、數字整段轉 90 度
	return c.unicode() >= 0x2E80 || c.isSpace() == false && c.unicode() >= 0x1100 && c.unicode() < 0x11FF;
}

double VStep(const QFont &f)
{
	return f.pixelSize() * 1.04;
}

double VColumnWidth(const QFont &f)
{
	return f.pixelSize() * 1.12;
}

// 直式文字的路徑：cx 是欄位中線，top 是起點；回傳總高度
double VText(QPainterPath *path, const QFont &f, double cx, double top, const QString &s)
{
	QFontMetricsF fm(f);
	double y = top;
	int i = 0;
	const QString t = s.trimmed();
	while (i < t.size()) {
		const QChar c = t.at(i);
		if (Upright(c)) {
			if (path) {
				const double w = fm.horizontalAdvance(c);
				// 讓字在一個方格內垂直置中
				const double base = y + (VStep(f) - (fm.ascent() + fm.descent())) / 2 + fm.ascent();
				path->addText(cx - w / 2, base, f, QString(c));
			}
			y += VStep(f);
			i++;
			continue;
		}
		int j = i;
		while (j < t.size() && !Upright(t.at(j)))
			j++;
		const QString run = t.mid(i, j - i).trimmed();
		i = j;
		if (run.isEmpty()) {
			y += VStep(f) * 0.35;
			continue;
		}
		const double rw = fm.horizontalAdvance(run);
		if (path) {
			QPainterPath sub;
			sub.addText(0, 0, f, run);
			// 轉 90 度後字身範圍是 x ∈ [-descent, ascent]，平移讓它置中在 cx
			QTransform center;
			center.translate(cx - (fm.ascent() - fm.descent()) / 2, y + VStep(f) * 0.08);
			center.rotate(90);
			path->addPath(center.map(sub));
		}
		y += rw + VStep(f) * 0.16;
	}
	return y - top;
}

void DrawVText(QPainter &p, const QFont &f, double cx, double top, const QString &s, const QColor &fill,
	       double stroke = 0.0, const QColor &strokeColor = Qt::black)
{
	if (s.trimmed().isEmpty())
		return;
	QPainterPath path;
	path.setFillRule(Qt::WindingFill);
	VText(&path, f, cx, top, s);
	if (stroke > 0.0)
		p.strokePath(path, QPen(strokeColor, stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	p.fillPath(path, fill);
}

// ------------------------------------------------------------ 直式姓名條

QImage RenderLowerVertical(const GraphicSpec &g)
{
	const double s = Scale(g);
	const QString name = U(g.text1).trimmed(), title = U(g.text2).trimmed();
	const QColor c1 = C(g.color), c2 = C(g.color2), tc = C(g.textColor);
	const double pad = 24 * s;
	const QFont nf = Font(62 * s, QFont::Black, g.italic), tf = Font(32 * s, QFont::Bold, g.italic);
	const double nW = VColumnWidth(nf), tW = title.isEmpty() ? 0 : VColumnWidth(tf);
	const double nH = VText(nullptr, nf, 0, 0, name), tH = title.isEmpty() ? 0 : VText(nullptr, tf, 0, 0, title);
	const std::string &st = g.style;

	if (st == "clean") {
		const double inX = 22 * s, inY = 30 * s, gap = 12 * s, topBar = 10 * s;
		const double w = inX * 2 + nW + (tW > 0 ? gap + tW : 0), h = topBar + inY * 2 + std::max(nH, tH);
		QImage img = Canvas(pad * 2 + w, pad * 2 + h);
		QPainter p;
		Begin(p, img, g);
		const QRectF r(pad, pad, w, h);
		QPainterPath card = Rounded(r, 16 * s);
		p.fillPath(card, c2);
		p.save();
		p.setClipPath(card);
		p.fillRect(QRectF(r.left(), r.top(), w, topBar), c1);
		p.restore();
		const double y = r.top() + topBar + inY;
		DrawVText(p, nf, r.left() + inX + nW / 2, y, name, tc);
		if (tW > 0)
			DrawVText(p, tf, r.left() + inX + nW + gap + tW / 2, y + 6 * s, title, c1.darker(115));
		return img;
	}

	if (st == "pill") {
		const double nColW = nW + 30 * s, tColW = tW + 22 * s;
		const double nh = nH + nColW, th = tH + tColW;
		const double offY = nColW * 0.6;
		const double w = nColW + (tW > 0 ? 10 * s + tColW : 0), h = std::max(nh, tW > 0 ? offY + th : 0.0);
		QImage img = Canvas(pad * 2 + w, pad * 2 + h);
		QPainter p;
		Begin(p, img, g);
		const QRectF nr(pad, pad, nColW, nh);
		QPainterPath pill = Rounded(nr, nColW / 2);
		QLinearGradient lg(nr.topLeft(), nr.bottomLeft());
		lg.setColorAt(0, c1.lighter(112));
		lg.setColorAt(1, c1.darker(135));
		p.fillPath(pill, lg);
		p.setPen(QPen(A(Qt::white, 0.35), 2 * s));
		p.drawRoundedRect(nr.adjusted(2 * s, 2 * s, -2 * s, -2 * s), nColW / 2 - 2 * s, nColW / 2 - 2 * s);
		DrawVText(p, nf, nr.center().x(), nr.top() + nColW / 2, name, tc);
		if (tW > 0) {
			const QRectF tr(nr.right() + 10 * s, pad + offY, tColW, th);
			p.fillPath(Rounded(tr, tColW / 2), c2);
			DrawVText(p, tf, tr.center().x(), tr.top() + tColW / 2, title, A(tc, 0.92));
		}
		return img;
	}

	if (st == "glass") {
		const double inX = 20 * s, inY = 30 * s, gap = 14 * s;
		const double w = inX * 2 + nW + (tW > 0 ? gap + tW : 0), h = inY * 2 + std::max(nH, tH) + 90 * s;
		QImage img = Canvas(pad * 2 + w, pad * 2 + h);
		QPainter p;
		Begin(p, img, g);
		const QRectF r(pad, pad, w, h);
		QLinearGradient bg(r.topLeft(), r.bottomLeft());
		bg.setColorAt(0, A(c2, 0.92));
		bg.setColorAt(0.7, A(c2, 0.78));
		bg.setColorAt(1, A(c2, 0.0));
		p.fillRect(r, bg);
		p.fillRect(QRectF(r.left(), r.top(), w, 6 * s), c1);
		QLinearGradient line(r.topLeft(), r.bottomLeft());
		line.setColorAt(0, c1);
		line.setColorAt(1, A(c1, 0.0));
		p.fillRect(QRectF(r.left(), r.top(), 4 * s, h), line);
		DrawVText(p, nf, r.left() + inX + nW / 2, r.top() + inY, name, tc);
		if (tW > 0)
			DrawVText(p, tf, r.left() + inX + nW + gap + tW / 2, r.top() + inY + 6 * s, title, c1.lighter(125));
		return img;
	}

	if (st == "outline" || st == "underline") {
		const double sw = 9 * s, line = 8 * s, gap = 14 * s;
		const double w = line + gap + nW + (tW > 0 ? gap + tW : 0) + sw * 2, h = std::max(nH, tH + 10 * s) + sw * 2;
		QImage img = Canvas(pad * 2 + w, pad * 2 + h);
		QPainter p;
		Begin(p, img, g);
		const double x = pad + sw, y = pad + sw;
		QPainterPath bar = Rounded(QRectF(x, y, line, std::max(nH * 0.7, 80 * s)), line / 2);
		p.strokePath(bar, QPen(c2, sw * 0.8));
		p.fillPath(bar, c1);
		DrawVText(p, nf, x + line + gap + nW / 2, y, name, tc, sw * 2, c2);
		if (tW > 0)
			DrawVText(p, tf, x + line + gap + nW + gap + tW / 2, y + 10 * s, title,
				  st == "underline" ? c1.lighter(125) : A(tc, 0.95), sw * 1.4, c2);
		return img;
	}

	if (st == "bracket") {
		const double inX = 22 * s, inY = 30 * s, gap = 14 * s, len = 30 * s, th = 6 * s;
		const double w = inX * 2 + nW + (tW > 0 ? gap + tW : 0), h = inY * 2 + std::max(nH, tH);
		QImage img = Canvas(pad * 2 + w, pad * 2 + h);
		QPainter p;
		Begin(p, img, g);
		const QRectF r(pad, pad, w, h);
		p.fillRect(r, A(c2, 0.6));
		p.setPen(Qt::NoPen);
		p.setBrush(c1);
		const double L = r.left(), T = r.top(), R = r.right(), B = r.bottom();
		for (const QRectF &q : {QRectF(L, T, len, th), QRectF(L, T, th, len), QRectF(R - len, T, len, th),
					QRectF(R - th, T, th, len), QRectF(L, B - th, len, th), QRectF(L, B - len, th, len),
					QRectF(R - len, B - th, len, th), QRectF(R - th, B - len, th, len)})
			p.drawRect(q);
		DrawVText(p, nf, L + inX + nW / 2, T + inY, name, tc);
		if (tW > 0)
			DrawVText(p, tf, L + inX + nW + gap + tW / 2, T + inY + 6 * s, title, c1.lighter(120));
		return img;
	}

	// 直式色條（新聞台、斜切運動、雙色分割、緞帶共用）：主色長條放姓名，右側錯開一條輔色放職稱
	const double inX = 18 * s, inY = 28 * s, cut = st == "sport" || st == "ribbon" ? 26 * s : 0.0;
	const double nColW = nW + inX * 2, tColW = tW + 20 * s;
	const double nh = nH + inY * 2 + cut, th = tH + 40 * s + cut;
	const double offY = 34 * s;
	const double accent = 12 * s;
	const double w = nColW + (tW > 0 ? tColW : 0), h = std::max(accent + nh, tW > 0 ? offY + th : 0.0);
	QImage img = Canvas(pad * 2 + w, pad * 2 + h);
	QPainter p;
	Begin(p, img, g);
	const double x = pad, y = pad + accent;
	auto column = [](double cx, double cy, double cw, double ch, double c) {
		QPainterPath path;
		path.moveTo(cx, cy);
		path.lineTo(cx + cw, cy);
		path.lineTo(cx + cw, cy + ch - c);
		path.lineTo(cx, cy + ch);
		path.closeSubpath();
		return path;
	};
	QPainterPath nameCol = column(x, y, nColW, nh, cut);
	QPainterPath all = nameCol;
	if (tW > 0)
		all = all.united(column(x + nColW, pad + offY, tColW, th, cut));
	all.addRect(x, pad, nColW, accent);
	if (tW > 0) {
		p.fillPath(column(x + nColW, pad + offY, tColW, th, cut), st == "split" ? c2.lighter(115) : c2);
		DrawVText(p, tf, x + nColW + tColW / 2, pad + offY + 20 * s, title, A(tc, 0.92));
	}
	QLinearGradient lg(x, y, x + nColW, y);
	lg.setColorAt(0, c1.lighter(115));
	lg.setColorAt(1, c1.darker(112));
	p.fillPath(nameCol, lg);
	p.fillRect(QRectF(x, pad, nColW, accent - 3 * s), c1.darker(140));
	if (st == "ribbon")
		p.fillRect(QRectF(x + 6 * s, y, 2 * s, nh - cut), A(Qt::white, 0.3));
	DrawVText(p, nf, x + nColW / 2, y + inY, name, tc);
	return img;
}

// ------------------------------------------------------------ 姓名條

QImage RenderLower(const GraphicSpec &g)
{
	if (g.vertical)
		return RenderLowerVertical(g);
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

	if (g.style == "split") {
		const QFont nf = Font(52 * s, QFont::Black, g.italic), tf = Font(32 * s, QFont::Bold, g.italic);
		const double h = 84 * s, slant = 26 * s;
		const double nw = TextW(nf, name) + 56 * s + slant, tw = title.isEmpty() ? 0 : TextW(tf, title) + 56 * s + slant;
		QImage img = Canvas(pad * 2 + nw + tw, pad * 2 + h);
		QPainter p;
		Begin(p, img, g);
		QPainterPath all;
		all.addRect(pad, pad, nw + tw, h);
		if (tw > 0) {
			p.fillRect(QRectF(pad + nw - slant, pad, tw + slant, h), c2);
			DrawText(p, tf, pad + nw + 20 * s, pad + (h - QFontMetricsF(tf).height()) / 2, title, A(tc, 0.92));
		}
		QLinearGradient lg(pad, pad, pad, pad + h);
		lg.setColorAt(0, c1.lighter(112));
		lg.setColorAt(1, c1.darker(112));
		p.fillPath(Slanted(pad, pad, nw, h, slant), lg);
		DrawText(p, nf, pad + 26 * s, pad + (h - QFontMetricsF(nf).height()) / 2, name, tc);
		return img;
	}

	if (g.style == "bracket") {
		const QFont nf = Font(58 * s, QFont::Black, g.italic), tf = Font(30 * s, QFont::Bold, g.italic);
		const double inX = 34 * s, inY = 18 * s, len = 30 * s, th = 6 * s;
		const double nh = QFontMetricsF(nf).height(), tth = title.isEmpty() ? 0 : QFontMetricsF(tf).height();
		const double w = std::max(TextW(nf, name), TextW(tf, title)) + inX * 2, h = nh + tth + inY * 2;
		QImage img = Canvas(pad * 2 + w, pad * 2 + h);
		QPainter p;
		Begin(p, img, g);
		const QRectF r(pad, pad, w, h);
		p.fillRect(r, A(c2, 0.6));
		p.setPen(Qt::NoPen);
		p.setBrush(c1);
		// 四個角的 L 形框
		const double L = r.left(), T = r.top(), R = r.right(), B = r.bottom();
		for (const QRectF &q : {QRectF(L, T, len, th), QRectF(L, T, th, len), QRectF(R - len, T, len, th),
					QRectF(R - th, T, th, len), QRectF(L, B - th, len, th), QRectF(L, B - len, th, len),
					QRectF(R - len, B - th, len, th), QRectF(R - th, B - len, th, len)})
			p.drawRect(q);
		DrawText(p, nf, L + inX, T + inY, name, tc);
		DrawText(p, tf, L + inX + 2 * s, T + inY + nh, title, c1.lighter(120));
		return img;
	}

	if (g.style == "ribbon") {
		const QFont nf = Font(56 * s, QFont::Black, g.italic), tf = Font(30 * s, QFont::Bold, g.italic);
		const double nh = 86 * s, fold = 22 * s, th = 48 * s;
		const double nw = TextW(nf, name) + 80 * s, tw = title.isEmpty() ? 0 : TextW(tf, title) + 48 * s;
		const double w = std::max(nw + fold, tw + 40 * s), h = nh + fold + (title.isEmpty() ? 0 : th);
		QImage img = Canvas(pad * 2 + w + 30 * s, pad * 2 + h);
		QPainter p;
		Begin(p, img, g);
		const double x = pad + fold, y = pad;
		// 主緞帶（右端燕尾）
		QPainterPath band;
		band.moveTo(x, y);
		band.lineTo(x + nw + 30 * s, y);
		band.lineTo(x + nw + 6 * s, y + nh / 2);
		band.lineTo(x + nw + 30 * s, y + nh);
		band.lineTo(x, y + nh);
		band.closeSubpath();
		// 左下摺角
		QPainterPath foldPath;
		foldPath.moveTo(x, y + nh);
		foldPath.lineTo(x - fold, y + nh);
		foldPath.lineTo(x, y + nh + fold);
		foldPath.closeSubpath();
		p.fillPath(foldPath, c1.darker(170));
		QLinearGradient lg(x, y, x, y + nh);
		lg.setColorAt(0, c1.lighter(115));
		lg.setColorAt(1, c1.darker(115));
		p.fillPath(band, lg);
		p.fillRect(QRectF(x, y + 6 * s, nw, 2 * s), A(Qt::white, 0.3));
		p.fillRect(QRectF(x, y + nh - 8 * s, nw, 2 * s), A(Qt::white, 0.3));
		DrawText(p, nf, x + 30 * s, y + (nh - QFontMetricsF(nf).height()) / 2, name, tc);
		if (tw > 0) {
			const QRectF tab(x + 20 * s, y + nh + fold - 6 * s, tw, th);
			p.fillPath(Rounded(tab, 8 * s), c2);
			DrawText(p, tf, tab.left() + 24 * s, tab.top() + (th - QFontMetricsF(tf).height()) / 2, title, A(tc, 0.92));
		}
		return img;
	}

	if (g.style == "underline") {
		const QFont nf = Font(66 * s, QFont::Black, g.italic), tf = Font(32 * s, QFont::Bold, g.italic);
		const double sq = 16 * s;
		const double nw = TextW(nf, name), tw = TextW(tf, title);
		const double nh = QFontMetricsF(nf).height(), tth = title.isEmpty() ? 0 : QFontMetricsF(tf).height();
		const double w = sq + 18 * s + std::max(nw, tw) + 40 * s, h = nh + 12 * s + 4 * s + 8 * s + tth;
		QImage img = Canvas(pad * 2 + w, pad * 2 + h);
		QPainter p;
		Begin(p, img, g);
		const double x = pad, y = pad;
		QPainterPath namePath;
		namePath.setFillRule(Qt::WindingFill);
		namePath.addText(x + sq + 18 * s, y + QFontMetricsF(nf).ascent(), nf, name);
		p.fillRect(QRectF(x, y + nh / 2 - sq / 2, sq, sq), c1);
		p.fillPath(namePath, tc);
		const double ly = y + nh + 6 * s;
		QLinearGradient lg(x, ly, x + w, ly);
		lg.setColorAt(0, c1);
		lg.setColorAt(0.8, c1);
		lg.setColorAt(1, A(c1, 0.0));
		p.fillRect(QRectF(x, ly, w, 4 * s), lg);
		QPainterPath titlePath;
		titlePath.setFillRule(Qt::WindingFill);
		titlePath.addText(x + sq + 18 * s, ly + 10 * s + QFontMetricsF(tf).ascent(), tf, title);
		p.fillPath(titlePath, c1.lighter(125));
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

	if (g.style == "flip") {
		// 時、分、秒各自一張卡片，中間一條翻頁線
		const QStringList parts = time.split(u':');
		QString suffix;
		QStringList groups;
		for (const QString &part : parts) {
			const int space = part.indexOf(u' ');
			if (space > 0) {
				groups << part.left(space);
				suffix = part.mid(space + 1);
			} else {
				groups << part;
			}
		}
		const QFont f = Font(56 * s, QFont::Black, g.italic);
		const double cellW = MonoWidth(f, QStringLiteral("00")) + 22 * s, cellH = QFontMetricsF(f).height() + 10 * s;
		const double colon = 22 * s, chipW = prefix.isEmpty() ? 0 : TextW(dateF, prefix) + 28 * s;
		const double sufW = suffix.isEmpty() ? 0 : TextW(dateF, suffix) + 16 * s;
		const double w = (chipW > 0 ? chipW + 12 * s : 0) + groups.size() * cellW + (groups.size() - 1) * colon + sufW;
		const double h = cellH + dh + (dh > 0 ? 6 * s : 0);
		QImage img = Canvas(pad * 2 + w, pad * 2 + h);
		QPainter p;
		Begin(p, img, g);
		double x = pad;
		if (chipW > 0) {
			const QRectF chip(x, pad + (cellH - 40 * s) / 2, chipW, 40 * s);
			p.fillPath(Rounded(chip, 8 * s), c1);
			DrawText(p, dateF, chip.left() + 14 * s, chip.top() + (40 * s - QFontMetricsF(dateF).height()) / 2, prefix, tc);
			x += chipW + 12 * s;
		}
		for (int i = 0; i < groups.size(); i++) {
			const QRectF card(x, pad, cellW, cellH);
			QPainterPath cp = Rounded(card, 10 * s);
			QLinearGradient lg(card.topLeft(), card.bottomLeft());
			lg.setColorAt(0, c2.lighter(140));
			lg.setColorAt(0.5, c2.lighter(115));
			lg.setColorAt(0.5001, c2);
			lg.setColorAt(1, c2.darker(110));
			p.fillPath(cp, lg);
			DrawMono(p, f, x + 11 * s, pad + 5 * s, groups[i], tc);
			p.fillRect(QRectF(x, pad + cellH / 2 - 1 * s, cellW, 2 * s), A(Qt::black, 0.45));
			x += cellW;
			if (i + 1 < groups.size()) {
				p.setPen(Qt::NoPen);
				p.setBrush(c1);
				p.drawEllipse(QPointF(x + colon / 2, pad + cellH * 0.35), 4 * s, 4 * s);
				p.drawEllipse(QPointF(x + colon / 2, pad + cellH * 0.65), 4 * s, 4 * s);
				x += colon;
			}
		}
		if (sufW > 0)
			DrawText(p, dateF, x + 10 * s, pad + cellH - QFontMetricsF(dateF).height() - 4 * s, suffix, tc, 4 * s, c2);
		DrawText(p, dateF, pad, pad + cellH + 6 * s, date, tc, 4 * s, c2);
		return img;
	}

	if (g.style == "pill") {
		const double h = th + dh + 18 * s, inX = h / 2;
		const double ph = prefix.isEmpty() ? 0 : TextW(dateF, prefix) + 30 * s;
		const double w = inX + ph + std::max(tw, dw) + inX;
		QImage img = Canvas(pad * 2 + w, pad * 2 + h);
		QPainter p;
		Begin(p, img, g);
		const QRectF r(pad, pad, w, h);
		QPainterPath pill = Rounded(r, h / 2);
		QLinearGradient lg(r.topLeft(), r.topRight());
		lg.setColorAt(0, c1.lighter(110));
		lg.setColorAt(1, c1.darker(130));
		p.fillPath(pill, lg);
		p.setPen(QPen(A(Qt::white, 0.35), 2 * s));
		p.drawRoundedRect(r.adjusted(2 * s, 2 * s, -2 * s, -2 * s), h / 2 - 2 * s, h / 2 - 2 * s);
		double x = pad + inX * 0.8;
		if (ph > 0) {
			p.setPen(Qt::NoPen);
			p.setBrush(tc);
			p.drawEllipse(QPointF(x + 6 * s, pad + h / 2), 6 * s, 6 * s);
			DrawText(p, dateF, x + 18 * s, pad + (h - QFontMetricsF(dateF).height()) / 2, prefix, tc);
			x += ph + 6 * s;
		}
		const double ty = pad + (h - th - dh) / 2;
		DrawMono(p, timeF, x, ty, time, tc);
		DrawText(p, dateF, x + 2 * s, ty + th, date, A(tc, 0.85));
		return img;
	}

	if (g.style == "minimal") {
		const double bar = 8 * s;
		const double ph = prefix.isEmpty() ? 0 : QFontMetricsF(dateF).height();
		const double w = bar + 18 * s + std::max({tw, dw, TextW(dateF, prefix)}) + 10 * s, h = ph + th + dh;
		QImage img = Canvas(pad * 2 + w, pad * 2 + h);
		QPainter p;
		Begin(p, img, g);
		p.fillRect(QRectF(pad, pad, bar, h), c1);
		const double x = pad + bar + 18 * s;
		DrawText(p, dateF, x, pad, prefix, c1.lighter(120));
		DrawMono(p, timeF, x, pad + ph, time, tc);
		DrawText(p, dateF, x, pad + ph + th, date, A(tc, 0.85));
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

// 文字顏色和底色太接近時自動換成看得清楚的顏色
QColor Readable(const QColor &text, const QColor &bg)
{
	auto lum = [](const QColor &c) { return 0.2126 * c.redF() + 0.7152 * c.greenF() + 0.0722 * c.blueF(); };
	if (std::fabs(lum(text) - lum(bg)) >= 0.3)
		return text;
	return lum(bg) > 0.5 ? QColor(16, 24, 38) : QColor(Qt::white);
}

// 台標文字的位置：right 右邊、left 左邊、top 上方、bottom 下方
std::string LogoTextPos(const GraphicSpec &g)
{
	const std::string &t = g.textPos;
	return (t == "left" || t == "top" || t == "bottom") ? t : std::string("right");
}

// 依位置排好「圖片框」與「文字框」，回傳兩者的左上角（以內容區域為原點）與內容大小
void LayoutLogo(const std::string &pos, double bw, double bh, double pw, double ph, double gap, QPointF &img, QPointF &txt,
		QSizeF &size)
{
	if (pos == "left") {
		txt = QPointF(0, (std::max(bh, ph) - ph) / 2);
		img = QPointF(pw + gap, (std::max(bh, ph) - bh) / 2);
		size = QSizeF(pw + gap + bw, std::max(bh, ph));
	} else if (pos == "top" || pos == "bottom") {
		const double w = std::max(bw, pw);
		const bool top = pos == "top";
		txt = QPointF((w - pw) / 2, top ? 0 : bh + gap);
		img = QPointF((w - bw) / 2, top ? ph + gap : 0);
		size = QSizeF(w, bh + gap + ph);
	} else {
		img = QPointF(0, (std::max(bh, ph) - bh) / 2);
		txt = QPointF(bw + gap, (std::max(bh, ph) - ph) / 2);
		size = QSizeF(bw + gap + pw, std::max(bh, ph));
	}
}

// 兩行文字依位置對齊：放右邊靠左、放左邊靠右、上下置中
void DrawLogoLines(QPainter &p, const std::string &pos, const QRectF &box, const QFont &f, const QFont &sf,
		   const QString &t, const QString &sub, const QColor &c, double s)
{
	const double th = t.isEmpty() ? 0 : QFontMetricsF(f).height();
	auto xFor = [&](double w) {
		if (pos == "left")
			return box.right() - w;
		if (pos == "top" || pos == "bottom")
			return box.left() + (box.width() - w) / 2;
		return box.left();
	};
	DrawText(p, f, xFor(TextW(f, t)), box.top(), t, c);
	DrawText(p, sf, xFor(TextW(sf, sub)) + (pos == "right" ? 2 * s : 0), box.top() + th, sub, A(c, 0.85));
}

// 圖片台標加上台標文字／副標（有填才加），位置可選
QImage AppendLogoText(const QImage &base, double pad, const GraphicSpec &g, const std::string &style)
{
	const QString t = U(g.text1), sub = U(g.text2);
	if (t.isEmpty() && sub.isEmpty())
		return base;
	const double s = Scale(g);
	const std::string pos = LogoTextPos(g);
	const QColor c1 = C(g.color), c2 = C(g.color2), tc = C(g.textColor);
	const QFont f = Font(40 * s, QFont::Black, g.italic), sf = Font(18 * s, QFont::Bold, g.italic);
	const double th = t.isEmpty() ? 0 : QFontMetricsF(f).height(), sh = sub.isEmpty() ? 0 : QFontMetricsF(sf).height();
	const bool bare = style == "plain" || style == "watermark";
	const double inX = bare ? 0 : 22 * s, inY = bare ? 0 : 10 * s, accent = bare ? 0 : 6 * s;
	const double textW = std::max(TextW(f, t), TextW(sf, sub));
	const double pw = textW + inX * 2 + accent, ph = th + sh + inY * 2;
	const double edge = bare ? 0 : pad; // 圖片四周原本留的空白
	const double margin = bare ? 8 * s : pad;
	const double bw = base.width() - edge * 2, bh = base.height() - edge * 2;
	QPointF ip, tp;
	QSizeF content;
	LayoutLogo(pos, bw, bh, pw, ph, 12 * s, ip, tp, content);
	QImage img = Canvas(content.width() + margin * 2, content.height() + margin * 2);
	QPainter p;
	Begin(p, img, g);
	p.setOpacity(1.0); // 圖片本身已經套過不透明度
	p.drawImage(QPointF(margin + ip.x() - edge, margin + ip.y() - edge), base);
	p.setOpacity(std::clamp(g.opacity, 5, 100) / 100.0);
	const QRectF r(margin + tp.x(), margin + tp.y(), pw, ph);
	if (bare) {
		const double th2 = th;
		auto xFor = [&](double w) {
			if (pos == "left")
				return r.right() - w;
			if (pos == "top" || pos == "bottom")
				return r.left() + (r.width() - w) / 2;
			return r.left();
		};
		DrawText(p, f, xFor(TextW(f, t)), r.top(), t, tc, 6 * s, A(Qt::black, 0.6));
		DrawText(p, sf, xFor(TextW(sf, sub)), r.top() + th2, sub, A(tc, 0.9), 4 * s, A(Qt::black, 0.6));
		return img;
	}
	QPainterPath box = Rounded(r, 12 * s);
	if (style == "rounded") {
		QLinearGradient lg(r.topLeft(), r.bottomRight());
		lg.setColorAt(0, c1.lighter(110));
		lg.setColorAt(1, c1.darker(125));
		p.fillPath(box, lg);
	} else {
		p.fillPath(box, c2);
	}
	p.save();
	p.setClipPath(box);
	p.fillRect(QRectF(r.left(), r.top(), accent, r.height()), style == "rounded" ? c2 : c1);
	p.restore();
	const QColor rc = Readable(tc, style == "rounded" ? c1 : c2);
	DrawLogoLines(p, pos, QRectF(r.left() + accent + inX, r.top() + inY, textW, th + sh), f, sf, t, sub, rc, s);
	return img;
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
		p.end();
		return AppendLogoText(img, pad, g, style);
	}

	if (style == "card") {
		const QImage fit = FitImage(src, 320 * s, 150 * s);
		const double inP = 18 * s;
		const std::string pos = LogoTextPos(g);
		const QString t = U(g.text1), sub = U(g.text2);
		const QFont f = Font(40 * s, QFont::Black, g.italic), sf = Font(18 * s, QFont::Bold, g.italic);
		const double th = t.isEmpty() ? 0 : QFontMetricsF(f).height(), sh = sub.isEmpty() ? 0 : QFontMetricsF(sf).height();
		const double tw = std::max(TextW(f, t), TextW(sf, sub));
		QPointF ip, tp;
		QSizeF content;
		if (tw > 0)
			LayoutLogo(pos, fit.width(), fit.height(), tw, th + sh, 18 * s, ip, tp, content);
		else
			content = QSizeF(fit.width(), fit.height());
		const double w = content.width() + inP * 2, h = content.height() + inP * 2;
		QImage img = Canvas(pad * 2 + w, pad * 2 + h);
		QPainter p;
		Begin(p, img, g);
		const QRectF r(pad, pad, w, h);
		QPainterPath box = Rounded(r, 18 * s);
		p.fillPath(box, c2);
		p.fillRect(QRectF(r.left() + 18 * s, r.bottom() - 5 * s, w - 36 * s, 5 * s), c1);
		p.drawImage(QPointF(r.left() + inP + ip.x(), r.top() + inP + ip.y()), fit);
		if (tw > 0)
			DrawLogoLines(p, pos, QRectF(r.left() + inP + tp.x(), r.top() + inP + tp.y(), tw, th + sh), f, sf, t, sub,
				      Readable(tc, c2), s);
		return img;
	}

	if (style == "rounded" || style == "frame") {
		const QImage fit = FitImage(src, 380 * s, 220 * s);
		const double border = (style == "frame" ? 8 : 4) * s, inP = style == "frame" ? 12 * s : 0;
		const double w = fit.width() + (inP + border) * 2, h = fit.height() + (inP + border) * 2;
		QImage img = Canvas(pad * 2 + w, pad * 2 + h);
		QPainter p;
		Begin(p, img, g);
		const QRectF r(pad, pad, w, h);
		const double radius = style == "rounded" ? 26 * s : 4 * s;
		QPainterPath box = Rounded(r, radius);
		p.fillPath(box, style == "frame" ? c2 : c1);
		QPainterPath inner = Rounded(r.adjusted(border + inP, border + inP, -border - inP, -border - inP),
					     std::max(0.0, radius - border - inP));
		p.save();
		p.setClipPath(inner);
		p.drawImage(QPointF(r.left() + border + inP, r.top() + border + inP), fit);
		p.restore();
		if (style == "frame") {
			p.strokePath(Rounded(r.adjusted(border / 2, border / 2, -border / 2, -border / 2), radius), QPen(c1, border));
			// 右上角小 LIVE 標
			const QFont lf = Font(16 * s, QFont::Black, g.italic);
			const double lw = TextW(lf, QStringLiteral("LIVE")) + 16 * s, lh = 24 * s;
			const QRectF tag(r.right() - lw - 6 * s, r.top() - lh / 2, lw, lh);
			p.fillPath(Rounded(tag, 5 * s), QColor(229, 57, 53));
			DrawText(p, lf, tag.left() + 8 * s, tag.top() + (lh - QFontMetricsF(lf).height()) / 2, QStringLiteral("LIVE"),
				 Qt::white);
		}
		p.end();
		return AppendLogoText(img, pad, g, style);
	}

	// 原圖／浮水印
	const QImage fit = FitImage(src, 420 * s, 220 * s);
	QImage img = Canvas(fit.width(), fit.height());
	QPainter p;
	Begin(p, img, g);
	p.drawImage(0, 0, fit);
	p.end();
	return AppendLogoText(img, pad, g, style);
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
		DropShadow(p, path, 8 * s, 5 * s, 0.6); // 「陰影」版型本身就是陰影字
		p.fillPath(path, tc);
		return img;
	}
	if (g.style == "neon") {
		const double glow = 18 * s;
		QImage img = Canvas(pad * 2 + tw + glow * 2, pad * 2 + th + glow * 2);
		QPainter p;
		Begin(p, img, g);
		QPainterPath path;
		path.setFillRule(Qt::WindingFill);
		path.addText(pad + glow, pad + glow + QFontMetricsF(f).ascent(), f, t);
		for (int i = 6; i >= 1; i--)
			p.strokePath(path, QPen(A(c1, 0.10), i * 5 * s, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		p.strokePath(path, QPen(c1, 3 * s, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		p.fillPath(path, tc);
		return img;
	}
	if (g.style == "gradient") {
		const double sw = 6 * s;
		QImage img = Canvas(pad * 2 + tw + sw * 2, pad * 2 + th + sw * 2);
		QPainter p;
		Begin(p, img, g);
		QPainterPath path;
		path.setFillRule(Qt::WindingFill);
		path.addText(pad + sw, pad + sw + QFontMetricsF(f).ascent(), f, t);
		p.strokePath(path, QPen(c2, sw * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		QLinearGradient lg(0, pad, 0, pad + th);
		lg.setColorAt(0, tc);
		lg.setColorAt(0.55, c1.lighter(130));
		lg.setColorAt(1, c1);
		p.fillPath(path, lg);
		return img;
	}
	if (g.style == "ribbon") {
		const double slant = th * 0.35, inX = 34 * s;
		QImage img = Canvas(pad * 2 + tw + inX * 2 + slant, pad * 2 + th + 16 * s);
		QPainter p;
		Begin(p, img, g);
		QPainterPath band = Slanted(pad, pad, tw + inX * 2 + slant, th + 16 * s, slant, slant);
		QLinearGradient lg(pad, pad, pad, pad + th);
		lg.setColorAt(0, c1.lighter(112));
		lg.setColorAt(1, c1.darker(118));
		p.fillPath(band, lg);
		DrawText(p, f, pad + inX + slant * 0.5, pad + 8 * s, t, tc);
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
		return {{"news", "新聞台"},     {"clean", "簡約白卡"},   {"sport", "斜切運動"}, {"pill", "圓角膠囊"},
			{"outline", "透明描邊"}, {"glass", "玻璃漸層"},   {"split", "雙色分割"}, {"bracket", "角框"},
			{"ribbon", "緞帶"},      {"underline", "細線極簡"}};
	if (kind == "clock")
		return {{"dark", "深色圓角"}, {"live", "LIVE 紅標"}, {"outline", "透明描邊"}, {"neon", "霓虹"},
			{"flip", "翻頁卡"},   {"pill", "膠囊"},       {"minimal", "極簡直線"}};
	if (kind == "ticker")
		return {{"news", "新聞快訊"}, {"light", "白底"},     {"gradient", "漸層"},
			{"glass", "半透明"},   {"sport", "斜切運動"}, {"minimal", "無底描邊"}};
	if (kind == "logo")
		return {{"plain", "原圖"},          {"circle", "圓形徽章"},  {"card", "圓角卡片"},     {"rounded", "圓角裁切"},
			{"frame", "框線＋LIVE"},    {"watermark", "半透明浮水印"}, {"text", "文字台標"}};
	if (kind == "text")
		return {{"stroke", "描邊"}, {"shadow", "陰影"}, {"box", "底框"},
			{"neon", "霓虹"},   {"gradient", "漸層字"}, {"ribbon", "斜切底"}};
	return {};
}

std::vector<Palette> GraphicPalettes()
{
	return {
		{"新聞紅", 0xFFD7263D, 0xFF101826, 0xFFFFFFFF}, {"蒂芬尼藍", 0xFF0ABAB5, 0xFF062B2A, 0xFFFFFFFF},
		{"科技青", 0xFF06B6D4, 0xFF04161C, 0xFFFFFFFF}, {"皇家藍", 0xFF2563EB, 0xFF0B1735, 0xFFFFFFFF},
		{"海軍藍", 0xFF1E3A8A, 0xFF0A0F1F, 0xFFF8FAFC}, {"森林綠", 0xFF16A34A, 0xFF0B1F13, 0xFFFFFFFF},
		{"活力橘", 0xFFF97316, 0xFF1C1208, 0xFFFFFFFF}, {"尊爵金", 0xFFC9971A, 0xFF14110A, 0xFFFFF8E1},
		{"紫羅蘭", 0xFF7C3AED, 0xFF140B26, 0xFFFFFFFF}, {"櫻花粉", 0xFFEC4899, 0xFF2A0B1C, 0xFFFFFFFF},
		{"酒紅", 0xFF9F1239, 0xFF1A0710, 0xFFFFF1F2},   {"經典黑", 0xFF2B2B2B, 0xFF000000, 0xFFFFFFFF},
	};
}

void ApplyPalette(GraphicSpec &g, const Palette &pal)
{
	// 淺色底的版型：底色用淺色、文字用深色；其他版型相反
	const bool lightBase = (g.kind == "lower" && g.style == "clean") || (g.kind == "ticker" && g.style == "light") ||
			       (g.kind == "logo" && (g.style == "card" || g.style == "circle" || g.style == "frame"));
	g.color = pal.primary;
	g.color2 = lightBase ? pal.light : pal.dark;
	g.textColor = lightBase ? pal.dark : pal.light;
	if (g.kind == "ticker" && !lightBase)
		g.color2 = (pal.dark & 0x00FFFFFF) | 0xF0000000; // 跑馬燈底條略透明
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
		{"lower", "split", 0xFF2563EB, 0xFF0B1735, 0xFFFFFFFF},
		{"lower", "bracket", 0xFF0ABAB5, 0xFF000000, 0xFFFFFFFF},
		{"lower", "ribbon", 0xFFD7263D, 0xFF101826, 0xFFFFFFFF},
		{"lower", "underline", 0xFF0ABAB5, 0xFF000000, 0xFFFFFFFF},
		{"clock", "flip", 0xFFF97316, 0xFF1F2937, 0xFFFFFFFF},
		{"clock", "pill", 0xFF0ABAB5, 0xFF062B2A, 0xFFFFFFFF},
		{"clock", "minimal", 0xFF0ABAB5, 0xFF000000, 0xFFFFFFFF},
		{"ticker", "glass", 0xFF0ABAB5, 0xFF000000, 0xFFFFFFFF},
		{"ticker", "sport", 0xFFF97316, 0xFF111827, 0xFFFFFFFF},
		{"ticker", "minimal", 0xFFD7263D, 0xFF000000, 0xFFFFFFFF},
		{"text", "neon", 0xFFEC4899, 0xFF000000, 0xFFFFFFFF},
		{"text", "gradient", 0xFFF59E0B, 0xFF1F1300, 0xFFFFFFFF},
		{"text", "ribbon", 0xFFD7263D, 0xFF000000, 0xFFFFFFFF},
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
	} else if (g.style == "glass") {
		p.fillRect(QRectF(0, 0, W, h), A(c2, 0.62));
		p.fillRect(QRectF(0, 0, W, 2 * s), A(Qt::white, 0.22));
		if (!label.isEmpty()) {
			const double lw = TextW(L.labelF, label) + 44 * s;
			const QRectF chip(14 * s, h * 0.15, lw, h * 0.70);
			p.fillPath(Rounded(chip, chip.height() / 2), c1);
			DrawText(p, L.labelF, chip.left() + 22 * s, chip.top() + (chip.height() - QFontMetricsF(L.labelF).height()) / 2,
				 label, Qt::white);
			textX = chip.right() + 24 * s;
		}
	} else if (g.style == "sport") {
		p.fillRect(QRectF(0, 0, W, h), c2);
		p.fillRect(QRectF(0, h - 4 * s, W, 4 * s), c1);
		if (!label.isEmpty()) {
			const double slant = h * 0.45;
			const double lw = TextW(L.labelF, label) + 60 * s + slant;
			p.fillPath(Slanted(0, 0, lw + 18 * s, h, slant), tc);
			p.fillPath(Slanted(0, 0, lw, h, slant), c1);
			DrawText(p, L.labelF, 24 * s, (h - QFontMetricsF(L.labelF).height()) / 2, label, Qt::white);
			textX = lw + 36 * s;
		}
	} else if (g.style == "minimal") {
		// 沒有底條：文字加描邊直接壓在畫面上
		if (!label.isEmpty()) {
			const double lw = TextW(L.labelF, label) + 40 * s;
			const QRectF chip(10 * s, h * 0.14, lw, h * 0.72);
			p.fillPath(Rounded(chip, 8 * s), c1);
			DrawText(p, L.labelF, chip.left() + 20 * s, chip.top() + (chip.height() - QFontMetricsF(L.labelF).height()) / 2,
				 label, Qt::white);
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
	if (g.style == "minimal")
		DrawText(q, L.textF, 4 * s, (h - QFontMetricsF(L.textF).height()) / 2, text, textColor, 7 * s, c2);
	else
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
