#include "replay-buffer-dock.hpp"
#include "hotkey-lookup.hpp"

#include <util/config-file.h>

#include <QVBoxLayout>
#include <QGridLayout>
#include <QScrollArea>
#include <QSlider>
#include <QLabel>
#include <QTimer>
#include <QStringList>

#include <cstring>

namespace {

constexpr int kMinDurationSeconds = 30;
constexpr int kMaxDurationSeconds = 900;

QString FormatDuration(int seconds)
{
	int m = seconds / 60;
	int s = seconds % 60;
	return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

int ClampDuration(int seconds)
{
	if (seconds < kMinDurationSeconds)
		return kMinDurationSeconds;
	if (seconds > kMaxDurationSeconds)
		return kMaxDurationSeconds;
	return seconds;
}

bool OutputModeIsAdvanced(config_t *config)
{
	const char *mode = config_get_string(config, "Output", "Mode");
	return mode && (strcmp(mode, "Advanced") == 0 || strcmp(mode, "advanced") == 0);
}

int GetMainReplayDurationSeconds()
{
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return 300;
	const bool adv = OutputModeIsAdvanced(config);
	long long val = config_get_int(config, adv ? "AdvOut" : "SimpleOutput", "RecRBTime");
	return val > 0 ? (int)val : 300;
}

void SetMainReplayDurationSecondsConfig(int seconds)
{
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return;
	const bool adv = OutputModeIsAdvanced(config);
	config_set_int(config, adv ? "AdvOut" : "SimpleOutput", "RecRBTime", seconds);
	config_save(config);
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
// and later disconnect from the main replay buffer output's "stop" signal.
void MainReplayStoppedSignalCallback(void *data, calldata_t *cd)
{
	auto *dock = static_cast<ReplayBufferDock *>(data);
	const long long code = calldata_int(cd, "code");
	QMetaObject::invokeMethod(dock, "NotifyMainReplayStopped", Qt::QueuedConnection, Q_ARG(qlonglong, code));
}

} // namespace

ReplayBufferDock::ReplayBufferDock(QWidget *parent) : QFrame(parent)
{
	setObjectName(QStringLiteral("ReplayBufferDock"));

	auto *outerLayout = new QVBoxLayout(this);
	outerLayout->setContentsMargins(0, 0, 0, 0);

	auto *content = new QWidget(this);
	grid = new QGridLayout(content);
	grid->setColumnStretch(1, 1);

	auto *scrollArea = new QScrollArea(this);
	scrollArea->setWidgetResizable(true);
	scrollArea->setWidget(content);
	outerLayout->addWidget(scrollArea);

	ReacquireMainOutput();

	refreshTimer = new QTimer(this);
	connect(refreshTimer, &QTimer::timeout, this, &ReplayBufferDock::RefreshAll);
	refreshTimer->start(1000);

	obs_frontend_add_event_callback(FrontendEventCallback, this);

	RefreshAll();
}

ReplayBufferDock::~ReplayBufferDock()
{
	obs_frontend_remove_event_callback(FrontendEventCallback, this);
	ReleaseMainOutput();
	for (auto &row : rows) {
		if (row.filterWeak)
			obs_weak_source_release(row.filterWeak);
	}
}

void ReplayBufferDock::NotifyMainReplayStopped(qlonglong code)
{
	mainReplayError = code != OBS_OUTPUT_SUCCESS;
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
	} else if (event == OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED) {
		if (pendingMainRestart) {
			pendingMainRestart = false;
			obs_frontend_replay_buffer_start();
		}
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		ReleaseMainOutput();
	}
}

void ReplayBufferDock::ReleaseMainOutput()
{
	if (!mainReplayOutputRef)
		return;
	signal_handler_t *sh = obs_output_get_signal_handler(mainReplayOutputRef);
	if (sh)
		signal_handler_disconnect(sh, "stop", MainReplayStoppedSignalCallback, this);
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
	if (sh)
		signal_handler_connect(sh, "stop", MainReplayStoppedSignalCallback, this);
	mainReplayError = false;
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

	const int gridRow = rows.size();

	row.nameLabel = new QLabel(label, this);
	row.slider = new QSlider(Qt::Horizontal, this);
	row.slider->setRange(kMinDurationSeconds, kMaxDurationSeconds);
	row.slider->setSingleStep(5);
	row.slider->setPageStep(30);
	row.valueLabel = new QLabel(this);
	row.valueLabel->setMinimumWidth(50);
	row.statusDot = new QLabel(this);
	row.statusDot->setFixedSize(14, 14);
	row.hotkeyLabel = new QLabel(this);
	row.hotkeyLabel->setMinimumWidth(70);
	SetStatusDot(row.statusDot, 0);

	grid->addWidget(row.nameLabel, gridRow, 0);
	grid->addWidget(row.slider, gridRow, 1);
	grid->addWidget(row.valueLabel, gridRow, 2);
	grid->addWidget(row.statusDot, gridRow, 3);
	grid->addWidget(row.hotkeyLabel, gridRow, 4);

	const int index = rows.size();
	rows.push_back(row);

	const QString rowKey = key;
	const bool rowIsMain = isMain;
	connect(row.slider, &QSlider::valueChanged, this, [this, rowKey, rowIsMain](int value) {
		const int idx = FindRowIndex(rowKey, rowIsMain);
		if (idx >= 0)
			rows[idx].valueLabel->setText(FormatDuration(value));
	});
	connect(row.slider, &QSlider::sliderReleased, this, [this, rowKey, rowIsMain]() {
		const int idx = FindRowIndex(rowKey, rowIsMain);
		if (idx < 0)
			return;
		const int seconds = rows[idx].slider->value();
		if (rowIsMain)
			ApplyMainDuration(seconds);
		else
			ApplyFilterDuration(rows[idx].filterWeak, seconds);
	});

	return index;
}

void ReplayBufferDock::RebuildRows(const QMap<QString, obs_weak_source_t *> &discoveredFilters)
{
	for (auto &row : rows) {
		if (row.filterWeak)
			obs_weak_source_release(row.filterWeak);
		delete row.nameLabel;
		delete row.slider;
		delete row.valueLabel;
		delete row.statusDot;
		delete row.hotkeyLabel;
	}
	rows.clear();

	QLayoutItem *item;
	while ((item = grid->takeAt(0)) != nullptr)
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
		const QString key = QString::fromUtf8(obs_source_get_uuid(strong));
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
	if (!row.slider->isSliderDown()) {
		const int seconds = ClampDuration(GetMainReplayDurationSeconds());
		if (row.slider->value() != seconds)
			row.slider->setValue(seconds);
		row.valueLabel->setText(FormatDuration(seconds));
	}

	const bool active = obs_frontend_replay_buffer_active();
	SetStatusDot(row.statusDot, mainReplayError ? 2 : (active ? 1 : 0));

	QString hotkey = mainReplayOutputRef ? FindOutputHotkeyString(mainReplayOutputRef) : QString();
	row.hotkeyLabel->setText(hotkey.isEmpty() ? QString::fromUtf8(obs_module_text("Unbound")) : hotkey);
}

void ReplayBufferDock::UpdateFilterRow(ReplayRow &row)
{
	obs_source_t *strong = obs_weak_source_get_source(row.filterWeak);
	if (!strong) {
		SetStatusDot(row.statusDot, 0);
		row.hotkeyLabel->setText(QString());
		return;
	}

	if (!row.slider->isSliderDown()) {
		obs_data_t *settings = obs_source_get_settings(strong);
		const int seconds = ClampDuration((int)obs_data_get_int(settings, "replay_duration"));
		obs_data_release(settings);
		if (row.slider->value() != seconds)
			row.slider->setValue(seconds);
		row.valueLabel->setText(FormatDuration(seconds));
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
	SetStatusDot(row.statusDot, state);
	row.hotkeyLabel->setText(hotkey.isEmpty() ? QString::fromUtf8(obs_module_text("Unbound")) : hotkey);

	obs_source_release(strong);
}

void ReplayBufferDock::ApplyMainDuration(int seconds)
{
	SetMainReplayDurationSecondsConfig(seconds);
	if (obs_frontend_replay_buffer_active()) {
		pendingMainRestart = true;
		obs_frontend_replay_buffer_stop();
	}
}

void ReplayBufferDock::ApplyFilterDuration(obs_weak_source_t *filterWeak, int seconds)
{
	if (!filterWeak)
		return;
	obs_source_t *strong = obs_weak_source_get_source(filterWeak);
	if (!strong)
		return;
	obs_data_t *settings = obs_source_get_settings(strong);
	obs_data_set_int(settings, "replay_duration", seconds);
	obs_source_update(strong, settings);
	obs_data_release(settings);
	obs_source_release(strong);
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
