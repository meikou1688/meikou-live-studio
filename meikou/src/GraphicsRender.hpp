#pragma once

#include <QImage>
#include <QRect>

#include <cstdint>
#include <string>
#include <vector>

namespace meikou {

// 內建圖卡（姓名條、時鐘、跑馬燈、台標、文字）的設計參數。
// 圖卡用 Qt 自己畫成圖片再交給 libobs 顯示，所以大小剛好等於圖卡本身。
struct GraphicSpec {
	std::string kind;  // logo / lower / ticker / clock / image / text
	std::string style; // 版型 id，見 GraphicStyles()
	std::string text1; // 姓名／跑馬燈內容／時鐘前置文字／文字／文字台標
	std::string text2; // 職稱
	std::string file;  // 台標、圖片的檔案
	std::string label; // 跑馬燈標籤（例如「快訊」）
	uint32_t color = 0xFFD7263D;     // 主色 0xAARRGGBB
	uint32_t color2 = 0xFF111827;    // 輔色
	uint32_t textColor = 0xFFFFFFFF; // 文字色
	bool showSeconds = true;
	bool h12 = false;
	bool showDate = false;
	int speed = 120;   // 跑馬燈速度（像素／秒）
	int opacity = 100; // 不透明度 %
	int size = 100;    // 字級 %
	bool italic = false; // 字形：false 正體、true 斜體
	bool vertical = false; // 姓名條方向：false 橫式、true 直式
	std::string textPos = "right"; // 台標文字位置：right／left／top／bottom
};

struct StyleChoice {
	std::string id;
	std::string name;
};

std::vector<StyleChoice> GraphicStyles(const std::string &kind);

// 預設配色組：主色、深色、淺色。套用時依版型決定底色用深色或淺色
struct Palette {
	std::string name;
	uint32_t primary, dark, light;
};
std::vector<Palette> GraphicPalettes();
void ApplyPalette(GraphicSpec &g, const Palette &p);
// 新增素材時的預設樣式與配色
void ApplyGraphicDefaults(GraphicSpec &g);
// 換版型時套用該版型建議的配色
void ApplyStyleColors(GraphicSpec &g);

// 載入內建的思源黑體（Noto Sans TC Black／Bold）
void LoadBrandFonts();

QImage RenderGraphic(const GraphicSpec &g);

struct TickerParts {
	QImage bar;  // 底條（含標籤）
	QImage text; // 一段要捲動的文字（含尾端間距），會重複接續
	QRect area;  // 文字可見範圍（在底條座標裡）
};
TickerParts RenderTicker(const GraphicSpec &g);

// 給編輯視窗用的預覽（跑馬燈會把文字直接放在底條上）
QImage RenderPreviewImage(const GraphicSpec &g);

} // namespace meikou
