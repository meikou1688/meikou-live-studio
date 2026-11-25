#pragma once

#include "ObsCore.hpp"

#include <QMainWindow>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTimer;

namespace meikou {

class PreviewWidget;

class MainWindow : public QMainWindow {
	Q_OBJECT

public:
	explicit MainWindow(ObsCore &core, QWidget *parent = nullptr);
	~MainWindow() override;

protected:
	void closeEvent(QCloseEvent *event) override;

private:
	QWidget *BuildSourcesPanel();
	QWidget *BuildAudioPanel();
	QWidget *BuildStreamPanel();

	void RefreshCameras();
	void AddItem(obs_sceneitem_t *item, const QString &label);
	void RemoveSelected();
	void ToggleStream();
	void OnStreamEvent(const QString &kind, const QString &message);
	void UpdateStats();
	void LoadSettings();
	void SaveSettings();
	void SetStreamUi(bool streaming);

	ObsCore &core_;
	PreviewWidget *preview_ = nullptr;

	QComboBox *cameraBox_ = nullptr;
	QLineEdit *urlEdit_ = nullptr;
	QListWidget *sourceList_ = nullptr;

	QComboBox *platformBox_ = nullptr;
	QLineEdit *serverEdit_ = nullptr;
	QLineEdit *keyEdit_ = nullptr;
	QComboBox *encoderBox_ = nullptr;
	QSpinBox *bitrateBox_ = nullptr;
	QCheckBox *lowLatencyBox_ = nullptr;
	QCheckBox *dynBitrateBox_ = nullptr;
	QPushButton *streamButton_ = nullptr;
	QLabel *streamStatus_ = nullptr;

	QLabel *statsLabel_ = nullptr;
	QTimer *statsTimer_ = nullptr;
	bool stopping_ = false;
};

} // namespace meikou
