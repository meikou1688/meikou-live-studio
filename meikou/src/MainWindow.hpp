#pragma once

#include "Engine.hpp"

#include <QMainWindow>

class QCheckBox;
class QComboBox;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;

namespace meikou {

class AudioMixer;
class Display;

class MainWindow : public QMainWindow {
	Q_OBJECT

public:
	MainWindow(Engine &engine, const QString &projectPath, QWidget *parent = nullptr);
	~MainWindow() override;

protected:
	void closeEvent(QCloseEvent *event) override;

private:
	QWidget *BuildTopBar();
	QWidget *BuildSwitcher();
	QWidget *BuildSourcesTab();
	QWidget *BuildOverlayTab();
	QWidget *BuildOutputTab();
	void SetupShortcuts();

	void RebuildBus();
	void RebuildScenes();
	void RefreshBusState();
	void RebuildOverlayButtons();
	void RefreshOverlayButtons();
	void RebuildSourceList();
	void RebuildOverlayList();
	void RebuildTargets();
	void RefreshTargetStatus();

	void AddSourceOfType(const QString &kind);
	void EditSource(const std::string &uid);
	void RenameSource(const std::string &uid);
	void ComposeSource(const std::string &uid);
	void NewScene();
	void AddSourceToScene(const std::string &sceneUid, const std::string &sourceUid);
	void RemoveSource(const std::string &uid);
	void AddOverlayOfKind(const QString &kind);
	void EditOverlay(const std::string &uid);
	void SourcesChanged();

	void DoCut();
	void DoAuto();
	void ToggleStream();
	void ToggleRecord();
	EncodeConfig CurrentEncodeConfig() const;
	void OnOutputEvent(int index, const QString &kind, const QString &message);
	void UpdateStats();
	void ApplyFps();

	void ScheduleSave();
	void SaveProject();
	void LoadSettings();
	void SaveSettings();

	Engine &engine_;
	QString projectPath_;

	Display *pvwDisplay_ = nullptr;
	Display *pgmDisplay_ = nullptr;
	QHBoxLayout *busLayout_ = nullptr;
	QHBoxLayout *ovlLayout_ = nullptr;
	QCheckBox *directSend_ = nullptr;
	QComboBox *transitionBox_ = nullptr;
	QSpinBox *durationBox_ = nullptr;
	QPushButton *cutButton_ = nullptr;
	QPushButton *autoButton_ = nullptr;

	QListWidget *sourceList_ = nullptr;
	QListWidget *sceneList_ = nullptr;
	QListWidget *overlayList_ = nullptr;
	AudioMixer *mixer_ = nullptr;

	QTableWidget *targetTable_ = nullptr;
	QComboBox *encoderBox_ = nullptr;
	QSpinBox *bitrateBox_ = nullptr;
	QComboBox *fpsBox_ = nullptr;
	QCheckBox *lowLatencyBox_ = nullptr;
	QCheckBox *dynBitrateBox_ = nullptr;
	QPushButton *streamButton_ = nullptr;
	QPushButton *recordButton_ = nullptr;
	QLabel *outputStatus_ = nullptr;

	QLabel *liveLabel_ = nullptr;
	QLabel *recLabel_ = nullptr;
	QLabel *statsLabel_ = nullptr;
	QLabel *clockLabel_ = nullptr;

	QTimer *statsTimer_ = nullptr;
	QTimer *saveTimer_ = nullptr;
	bool stoppingStream_ = false;
	int appliedFpsIndex_ = 3;
};

} // namespace meikou
