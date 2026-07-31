#include "websocket-bridge.hpp"
#include "replay-buffer-dock.hpp"
#include "obs-websocket-api.h"

#include <obs-module.h>
#include <QMetaObject>

namespace {

obs_websocket_vendor g_vendor = nullptr;
ReplayBufferDock *g_dock = nullptr;

// NOTE: these run on obs-websocket's own worker thread, never on the Qt/main
// thread -- that's what makes the Qt::BlockingQueuedConnection calls into the
// dock below safe (it deadlocks if the calling and target threads are the
// same, which never happens here).

void HandleListRows(obs_data_t *, obs_data_t *response_data, void *)
{
	QString json = QStringLiteral("{\"rows\":[]}");
	if (g_dock)
		QMetaObject::invokeMethod(g_dock, "BuildRowsJson", Qt::BlockingQueuedConnection, Q_RETURN_ARG(QString, json));

	// Round-trip through libobs's own JSON parser rather than building an
	// obs_data_array_t by hand across the Qt/plugin boundary. The dock hands
	// back a JSON *object* (`{"rows":[...]}`) because obs_data_create_from_json
	// only parses objects at the top level -- the array itself is nested fine.
	obs_data_t *parsed = obs_data_create_from_json(json.toUtf8().constData());
	if (parsed) {
		obs_data_array_t *rows = obs_data_get_array(parsed, "rows");
		obs_data_set_array(response_data, "rows", rows);
		if (rows)
			obs_data_array_release(rows);
		obs_data_release(parsed);
	}
	obs_data_set_bool(response_data, "success", true);
}

void HandleSaveRow(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	const char *key = request_data ? obs_data_get_string(request_data, "key") : nullptr;
	bool ok = false;
	if (g_dock && key && *key) {
		QMetaObject::invokeMethod(g_dock, "SaveRowByKey", Qt::BlockingQueuedConnection, Q_RETURN_ARG(bool, ok),
					  Q_ARG(QString, QString::fromUtf8(key)));
	}
	obs_data_set_bool(response_data, "success", ok);
	if (!ok)
		obs_data_set_string(response_data, "error", "unknown row key");
}

} // namespace

namespace WebsocketBridge {

void Register(ReplayBufferDock *dock)
{
	g_dock = dock;

	// ALWAYS CALL ONLY FROM obs_module_post_load(), per obs-websocket-api.h.
	g_vendor = obs_websocket_register_vendor("replay-buffer-slider");
	if (!g_vendor) {
		// obs-websocket isn't installed, or hasn't finished loading yet -- the
		// rest of this plugin works fine either way, this bridge is just dark.
		return;
	}

	obs_websocket_vendor_register_request(g_vendor, "list_rows", HandleListRows, nullptr);
	obs_websocket_vendor_register_request(g_vendor, "save_row", HandleSaveRow, nullptr);
}

void Unregister()
{
	if (g_vendor) {
		obs_websocket_vendor_unregister_request(g_vendor, "list_rows");
		obs_websocket_vendor_unregister_request(g_vendor, "save_row");
	}
	g_vendor = nullptr;
	g_dock = nullptr;
}

void EmitRowSaved(const QString &rowKey, const QString &path)
{
	if (!g_vendor)
		return;
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "key", rowKey.toUtf8().constData());
	obs_data_set_string(data, "path", path.toUtf8().constData());
	obs_websocket_vendor_emit_event(g_vendor, "row_saved", data);
	obs_data_release(data);
}

} // namespace WebsocketBridge
