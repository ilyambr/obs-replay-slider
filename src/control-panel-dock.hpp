#pragma once

#include <obs.h>
#include <obs-frontend-api.h>

#include <QFrame>
#include <QVector>
#include <QMap>
#include <QString>
#include <QSize>

class QVBoxLayout;
class QLabel;
class QTimer;
class QPushButton;

struct ControlRow {
	obs_weak_source_t *filterWeak = nullptr; // owned
	QString key;                             // filter source's address-derived id, see RefreshAll()

	QFrame *container = nullptr; // owns everything below; the only thing added to the outer layout
	QLabel *nameLabel = nullptr; // caption under the button, like the source's name under a native control
	QPushButton *recordButton = nullptr;
	// 0 Inactive (parent source isn't actively capturing anything, e.g. a
	// Window Capture with no window selected), 1 Stopped (capturing fine,
	// record_mode off), 2 Recording, 3 Error (record_mode was on and the
	// output stopped with a failure). Last known value, so the button only
	// needs restyling on an actual change.
	int status = 0;
};

// A second, general-purpose control panel dock alongside ReplayBufferDock --
// same "one row per source_record_filter" discovery, but for controls other
// than the replay-buffer/save-length workflow that dock already owns. Starts
// with per-filter Record start/stop; the row/button plumbing here is meant to
// grow more controls over time without needing a new dock per idea.
//
// Styled to read as an extension of OBS's own Controls dock rather than a
// distinct plugin UI: full-width buttons, "active" shown by highlighting the
// button itself (checked state, using the current Qt theme's own highlight
// color) instead of a separate status dot, source name captioned underneath.
class ControlPanelDock : public QFrame {
	Q_OBJECT

public:
	explicit ControlPanelDock(QWidget *parent = nullptr);
	~ControlPanelDock() override;

	QSize sizeHint() const override { return QSize(240, 160); }

public slots:
	// Called (only) via QMetaObject::invokeMethod(..., Qt::BlockingQueuedConnection)
	// from WebsocketBridge, which runs on obs-websocket's own thread -- same
	// pattern as ReplayBufferDock's BuildRowsJson/SaveRowByKey. Lets an
	// external tool list these rows and start/stop one specific filter's
	// recording, same as clicking its button here would.
	QString BuildRowsJson();
	bool StartRowByKey(QString key);
	bool StopRowByKey(QString key);

private slots:
	void RefreshAll();

private:
	int AddRow(const QString &key, const QString &label, obs_weak_source_t *weak);
	void RebuildRows(const QMap<QString, obs_weak_source_t *> &discoveredFilters);
	int FindRowIndex(const QString &key) const;

	void UpdateRow(ControlRow &row);
	void ToggleRecord(obs_weak_source_t *filterWeak, bool start);

	void ApplyButtonState(QPushButton *button, int status);

	QVBoxLayout *rowsLayout = nullptr;
	QTimer *refreshTimer = nullptr;
	QVector<ControlRow> rows;
};
