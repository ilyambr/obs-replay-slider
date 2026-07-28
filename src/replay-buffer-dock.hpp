#pragma once

#include <obs.h>
#include <obs-frontend-api.h>

#include <QDockWidget>
#include <QVector>
#include <QMap>
#include <QString>

class QGridLayout;
class QSlider;
class QLabel;
class QTimer;

struct ReplayRow {
	bool isMain = false;
	obs_weak_source_t *filterWeak = nullptr; // owned; null for the main row
	QString key;                             // "main", or the filter source's uuid

	QLabel *nameLabel = nullptr;
	QSlider *slider = nullptr;
	QLabel *valueLabel = nullptr;
	QLabel *statusDot = nullptr;
	QLabel *hotkeyLabel = nullptr;
};

// Dock showing one row per replay buffer found (the built-in OBS replay buffer,
// plus every "source_record_filter" instance that has its own replay buffer) --
// a duration slider (30s-15min), a green/grey/red status dot, and the bound
// "Save Replay" hotkey for each.
class ReplayBufferDock : public QDockWidget {
	Q_OBJECT

public:
	explicit ReplayBufferDock(QWidget *parent = nullptr);
	~ReplayBufferDock() override;

public slots:
	void NotifyMainReplayStopped(qlonglong code);

private slots:
	void RefreshAll();

private:
	int AddRow(bool isMain, const QString &key, const QString &label, obs_weak_source_t *weak);
	void RebuildRows(const QMap<QString, obs_weak_source_t *> &discoveredFilters);
	int FindRowIndex(const QString &key, bool isMain) const;

	void UpdateRow(int index);
	void UpdateMainRow(ReplayRow &row);
	void UpdateFilterRow(ReplayRow &row);

	void ApplyMainDuration(int seconds);
	void ApplyFilterDuration(obs_weak_source_t *filterWeak, int seconds);

	void ReacquireMainOutput();
	void ReleaseMainOutput();

	static void SetStatusDot(QLabel *dot, int state); // 0 grey, 1 green, 2 red
	static void FrontendEventCallback(enum obs_frontend_event event, void *data);
	void HandleFrontendEvent(enum obs_frontend_event event);

	QGridLayout *grid = nullptr;
	QTimer *refreshTimer = nullptr;
	QVector<ReplayRow> rows;

	obs_output_t *mainReplayOutputRef = nullptr;
	bool mainReplayError = false;
	bool pendingMainRestart = false;
};
