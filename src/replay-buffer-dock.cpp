#include "replay-buffer-dock.hpp"
#include "hotkey-lookup.hpp"
#include "clip-trim.hpp"
#include "websocket-bridge.hpp"

#include <obs-module.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QSlider>
#include <QLabel>
#include <QTimer>
#include <QStringList>
#include <QPushButton>
#include <QFrame>
#include <QMainWindow>
#include <QStatusBar>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QFileInfo>
#include <QPointer>

#include <cstring>
#include <thread>

namespace {

constexpr int kMinDurationSeconds = 15;
constexpr int kMaxDurationSeconds = 3600; // 60 minutes
constexpr int kDefaultDurationSeconds = 60;

QString FormatDuration(int seconds)
{
	int m = seconds / 60;
	int s = seconds % 60;
	return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

// Minimal JSON string escaping -- row keys/labels/hotkeys are plain source
// names and key combos, but escape defensively rather than assume that.
QString JsonEscape(const QString &s)
{
	QString out = s;
	out.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
	out.replace(QLatin1Char('"'), QStringLiteral("\\\""));
	return out;
}

void EnumFilterCallback(obs_source_t *, obs_source_t *child, void *param)
{
	auto *out = static_cast<std::vector<obs_weak_source_t *> *>(param);
	if (strcmp(obs_source_get_id(child), "source_record_filter") == 0) {
		obs_weak_source_t *weak = obs_source_get_weak_source(child);
		if (weak)
			out->push_back(weak);
	}
}

bool EnumSourceCallback(void *param, obs_source_t *source)
{
	obs_source_enum_filters(source, EnumFilterCallback, param);
	return true;
}

// Returns one owned weak ref per distinct "source_record_filter" instance found.
std::vector<obs_weak_source_t *> DiscoverSourceRecordFilters()
{
	std::vector<obs_weak_source_t *> found;
	obs_enum_sources(EnumSourceCallback, &found);
	obs_enum_scenes(EnumSourceCallback, &found);
	return found;
}

QString FilterRowLabel(obs_source_t *filterSource)
{
	obs_source_t *parent = obs_filter_get_parent(filterSource);
	if (parent)
		return QString::fromUtf8(obs_source_get_name(parent)) + QStringLiteral(" - ") +
		       QString::fromUtf8(obs_source_get_name(filterSource));
	return QString::fromUtf8(obs_source_get_name(filterSource));
}

// Named (not a lambda) so the exact same pointer can be used to both connect
// and later disconnect from the main replay buffer output's signals.
void MainReplayStoppedSignalCallback(void *data, calldata_t *cd)
{
	auto *dock = static_cast<ReplayBufferDock *>(data);
	const long long code = calldata_int(cd, "code");
	QMetaObject::invokeMethod(dock, "NotifyMainReplayStopped", Qt::QueuedConnection, Q_ARG(qlonglong, code));
}

void MainReplaySavedSignalCallback(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	auto *dock = static_cast<ReplayBufferDock *>(data);
	QMetaObject::invokeMethod(dock, "NotifyMainReplaySaved", Qt::QueuedConnection);
}

// Ties a Source Record filter's "replay_saved" signal connection back to the
// dock and the specific row it belongs to. Owned by that row's
// ReplayRow::filterSaveConn for as long as the connection is live.
struct FilterSaveConnection {
	ReplayBufferDock *dock;
	QString rowKey;
};

void FilterReplaySavedSignalCallback(void *data, calldata_t *cd)
{
	auto *conn = static_cast<FilterSaveConnection *>(data);
	const char *path = calldata_string(cd, "path");
	const QString qpath = path ? QString::fromUtf8(path) : QString();
	QMetaObject::invokeMethod(conn->dock, "NotifyFilterReplaySaved", Qt::QueuedConnection, Q_ARG(QString, conn->rowKey),
				  Q_ARG(QString, qpath));
}

} // namespace

ReplayBufferDock::ReplayBufferDock(QWidget *parent) : QFrame(parent)
{
	setObjectName(QStringLiteral("ReplayBufferDock"));

	auto *outerLayout = new QVBoxLayout(this);
	outerLayout->setContentsMargins(0, 0, 0, 0);

	// Where trimmed clips actually land (e.g. moved off a RAM disk onto
	// persistent storage) is set-only, via SetDestDirByPath over the
	// websocket bridge -- there's no manual UI control for it here. RAM disk
	// mounting only ever happens through the companion Backtrack app, and
	// that app always pushes this field itself whenever it's running, which
	// made a second, manually-editable copy of the same value redundant and
	// a way for the two to disagree. With no companion app driving it, this
	// just stays empty and clips land exactly where the buffer wrote them
	// (the plain default-file-location behavior), same as always.
	auto *content = new QWidget(this);
	rowsLayout = new QVBoxLayout(content);

	auto *scrollArea = new QScrollArea(this);
	scrollArea->setWidgetResizable(true);
	scrollArea->setWidget(content);
	outerLayout->addWidget(scrollArea);

	LoadDestDir();
	LoadBufferDuration();

	refreshTimer = new QTimer(this);
	connect(refreshTimer, &QTimer::timeout, this, &ReplayBufferDock::RefreshAll);
	refreshTimer->start(1000);

	obs_frontend_add_event_callback(FrontendEventCallback, this);

	// obs_module_post_load() (where this dock is constructed) fires very early
	// in OBS's startup, before the frontend's replay-buffer-output machinery is
	// necessarily ready -- touching it synchronously here can crash. Defer the
	// first real query to just after the event loop goes idle.
	QTimer::singleShot(0, this, [this]() {
		ReacquireMainOutput();
		RefreshAll();
	});
}

ReplayBufferDock::~ReplayBufferDock()
{
	obs_frontend_remove_event_callback(FrontendEventCallback, this);
	ReleaseMainOutput();
	for (auto &row : rows) {
		DisconnectFilterSaveSignal(row);
		if (row.filterWeak)
			obs_weak_source_release(row.filterWeak);
	}
}

void ReplayBufferDock::NotifyMainReplayStopped(qlonglong code)
{
	mainReplayError = code != OBS_OUTPUT_SUCCESS;
}

void ReplayBufferDock::NotifyMainReplaySaved()
{
	if (!mainReplayOutputRef)
		return;
	proc_handler_t *ph = obs_output_get_proc_handler(mainReplayOutputRef);
	if (!ph)
		return;

	calldata_t cd;
	calldata_init(&cd);
	QString path;
	if (proc_handler_call(ph, "get_last_replay", &cd)) {
		const char *p = calldata_string(&cd, "path");
		if (p && strlen(p))
			path = QString::fromUtf8(p);
	}
	calldata_free(&cd);
	if (path.isEmpty())
		return;

	const int idx = FindRowIndex(QStringLiteral("main"), true);
	if (idx < 0)
		return;
	TrimAndReplace(QStringLiteral("main"), true, path, rows[idx].slider->value());
}

void ReplayBufferDock::NotifyFilterReplaySaved(QString rowKey, QString path)
{
	if (path.isEmpty())
		return;
	const int idx = FindRowIndex(rowKey, false);
	if (idx < 0)
		return;
	TrimAndReplace(rowKey, false, path, rows[idx].slider->value());
}

void ReplayBufferDock::TrimAndReplace(const QString &rowKey, bool isMain, const QString &path, int seconds)
{
	std::string stdPath = path.toStdString();
	std::string stdDestDir = destDir.toStdString();
	QPointer<ReplayBufferDock> self(this);
	std::thread([self, rowKey, isMain, path, stdPath, seconds, stdDestDir]() {
		const std::string trimmedPath = TrimReplayToLastSeconds(stdPath, seconds, stdDestDir);
		if (!self)
			return;
		// The save itself already happened regardless of whether trimming/moving
		// afterward succeeded -- if that failed, the original file is left
		// untouched at `path`, so fall back to announcing that instead of
		// dropping the notification entirely.
		const QString finalPath = trimmedPath.empty() ? path : QString::fromStdString(trimmedPath);
		QMetaObject::invokeMethod(self, "NotifyTrimComplete", Qt::QueuedConnection, Q_ARG(QString, rowKey),
					  Q_ARG(bool, isMain), Q_ARG(QString, finalPath));
	}).detach();
}

void ReplayBufferDock::NotifyTrimComplete(QString rowKey, bool isMain, QString finalPath)
{
	// OBS's own status bar already announces the main replay buffer's saves;
	// source-record filters are plugin-created outputs it doesn't know about,
	// so nothing shows for those unless we do it ourselves here. Shown only
	// now (with the clip's real final location) rather than at save time,
	// since a configured destination folder can move it after trimming.
	if (!isMain) {
		const int idx = FindRowIndex(rowKey, false);
		QMainWindow *mainWindow = qobject_cast<QMainWindow *>(static_cast<QWidget *>(obs_frontend_get_main_window()));
		if (idx >= 0 && mainWindow && mainWindow->statusBar()) {
			const QString msg = QString::fromUtf8(obs_module_text("FilterReplaySavedTo"))
						     .arg(rows[idx].nameLabel->text(), finalPath);
			mainWindow->statusBar()->showMessage(msg, 10000);
		}
	}

	WebsocketBridge::EmitRowSaved(rowKey, finalPath);
}

void ReplayBufferDock::LoadDestDir()
{
	char *cpath = obs_module_config_path("dest-dir.txt");
	if (!cpath)
		return;
	QFile file(QString::fromUtf8(cpath));
	bfree(cpath);
	if (file.open(QIODevice::ReadOnly | QIODevice::Text))
		destDir = QString::fromUtf8(file.readAll()).trimmed();
}

void ReplayBufferDock::SaveDestDir()
{
	char *cpath = obs_module_config_path("dest-dir.txt");
	if (!cpath)
		return;
	const QString qpath = QString::fromUtf8(cpath);
	bfree(cpath);

	QDir().mkpath(QFileInfo(qpath).absolutePath());
	QFile file(qpath);
	if (file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
		QTextStream stream(&file);
		stream << destDir;
	}
}

void ReplayBufferDock::LoadBufferDuration()
{
	char *cpath = obs_module_config_path("buffer-duration.txt");
	if (!cpath)
		return;
	QFile file(QString::fromUtf8(cpath));
	bfree(cpath);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
		return;
	bool ok = false;
	const int seconds = QString::fromUtf8(file.readAll()).trimmed().toInt(&ok);
	if (ok && seconds > 0)
		bufferDurationSeconds = seconds;
}

void ReplayBufferDock::SaveBufferDuration()
{
	char *cpath = obs_module_config_path("buffer-duration.txt");
	if (!cpath)
		return;
	const QString qpath = QString::fromUtf8(cpath);
	bfree(cpath);

	QDir().mkpath(QFileInfo(qpath).absolutePath());
	QFile file(qpath);
	if (file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
		QTextStream stream(&file);
		stream << bufferDurationSeconds;
	}
}

// Static: called from AddRow (no live ReplayRow to read a slider off of yet)
// as well as from SetBufferDurationSeconds. obs_source_update merges these
// keys into the filter's existing settings and, per source-record.c's own
// update handler, tears down and restarts just the replay output with the
// new duration if it actually changed -- this mirrors exactly what happens
// when a user drags the filter's own "replay_duration" slider in its
// Properties dialog, just triggered externally instead.
void ReplayBufferDock::ApplyBufferDurationToFilter(obs_weak_source_t *filterWeak, int seconds)
{
	if (!filterWeak)
		return;
	obs_source_t *strong = obs_weak_source_get_source(filterWeak);
	if (!strong)
		return;
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "replay_duration", seconds);
	obs_source_update(strong, settings);
	obs_data_release(settings);
	obs_source_release(strong);
}

void ReplayBufferDock::FrontendEventCallback(enum obs_frontend_event event, void *data)
{
	auto *dock = static_cast<ReplayBufferDock *>(data);
	dock->HandleFrontendEvent(event);
}

void ReplayBufferDock::HandleFrontendEvent(enum obs_frontend_event event)
{
	if (event == OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTING || event == OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED) {
		ReacquireMainOutput();
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		ReleaseMainOutput();
	}
}

void ReplayBufferDock::ReleaseMainOutput()
{
	if (!mainReplayOutputRef)
		return;
	signal_handler_t *sh = obs_output_get_signal_handler(mainReplayOutputRef);
	if (sh) {
		signal_handler_disconnect(sh, "stop", MainReplayStoppedSignalCallback, this);
		signal_handler_disconnect(sh, "saved", MainReplaySavedSignalCallback, this);
	}
	obs_output_release(mainReplayOutputRef);
	mainReplayOutputRef = nullptr;
}

void ReplayBufferDock::ReacquireMainOutput()
{
	ReleaseMainOutput();
	mainReplayOutputRef = obs_frontend_get_replay_buffer_output();
	if (!mainReplayOutputRef)
		return;

	signal_handler_t *sh = obs_output_get_signal_handler(mainReplayOutputRef);
	if (sh) {
		signal_handler_connect(sh, "stop", MainReplayStoppedSignalCallback, this);
		signal_handler_connect(sh, "saved", MainReplaySavedSignalCallback, this);
	}
	mainReplayError = false;
}

void ReplayBufferDock::ConnectFilterSaveSignal(ReplayRow &row)
{
	if (row.isMain || !row.filterWeak || row.filterSaveConn)
		return;
	obs_source_t *strong = obs_weak_source_get_source(row.filterWeak);
	if (!strong)
		return;
	signal_handler_t *sh = obs_source_get_signal_handler(strong);
	if (sh) {
		auto *conn = new FilterSaveConnection{this, row.key};
		signal_handler_connect(sh, "replay_saved", FilterReplaySavedSignalCallback, conn);
		row.filterSaveConn = conn;
	}
	obs_source_release(strong);
}

void ReplayBufferDock::DisconnectFilterSaveSignal(ReplayRow &row)
{
	if (!row.filterSaveConn)
		return;
	auto *conn = static_cast<FilterSaveConnection *>(row.filterSaveConn);
	obs_source_t *strong = row.filterWeak ? obs_weak_source_get_source(row.filterWeak) : nullptr;
	if (strong) {
		signal_handler_t *sh = obs_source_get_signal_handler(strong);
		if (sh)
			signal_handler_disconnect(sh, "replay_saved", FilterReplaySavedSignalCallback, conn);
		obs_source_release(strong);
	}
	delete conn;
	row.filterSaveConn = nullptr;
}

int ReplayBufferDock::FindRowIndex(const QString &key, bool isMain) const
{
	for (int i = 0; i < rows.size(); i++) {
		if (rows[i].isMain == isMain && rows[i].key == key)
			return i;
	}
	return -1;
}

int ReplayBufferDock::AddRow(bool isMain, const QString &key, const QString &label, obs_weak_source_t *weak)
{
	ReplayRow row;
	row.isMain = isMain;
	row.key = key;
	row.filterWeak = weak;

	row.container = new QFrame(this);
	row.container->setFrameShape(QFrame::NoFrame);

	row.nameLabel = new QLabel(label, row.container);
	row.slider = new QSlider(Qt::Horizontal, row.container);
	row.slider->setRange(kMinDurationSeconds, kMaxDurationSeconds);
	row.slider->setSingleStep(5);
	row.slider->setPageStep(30);
	row.slider->setValue(kDefaultDurationSeconds);
	row.valueLabel = new QLabel(row.container);
	row.valueLabel->setMinimumWidth(50);
	row.valueLabel->setText(FormatDuration(kDefaultDurationSeconds));
	row.statusDot = new QLabel(row.container);
	row.statusDot->setFixedSize(14, 14);
	row.hotkeyLabel = new QLabel(row.container);
	row.hotkeyLabel->setMinimumWidth(70);
	row.saveButton = new QPushButton(QString::fromUtf8(obs_module_text("Save")), row.container);
	SetStatusDot(row.statusDot, 0);

	// Top: name, status dot, hotkey, save button. Bottom: the slider, full
	// width, with its value beside it -- keeping the slider on its own row
	// means it stays usable even when the dock is narrow.
	auto *topRow = new QHBoxLayout();
	topRow->addWidget(row.nameLabel, 1);
	topRow->addWidget(row.statusDot);
	topRow->addWidget(row.hotkeyLabel);
	topRow->addWidget(row.saveButton);

	auto *bottomRow = new QHBoxLayout();
	bottomRow->addWidget(row.slider, 1);
	bottomRow->addWidget(row.valueLabel);

	auto *containerLayout = new QVBoxLayout(row.container);
	containerLayout->addLayout(topRow);
	containerLayout->addLayout(bottomRow);

	rowsLayout->insertWidget(static_cast<int>(rows.size()), row.container);

	const int index = rows.size();
	rows.push_back(row);

	if (!isMain) {
		ConnectFilterSaveSignal(rows[index]);
		// A freshly-discovered filter still has whatever replay_duration it was
		// last saved with in the scene file (or the plugin's own hardcoded
		// default) -- push our configured length onto it now so newly-added
		// filters don't silently reintroduce an oversized buffer.
		ApplyBufferDurationToFilter(weak, bufferDurationSeconds);
	}

	const QString rowKey = key;
	const bool rowIsMain = isMain;
	connect(row.slider, &QSlider::valueChanged, this, [this, rowKey, rowIsMain](int value) {
		const int idx = FindRowIndex(rowKey, rowIsMain);
		if (idx >= 0)
			rows[idx].valueLabel->setText(FormatDuration(value));
	});
	connect(row.saveButton, &QPushButton::clicked, this, [this, rowKey, rowIsMain]() {
		const int idx = FindRowIndex(rowKey, rowIsMain);
		if (idx < 0)
			return;
		if (rowIsMain)
			TriggerMainSave();
		else
			TriggerFilterSave(rows[idx].filterWeak);
	});

	return index;
}

void ReplayBufferDock::RebuildRows(const QMap<QString, obs_weak_source_t *> &discoveredFilters)
{
	for (auto &row : rows) {
		DisconnectFilterSaveSignal(row);
		if (row.filterWeak)
			obs_weak_source_release(row.filterWeak);
		delete row.container; // also deletes nameLabel/slider/valueLabel/statusDot/hotkeyLabel/saveButton
	}
	rows.clear();

	QLayoutItem *item;
	while ((item = rowsLayout->takeAt(0)) != nullptr)
		delete item;

	AddRow(true, QStringLiteral("main"), QString::fromUtf8(obs_module_text("MainReplayBuffer")), nullptr);

	QStringList keys = discoveredFilters.keys();
	keys.sort(Qt::CaseInsensitive);
	for (const auto &key : keys) {
		obs_weak_source_t *weak = discoveredFilters.value(key);
		QString label = key;
		obs_source_t *strong = obs_weak_source_get_source(weak);
		if (strong) {
			label = FilterRowLabel(strong);
			obs_source_release(strong);
		}
		AddRow(false, key, label, weak);
	}

	rowsLayout->addStretch(1);
}

void ReplayBufferDock::RefreshAll()
{
	std::vector<obs_weak_source_t *> discovered = DiscoverSourceRecordFilters();

	QMap<QString, obs_weak_source_t *> discoveredMap;
	for (auto *weak : discovered) {
		obs_source_t *strong = obs_weak_source_get_source(weak);
		if (!strong) {
			obs_weak_source_release(weak);
			continue;
		}
		// Not all OBS versions this plugin targets have obs_source_get_uuid(), so use the
		// source object's own (stable-for-its-lifetime) address as the identity key instead.
		const QString key = QString::number(reinterpret_cast<quintptr>(strong), 16);
		obs_source_release(strong);
		if (discoveredMap.contains(key)) {
			obs_weak_source_release(weak);
			continue;
		}
		discoveredMap.insert(key, weak);
	}

	bool setChanged = rows.isEmpty();
	if (!setChanged) {
		QStringList existingKeys;
		for (const auto &row : rows)
			if (!row.isMain)
				existingKeys << row.key;

		for (auto it = discoveredMap.constBegin(); it != discoveredMap.constEnd(); ++it) {
			if (!existingKeys.contains(it.key())) {
				setChanged = true;
				break;
			}
		}
		if (!setChanged) {
			for (const auto &k : existingKeys) {
				if (!discoveredMap.contains(k)) {
					setChanged = true;
					break;
				}
			}
		}
	}

	if (setChanged) {
		RebuildRows(discoveredMap);
	} else {
		for (auto *weak : discoveredMap.values())
			obs_weak_source_release(weak);
	}

	for (int i = 0; i < rows.size(); i++)
		UpdateRow(i);
}

void ReplayBufferDock::UpdateRow(int index)
{
	ReplayRow &row = rows[index];
	if (row.isMain)
		UpdateMainRow(row);
	else
		UpdateFilterRow(row);
}

void ReplayBufferDock::UpdateMainRow(ReplayRow &row)
{
	const bool active = obs_frontend_replay_buffer_active();
	row.statusState = mainReplayError ? 2 : (active ? 1 : 0);
	SetStatusDot(row.statusDot, row.statusState);

	QString hotkey = mainReplayOutputRef ? FindOutputHotkeyString(mainReplayOutputRef) : QString();
	row.hotkeyLabel->setText(hotkey.isEmpty() ? QString::fromUtf8(obs_module_text("Unbound")) : hotkey);
}

void ReplayBufferDock::UpdateFilterRow(ReplayRow &row)
{
	obs_source_t *strong = obs_weak_source_get_source(row.filterWeak);
	if (!strong) {
		row.statusState = 0;
		SetStatusDot(row.statusDot, 0);
		row.hotkeyLabel->setText(QString());
		return;
	}

	int state = 0;
	QString hotkey;
	proc_handler_t *ph = obs_source_get_proc_handler(strong);
	if (ph) {
		calldata_t cd;
		calldata_init(&cd);
		if (proc_handler_call(ph, "get_replay_buffer_status", &cd)) {
			const bool active = calldata_bool(&cd, "active");
			const bool error = calldata_bool(&cd, "error");
			const char *hk = calldata_string(&cd, "hotkey");
			hotkey = hk ? QString::fromUtf8(hk) : QString();
			state = error ? 2 : (active ? 1 : 0);
		}
		calldata_free(&cd);
	}
	row.statusState = state;
	SetStatusDot(row.statusDot, state);
	row.hotkeyLabel->setText(hotkey.isEmpty() ? QString::fromUtf8(obs_module_text("Unbound")) : hotkey);

	obs_source_release(strong);
}

void ReplayBufferDock::TriggerMainSave()
{
	obs_frontend_replay_buffer_save();
}

void ReplayBufferDock::TriggerFilterSave(obs_weak_source_t *filterWeak)
{
	if (!filterWeak)
		return;
	obs_source_t *strong = obs_weak_source_get_source(filterWeak);
	if (!strong)
		return;
	proc_handler_t *ph = obs_source_get_proc_handler(strong);
	if (ph) {
		calldata_t cd;
		calldata_init(&cd);
		proc_handler_call(ph, "save_replay_buffer", &cd);
		calldata_free(&cd);
	}
	obs_source_release(strong);
}

QString ReplayBufferDock::BuildRowsJson()
{
	QString json = QStringLiteral("{\"rows\":[");
	for (int i = 0; i < rows.size(); i++) {
		const ReplayRow &row = rows[i];
		if (i)
			json += QLatin1Char(',');
		json += QStringLiteral("{\"key\":\"%1\",\"label\":\"%2\",\"is_main\":%3,\"length_seconds\":%4,"
					"\"hotkey\":\"%5\",\"status\":%6}")
				.arg(JsonEscape(row.key), JsonEscape(row.nameLabel->text()),
				     row.isMain ? QStringLiteral("true") : QStringLiteral("false"),
				     QString::number(row.slider->value()), JsonEscape(row.hotkeyLabel->text()),
				     QString::number(row.statusState));
	}
	json += QStringLiteral("]}");
	return json;
}

bool ReplayBufferDock::SaveRowByKey(QString key)
{
	const bool isMain = key == QStringLiteral("main");
	const int idx = FindRowIndex(key, isMain);
	if (idx < 0)
		return false;

	if (isMain)
		TriggerMainSave();
	else
		TriggerFilterSave(rows[idx].filterWeak);
	return true;
}

bool ReplayBufferDock::SetRowLengthByKey(QString key, int seconds)
{
	const bool isMain = key == QStringLiteral("main");
	const int idx = FindRowIndex(key, isMain);
	if (idx < 0)
		return false;

	// QSlider::setValue clamps to [kMinDurationSeconds, kMaxDurationSeconds] itself,
	// and its existing valueChanged connection already keeps valueLabel in sync --
	// this is exactly what dragging the slider by hand does.
	rows[idx].slider->setValue(seconds);
	return true;
}

bool ReplayBufferDock::SetDestDirByPath(QString path)
{
	const QString trimmed = path.trimmed();
	if (!trimmed.isEmpty() && !QDir(trimmed).exists())
		return false;

	destDir = trimmed;
	SaveDestDir();
	return true;
}

bool ReplayBufferDock::SetBufferDurationSeconds(int seconds)
{
	if (seconds <= 0)
		return false;

	bufferDurationSeconds = seconds;
	SaveBufferDuration();

	for (const ReplayRow &row : rows) {
		if (!row.isMain)
			ApplyBufferDurationToFilter(row.filterWeak, seconds);
	}
	return true;
}

void ReplayBufferDock::SetStatusDot(QLabel *dot, int state)
{
	const char *color = "#808080";
	if (state == 1)
		color = "#2ecc71";
	else if (state == 2)
		color = "#e74c3c";
	dot->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 7px;").arg(QString::fromUtf8(color)));
}
