#include "MainWindow.hpp"

#include "AudioMixer.hpp"
#include "Display.hpp"
#include "Log.hpp"
#include "OverlayDialog.hpp"
#include "Platform.hpp"
#include "PropertiesDialog.hpp"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTime>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <util/platform.h>

#include <algorithm>

namespace meikou {

namespace {

QString Q(const std::string &s)
{
	return QString::fromUtf8(s.c_str());
}

std::string S(const QString &s)
{
	return s.toUtf8().toStdString();
}

struct PlatformPreset {
	const char *name;
	const char *server;
};

const PlatformPreset kPlatforms[] = {
	{"YouTube", "rtmp://a.rtmp.youtube.com/live2"},
	{"YouTube（RTMPS 加密）", "rtmps://a.rtmps.youtube.com:443/live2"},
	{"Facebook", "rtmps://live-api-s.facebook.com:443/rtmp/"},
	{"Twitch", "rtmp://live.twitch.tv/app"},
	{"自訂 RTMP／RTMPS", ""},
};

struct Fps {
	const char *name;
	int num, den;
};

const Fps kFps[] = {
	{"24", 24, 1}, {"25", 25, 1}, {"29.97", 30000, 1001}, {"30", 30, 1}, {"60", 60, 1},
};
constexpr int kFpsCount = (int)(sizeof(kFps) / sizeof(kFps[0]));

const char *kOverlayKeys = "QWERTYUIOP";

QString KindName(const std::string &kind)
{
	if (kind == "logo")
		return QStringLiteral("台標");
	if (kind == "lower")
		return QStringLiteral("姓名條");
	if (kind == "ticker")
		return QStringLiteral("跑馬燈");
	if (kind == "clock")
		return QStringLiteral("時鐘");
	if (kind == "text")
		return QStringLiteral("文字");
	return QStringLiteral("圖片");
}

QString FmtTime(uint64_t startNs)
{
	if (!startNs)
		return QStringLiteral("00:00:00");
	const qulonglong secs = (os_gettime_ns() - startNs) / 1000000000ULL;
	return QStringLiteral("%1:%2:%3")
		.arg(secs / 3600, 2, 10, QLatin1Char('0'))
		.arg((secs / 60) % 60, 2, 10, QLatin1Char('0'))
		.arg(secs % 60, 2, 10, QLatin1Char('0'));
}

QPushButton *BigButton(const QString &text, const QString &objName)
{
	auto *b = new QPushButton(text);
	b->setObjectName(objName);
	b->setMinimumHeight(44);
	b->setCursor(Qt::PointingHandCursor);
	return b;
}

void Repolish(QWidget *w)
{
	w->style()->unpolish(w);
	w->style()->polish(w);
	w->update();
}

// 正在輸入文字或數字時，快捷鍵不要搶走按鍵（例如在金鑰欄按 Enter 不該切換畫面）
bool TypingFocus()
{
	QWidget *f = QApplication::focusWidget();
	if (!f)
		return false;
	if (qobject_cast<QLineEdit *>(f) || qobject_cast<QAbstractSpinBox *>(f))
		return true;
	if (auto *c = qobject_cast<QComboBox *>(f))
		return c->isEditable();
	return false;
}

} // namespace

MainWindow::MainWindow(Engine &engine, const QString &projectPath, QWidget *parent)
	: QMainWindow(parent),
	  engine_(engine),
	  projectPath_(projectPath)
{
	setWindowTitle(QStringLiteral("Meikou Live Studio ") + QString::fromUtf8(MEIKOU_VERSION));
	resize(1600, 940);

	auto *central = new QWidget;
	auto *root = new QVBoxLayout(central);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(0);
	root->addWidget(BuildTopBar());

	auto *split = new QSplitter(Qt::Horizontal);
	split->addWidget(BuildSwitcher());

	auto *tabs = new QTabWidget;
	tabs->setMinimumWidth(420);
	mixer_ = new AudioMixer(engine_);
	tabs->addTab(mixer_, QStringLiteral("混音台"));
	tabs->addTab(BuildOutputTab(), QStringLiteral("推流／錄影"));
	tabs->addTab(BuildOverlayTab(), QStringLiteral("疊加素材"));
	tabs->addTab(BuildSourcesTab(), QStringLiteral("訊號源"));
	split->addWidget(tabs);
	split->setStretchFactor(0, 1);
	split->setStretchFactor(1, 0);
	split->setSizes({1150, 450});
	root->addWidget(split, 1);
	setCentralWidget(central);

	engine_.onOutputEvent = [this](int index, const std::string &kind, const std::string &message) {
		const QString k = Q(kind), m = Q(message);
		QMetaObject::invokeMethod(
			this, [this, index, k, m] { OnOutputEvent(index, k, m); }, Qt::QueuedConnection);
	};

	saveTimer_ = new QTimer(this);
	saveTimer_->setSingleShot(true);
	saveTimer_->setInterval(1500);
	connect(saveTimer_, &QTimer::timeout, this, &MainWindow::SaveProject);

	statsTimer_ = new QTimer(this);
	connect(statsTimer_, &QTimer::timeout, this, &MainWindow::UpdateStats);
	statsTimer_->start(1000);

	LoadSettings();
	SetupShortcuts();
	RebuildBus();
	RebuildSourceList();
	mixer_->Rebuild();
	RebuildScenes();
	RebuildOverlayButtons();
	RebuildOverlayList();
	RebuildTargets();
	UpdateStats();
}

MainWindow::~MainWindow()
{
	engine_.onOutputEvent = nullptr;
}

// ---------------------------------------------------------------- 版面

QWidget *MainWindow::BuildTopBar()
{
	auto *bar = new QFrame;
	bar->setObjectName(QStringLiteral("topBar"));
	auto *h = new QHBoxLayout(bar);
	h->setContentsMargins(14, 8, 14, 8);
	h->setSpacing(14);

	// 頂列放橫式字標（Meikou Live Studio）
	auto *logo = new QLabel;
	QPixmap pm(QStringLiteral(":/meikou/logo.png"));
	if (!pm.isNull()) {
		const qreal dpr = devicePixelRatioF();
		QPixmap scaled = pm.scaledToHeight((int)(38 * dpr), Qt::SmoothTransformation);
		scaled.setDevicePixelRatio(dpr);
		logo->setPixmap(scaled);
	} else {
		logo->setText(QStringLiteral("Meikou Live Studio"));
		logo->setObjectName(QStringLiteral("brand"));
	}
	h->addWidget(logo);
	h->addSpacing(16);

	liveLabel_ = new QLabel(QStringLiteral("● 未直播"));
	liveLabel_->setObjectName(QStringLiteral("pill"));
	recLabel_ = new QLabel(QStringLiteral("● 未錄影"));
	recLabel_->setObjectName(QStringLiteral("pill"));
	h->addWidget(liveLabel_);
	h->addWidget(recLabel_);
	h->addStretch(1);
	statsLabel_ = new QLabel;
	statsLabel_->setObjectName(QStringLiteral("stats"));
	h->addWidget(statsLabel_);
	// 查到新版時才出現
	updateBtn_ = new QPushButton;
	updateBtn_->setObjectName(QStringLiteral("accent"));
	updateBtn_->setVisible(false);
	connect(updateBtn_, &QPushButton::clicked, this, [this] { OpenUpdateDialog(true); });
	h->addWidget(updateBtn_);

	// 說明：更新歷程、檢查更新、問題報告、關於
	auto *helpBtn = new QPushButton(QStringLiteral("說明"));
	helpBtn->setObjectName(QStringLiteral("flat"));
	auto *help = new QMenu(helpBtn);
	help->addAction(QStringLiteral("更新歷程…"), this, [this] { ShowChangelogDialog(this); });
	help->addAction(QStringLiteral("檢查更新…"), this, [this] { OpenUpdateDialog(false); });
	QAction *autoCheck = help->addAction(QStringLiteral("啟動時自動檢查更新"));
	autoCheck->setCheckable(true);
	autoCheck->setChecked(QSettings().value("update/autoCheck", true).toBool());
	connect(autoCheck, &QAction::toggled, this, [](bool on) { QSettings().setValue("update/autoCheck", on); });
	help->addSeparator();
	help->addAction(QStringLiteral("匯出問題報告…"), this, [this] { ExportReport(); });
	help->addAction(QStringLiteral("開啟紀錄資料夾"), this, [] {
		QFileInfo fi(Q(CurrentLogPath()));
		QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
	});
	help->addSeparator();
	help->addAction(QStringLiteral("關於 Meikou Live Studio"), this, [this] { ShowAboutDialog(this); });
	helpBtn->setMenu(help);
	h->addWidget(helpBtn);
	clockLabel_ = new QLabel;
	clockLabel_->setObjectName(QStringLiteral("clock"));
	h->addWidget(clockLabel_);
	return bar;
}

QWidget *MainWindow::BuildSwitcher()
{
	auto *w = new QWidget;
	auto *v = new QVBoxLayout(w);
	v->setContentsMargins(10, 10, 10, 10);
	v->setSpacing(10);

	// 預覽 ｜ 轉場 ｜ 播出
	auto *monitors = new QHBoxLayout;
	monitors->setSpacing(10);

	auto *pvwBox = new QVBoxLayout;
	auto *pvwTitle = new QLabel(
		QStringLiteral("預覽 PVW　<span style='color:#8a90a0'>點選後拖曳移動、拖四個角縮放、右鍵更多選項</span>"));
	pvwTitle->setObjectName(QStringLiteral("pvwTitle"));
	pvwDisplay_ = new Display(engine_, Display::Mode::Preview);
	pvwDisplay_->onOverlayChanged = [this] { ScheduleSave(); };
	pvwDisplay_->onLayersChanged = [this] {
		rowSig_.clear();
		RefreshBusState();
		ScheduleSave();
	};
	auto *pvwFrame = new QFrame;
	pvwFrame->setObjectName(QStringLiteral("pvwFrame"));
	auto *pf = new QVBoxLayout(pvwFrame);
	pf->setContentsMargins(3, 3, 3, 3);
	pf->addWidget(pvwDisplay_);
	pvwBox->addWidget(pvwTitle);
	pvwBox->addWidget(pvwFrame, 1);

	auto *mid = new QVBoxLayout;
	mid->setSpacing(8);
	mid->addStretch(1);
	cutButton_ = BigButton(QStringLiteral("送出 CUT"), QStringLiteral("cut"));
	autoButton_ = BigButton(QStringLiteral("送出 AUTO"), QStringLiteral("take"));
	cutButton_->setToolTip(QStringLiteral("直接切換（Enter）。按一次送出，再按一次撤回到預覽"));
	autoButton_->setToolTip(QStringLiteral("轉場切換（空白鍵）。按一次送出，再按一次撤回到預覽"));
	connect(cutButton_, &QPushButton::clicked, this, &MainWindow::DoCut);
	connect(autoButton_, &QPushButton::clicked, this, &MainWindow::DoAuto);
	transitionBox_ = new QComboBox;
	for (const auto &t : engine_.TransitionTypes())
		transitionBox_->addItem(Q(t.name), Q(t.id));
	transitionBox_->setCurrentIndex(std::max(0, transitionBox_->findData(Q(engine_.TransitionType()))));
	connect(transitionBox_, &QComboBox::activated, this, [this](int) {
		engine_.SetTransitionType(S(transitionBox_->currentData().toString()));
		ScheduleSave();
	});
	durationBox_ = new QSpinBox;
	durationBox_->setRange(0, 5000);
	durationBox_->setSingleStep(100);
	durationBox_->setSuffix(QStringLiteral(" ms"));
	durationBox_->setValue(engine_.TransitionDuration());
	connect(durationBox_, &QSpinBox::valueChanged, this, [this](int value) {
		engine_.SetTransitionDuration(value);
		ScheduleSave();
	});
	mid->addWidget(cutButton_);
	mid->addWidget(autoButton_);
	mid->addWidget(new QLabel(QStringLiteral("轉場")));
	mid->addWidget(transitionBox_);
	mid->addWidget(durationBox_);
	mid->addStretch(1);
	auto *midW = new QWidget;
	midW->setLayout(mid);
	midW->setFixedWidth(150); // 「送出 AUTO／撤回 AUTO」要放得下

	auto *pgmBox = new QVBoxLayout;
	auto *pgmTitle = new QLabel(QStringLiteral("播出 PGM"));
	pgmTitle->setObjectName(QStringLiteral("pgmTitle"));
	pgmDisplay_ = new Display(engine_, Display::Mode::Program);
	auto *pgmFrame = new QFrame;
	pgmFrame->setObjectName(QStringLiteral("pgmFrame"));
	auto *gf = new QVBoxLayout(pgmFrame);
	gf->setContentsMargins(3, 3, 3, 3);
	gf->addWidget(pgmDisplay_);
	pgmBox->addWidget(pgmTitle);
	pgmBox->addWidget(pgmFrame, 1);

	monitors->addLayout(pvwBox, 1);
	monitors->addWidget(midW);
	monitors->addLayout(pgmBox, 1);
	v->addLayout(monitors, 1);

	// 場景列：點一下放到預覽並成為「目前場景」，下面兩列只顯示這個場景的內容
	auto *sceneGroup = new QGroupBox(QStringLiteral("場景　（點一下放到預覽，F1–F12 快捷鍵；按「送出」上播出，再按一次撤回到預覽）"));
	auto *sceneOuter = new QHBoxLayout(sceneGroup);
	sceneList_ = new QListWidget;
	sceneList_->setObjectName(QStringLiteral("sceneList"));
	sceneList_->setViewMode(QListView::IconMode);
	sceneList_->setFlow(QListView::LeftToRight);
	sceneList_->setWrapping(true);
	sceneList_->setResizeMode(QListView::Adjust);
	sceneList_->setMovement(QListView::Static);
	sceneList_->setGridSize(QSize(150, 56));
	sceneList_->setUniformItemSizes(true);
	sceneList_->setSelectionMode(QAbstractItemView::NoSelection);
	sceneList_->setFocusPolicy(Qt::NoFocus);
	sceneList_->setFixedHeight(128);
	sceneList_->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(sceneList_, &QListWidget::itemClicked, this,
		[this](QListWidgetItem *it) { SelectScene(S(it->data(Qt::UserRole).toString())); });
	connect(sceneList_, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
		QListWidgetItem *it = sceneList_->itemAt(pos);
		if (!it)
			return;
		const std::string uid = S(it->data(Qt::UserRole).toString());
		if (uid.empty())
			return;
		QMenu m;
		QAction *rename = m.addAction(QStringLiteral("重新命名…"));
		QAction *dup = m.addAction(QStringLiteral("複製場景"));
		m.addSeparator();
		QAction *remove = m.addAction(QStringLiteral("刪除場景"));
		QAction *chosen = m.exec(sceneList_->viewport()->mapToGlobal(pos));
		if (chosen == rename)
			QTimer::singleShot(0, this, [this, uid] { RenameSource(uid); });
		else if (chosen == dup)
			QTimer::singleShot(0, this, [this, uid] {
				if (Input *c = engine_.DuplicateScene(uid)) {
					SourcesChanged();
					SelectScene(c->uid);
				}
			});
		else if (chosen == remove)
			QTimer::singleShot(0, this, [this, uid] { RemoveSource(uid); });
	});
	sceneOuter->addWidget(sceneList_, 1);
	auto *newScene = new QPushButton(QStringLiteral("＋ 新增場景"));
	newScene->setObjectName(QStringLiteral("accent"));
	connect(newScene, &QPushButton::clicked, this, &MainWindow::NewScene);
	auto *sceneBtns = new QVBoxLayout;
	sceneBtns->addWidget(newScene);
	sceneBtns->addStretch(1);
	sceneOuter->addLayout(sceneBtns);
	v->addWidget(sceneGroup);

	// 目前場景的訊號源
	busGroup_ = new QGroupBox;
	auto *busOuter = new QHBoxLayout(busGroup_);
	auto *busScroll = new QScrollArea;
	busScroll->setWidgetResizable(true);
	busScroll->setFrameShape(QFrame::NoFrame);
	busScroll->setFixedHeight(78);
	auto *busInner = new QWidget;
	busLayout_ = new QHBoxLayout(busInner);
	busLayout_->setContentsMargins(0, 0, 0, 0);
	busLayout_->setSpacing(8);
	busScroll->setWidget(busInner);
	busOuter->addWidget(busScroll, 1);
	auto *addSrc = new QPushButton(QStringLiteral("＋ 新增訊號源"));
	addSrc->setObjectName(QStringLiteral("accent"));
	auto *menu = new QMenu(addSrc);
	struct Item {
		const char *label;
		const char *kind;
	};
	const Item items[] = {
		{"攝影機／擷取卡／導播機 USB", "camera"},
		{"螢幕擷取", "screen"},
		{"視窗擷取", "window"},
		{"遊戲擷取", "game"},
		{"網路影片（RTMP／SRT／RTSP／HLS／網址）", "url"},
		{"影片／音樂檔案", "media"},
		{"圖片", "image"},
		{"純色", "color"},
		{"文字", "text"},
		{"麥克風／音訊介面", "mic"},
		{"系統聲音（桌面音訊）", "desktop"},
		{"指定程式的聲音", "app"},
	};
	for (const auto &it : items) {
		const QString kind = QString::fromLatin1(it.kind);
		if (kind == QLatin1String("game") && platform::kGame == nullptr)
			continue;
		QAction *a = menu->addAction(QString::fromUtf8(it.label));
		connect(a, &QAction::triggered, this, [this, kind] { AddSourceOfType(kind); });
	}
	addSrc->setMenu(menu);
	auto *addExisting = new QPushButton(QStringLiteral("＋ 加入既有"));
	addExisting->setToolTip(QStringLiteral("把其他場景用過的訊號源加進目前場景"));
	auto *existMenu = new QMenu(addExisting);
	connect(existMenu, &QMenu::aboutToShow, this, [this, existMenu] {
		existMenu->clear();
		Input *scene = CurrentScene();
		const auto layers = engine_.SceneLayers(scene);
		for (Input *in : engine_.VideoInputs()) {
			bool inScene = false;
			for (const auto &l : layers)
				inScene = inScene || l.uid == in->uid;
			if (inScene)
				continue;
			const std::string uid = in->uid;
			existMenu->addAction(Q(in->name), this, [this, uid] { ComposeSource(uid); });
		}
		if (existMenu->isEmpty())
			existMenu->addAction(QStringLiteral("（沒有其他訊號源）"))->setEnabled(false);
	});
	addExisting->setMenu(existMenu);
	auto *btnCol = new QVBoxLayout;
	btnCol->setSpacing(6);
	btnCol->addWidget(addSrc);
	btnCol->addWidget(addExisting);
	busOuter->addLayout(btnCol);
	v->addWidget(busGroup_);

	// 目前場景的疊加
	ovlGroup_ = new QGroupBox;
	auto *ovlOuter = new QHBoxLayout(ovlGroup_);
	auto *ovlInner = new QWidget;
	ovlLayout_ = new QHBoxLayout(ovlInner);
	ovlLayout_->setContentsMargins(0, 0, 0, 0);
	ovlLayout_->setSpacing(8);
	ovlOuter->addWidget(ovlInner, 1);
	auto *addOvl = new QPushButton(QStringLiteral("＋ 新增疊加"));
	addOvl->setObjectName(QStringLiteral("accent"));
	auto *ovlMenu = new QMenu(addOvl);
	const char *kinds[][2] = {{"台標", "logo"}, {"姓名條", "lower"}, {"跑馬燈", "ticker"},
				  {"時鐘", "clock"}, {"圖片", "image"},   {"文字", "text"}};
	for (const auto &k : kinds) {
		const QString kind = QString::fromLatin1(k[1]);
		ovlMenu->addAction(QString::fromUtf8(k[0]), this, [this, kind] { AddOverlayOfKind(kind); });
	}
	addOvl->setMenu(ovlMenu);
	auto *addExistingOvl = new QPushButton(QStringLiteral("＋ 加入既有"));
	addExistingOvl->setToolTip(QStringLiteral("把其他場景用過的疊加素材加進目前場景"));
	auto *existOvlMenu = new QMenu(addExistingOvl);
	connect(existOvlMenu, &QMenu::aboutToShow, this, [this, existOvlMenu] {
		existOvlMenu->clear();
		const auto layers = engine_.SceneLayers(CurrentScene());
		for (const auto &ov : engine_.Overlays()) {
			bool inScene = false;
			for (const auto &l : layers)
				inScene = inScene || l.uid == ov->uid;
			if (inScene)
				continue;
			const std::string uid = ov->uid;
			existOvlMenu->addAction(Q(ov->name), this, [this, uid] { AddOverlayToCurrentScene(uid); });
		}
		if (existOvlMenu->isEmpty())
			existOvlMenu->addAction(QStringLiteral("（沒有其他疊加素材）"))->setEnabled(false);
	});
	addExistingOvl->setMenu(existOvlMenu);
	auto *ovlCol = new QVBoxLayout;
	ovlCol->setSpacing(6);
	ovlCol->addWidget(addOvl);
	ovlCol->addWidget(addExistingOvl);
	ovlOuter->addLayout(ovlCol);
	v->addWidget(ovlGroup_);
	return w;
}

QWidget *MainWindow::BuildSourcesTab()
{
	auto *w = new QWidget;
	auto *v = new QVBoxLayout(w);
	sourceList_ = new QListWidget;
	v->addWidget(new QLabel(QStringLiteral("素材庫：所有訊號源（影像與音訊）。雙擊開啟設定；「加入目前場景」放進正在編輯的場景。")));
	v->addWidget(sourceList_, 1);
	connect(sourceList_, &QListWidget::itemDoubleClicked, this,
		[this](QListWidgetItem *it) { EditSource(S(it->data(Qt::UserRole).toString())); });
	auto *row = new QHBoxLayout;
	auto *props = new QPushButton(QStringLiteral("設定"));
	auto *rename = new QPushButton(QStringLiteral("重新命名"));
	auto *remove = new QPushButton(QStringLiteral("刪除"));
	auto *toScene = new QPushButton(QStringLiteral("加入目前場景"));
	toScene->setObjectName(QStringLiteral("accent"));
	row->addWidget(toScene);
	row->addWidget(props);
	row->addWidget(rename);
	row->addWidget(remove);
	v->addLayout(row);
	auto current = [this] {
		auto *it = sourceList_->currentItem();
		return it ? S(it->data(Qt::UserRole).toString()) : std::string();
	};
	connect(toScene, &QPushButton::clicked, this, [this, current] { ComposeSource(current()); });
	connect(props, &QPushButton::clicked, this, [this, current] { EditSource(current()); });
	connect(rename, &QPushButton::clicked, this, [this, current] { RenameSource(current()); });
	connect(remove, &QPushButton::clicked, this, [this, current] { RemoveSource(current()); });
	return w;
}

QWidget *MainWindow::BuildOverlayTab()
{
	auto *w = new QWidget;
	auto *v = new QVBoxLayout(w);
	auto *grid = new QGridLayout;
	const char *kinds[][2] = {{"台標", "logo"},  {"姓名條", "lower"}, {"跑馬燈", "ticker"},
				  {"時鐘", "clock"}, {"圖片", "image"},   {"文字", "text"}};
	for (int i = 0; i < 6; i++) {
		auto *b = new QPushButton(QStringLiteral("＋ ") + QString::fromUtf8(kinds[i][0]));
		const QString kind = QString::fromLatin1(kinds[i][1]);
		connect(b, &QPushButton::clicked, this, [this, kind] { AddOverlayOfKind(kind); });
		grid->addWidget(b, i / 3, i % 3);
	}
	v->addLayout(grid);
	overlayList_ = new QListWidget;
	v->addWidget(new QLabel(QStringLiteral("素材庫：雙擊開啟設計視窗（版型、配色、字級）；「加入目前場景」放進正在編輯的場景，再到預覽畫面拖曳、縮放。")));
	v->addWidget(overlayList_, 1);
	connect(overlayList_, &QListWidget::itemDoubleClicked, this,
		[this](QListWidgetItem *it) { EditOverlay(S(it->data(Qt::UserRole).toString())); });
	connect(overlayList_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *it, QListWidgetItem *) {
		engine_.SetSelectedOverlay(it ? S(it->data(Qt::UserRole).toString()) : std::string());
	});
	auto *row = new QHBoxLayout;
	auto *edit = new QPushButton(QStringLiteral("編輯"));
	auto *reset = new QPushButton(QStringLiteral("加入目前場景"));
	reset->setObjectName(QStringLiteral("accent"));
	auto *remove = new QPushButton(QStringLiteral("刪除"));
	row->addWidget(reset);
	row->addWidget(edit);
	row->addWidget(remove);
	v->addLayout(row);
	auto current = [this] {
		auto *it = overlayList_->currentItem();
		return it ? S(it->data(Qt::UserRole).toString()) : std::string();
	};
	connect(edit, &QPushButton::clicked, this, [this, current] { EditOverlay(current()); });
	connect(reset, &QPushButton::clicked, this, [this, current] { AddOverlayToCurrentScene(current()); });
	connect(remove, &QPushButton::clicked, this, [this, current] {
		const std::string uid = current();
		if (uid.empty())
			return;
		engine_.RemoveOverlay(uid);
		RebuildOverlayButtons();
		RebuildOverlayList();
		ScheduleSave();
	});
	return w;
}

QWidget *MainWindow::BuildOutputTab()
{
	auto *w = new QWidget;
	auto *v = new QVBoxLayout(w);

	auto *tg = new QGroupBox(QStringLiteral("推流目標（可同時推多個平台，只編碼一次）"));
	auto *tv = new QVBoxLayout(tg);
	targetBox_ = new QWidget;
	targetLayout_ = new QVBoxLayout(targetBox_);
	targetLayout_->setContentsMargins(0, 0, 0, 0);
	targetLayout_->setSpacing(8);
	tv->addWidget(targetBox_);
	auto *trow = new QHBoxLayout;
	auto *addT = new QPushButton(QStringLiteral("＋ 新增目標"));
	trow->addWidget(addT);
	trow->addStretch(1);
	tv->addLayout(trow);
	connect(addT, &QPushButton::clicked, this, [this] {
		if (engine_.Streaming())
			return;
		Target t;
		t.name = kPlatforms[0].name;
		t.server = kPlatforms[0].server;
		engine_.Targets().push_back(t);
		RebuildTargets();
		ScheduleSave();
	});
	v->addWidget(tg);

	auto *eg = new QGroupBox(QStringLiteral("輸出設定（1920×1080）"));
	auto *ef = new QFormLayout(eg);
	encoderBox_ = new QComboBox;
	for (const auto &e : engine_.VideoEncoders())
		encoderBox_->addItem(Q(e.name), Q(e.id));
	bitrateBox_ = new QSpinBox;
	bitrateBox_->setRange(1000, 20000);
	bitrateBox_->setSingleStep(500);
	bitrateBox_->setSuffix(QStringLiteral(" kbps"));
	bitrateBox_->setValue(6000);
	bitrateBox_->setToolTip(
		QStringLiteral("1080p30 建議 4500–6000；1080p60 建議 6000–9000。網路上傳速度至少要碼率的 1.5 倍。"));
	fpsBox_ = new QComboBox;
	for (const auto &f : kFps)
		fpsBox_->addItem(QString::fromLatin1(f.name) + QStringLiteral(" fps"));
	connect(fpsBox_, &QComboBox::activated, this, [this](int) { ApplyFps(); });
	lowLatencyBox_ = new QCheckBox(QStringLiteral("低延遲（互動直播建議開啟）"));
	lowLatencyBox_->setChecked(true);
	dynBitrateBox_ = new QCheckBox(QStringLiteral("網路壅塞時自動降碼率"));
	ef->addRow(QStringLiteral("編碼器"), encoderBox_);
	ef->addRow(QStringLiteral("視訊碼率"), bitrateBox_);
	ef->addRow(QStringLiteral("幀率"), fpsBox_);
	ef->addRow(QString(), lowLatencyBox_);
	ef->addRow(QString(), dynBitrateBox_);
	v->addWidget(eg);

	auto *btns = new QHBoxLayout;
	streamButton_ = BigButton(QStringLiteral("開始推流"), QStringLiteral("stream"));
	recordButton_ = BigButton(QStringLiteral("開始錄影"), QStringLiteral("record"));
	connect(streamButton_, &QPushButton::clicked, this, &MainWindow::ToggleStream);
	connect(recordButton_, &QPushButton::clicked, this, &MainWindow::ToggleRecord);
	btns->addWidget(streamButton_);
	btns->addWidget(recordButton_);
	v->addLayout(btns);
	outputStatus_ = new QLabel(QStringLiteral("未推流"));
	outputStatus_->setWordWrap(true);
	v->addWidget(outputStatus_);
	auto *recDir = new QPushButton(QStringLiteral("開啟錄影資料夾"));
	connect(recDir, &QPushButton::clicked, this, [] {
		const QString dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) +
				    QStringLiteral("/Meikou Live Studio");
		QDir().mkpath(dir);
		QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
	});
	v->addWidget(recDir);
	v->addStretch(1);
	return w;
}

void MainWindow::SetupShortcuts()
{
	// 1–9：目前場景的訊號源顯示／隱藏；Q W E R T Y…：目前場景的疊加顯示／隱藏；F1–F12：場景
	auto layerKey = [this](QKeySequence seq, bool overlays, int index) {
		auto *sc = new QShortcut(seq, this);
		connect(sc, &QShortcut::activated, this, [this, overlays, index] {
			if (TypingFocus())
				return;
			int n = 0;
			for (const auto &l : engine_.SceneLayers(CurrentScene())) {
				if (l.isOverlay != overlays)
					continue;
				if (n++ == index) {
					ToggleLayer(l.id);
					return;
				}
			}
		});
	};
	for (int i = 0; i < 9; i++)
		layerKey(QKeySequence(Qt::Key_1 + i), false, i);
	for (int i = 0; kOverlayKeys[i]; i++)
		layerKey(QKeySequence(QString(QLatin1Char(kOverlayKeys[i]))), true, i);
	for (int i = 0; i < 12; i++) {
		auto *sc = new QShortcut(QKeySequence(Qt::Key_F1 + i), this);
		connect(sc, &QShortcut::activated, this, [this, i] {
			auto scenes = engine_.Scenes();
			if (i < (int)scenes.size())
				SelectScene(scenes[i]->uid);
		});
	}
	auto onKey = [this](int key, void (MainWindow::*fn)()) {
		auto *sc = new QShortcut(QKeySequence(key), this);
		connect(sc, &QShortcut::activated, this, [this, fn] {
			if (!TypingFocus())
				(this->*fn)();
		});
	};
	onKey(Qt::Key_Space, &MainWindow::DoAuto);
	onKey(Qt::Key_Return, &MainWindow::DoCut);
	onKey(Qt::Key_Enter, &MainWindow::DoCut);
}

// ---------------------------------------------------------------- 訊號源

Input *MainWindow::CurrentScene()
{
	Input *in = engine_.FindInput(currentScene_);
	if (in && in->isScene)
		return in;
	Input *pvw = engine_.Preview();
	if (pvw && pvw->isScene) {
		currentScene_ = pvw->uid;
		return pvw;
	}
	Input *pgm = engine_.Program();
	if (pgm && pgm->isScene) {
		currentScene_ = pgm->uid;
		return pgm;
	}
	auto scenes = engine_.Scenes();
	if (!scenes.empty()) {
		currentScene_ = scenes.front()->uid;
		return scenes.front();
	}
	currentScene_.clear();
	return nullptr;
}

Input *MainWindow::EnsureScene()
{
	if (Input *sc = CurrentScene())
		return sc;
	Input *sc = engine_.CreateScene(engine_.NextSceneName());
	if (!sc)
		return nullptr;
	if (!engine_.Preview() && engine_.Program() != sc)
		engine_.SetPreview(sc);
	currentScene_ = sc->uid;
	RebuildScenes();
	return sc;
}

void MainWindow::SelectScene(const std::string &uid)
{
	Input *sc = engine_.FindInput(uid);
	if (!sc || !sc->isScene)
		return;
	currentScene_ = uid;
	// 播出中的場景只選取（下面可以直接開關它的內容），其他場景放到預覽
	if (engine_.Program() != sc)
		engine_.SetPreview(sc);
	rowSig_.clear();
	RefreshBusState();
}

void MainWindow::ToggleLayer(int64_t layerId)
{
	Input *sc = CurrentScene();
	if (!sc)
		return;
	engine_.SetLayerVisible(sc, layerId, !engine_.LayerVisible(sc, layerId));
	RefreshBusState();
	ScheduleSave();
}

void MainWindow::ShowLayerMenu(QWidget *anchor, const QPoint &pos, const LayerInfo &layer)
{
	Input *sc = CurrentScene();
	if (!sc)
		return;
	QMenu m;
	m.addSection(Q(layer.name));
	QAction *edit = m.addAction(layer.isOverlay ? QStringLiteral("編輯設計…") : QStringLiteral("訊號源設定…"));
	QAction *reset = m.addAction(layer.isOverlay ? QStringLiteral("回到預設位置") : QStringLiteral("填滿畫面"));
	m.addSeparator();
	QAction *top = m.addAction(QStringLiteral("移到最上層"));
	QAction *bottom = m.addAction(QStringLiteral("移到最下層"));
	m.addSeparator();
	QAction *remove = m.addAction(QStringLiteral("從這個場景移除"));
	QAction *chosen = m.exec(anchor->mapToGlobal(pos));
	const int64_t id = layer.id;
	const std::string uid = layer.uid;
	const bool isOverlay = layer.isOverlay;
	QTimer::singleShot(0, this, [this, chosen, edit, reset, top, bottom, remove, id, uid, isOverlay] {
		Input *scene = CurrentScene();
		if (!chosen || !scene)
			return;
		if (chosen == edit)
			isOverlay ? EditOverlay(uid) : EditSource(uid);
		else if (chosen == reset)
			engine_.LayerResetIn(scene, id);
		else if (chosen == top)
			engine_.LayerOrderIn(scene, id, OBS_ORDER_MOVE_TOP);
		else if (chosen == bottom)
			engine_.LayerOrderIn(scene, id, OBS_ORDER_MOVE_BOTTOM);
		else if (chosen == remove)
			engine_.LayerRemoveIn(scene, id);
		rowSig_.clear();
		RefreshBusState();
		ScheduleSave();
	});
}

void MainWindow::RebuildRows()
{
	for (QHBoxLayout *lay : {busLayout_, ovlLayout_}) {
		while (QLayoutItem *it = lay->takeAt(0)) {
			delete it->widget();
			delete it;
		}
	}
	Input *sc = CurrentScene();
	const QString sceneName = sc ? Q(sc->name) : QStringLiteral("（還沒有場景）");
	busGroup_->setTitle(QStringLiteral("「%1」的訊號源　（點一下顯示／隱藏，1–9 快捷鍵；右鍵更多）").arg(sceneName));
	ovlGroup_->setTitle(QStringLiteral("「%1」的疊加 OVL　（點一下顯示／隱藏，Q W E R T Y 快捷鍵；右鍵更多）").arg(sceneName));

	int nSrc = 0, nOvl = 0;
	for (const auto &l : engine_.SceneLayers(sc)) {
		const bool ov = l.isOverlay;
		int &n = ov ? nOvl : nSrc;
		QString key;
		if (!ov && n < 9)
			key = QString::number(n + 1);
		if (ov && n < (int)std::strlen(kOverlayKeys))
			key = QString(QLatin1Char(kOverlayKeys[n]));
		n++;
		auto *b = new QPushButton(ov ? (key.isEmpty() ? Q(l.name) : QStringLiteral("%1  %2").arg(key, Q(l.name)))
					     : QStringLiteral("%1\n%2").arg(key, Q(l.name)));
		b->setObjectName(ov ? QStringLiteral("ovlButton") : QStringLiteral("busButton"));
		b->setProperty("layer", QVariant::fromValue<qlonglong>(l.id));
		b->setMinimumSize(ov ? 90 : 128, ov ? 38 : 62);
		if (!ov)
			b->setMaximumWidth(220);
		b->setFocusPolicy(Qt::NoFocus);
		b->setContextMenuPolicy(Qt::CustomContextMenu);
		b->setToolTip(QStringLiteral("點一下在這個場景裡顯示／隱藏"));
		const int64_t id = l.id;
		const LayerInfo info = l;
		connect(b, &QPushButton::clicked, this, [this, id] { ToggleLayer(id); });
		connect(b, &QPushButton::customContextMenuRequested, this,
			[this, b, info](const QPoint &pos) { ShowLayerMenu(b, pos, info); });
		(ov ? ovlLayout_ : busLayout_)->addWidget(b);
	}
	if (nSrc == 0) {
		auto *hint = new QLabel(sc ? QStringLiteral("這個場景還沒有訊號源。按右邊「＋ 新增訊號源」或「＋ 加入既有」。")
					   : QStringLiteral("按「＋ 新增訊號源」開始（會自動建立第一個場景）。"));
		hint->setObjectName(QStringLiteral("hint"));
		busLayout_->addWidget(hint);
	}
	if (nOvl == 0) {
		auto *hint = new QLabel(QStringLiteral("這個場景還沒有疊加。按右邊「＋ 新增疊加」加入台標、姓名條、跑馬燈或時鐘。"));
		hint->setObjectName(QStringLiteral("hint"));
		ovlLayout_->addWidget(hint);
	}
	busLayout_->addStretch(1);
	ovlLayout_->addStretch(1);
}

void MainWindow::RebuildBus()
{
	rowSig_.clear();
	RefreshBusState();
}

void MainWindow::RebuildScenes()
{
	sceneList_->clear();
	int n = 0;
	for (Input *sc : engine_.Scenes()) {
		n++;
		const QString key = n <= 12 ? QStringLiteral("F%1  ").arg(n) : QString();
		auto *it = new QListWidgetItem(key + Q(sc->name));
		it->setData(Qt::UserRole, Q(sc->uid));
		it->setSizeHint(QSize(142, 48));
		it->setTextAlignment(Qt::AlignCenter);
		it->setToolTip(QStringLiteral("點一下放到預覽並顯示這個場景的內容；右鍵可改名、複製、刪除"));
		sceneList_->addItem(it);
	}
	if (n == 0) {
		auto *it = new QListWidgetItem(QStringLiteral("按「＋ 新增場景」建立第一個場景"));
		it->setFlags(Qt::NoItemFlags);
		it->setSizeHint(QSize(300, 48));
		sceneList_->addItem(it);
	}
	rowSig_.clear();
	RefreshBusState();
}

void MainWindow::RefreshBusState()
{
	const Input *pvw = engine_.Preview(), *pgm = engine_.Program();
	Input *cur = CurrentScene();

	// 場景列：綠＝預覽、紅＝播出；目前場景加上 ▶
	for (int i = 0; i < sceneList_->count(); i++) {
		QListWidgetItem *it = sceneList_->item(i);
		const std::string uid = S(it->data(Qt::UserRole).toString());
		if (uid.empty())
			continue;
		QColor bg(42, 45, 56);
		if (pgm && pgm->uid == uid)
			bg = QColor(192, 57, 43);
		else if (pvw && pvw->uid == uid)
			bg = QColor(30, 142, 78);
		it->setBackground(bg);
		it->setForeground(Qt::white);
		QFont f = it->font();
		f.setBold(cur && cur->uid == uid);
		it->setFont(f);
		Input *sc = engine_.FindInput(uid);
		int n = 0;
		for (Input *s2 : engine_.Scenes()) {
			n++;
			if (s2 == sc)
				break;
		}
		const QString key = n <= 12 ? QStringLiteral("F%1  ").arg(n) : QString();
		const QString text = (cur && cur->uid == uid ? QStringLiteral("▶ ") : QString()) + key + (sc ? Q(sc->name) : QString());
		if (it->text() != text)
			it->setText(text);
	}

	// 目前場景的內容有變（新增、移除、改名、換場景）就重建下面兩列
	QString sig = cur ? Q(cur->uid) : QString();
	const auto layers = engine_.SceneLayers(cur);
	for (const auto &l : layers)
		sig += QStringLiteral("|%1:%2").arg(l.id).arg(Q(l.name));
	if (sig != rowSig_) {
		rowSig_ = sig;
		RebuildRows();
	}

	// 按鈕顏色：隱藏＝灰；顯示中＝綠（場景在預覽）或紅（場景播出中）
	const bool live = cur && pgm == cur;
	for (QHBoxLayout *lay : {busLayout_, ovlLayout_}) {
		for (int i = 0; i < lay->count(); i++) {
			auto *b = qobject_cast<QPushButton *>(lay->itemAt(i)->widget());
			if (!b)
				continue;
			const int64_t id = b->property("layer").toLongLong();
			QString state;
			if (engine_.LayerVisible(cur, id))
				state = live ? QStringLiteral("pgm") : QStringLiteral("pvw");
			if (b->property("state").toString() != state) {
				b->setProperty("state", state);
				Repolish(b);
			}
		}
	}
	if (live)
		ovlGroup_->setToolTip(QStringLiteral("這個場景正在播出：開關會立即反映在播出畫面"));
	else
		ovlGroup_->setToolTip(QString());

	const bool withdraw = engine_.WillWithdraw();
	cutButton_->setText(withdraw ? QStringLiteral("撤回 CUT") : QStringLiteral("送出 CUT"));
	autoButton_->setText(withdraw ? QStringLiteral("撤回 AUTO") : QStringLiteral("送出 AUTO"));
}

void MainWindow::RebuildSourceList()
{
	sourceList_->clear();
	for (const auto &in : engine_.Inputs()) {
		const QString icon = in->isScene     ? QStringLiteral("▦  ")
				     : in->audioOnly ? QStringLiteral("🔊  ")
						     : QStringLiteral("🎥  ");
		auto *it = new QListWidgetItem(icon + Q(in->name));
		it->setData(Qt::UserRole, Q(in->uid));
		sourceList_->addItem(it);
	}
}

void MainWindow::SourcesChanged()
{
	RebuildScenes();
	RebuildBus();
	RebuildSourceList();
	mixer_->Rebuild();
	ScheduleSave();
}

void MainWindow::AddSourceOfType(const QString &kind)
{
	auto countOf = [this](const std::string &type) {
		int n = 0;
		for (const auto &in : engine_.Inputs())
			if (in->typeId == type)
				n++;
		return std::to_string(n + 1);
	};

	std::string type, name;
	obs_data_t *s = obs_data_create();
	bool openProps = true;
	auto cancel = [&s] {
		obs_data_release(s);
		s = nullptr;
	};

	if (kind == QLatin1String("camera")) {
		type = platform::kCamera;
		name = "攝影機 " + countOf(type);
		const char *prop = platform::kTextIsGdi ? "video_device_id" : "device";
		auto devices = engine_.ListProperty(type.c_str(), prop);
		for (const auto &d : devices) {
			if (d.id.empty())
				continue;
			obs_data_set_string(s, prop, d.id.c_str());
			name = d.name;
			break;
		}
	} else if (kind == QLatin1String("screen")) {
		type = platform::kScreen;
		name = "螢幕擷取 " + countOf(type);
	} else if (kind == QLatin1String("window")) {
		type = platform::kWindow;
		name = "視窗擷取 " + countOf(type);
#ifdef __APPLE__
		obs_data_set_int(s, "type", 1); // ScreenCaptureKit：單一視窗
#endif
	} else if (kind == QLatin1String("game")) {
		type = platform::kGame ? platform::kGame : "";
		name = "遊戲擷取 " + countOf(type);
	} else if (kind == QLatin1String("url")) {
		bool ok = false;
		const QString url = QInputDialog::getText(
			this, QStringLiteral("加入網路影片"),
			QStringLiteral("網址（rtmp://、srt://、rtsp://、https://….m3u8 或影片網址）："), QLineEdit::Normal,
			QString(), &ok);
		if (!ok || url.trimmed().isEmpty()) {
			cancel();
			return;
		}
		type = "ffmpeg_source";
		name = "網路影片 " + countOf(type);
		obs_data_set_bool(s, "is_local_file", false);
		obs_data_set_string(s, "input", S(url.trimmed()).c_str());
		obs_data_set_bool(s, "restart_on_activate", false);
		obs_data_set_bool(s, "close_when_inactive", false);
		obs_data_set_int(s, "reconnect_delay_sec", 3);
		obs_data_set_int(s, "buffering_mb", 1);
		obs_data_set_bool(s, "hw_decode", true);
		openProps = false;
	} else if (kind == QLatin1String("media")) {
		const QString f = QFileDialog::getOpenFileName(
			this, QStringLiteral("選擇影片或音樂"), QString(),
			QStringLiteral("影音檔 (*.mp4 *.mov *.mkv *.avi *.webm *.ts *.flv *.mp3 *.wav *.aac *.m4a *.flac);;所有檔案 (*.*)"));
		if (f.isEmpty()) {
			cancel();
			return;
		}
		type = "ffmpeg_source";
		name = S(QFileInfo(f).completeBaseName());
		obs_data_set_bool(s, "is_local_file", true);
		obs_data_set_string(s, "local_file", S(f).c_str());
		obs_data_set_bool(s, "looping", true);
		obs_data_set_bool(s, "restart_on_activate", true);
		obs_data_set_bool(s, "hw_decode", true);
		openProps = false;
	} else if (kind == QLatin1String("image")) {
		const QString f = QFileDialog::getOpenFileName(
			this, QStringLiteral("選擇圖片"), QString(),
			QStringLiteral("圖片 (*.png *.jpg *.jpeg *.bmp *.gif *.webp);;所有檔案 (*.*)"));
		if (f.isEmpty()) {
			cancel();
			return;
		}
		type = "image_source";
		name = S(QFileInfo(f).completeBaseName());
		obs_data_set_string(s, "file", S(f).c_str());
		openProps = false;
	} else if (kind == QLatin1String("color")) {
		const QColor c = QColorDialog::getColor(QColor(30, 40, 80), this, QStringLiteral("選擇顏色"));
		if (!c.isValid()) {
			cancel();
			return;
		}
		type = "color_source_v3";
		name = "純色 " + S(c.name().toUpper());
		obs_data_set_int(s, "color",
				 (long long)(((uint32_t)255 << 24) | ((uint32_t)c.blue() << 16) |
					     ((uint32_t)c.green() << 8) | (uint32_t)c.red()));
		obs_data_set_int(s, "width", 1920);
		obs_data_set_int(s, "height", 1080);
		openProps = false;
	} else if (kind == QLatin1String("text")) {
		type = platform::kText;
		name = "文字 " + countOf(type);
		obs_data_set_string(s, "text", "Meikou Live Studio");
	} else if (kind == QLatin1String("mic")) {
		type = platform::kAudioIn;
		name = "麥克風 " + countOf(type);
		obs_data_set_string(s, "device_id", "default");
	} else if (kind == QLatin1String("desktop")) {
		type = platform::kAudioOut;
		name = "系統聲音 " + countOf(type);
		obs_data_set_string(s, "device_id", "default");
#ifdef __APPLE__
		obs_data_set_int(s, "type", 0); // ScreenCaptureKit：整台電腦的聲音
#endif
	} else if (kind == QLatin1String("app")) {
		type = platform::kAppAudio;
		name = "程式聲音 " + countOf(type);
#ifdef __APPLE__
		obs_data_set_int(s, "type", 1); // ScreenCaptureKit：只擷取指定應用程式的聲音
#endif
	}

	Input *in = type.empty() ? nullptr : engine_.AddInput(type, name, s);
	obs_data_release(s);
	if (!in) {
		QMessageBox::warning(this, windowTitle(), QStringLiteral("無法建立這個訊號源（這台電腦可能不支援）。"));
		return;
	}
	if (!in->audioOnly) {
		if (Input *scene = EnsureScene())
			engine_.AddSourceToScene(scene, in); // 新的影像訊號源直接放進目前場景
	}
	SourcesChanged();
	if (openProps)
		EditSource(in->uid);
}

void MainWindow::EditSource(const std::string &uid)
{
	Input *in = engine_.FindInput(uid);
	if (!in || !in->source || in->isScene)
		return;
	PropertiesDialog dlg(in->source, QStringLiteral("設定：%1").arg(Q(in->name)), this);
	dlg.exec();
	// 設定改變可能讓「只有聲音」的狀態或音軌變化，重建一次比較穩
	SourcesChanged();
}

void MainWindow::ComposeSource(const std::string &uid)
{
	Input *src = engine_.FindInput(uid);
	if (!src || src->audioOnly || src->isScene)
		return;
	Input *scene = EnsureScene();
	if (!scene)
		return;
	engine_.AddSourceToScene(scene, src);
	rowSig_.clear();
	RebuildSourceList();
	RefreshBusState();
	ScheduleSave();
}

void MainWindow::NewScene()
{
	Input *scene = engine_.CreateScene(engine_.NextSceneName());
	if (!scene)
		return;
	SourcesChanged();
	SelectScene(scene->uid);
	if (outputStatus_)
		outputStatus_->setText(QStringLiteral("已新增「%1」。用下面的「＋ 新增訊號源」「＋ 新增疊加」把內容放進這個場景。")
					       .arg(Q(scene->name)));
}

void MainWindow::AddOverlayToCurrentScene(const std::string &overlayUid)
{
	Overlay *ov = engine_.FindOverlay(overlayUid);
	Input *scene = EnsureScene();
	if (!ov || !scene)
		return;
	engine_.AddOverlayToScene(scene, ov);
	rowSig_.clear();
	RefreshBusState();
	ScheduleSave();
}

void MainWindow::RenameSource(const std::string &uid)
{
	Input *in = engine_.FindInput(uid);
	if (!in)
		return;
	bool ok = false;
	const QString name = QInputDialog::getText(this, QStringLiteral("重新命名"), QStringLiteral("名稱："),
						   QLineEdit::Normal, Q(in->name), &ok);
	if (!ok || name.trimmed().isEmpty())
		return;
	engine_.RenameInput(uid, S(name.trimmed()));
	SourcesChanged();
}

void MainWindow::RemoveSource(const std::string &uid)
{
	Input *in = engine_.FindInput(uid);
	if (!in)
		return;
	const QString question = engine_.Program() == in
					 ? QStringLiteral("「%1」正在播出，確定要移除嗎？").arg(Q(in->name))
					 : QStringLiteral("確定要移除「%1」嗎？").arg(Q(in->name));
	if (QMessageBox::question(this, windowTitle(), question) != QMessageBox::Yes)
		return;
	engine_.RemoveInput(uid);
	SourcesChanged();
}

// ---------------------------------------------------------------- 疊加

void MainWindow::RebuildOverlayButtons()
{
	rowSig_.clear();
	RefreshBusState();
}

void MainWindow::RefreshOverlayButtons()
{
	RefreshBusState();
}

void MainWindow::RebuildOverlayList()
{
	overlayList_->clear();
	for (const auto &ov : engine_.Overlays()) {
		auto *it = new QListWidgetItem(QStringLiteral("［%1］%2").arg(KindName(ov->kind), Q(ov->name)));
		it->setData(Qt::UserRole, Q(ov->uid));
		overlayList_->addItem(it);
	}
}

void MainWindow::AddOverlayOfKind(const QString &kind)
{
	GraphicSpec g;
	g.kind = S(kind);
	QString name;
	if (kind == QLatin1String("logo") || kind == QLatin1String("image")) {
		const QString f = QFileDialog::getOpenFileName(
			this, kind == QLatin1String("logo") ? QStringLiteral("選擇台標圖片（取消＝做文字台標）") : QStringLiteral("選擇圖片"),
			QString(), QStringLiteral("圖片 (*.png *.jpg *.jpeg *.gif *.webp *.bmp)"));
		if (f.isEmpty() && kind == QLatin1String("image"))
			return;
		g.file = S(f);
		name = (kind == QLatin1String("logo") ? QStringLiteral("台標 ") : QStringLiteral("圖片 ")) +
		       (f.isEmpty() ? QStringLiteral("文字") : QFileInfo(f).completeBaseName());
		if (f.isEmpty() || kind == QLatin1String("logo"))
			g.text1 = f.isEmpty() ? "MEIKOU LIVE" : ""; // 選了圖片：文字留空，要的話再填
	} else if (kind == QLatin1String("lower")) {
		name = QStringLiteral("姓名條");
		g.text1 = "王小明";
		g.text2 = "主持人";
	} else if (kind == QLatin1String("ticker")) {
		name = QStringLiteral("跑馬燈");
		g.text1 = "歡迎收看本節目　記得訂閱、按讚、開啟小鈴鐺";
	} else if (kind == QLatin1String("clock")) {
		name = QStringLiteral("時鐘");
		g.text1 = "LIVE";
	} else {
		name = QStringLiteral("文字");
		g.text1 = "Meikou Live Studio";
	}
	ApplyGraphicDefaults(g);

	OverlayDialog dlg(g, name, this);
	if (dlg.exec() != QDialog::Accepted)
		return;
	Overlay *ov = engine_.AddOverlay(dlg.Spec(), S(dlg.Name()));
	if (!ov) {
		QMessageBox::warning(this, windowTitle(), QStringLiteral("無法建立這個疊加素材。"));
		return;
	}
	if (Input *scene = EnsureScene())
		engine_.AddOverlayToScene(scene, ov); // 新的疊加直接放進目前場景
	RebuildOverlayButtons();
	RebuildOverlayList();
	ScheduleSave();
}

void MainWindow::EditOverlay(const std::string &uid)
{
	Overlay *ov = engine_.FindOverlay(uid);
	if (!ov)
		return;
	OverlayDialog dlg(*ov, Q(ov->name), this);
	if (dlg.exec() != QDialog::Accepted)
		return;
	ov = engine_.FindOverlay(uid);
	if (!ov)
		return;
	const bool wasVertical = ov->vertical;
	static_cast<GraphicSpec &>(*ov) = dlg.Spec();
	ov->name = S(dlg.Name());
	engine_.UpdateOverlay(ov);
	if (ov->vertical != wasVertical)
		engine_.ResetOverlayPosition(ov); // 橫直切換後放回該方向的預設位置
	RebuildOverlayButtons();
	RebuildOverlayList();
	ScheduleSave();
}

// ---------------------------------------------------------------- 切換與輸出

void MainWindow::DoCut()
{
	engine_.Cut();
	RefreshBusState();
	RefreshOverlayButtons();
	ScheduleSave();
}

void MainWindow::DoAuto()
{
	engine_.Auto();
	RefreshBusState();
	RefreshOverlayButtons();
	ScheduleSave();
}

// 推流目標：每個目標一張卡片。上面一列是啟用／平台／狀態，下面是網址與金鑰（上下排列、完整寬度）。
// 填好後可以收合成一行摘要，點摘要就展開修改。
void MainWindow::RebuildTargets()
{
	while (QLayoutItem *it = targetLayout_->takeAt(0)) {
		delete it->widget();
		delete it;
	}
	targetRows_.clear();
	auto &ts = engine_.Targets();
	targetRows_.resize(ts.size());
	for (int r = 0; r < (int)ts.size(); r++) {
		auto *card = new QFrame;
		card->setObjectName(QStringLiteral("targetCard"));
		auto *cv = new QVBoxLayout(card);
		cv->setContentsMargins(10, 8, 10, 10);
		cv->setSpacing(6);

		// 第一列：啟用、平台、狀態、移除
		auto *head = new QHBoxLayout;
		auto *en = new QCheckBox(QStringLiteral("啟用"));
		en->setChecked(ts[r].enabled);
		connect(en, &QCheckBox::toggled, this, [this, r](bool on) {
			if (r < (int)engine_.Targets().size())
				engine_.Targets()[r].enabled = on;
			ScheduleSave();
		});
		auto *plat = new QComboBox;
		for (const auto &p : kPlatforms)
			plat->addItem(QString::fromUtf8(p.name), QString::fromUtf8(p.server));
		int idx = plat->count() - 1;
		for (int i = 0; i < plat->count() - 1; i++)
			if (plat->itemData(i).toString() == Q(ts[r].server))
				idx = i;
		plat->setCurrentIndex(idx);
		auto *status = new QLabel(Q(ts[r].status));
		status->setObjectName(QStringLiteral("hint"));
		auto *del = new QPushButton(QStringLiteral("移除"));
		del->setToolTip(QStringLiteral("移除這個推流目標"));
		head->addWidget(en);
		head->addWidget(plat, 1);
		head->addWidget(status);
		head->addWidget(del);
		cv->addLayout(head);

		// 收合時的摘要：點一下展開
		auto *summary = new QPushButton;
		summary->setObjectName(QStringLiteral("targetSummary"));
		summary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		summary->setCursor(Qt::PointingHandCursor);
		summary->setToolTip(QStringLiteral("點一下展開修改網址與金鑰"));
		cv->addWidget(summary);

		// 展開時：網址、金鑰上下排列，完整寬度
		auto *detail = new QWidget;
		auto *dv = new QVBoxLayout(detail);
		dv->setContentsMargins(0, 0, 0, 0);
		dv->setSpacing(4);
		dv->addWidget(new QLabel(QStringLiteral("伺服器網址")));
		auto *server = new QLineEdit(Q(ts[r].server));
		server->setPlaceholderText(QStringLiteral("rtmp://… 或 rtmps://…"));
		server->setClearButtonEnabled(true);
		dv->addWidget(server);
		dv->addSpacing(4);
		dv->addWidget(new QLabel(QStringLiteral("串流金鑰（只存在這台電腦）")));
		auto *keyRow = new QHBoxLayout;
		auto *key = new QLineEdit(Q(ts[r].key));
		key->setEchoMode(QLineEdit::Password);
		key->setPlaceholderText(QStringLiteral("貼上串流金鑰"));
		auto *show = new QPushButton(QStringLiteral("顯示"));
		show->setCheckable(true);
		show->setToolTip(QStringLiteral("暫時顯示金鑰內容，方便核對"));
		connect(show, &QPushButton::toggled, this, [key, show](bool on) {
			key->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
			show->setText(on ? QStringLiteral("隱藏") : QStringLiteral("顯示"));
		});
		keyRow->addWidget(key, 1);
		keyRow->addWidget(show);
		dv->addLayout(keyRow);
		auto *done = new QPushButton(QStringLiteral("完成（收合）"));
		auto *doneRow = new QHBoxLayout;
		doneRow->addStretch(1);
		doneRow->addWidget(done);
		dv->addLayout(doneRow);
		cv->addWidget(detail);

		targetRows_[r] = {server, key, status, summary, detail};

		connect(summary, &QPushButton::clicked, this, [this, r, server] {
			SetTargetExpanded(r, true);
			server->setFocus();
		});
		connect(done, &QPushButton::clicked, this, [this, r, server, key, show] {
			if (r < (int)engine_.Targets().size()) {
				engine_.Targets()[r].server = S(server->text().trimmed());
				engine_.Targets()[r].key = S(key->text().trimmed());
			}
			show->setChecked(false);
			ScheduleSave();
			SetTargetExpanded(r, false);
		});
		connect(del, &QPushButton::clicked, this, [this, r] {
			auto &list = engine_.Targets();
			if (r >= (int)list.size() || engine_.Streaming())
				return;
			if (QMessageBox::question(this, windowTitle(),
						  QStringLiteral("確定要移除「%1」這個推流目標嗎？").arg(Q(list[r].name))) !=
			    QMessageBox::Yes)
				return;
			list.erase(list.begin() + r);
			QTimer::singleShot(0, this, [this] { RebuildTargets(); });
			ScheduleSave();
		});
		connect(plat, &QComboBox::activated, this, [this, r, plat, server](int) {
			const QString sv = plat->currentData().toString();
			if (!sv.isEmpty())
				server->setText(sv);
			if (r < (int)engine_.Targets().size()) {
				engine_.Targets()[r].server = S(server->text().trimmed());
				engine_.Targets()[r].name = S(plat->currentText());
			}
			UpdateTargetSummary(r);
			ScheduleSave();
		});
		connect(server, &QLineEdit::editingFinished, this, [this, r, server] {
			if (r < (int)engine_.Targets().size())
				engine_.Targets()[r].server = S(server->text().trimmed());
			UpdateTargetSummary(r);
			ScheduleSave();
		});
		connect(key, &QLineEdit::editingFinished, this, [this, r, key] {
			if (r < (int)engine_.Targets().size())
				engine_.Targets()[r].key = S(key->text().trimmed());
			UpdateTargetSummary(r);
			ScheduleSave();
		});

		targetLayout_->addWidget(card);
		UpdateTargetSummary(r);
		// 還沒填完的目標展開，填好的收合成一行
		SetTargetExpanded(r, ts[r].server.empty() || ts[r].key.empty());
	}
	if (ts.empty()) {
		auto *hint = new QLabel(QStringLiteral("還沒有推流目標。按「＋ 新增目標」加入 YouTube、Facebook、Twitch 或自訂伺服器。"));
		hint->setObjectName(QStringLiteral("hint"));
		hint->setWordWrap(true);
		targetLayout_->addWidget(hint);
	}
}

void MainWindow::SetTargetExpanded(int row, bool expanded)
{
	if (row < 0 || row >= (int)targetRows_.size())
		return;
	targetRows_[row].detail->setVisible(expanded);
	targetRows_[row].summary->setVisible(!expanded);
}

void MainWindow::UpdateTargetSummary(int row)
{
	if (row < 0 || row >= (int)targetRows_.size())
		return;
	const TargetRow &tr = targetRows_[row];
	const QString server = tr.server->text().trimmed();
	const QString key = tr.key->text().trimmed();
	QString keyText;
	if (key.isEmpty())
		keyText = QStringLiteral("金鑰：尚未填寫");
	else
		keyText = QStringLiteral("金鑰：已填（••••%1）").arg(key.right(4));
	tr.summary->setText(QStringLiteral("%1\n%2　　✎ 點一下修改")
				    .arg(server.isEmpty() ? QStringLiteral("伺服器網址：尚未填寫") : server, keyText));
	tr.summary->setToolTip(server);
}

void MainWindow::RefreshTargetStatus()
{
	auto &ts = engine_.Targets();
	for (int r = 0; r < (int)ts.size() && r < (int)targetRows_.size(); r++) {
		QString st = Q(ts[r].status);
		if (ts[r].active && !ts[r].reconnecting)
			st = QStringLiteral("推流中 %1 kbps").arg(ts[r].kbps, 0, 'f', 0);
		if (QLabel *l = targetRows_[r].status; l && l->text() != st)
			l->setText(st);
	}
}

EncodeConfig MainWindow::CurrentEncodeConfig() const
{
	EncodeConfig cfg;
	cfg.encoderId = S(encoderBox_->currentData().toString());
	cfg.videoKbps = bitrateBox_->value();
	cfg.lowLatency = lowLatencyBox_->isChecked();
	cfg.dynamicBitrate = dynBitrateBox_->isChecked();
	return cfg;
}

void MainWindow::ToggleStream()
{
	if (engine_.Streaming()) {
		if (QMessageBox::question(this, windowTitle(), QStringLiteral("確定要停止直播嗎？")) != QMessageBox::Yes)
			return;
		stoppingStream_ = true;
		streamButton_->setEnabled(false);
		outputStatus_->setText(QStringLiteral("正在停止推流…"));
		engine_.StopStreaming();
		return;
	}
	// 先把表格裡還沒離開欄位的內容寫回去
	auto &ts = engine_.Targets();
	for (int r = 0; r < (int)ts.size() && r < (int)targetRows_.size(); r++) {
		ts[r].server = S(targetRows_[r].server->text().trimmed());
		ts[r].key = S(targetRows_[r].key->text().trimmed());
		UpdateTargetSummary(r);
		SetTargetExpanded(r, false);
	}
	SaveSettings();
	SaveProject();
	std::string err;
	if (!engine_.StartStreaming(CurrentEncodeConfig(), err)) {
		RefreshTargetStatus();
		QMessageBox::warning(this, windowTitle(), Q(err));
		return;
	}
	stoppingStream_ = false;
	outputStatus_->setText(QStringLiteral("正在連線…"));
	RefreshTargetStatus();
	UpdateStats();
}

void MainWindow::ToggleRecord()
{
	if (engine_.Recording()) {
		recordButton_->setEnabled(false);
		engine_.StopRecording();
		return;
	}
	const QString dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) +
			    QStringLiteral("/Meikou Live Studio");
	QDir().mkpath(dir);
	const QString path =
		dir + QStringLiteral("/Meikou %1.mkv")
			      .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH-mm-ss")));
	SaveSettings();
	std::string err;
	if (!engine_.StartRecording(CurrentEncodeConfig(), S(QDir::toNativeSeparators(path)), err)) {
		QMessageBox::warning(this, windowTitle(), Q(err));
		return;
	}
	UpdateStats();
}

void MainWindow::OnOutputEvent(int index, const QString &kind, const QString &message)
{
	if (index < 0) { // 錄影
		recordButton_->setEnabled(true);
		if (kind == QLatin1String("failed"))
			QMessageBox::warning(this, windowTitle(), QStringLiteral("錄影停止：%1").arg(message));
		else if (kind == QLatin1String("stopped"))
			outputStatus_->setText(QStringLiteral("錄影已存檔：%1").arg(Q(engine_.RecordingPath())));
	} else if (!engine_.Streaming()) {
		streamButton_->setEnabled(true);
		if (!stoppingStream_ && kind == QLatin1String("failed"))
			outputStatus_->setText(QStringLiteral("推流停止：%1").arg(message));
		else if (kind == QLatin1String("stopped") || kind == QLatin1String("failed"))
			outputStatus_->setText(QStringLiteral("未推流"));
		stoppingStream_ = false;
	} else if (kind == QLatin1String("failed")) {
		outputStatus_->setText(QStringLiteral("有一個目標停止：%1（其他目標繼續推流）").arg(message));
	}
	RefreshTargetStatus();
	UpdateStats();
}

void MainWindow::ApplyFps()
{
	const int i = fpsBox_->currentIndex();
	if (i == appliedFpsIndex_ || i < 0)
		return;
	if (engine_.OutputsActive()) {
		fpsBox_->setCurrentIndex(appliedFpsIndex_);
		return;
	}
	std::string err;
	pvwDisplay_->DestroyDisplay();
	pgmDisplay_->DestroyDisplay();
	const bool ok = engine_.ResetVideo(kFps[i].num, kFps[i].den, err);
	// 送一次 resize 讓兩個畫面重新建立 obs_display
	for (Display *d : {pvwDisplay_, pgmDisplay_}) {
		QResizeEvent e(d->size(), d->size());
		QApplication::sendEvent(d, &e);
	}
	if (!ok) {
		fpsBox_->setCurrentIndex(appliedFpsIndex_);
		QMessageBox::warning(this, windowTitle(), Q(err));
		return;
	}
	appliedFpsIndex_ = i;
	SaveSettings();
}

void MainWindow::UpdateStats()
{
	engine_.Tick();
	const Stats s = engine_.QueryStats();
	clockLabel_->setText(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")));

	if (s.streaming) {
		double kbps = 0;
		int dropped = 0;
		bool reconnecting = false;
		for (const auto &t : engine_.Targets()) {
			kbps += t.kbps;
			dropped += t.dropped;
			reconnecting = reconnecting || t.reconnecting;
		}
		liveLabel_->setText(QStringLiteral("● LIVE %1").arg(FmtTime(s.streamStartNs)));
		liveLabel_->setProperty("state", reconnecting ? QStringLiteral("warn") : QStringLiteral("live"));
		if (!stoppingStream_)
			outputStatus_->setText(QStringLiteral("推流中 · 合計 %1 kbps · 網路掉格 %2%3")
						       .arg(kbps, 0, 'f', 0)
						       .arg(dropped)
						       .arg(reconnecting ? QStringLiteral(" · 有目標正在自動重連")
									 : QString()));
		streamButton_->setText(QStringLiteral("停止推流"));
		streamButton_->setProperty("state", QStringLiteral("on"));
	} else {
		liveLabel_->setText(QStringLiteral("● 未直播"));
		liveLabel_->setProperty("state", QString());
		streamButton_->setText(QStringLiteral("開始推流"));
		streamButton_->setProperty("state", QString());
	}
	if (s.recording) {
		recLabel_->setText(QStringLiteral("● REC %1　%2 MB")
					   .arg(FmtTime(s.recordStartNs))
					   .arg((qulonglong)(s.recordBytes / 1048576ULL)));
		recLabel_->setProperty("state", QStringLiteral("live"));
		recordButton_->setText(QStringLiteral("停止錄影"));
		recordButton_->setProperty("state", QStringLiteral("on"));
	} else {
		recLabel_->setText(QStringLiteral("● 未錄影"));
		recLabel_->setProperty("state", QString());
		recordButton_->setText(QStringLiteral("開始錄影"));
		recordButton_->setProperty("state", QString());
	}
	for (QWidget *w : {static_cast<QWidget *>(liveLabel_), static_cast<QWidget *>(recLabel_),
			   static_cast<QWidget *>(streamButton_), static_cast<QWidget *>(recordButton_)})
		Repolish(w);

	const bool busy = s.streaming || s.recording;
	encoderBox_->setEnabled(!busy);
	bitrateBox_->setEnabled(!busy);
	fpsBox_->setEnabled(!busy);
	lowLatencyBox_->setEnabled(!busy);
	dynBitrateBox_->setEnabled(!busy);
	targetBox_->setEnabled(!s.streaming);

	statsLabel_->setText(QStringLiteral("CPU %1%　%2 fps　渲染掉格 %3　編碼掉格 %4")
				     .arg(s.cpu, 0, 'f', 0)
				     .arg(s.renderFps, 0, 'f', 1)
				     .arg(s.laggedFrames)
				     .arg(s.skippedFrames));
	RefreshTargetStatus();
	RefreshBusState();
	RefreshOverlayButtons();
}

// ---------------------------------------------------------------- 存檔

void MainWindow::ScheduleSave()
{
	saveTimer_->start();
}

void MainWindow::SaveProject()
{
	QSaveFile f(projectPath_);
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QByteArray::fromStdString(engine_.SaveProjectJson()));
		f.commit();
	}
}

void MainWindow::LoadSettings()
{
	QSettings st;
	const int enc = encoderBox_->findData(st.value("output/encoder").toString());
	if (enc >= 0)
		encoderBox_->setCurrentIndex(enc);
	bitrateBox_->setValue(st.value("output/kbps", 6000).toInt());
	lowLatencyBox_->setChecked(st.value("output/lowLatency", true).toBool());
	dynBitrateBox_->setChecked(st.value("output/dynBitrate", false).toBool());
	// main.cpp 已依這個設定啟動視訊，這裡只同步畫面上的選項
	appliedFpsIndex_ = std::clamp(st.value("output/fpsIndex", 3).toInt(), 0, kFpsCount - 1);
	fpsBox_->setCurrentIndex(appliedFpsIndex_);
	restoreGeometry(st.value("window/geometry").toByteArray());
}

void MainWindow::SaveSettings()
{
	QSettings st;
	st.setValue("output/encoder", encoderBox_->currentData().toString());
	st.setValue("output/kbps", bitrateBox_->value());
	st.setValue("output/lowLatency", lowLatencyBox_->isChecked());
	st.setValue("output/dynBitrate", dynBitrateBox_->isChecked());
	st.setValue("output/fpsIndex", appliedFpsIndex_);
}

void MainWindow::OpenUpdateDialog(bool useKnownRelease)
{
	UpdateDialog dlg(this, engine_.Streaming() || engine_.Recording());
	connect(&dlg, &UpdateDialog::installRequested, this, [this, &dlg] {
		if (engine_.Streaming() || engine_.Recording()) {
			QMessageBox::information(this, windowTitle(), QStringLiteral("直播或錄影中不能更新，結束後再安裝。"));
			return;
		}
		dlg.accept();
		// 存檔後關閉程式；main() 在引擎關閉後啟動安裝檔
		QTimer::singleShot(0, this, [this] { close(); });
	});
	if (useKnownRelease && !newRelease_.version.isEmpty())
		dlg.ShowRelease(newRelease_);
	else
		dlg.StartCheck();
	dlg.exec();
	if (PendingInstaller().isEmpty() || dlg.result() != QDialog::Accepted)
		SetPendingInstaller(QString()); // 沒按安裝就不要在關閉時偷偷安裝
}

void MainWindow::StartAutoUpdateCheck()
{
	QSettings st;
	if (!st.value("update/autoCheck", true).toBool())
		return;
	CheckForUpdateInBackground(this, [this](const ReleaseInfo &rel) {
		newRelease_ = rel;
		updateBtn_->setText(QStringLiteral("有新版 %1").arg(rel.version));
		updateBtn_->setToolTip(QStringLiteral("點一下查看更新內容並安裝"));
		updateBtn_->setVisible(true);
	});
}

void MainWindow::ExportReport()
{
	SaveProject();
	QString err;
	const QString dir = ExportProblemReport(QFileInfo(projectPath_).absolutePath(), err);
	if (dir.isEmpty()) {
		QMessageBox::warning(this, windowTitle(), err);
		return;
	}
	QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
	QMessageBox::information(this, windowTitle(),
				 QStringLiteral("問題報告已放在：\n%1\n\n請在「系統資訊.txt」寫下發生了什麼事，"
						"再把整個資料夾壓縮後傳給開發者。")
					 .arg(QDir::toNativeSeparators(dir)));
}

void MainWindow::ShowUncleanShutdownNotice()
{
	const auto r = QMessageBox::question(
		this, windowTitle(),
		QStringLiteral("上次 Meikou Live Studio 沒有正常關閉（可能當機或被強制結束）。\n\n"
			       "要匯出問題報告嗎？報告裡有上次的紀錄檔，可以幫助找出原因。"));
	if (r == QMessageBox::Yes)
		ExportReport();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
	if (engine_.Streaming() || engine_.Recording()) {
		const auto r = QMessageBox::question(this, windowTitle(),
						     QStringLiteral("目前正在推流或錄影，關閉會立即停止。確定要關閉嗎？"));
		if (r != QMessageBox::Yes) {
			event->ignore();
			return;
		}
	}
	SaveSettings();
	SaveProject();
	QSettings().setValue("window/geometry", saveGeometry());
	statsTimer_->stop();
	saveTimer_->stop();
	pvwDisplay_->DestroyDisplay();
	pgmDisplay_->DestroyDisplay();
	event->accept();
}

} // namespace meikou
