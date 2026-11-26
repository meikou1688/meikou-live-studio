#pragma once

#include <QDialog>

#include <obs.h>

class QFormLayout;
class QScrollArea;

namespace meikou {

// 通用的來源設定視窗：直接讀 libobs 外掛提供的設定項目（裝置、解析度、檔案、網址…），
// 修改即時生效；按「取消」還原。
class PropertiesDialog : public QDialog {
	Q_OBJECT

public:
	PropertiesDialog(obs_source_t *source, const QString &title, QWidget *parent = nullptr);
	~PropertiesDialog() override;

protected:
	void reject() override;

private:
	void Rebuild();
	void AddProperties(obs_properties_t *props, QFormLayout *form);
	void Changed(obs_property_t *p);

	obs_source_t *source_ = nullptr;
	obs_data_t *settings_ = nullptr;
	obs_properties_t *props_ = nullptr;
	QScrollArea *scroll_ = nullptr;
	QString original_;
	bool rebuildPending_ = false;
};

} // namespace meikou
