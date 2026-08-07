#include "control-panel-dock.hpp"
#include "source-record-discovery.hpp"

#include <obs-module.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QLabel>
#include <QTimer>
#include <QPushButton>
#include <QMap>
#include <QStringList>

// Mirrors OUTPUT_MODE_NONE / OUTPUT_MODE_ALWAYS from obs-source-record's
// source-record.c -- not exposed via a shared header (source-record.h is
// empty), so these are just the same two integer values by convention, same
// as this dock already relies on the "replay_buffer"/"replay_duration"
// setting *names* matching without a shared header.
namespace {
constexpr int kRecordModeNone = 0;
constexpr int kRecordModeAlways = 1;
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

	row.nameLabel = new QLabel(label, row.container);
	row.statusDot = new QLabel(row.container);
	row.statusDot->setFixedSize(14, 14);
	row.recordButton = new QPushButton(QString::fromUtf8(obs_module_text("StartRecord")), row.container);
	SetStatusDot(row.statusDot, 0);

	auto *rowLayout = new QHBoxLayout(row.container);
	rowLayout->addWidget(row.nameLabel, 1);
	rowLayout->addWidget(row.statusDot);
	rowLayout->addWidget(row.recordButton);

	rowsLayout->insertWidget(static_cast<int>(rows.size()), row.container);

	const int index = rows.size();
	rows.push_back(row);

	const QString rowKey = key;
	connect(row.recordButton, &QPushButton::clicked, this, [this, rowKey]() {
		const int idx = FindRowIndex(rowKey);
		if (idx < 0)
			return;
		ToggleRecord(rows[idx].filterWeak, !rows[idx].recordActive);
	});

	return index;
}

void ControlPanelDock::RebuildRows(const QMap<QString, obs_weak_source_t *> &discoveredFilters)
{
	for (auto &row : rows) {
		if (row.filterWeak)
			obs_weak_source_release(row.filterWeak);
		delete row.container; // also deletes nameLabel/statusDot/recordButton
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

void ControlPanelDock::UpdateRow(ControlRow &row)
{
	obs_source_t *strong = obs_weak_source_get_source(row.filterWeak);
	if (!strong) {
		SetStatusDot(row.statusDot, 0);
		return;
	}

	bool active = false;
	proc_handler_t *ph = obs_source_get_proc_handler(strong);
	if (ph) {
		calldata_t cd;
		calldata_init(&cd);
		if (proc_handler_call(ph, "get_record_status", &cd))
			active = calldata_bool(&cd, "active");
		calldata_free(&cd);
	}
	obs_source_release(strong);

	if (active != row.recordActive) {
		row.recordActive = active;
		row.recordButton->setText(
			QString::fromUtf8(obs_module_text(active ? "StopRecord" : "StartRecord")));
	}
	SetStatusDot(row.statusDot, active ? 1 : 0);
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

void ControlPanelDock::SetStatusDot(QLabel *dot, int state)
{
	const char *color = state == 1 ? "#2ecc71" : "#808080";
	dot->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 7px;").arg(QString::fromUtf8(color)));
}
