#include "control-panel-dock.hpp"
#include "source-record-discovery.hpp"

#include <obs-module.h>

#include <QVBoxLayout>
#include <QScrollArea>
#include <QLabel>
#include <QTimer>
#include <QPushButton>
#include <QMap>
#include <QStringList>
#include <QFont>
#include <QPalette>
#include <QColor>

// Mirrors OUTPUT_MODE_NONE / OUTPUT_MODE_ALWAYS from obs-source-record's
// source-record.c -- not exposed via a shared header (source-record.h is
// empty), so these are just the same two integer values by convention, same
// as this dock already relies on the "replay_buffer"/"replay_duration"
// setting *names* matching without a shared header.
namespace {
constexpr int kRecordModeNone = 0;
constexpr int kRecordModeAlways = 1;

// Mirrors ReplayRow's own 0/1/2 status-int convention (see
// replay-buffer-dock.cpp's BuildRowsJson), extended to a 4th state: a Source
// Record filter's parent source can itself be "there but not actually
// capturing anything" (e.g. a Window Capture with no window selected, or a
// Video Capture Device that's unplugged) -- distinct from "capturing fine,
// just not recording".
constexpr int kStatusInactive = 0;  // parent source isn't actively capturing
constexpr int kStatusStopped = 1;   // capturing fine, record_mode off
constexpr int kStatusRecording = 2; // actually recording right now
constexpr int kStatusError = 3;     // record_mode was on, output stopped with a failure

// Minimal JSON string escaping -- row keys/labels are plain source-derived
// text, but escape defensively rather than assume that. Duplicated from
// replay-buffer-dock.cpp's own (anonymous-namespace, so not shared) helper
// of the same name.
QString JsonEscape(const QString &s)
{
	QString out = s;
	out.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
	out.replace(QLatin1Char('"'), QStringLiteral("\\\""));
	return out;
}
} // namespace

ControlPanelDock::ControlPanelDock(QWidget *parent) : QFrame(parent)
{
	setObjectName(QStringLiteral("ControlPanelDock"));

	auto *outerLayout = new QVBoxLayout(this);
	outerLayout->setContentsMargins(0, 0, 0, 0);

	auto *content = new QWidget(this);
	rowsLayout = new QVBoxLayout(content);

	auto *scrollArea = new QScrollArea(this);
	scrollArea->setWidgetResizable(true);
	scrollArea->setWidget(content);
	outerLayout->addWidget(scrollArea);

	refreshTimer = new QTimer(this);
	connect(refreshTimer, &QTimer::timeout, this, &ControlPanelDock::RefreshAll);
	refreshTimer->start(1000);

	// Same deferred-first-query reasoning as ReplayBufferDock: this dock is
	// constructed from obs_module_post_load(), still early in OBS's startup.
	QTimer::singleShot(0, this, [this]() { RefreshAll(); });
}

ControlPanelDock::~ControlPanelDock()
{
	for (auto &row : rows) {
		if (row.filterWeak)
			obs_weak_source_release(row.filterWeak);
	}
}

int ControlPanelDock::FindRowIndex(const QString &key) const
{
	for (int i = 0; i < rows.size(); i++) {
		if (rows[i].key == key)
			return i;
	}
	return -1;
}

int ControlPanelDock::AddRow(const QString &key, const QString &label, obs_weak_source_t *weak)
{
	ControlRow row;
	row.key = key;
	row.filterWeak = weak;

	row.container = new QFrame(this);
	row.container->setFrameShape(QFrame::NoFrame);

	// Full-width, checkable button on top (same shape as OBS's own Controls
	// dock buttons), with the source's name captioned underneath -- rather
	// than a name/dot/button row -- so this reads as an extension of that
	// dock instead of a distinct plugin widget.
	row.recordButton = new QPushButton(QString::fromUtf8(obs_module_text("StartRecord")), row.container);
	row.recordButton->setCheckable(true);
	row.recordButton->setMinimumHeight(28);

	row.nameLabel = new QLabel(label, row.container);
	row.nameLabel->setAlignment(Qt::AlignHCenter);
	{
		QFont f = row.nameLabel->font();
		f.setPointSizeF(f.pointSizeF() * 0.9);
		row.nameLabel->setFont(f);
	}
	// Muted relative to normal text, same as a caption -- matches the
	// hotkey/status labels ReplayBufferDock already dims via its own palette.
	QPalette mutedPalette = row.nameLabel->palette();
	QColor mutedColor = mutedPalette.color(QPalette::WindowText);
	mutedColor.setAlpha(160);
	mutedPalette.setColor(QPalette::WindowText, mutedColor);
	row.nameLabel->setPalette(mutedPalette);

	ApplyButtonState(row.recordButton, kStatusInactive);

	auto *rowLayout = new QVBoxLayout(row.container);
	rowLayout->setSpacing(2);
	rowLayout->addWidget(row.recordButton);
	rowLayout->addWidget(row.nameLabel);

	rowsLayout->insertWidget(static_cast<int>(rows.size()), row.container);

	const int index = rows.size();
	rows.push_back(row);

	const QString rowKey = key;
	connect(row.recordButton, &QPushButton::clicked, this, [this, rowKey]() {
		const int idx = FindRowIndex(rowKey);
		if (idx < 0)
			return;
		ToggleRecord(rows[idx].filterWeak, rows[idx].status != kStatusRecording);
	});

	return index;
}

void ControlPanelDock::RebuildRows(const QMap<QString, obs_weak_source_t *> &discoveredFilters)
{
	for (auto &row : rows) {
		if (row.filterWeak)
			obs_weak_source_release(row.filterWeak);
		delete row.container; // also deletes nameLabel/recordButton
	}
	rows.clear();

	QLayoutItem *item;
	while ((item = rowsLayout->takeAt(0)) != nullptr)
		delete item;

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
		AddRow(key, label, weak);
	}

	rowsLayout->addStretch(1);
}

void ControlPanelDock::RefreshAll()
{
	std::vector<obs_weak_source_t *> discovered = DiscoverSourceRecordFilters();

	QMap<QString, obs_weak_source_t *> discoveredMap;
	for (auto *weak : discovered) {
		obs_source_t *strong = obs_weak_source_get_source(weak);
		if (!strong) {
			obs_weak_source_release(weak);
			continue;
		}
		// Same address-as-identity-key reasoning as ReplayBufferDock::RefreshAll.
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

	for (auto &row : rows)
		UpdateRow(row);
}

// NOTE: this used to also gate on obs_source_active(parent) to derive
// kStatusInactive ("this source has nothing to capture", e.g. a Window
// Capture with no window selected) -- reverted. obs_source_active() reflects
// activate_refs, i.e. whether the source is part of the currently-showing
// program scene tree; it has nothing to do with whether the capture device
// itself has valid content, and a Source Record filter renders through its
// own private obs_view regardless of whether the parent is in the active
// scene right now -- so that check was going false (and clamping every row
// to Inactive) for perfectly healthy, actively-recording sources any time
// they weren't also the front-and-center program scene. There's no known
// generic libobs signal for "this specific capture source currently has
// valid content" -- so THAT specific case is still unhandled.
//
// obs_source_enabled(parent) is a different, narrower signal added later:
// the literal Sources-list eye-icon toggle, which has nothing to do with
// scene membership (a source can be enabled but not in the active scene, or
// disabled but still in it) -- so it doesn't have the false-positive problem
// that sank obs_source_active() above. A disabled parent genuinely can't be
// capturing anything real regardless of what get_record_status below says.
void ControlPanelDock::UpdateRow(ControlRow &row)
{
	obs_source_t *strong = obs_weak_source_get_source(row.filterWeak);
	if (!strong) {
		row.status = kStatusInactive;
		ApplyButtonState(row.recordButton, kStatusInactive);
		return;
	}

	obs_source_t *parent = obs_filter_get_parent(strong); // borrowed, no release
	if (parent && !obs_source_enabled(parent)) {
		obs_source_release(strong);
		row.status = kStatusInactive;
		ApplyButtonState(row.recordButton, kStatusInactive);
		return;
	}

	bool active = false;
	bool error = false;
	QString path;
	QString hotkey;
	proc_handler_t *ph = obs_source_get_proc_handler(strong);
	if (ph) {
		calldata_t cd;
		calldata_init(&cd);
		if (proc_handler_call(ph, "get_record_status", &cd)) {
			active = calldata_bool(&cd, "active");
			error = calldata_bool(&cd, "error");
			const char *pathStr = calldata_string(&cd, "path");
			if (pathStr)
				path = QString::fromUtf8(pathStr);
			const char *hotkeyStr = calldata_string(&cd, "hotkey");
			if (hotkeyStr)
				hotkey = QString::fromUtf8(hotkeyStr);
		}
		calldata_free(&cd);
	}
	obs_source_release(strong);

	row.lastOutputPath = path;
	row.lastHotkey = hotkey;

	const int status = active ? kStatusRecording : error ? kStatusError : kStatusStopped;

	if (status != row.status) {
		row.status = status;
		ApplyButtonState(row.recordButton, status);
	}
}

// source/filter are the parent source's and the filter's own obs_source_get_name(),
// separate from the "{source} - {filter}" concatenated display label -- so a caller
// (e.g. Backtrack) can reliably look up this filter's settings via the plain
// obs-websocket GetSourceFilterList/SetSourceFilterSettings requests (source name +
// filter name) without parsing the label string, which isn't safe to split on
// " - " if either name happens to contain that substring itself.
QString ControlPanelDock::BuildRowsJson()
{
	QString json = QStringLiteral("{\"rows\":[");
	for (int i = 0; i < rows.size(); i++) {
		const ControlRow &row = rows[i];
		if (i)
			json += QLatin1Char(',');

		QString sourceName;
		QString filterName;
		obs_source_t *strong = obs_weak_source_get_source(row.filterWeak);
		if (strong) {
			filterName = QString::fromUtf8(obs_source_get_name(strong));
			obs_source_t *parent = obs_filter_get_parent(strong); // borrowed, no release
			if (parent)
				sourceName = QString::fromUtf8(obs_source_get_name(parent));
			obs_source_release(strong);
		}

		json += QStringLiteral(
				"{\"key\":\"%1\",\"label\":\"%2\",\"status\":%3,\"source\":\"%4\",\"filter\":\"%5\",\"path\":\"%6\",\"hotkey\":\"%7\"}")
				.arg(JsonEscape(row.key), JsonEscape(row.nameLabel->text()), QString::number(row.status),
				     JsonEscape(sourceName), JsonEscape(filterName), JsonEscape(row.lastOutputPath),
				     JsonEscape(row.lastHotkey));
	}
	json += QStringLiteral("]}");
	return json;
}

bool ControlPanelDock::StartRowByKey(QString key)
{
	const int idx = FindRowIndex(key);
	if (idx < 0)
		return false;
	ToggleRecord(rows[idx].filterWeak, true);
	return true;
}

bool ControlPanelDock::StopRowByKey(QString key)
{
	const int idx = FindRowIndex(key);
	if (idx < 0)
		return false;
	ToggleRecord(rows[idx].filterWeak, false);
	return true;
}

void ControlPanelDock::ToggleRecord(obs_weak_source_t *filterWeak, bool start)
{
	if (!filterWeak)
		return;
	obs_source_t *strong = obs_weak_source_get_source(filterWeak);
	if (!strong)
		return;

	// Same mechanism as obs-source-record's own start_record_source /
	// stop_record_source websocket handlers: "record_mode" is just a filter
	// setting, flip it and push the update. No obs-websocket needed since
	// this dock lives in the same OBS process as the filter.
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "record_mode", start ? kRecordModeAlways : kRecordModeNone);
	obs_source_update(strong, settings);
	obs_data_release(settings);
	obs_source_release(strong);
}

// "Recording" is shown by highlighting the button itself -- like OBS's own
// "Stop Recording" button when a recording is running -- rather than a
// separate status dot. setChecked() alone would already pick up whatever
// checked-state styling the active Qt/OBS theme applies to QPushButton, but
// that's easy to lose track of across themes, so pin it explicitly to the
// theme's own highlight color rather than a hardcoded blue: stays correct
// whether OBS is in a dark, light, or fully custom theme. Error gets its own
// fixed warning red rather than reusing the highlight color, so a failed row
// still reads as distinct from a recording one at a glance.
void ControlPanelDock::ApplyButtonState(QPushButton *button, int status)
{
	const bool recording = status == kStatusRecording;
	button->setChecked(recording);
	button->setText(QString::fromUtf8(obs_module_text(recording ? "StopRecord" : "StartRecord")));
	if (recording) {
		const QColor highlight = button->palette().color(QPalette::Highlight);
		const QColor text = button->palette().color(QPalette::HighlightedText);
		button->setStyleSheet(QStringLiteral("QPushButton { background-color: %1; color: %2; }")
					      .arg(highlight.name(), text.name()));
	} else if (status == kStatusError) {
		button->setStyleSheet(QStringLiteral("QPushButton { background-color: #7a1f1f; color: white; }"));
	} else {
		button->setStyleSheet(QString());
	}
}
