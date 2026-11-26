#pragma once

#include "GraphicsRender.hpp"

#include <obs.h>

namespace meikou {

// 程式內建的 libobs 來源「meikou_graphic」：顯示 Qt 畫好的圖卡，大小等於圖卡本身；
// 跑馬燈模式會在底條的文字區裡平順捲動文字。
inline constexpr const char *kGraphicSourceId = "meikou_graphic";

void RegisterGraphicSource();
void GraphicPush(obs_source_t *source, const QImage &image);
void GraphicPushTicker(obs_source_t *source, const TickerParts &parts, int speed);

} // namespace meikou
