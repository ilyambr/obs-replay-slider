#pragma once

#include <obs.h>
#include <obs-frontend-api.h>

#include <QFrame>
#include <QVector>
#include <QMap>
#include <QString>
#include <QSize>

class QVBoxLayout;
class QSlider;
class QLabel;
class QTimer;
class QPushButton;

struct ReplayRow {
	bool isMain = false;
	obs_weak_source_t *filterWeak = nullptr; // owned; null for the main row
	QString key;                             // "main", or the filter source's uuid

	QFrame *container = nullptr; // owns everything below; the only thing added to the outer layout
	QLabel *nameLabel = nullptr;
	QSlider *slider = nullptr; // "save length" in seconds -- purely local UI state
	QLabel *valueLabel = nullptr;
	QLabel *statusDot = nullptr;
	QLabel *hotkeyLabel = nullptr;
	QPushButton *saveButton = nullptr;
	int statusState = 0; // 0 grey, 1 green, 2 red -- mirrors statusDot; kept for the websocket bridge

	// Owned FilterSaveConnection*, connecting this filter's own "replay_saved"
	// signal back to this row; null for the main row.
	void *filterSaveConn = nullptr;
};

// Dock showing one row per replay buffer found (the built-in OBS replay buffer,
// plus every "source_record_filter" instance that has its own replay buffer).
// Each row has a "save length" slider (30s-60min) and a Save button: pressing
// it (or the buffer's own hotkey) saves the full rolling buffer as usual, and
// this dock then trims the result down to just the selected length -- like a
// game console's "save last N minutes of gameplay" feature. A green/grey/red
// status dot and the bound "Save Replay" hotkey are shown for reference.
class ReplayBufferDock : public QFrame {
	Q_OBJECT

public:
	explicit ReplayBufferDock(QWidget *parent = nullptr);
	~ReplayBufferDock() override;

	QSize sizeHint() const override { return QSize(420, 240); }

public slots:
	void NotifyMainReplayStopped(qlonglong code);
	void NotifyMainReplaySaved();
	void NotifyFilterReplaySaved(QString rowKey, QString path);

	// Called (only) via QMetaObject::invokeMethod(..., Qt::BlockingQueuedConnection)
	// from WebsocketBridge, which runs on obs-websocket's own thread -- these are
	// the only two things an external tool can do through that bridge: list the
	// current rows, and trigger one row's save, same as pressing its Save button.
	QString BuildRowsJson();
	bool SaveRowByKey(QString key);

private slots:
	void RefreshAll();

private:
	int AddRow(bool isMain, const QString &key, const QString &label, obs_weak_source_t *weak);
	void RebuildRows(const QMap<QString, obs_weak_source_t *> &discoveredFilters);
	int FindRowIndex(const QString &key, bool isMain) const;

	void UpdateRow(int index);
	void UpdateMainRow(ReplayRow &row);
	void UpdateFilterRow(ReplayRow &row);

	void TriggerMainSave();
	void TriggerFilterSave(obs_weak_source_t *filterWeak);
	void TrimAndReplace(const QString &path, int seconds);

	void ConnectFilterSaveSignal(ReplayRow &row);
	void DisconnectFilterSaveSignal(ReplayRow &row);

	void ReacquireMainOutput();
	void ReleaseMainOutput();

	static void SetStatusDot(QLabel *dot, int state); // 0 grey, 1 green, 2 red
	static void FrontendEventCallback(enum obs_frontend_event event, void *data);
	void HandleFrontendEvent(enum obs_frontend_event event);

	QVBoxLayout *rowsLayout = nullptr;
	QTimer *refreshTimer = nullptr;
	QVector<ReplayRow> rows;

	obs_output_t *mainReplayOutputRef = nullptr;
	bool mainReplayError = false;
};
