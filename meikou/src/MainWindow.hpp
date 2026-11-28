#pragma once

#include "Engine.hpp"
#include "Updater.hpp"

#include <QMainWindow>

#include <vector>

class QAction;
class QCheckBox;
class QGroupBox;
class QSplitter;
class QComboBox;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QLayout;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QVBoxLayout;
class QTimer;

namespace meikou {

class AudioMixer;
class Display;
class PhoneLink;

class MainWindow : public QMainWindow {
	Q_OBJECT

public:
	MainWindow(Engine &engine, const QString &projectPath, QWidget *parent = nullptr);
	~MainWindow() override;

public:
	// 上次沒有正常關閉時，提醒匯出問題報告
	void ShowUncleanShutdownNotice();
	// 啟動後在背景檢查更新（設定裡可以關掉）
	void StartAutoUpdateCheck();

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
	void AddOverlayToCurrentScene(const std::string &overlayUid);
	Input *CurrentScene();
	Input *EnsureScene();
	void SelectScene(const std::string &uid);
	void ToggleLayer(int64_t layerId);
	void ShowLayerMenu(QWidget *anchor, const QPoint &pos, const LayerInfo &layer);
	void RebuildRows();
	static bool IsMediaInput(const Input *in);
	void RefreshMediaLabels();
	void RemoveSource(const std::string &uid);
	void AddOverlayOfKind(const QString &kind);
	void EditOverlay(const std::string &uid);
	void SourcesChanged();
	// 手機攝影機（Meikou Live 手機版）
	std::string AddPhoneCamera();
	void ShowPhoneDialog(const std::string &focusUid = std::string());
	void UpdatePhoneButton();

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
	QLayout *busLayout_ = nullptr; // 自動換行排列
	QLayout *ovlLayout_ = nullptr;
	QGroupBox *busGroup_ = nullptr;
	QWidget *rowsPanel_ = nullptr; // 下方左半：場景／訊號源／疊加
	QSplitter *mainSplit_ = nullptr;
	QSplitter *bottomSplit_ = nullptr;
	QPushButton *layoutBtn_ = nullptr;
	QAction *lockAction_ = nullptr;
	QAction *resetLayoutAction_ = nullptr;
	bool layoutLocked_ = false;
	void SetLayoutLocked(bool locked, bool save);
	QGroupBox *ovlGroup_ = nullptr;
	std::string currentScene_;
	QString rowSig_;
	QComboBox *transitionBox_ = nullptr;
	QSpinBox *durationBox_ = nullptr;
	QPushButton *cutButton_ = nullptr;
	QPushButton *autoButton_ = nullptr;

	QListWidget *sourceList_ = nullptr;
	QListWidget *sceneList_ = nullptr;
	QListWidget *overlayList_ = nullptr;
	AudioMixer *mixer_ = nullptr;

	QWidget *targetBox_ = nullptr; // 推流目標卡片的容器
	QVBoxLayout *targetLayout_ = nullptr;
	struct TargetRow {
		QLineEdit *server = nullptr;
		QLineEdit *key = nullptr;
		QLabel *status = nullptr;
		QPushButton *summary = nullptr;
		QWidget *detail = nullptr;
	};
	std::vector<TargetRow> targetRows_;
	void SetTargetExpanded(int row, bool expanded);
	void UpdateTargetSummary(int row);
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
	QPushButton *updateBtn_ = nullptr; // 查到新版時出現在頂列
	QPushButton *phoneBtn_ = nullptr;  // 手機攝影機：QR Code 與連線狀態
	PhoneLink *phone_ = nullptr;
	ReleaseInfo newRelease_;
	void OpenUpdateDialog(bool useKnownRelease);
	void ExportReport();
	void RefreshPageStreams();

	QTimer *statsTimer_ = nullptr;
	QTimer *saveTimer_ = nullptr;
	bool stoppingStream_ = false;
	int appliedFpsIndex_ = 3;
};

} // namespace meikou
