/* @@@LICENSE
*
* Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* LICENSE@@@ */

#include <string.h>

#include "connman_vpn_client.h"
#include "connman-vpn-interface.h"
#include "gvariant_json.h"
#include "vpn_agent.h"

#define CONNMAN_VPN_SERVICE		"net.connman.vpn"
#define CONNMAN_VPN_MANAGER_PATH	"/"

/*
 * Connect()/Disconnect() are documented as blocking until the tunnel is up or
 * fails, which can include waiting on our own agent's RequestInput reply -
 * i.e. on a human. GDBusProxy's built-in default (25s,
 * G_DBUS_PROXY_DEFAULT_TIMEOUT) would spuriously fail a slow credential entry.
 * G_MAXINT means "no timeout" for g_dbus_proxy_set_default_timeout().
 */
#define CONNECTION_PROXY_TIMEOUT_MS	G_MAXINT

struct connman_vpn_client {
	guint watch_id;
	GDBusConnection *bus;		/* owned once acquired; independent of the
	                             * watched name's presence */
	struct vpn_agent *agent;	/* likewise independent - exported as soon as
	                             * we have a bus, whether or not connman-vpnd
	                             * is running yet */
	VpnInterfaceManager *manager;
	GCancellable *cancellable;

	/* struct vpn_connection*, keyed by object path for lookup */
	GHashTable *connections;

	vpn_client_available_cb available_cb;
	vpn_client_connections_cb connections_cb;
	vpn_client_connection_cb connection_cb;
	vpn_client_prompt_cb prompt_cb;
	vpn_client_notice_cb notice_cb;
	vpn_client_prompt_cancel_cb prompt_cancel_cb;
	gpointer user_data;
};

struct vpn_connection {
	char *path;
	GVariant *properties;		/* a{sv}, owned */
	VpnInterfaceConnection *proxy;
	gulong property_changed_id;
	struct connman_vpn_client *client;
};

static void connection_free(gpointer data)
{
	struct vpn_connection *connection = data;

	if (!connection)
		return;

	if (connection->proxy) {
		if (connection->property_changed_id)
			g_signal_handler_disconnect(connection->proxy,
			                            connection->property_changed_id);
		g_object_unref(connection->proxy);
	}

	if (connection->properties)
		g_variant_unref(connection->properties);

	g_free(connection->path);
	g_free(connection);
}

/*
 * connman sends PropertyChanged one property at a time. Rebuild the cached dict
 * with that single key replaced rather than re-fetching the lot: State changes
 * several times during a connect and a round trip per change is wasteful.
 */
static void on_property_changed(VpnInterfaceConnection *proxy, const gchar *name,
                                GVariant *value, gpointer user_data)
{
	struct vpn_connection *connection = user_data;
	GVariantDict dict;
	GVariant *unwrapped;
	GVariant *updated;

	if (!connection || !name || !value)
		return;

	/* Signal args of D-Bus type "v" arrive still wrapped. Storing the wrapper
	 * in an a{sv} would nest a variant inside a variant. */
	if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT))
		unwrapped = g_variant_get_variant(value);
	else
		unwrapped = g_variant_ref(value);

	g_variant_dict_init(&dict, connection->properties);
	g_variant_dict_insert_value(&dict, name, unwrapped);

	/* Build the replacement before dropping the original. g_variant_dict_init
	 * copies the entries out rather than holding a reference to the dict it was
	 * seeded from, so releasing first would happen to work - but only by relying
	 * on that internal detail. */
	updated = g_variant_ref_sink(g_variant_dict_end(&dict));

	g_variant_unref(connection->properties);
	connection->properties = updated;

	g_variant_unref(unwrapped);

	if (connection->client->connection_cb)
		connection->client->connection_cb(connection, connection->client->user_data);
}

static struct vpn_connection *connection_new(struct connman_vpn_client *client,
                                             const char *path, GVariant *properties)
{
	struct vpn_connection *connection;
	GError *error = NULL;

	connection = g_new0(struct vpn_connection, 1);
	connection->client = client;
	connection->path = g_strdup(path);
	connection->properties = g_variant_ref_sink(properties);

	connection->proxy = vpn_interface_connection_proxy_new_sync(client->bus,
	                        G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
	                        CONNMAN_VPN_SERVICE, path, NULL, &error);

	if (!connection->proxy) {
		g_warning("Failed to create a proxy for VPN connection %s: %s", path,
		          error ? error->message : "unknown error");
		g_clear_error(&error);
		/* Keep the cached properties: the connection is still listable, just
		 * not controllable, and reporting it is better than hiding it. */
		return connection;
	}

	g_dbus_proxy_set_default_timeout(G_DBUS_PROXY(connection->proxy),
	                                 CONNECTION_PROXY_TIMEOUT_MS);

	connection->property_changed_id = g_signal_connect(connection->proxy,
	                                      "property-changed",
	                                      G_CALLBACK(on_property_changed), connection);

	return connection;
}

static void add_connection(struct connman_vpn_client *client, const char *path,
                           GVariant *properties)
{
	struct vpn_connection *connection;

	if (!path || !properties)
		return;

	/* connman re-announces a provisioned connection when its .config file is
	 * rewritten; replace rather than duplicate. */
	g_hash_table_remove(client->connections, path);

	connection = connection_new(client, path, properties);
	g_hash_table_insert(client->connections, connection->path, connection);
}

static void on_connection_added(VpnInterfaceManager *manager, const gchar *path,
                                GVariant *properties, gpointer user_data)
{
	struct connman_vpn_client *client = user_data;

	add_connection(client, path, properties);

	if (client->connections_cb)
		client->connections_cb(client->user_data);
}

static void on_connection_removed(VpnInterfaceManager *manager, const gchar *path,
                                  gpointer user_data)
{
	struct connman_vpn_client *client = user_data;

	if (!g_hash_table_remove(client->connections, path))
		return;

	if (client->connections_cb)
		client->connections_cb(client->user_data);
}

static void get_connections_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	struct connman_vpn_client *client = user_data;
	GVariant *connections = NULL;
	GError *error = NULL;
	GVariantIter iter;
	const gchar *path;
	GVariant *properties;

	if (!vpn_interface_manager_call_get_connections_finish(VPN_INTERFACE_MANAGER(source),
	                                                       &connections, result, &error)) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_warning("Failed to list VPN connections: %s",
			          error ? error->message : "unknown error");
		g_clear_error(&error);
		return;
	}

	g_variant_iter_init(&iter, connections);

	while (g_variant_iter_next(&iter, "(&o@a{sv})", &path, &properties)) {
		add_connection(client, path, properties);
		g_variant_unref(properties);
	}

	g_variant_unref(connections);

	if (client->connections_cb)
		client->connections_cb(client->user_data);
}

static void on_register_agent_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	GError *error = NULL;

	if (!vpn_interface_manager_call_register_agent_finish(VPN_INTERFACE_MANAGER(source),
	                                                       result, &error)) {
		g_warning("Failed to register the VPN agent with connman-vpnd: %s",
		          error ? error->message : "unknown error");
		g_clear_error(&error);
		return;
	}

	g_message("VPN agent registered with connman-vpnd");
}

/* struct connman_vpn_client forward-declared to vpn_agent's callback type. */
static void on_agent_request_input(const char *service_path, GVariant *fields,
                                   gpointer user_data)
{
	struct connman_vpn_client *client = user_data;

	if (client->prompt_cb)
		client->prompt_cb(service_path, fields, client->user_data);
}

static void on_agent_report_error(const char *service_path, const char *error,
                                  gpointer user_data)
{
	struct connman_vpn_client *client = user_data;

	if (client->notice_cb)
		client->notice_cb(service_path, error, client->user_data);
}

static void on_agent_cancel(const char *service_path, gpointer user_data)
{
	struct connman_vpn_client *client = user_data;

	if (client->prompt_cancel_cb)
		client->prompt_cancel_cb(service_path, client->user_data);
}

static void on_name_appeared(GDBusConnection *bus, const gchar *name,
                             const gchar *name_owner, gpointer user_data)
{
	struct connman_vpn_client *client = user_data;
	GError *error = NULL;

	g_message("connman-vpnd appeared on the bus");

	client->manager = vpn_interface_manager_proxy_new_sync(client->bus,
	                      G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
	                      CONNMAN_VPN_SERVICE, CONNMAN_VPN_MANAGER_PATH, NULL, &error);

	if (!client->manager) {
		g_warning("Failed to create the connman-vpn manager proxy: %s",
		          error ? error->message : "unknown error");
		g_clear_error(&error);
		return;
	}

	g_signal_connect(client->manager, "connection-added",
	                 G_CALLBACK(on_connection_added), client);
	g_signal_connect(client->manager, "connection-removed",
	                 G_CALLBACK(on_connection_removed), client);

	vpn_interface_manager_call_get_connections(client->manager, client->cancellable,
	                                           get_connections_done, client);

	/* Re-registered on every appearance, including a connman-vpnd restart:
	 * RegisterAgent's effect does not survive the daemon losing its bus name,
	 * but our own skeleton stays exported at the same path throughout. */
	if (client->agent)
		vpn_interface_manager_call_register_agent(client->manager,
		        vpn_agent_get_object_path(client->agent), client->cancellable,
		        on_register_agent_done, client);

	if (client->available_cb)
		client->available_cb(TRUE, client->user_data);
}

static void on_name_vanished(GDBusConnection *bus, const gchar *name, gpointer user_data)
{
	struct connman_vpn_client *client = user_data;

	g_message("connman-vpnd left the bus");

	g_hash_table_remove_all(client->connections);

	g_clear_object(&client->manager);

	/* Nothing will ever call Cancel() for a prompt raised by a connman-vpnd
	 * that has now gone away; withdraw it ourselves so the UI does not sit on
	 * a dialog forever. */
	if (client->agent)
		vpn_agent_cancel_all(client->agent);

	if (client->available_cb)
		client->available_cb(FALSE, client->user_data);

	if (client->connections_cb)
		client->connections_cb(client->user_data);
}

struct connman_vpn_client *connman_vpn_client_create(vpn_client_available_cb available_cb,
                                                     vpn_client_connections_cb connections_cb,
                                                     vpn_client_connection_cb connection_cb,
                                                     vpn_client_prompt_cb prompt_cb,
                                                     vpn_client_notice_cb notice_cb,
                                                     vpn_client_prompt_cancel_cb prompt_cancel_cb,
                                                     gpointer user_data)
{
	struct connman_vpn_client *client = g_new0(struct connman_vpn_client, 1);
	GError *error = NULL;

	client->available_cb = available_cb;
	client->connections_cb = connections_cb;
	client->connection_cb = connection_cb;
	client->prompt_cb = prompt_cb;
	client->notice_cb = notice_cb;
	client->prompt_cancel_cb = prompt_cancel_cb;
	client->user_data = user_data;
	client->cancellable = g_cancellable_new();

	/* The path is the key and is owned by the connection, so only the value
	 * gets a destroy function. */
	client->connections = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
	                                            connection_free);

	/*
	 * Acquired eagerly and independent of connman-vpnd's presence: our own
	 * net.connman.vpn.Agent is a stable part of this service, not something
	 * that comes and goes with the backend. Exporting it up front means
	 * RegisterAgent can fire the instant connman-vpnd (re)appears rather than
	 * racing a lazily-created skeleton against that first appearance.
	 */
	client->bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
	if (!client->bus) {
		g_critical("Failed to connect to the system bus: %s",
		           error ? error->message : "unknown error");
		g_clear_error(&error);
	} else {
		client->agent = vpn_agent_create(client->bus, on_agent_request_input,
		                                 on_agent_report_error, on_agent_cancel,
		                                 client);
		if (!client->agent)
			g_critical("Failed to export the VPN agent; credential prompts "
			           "will not work");
	}

	/* Watch rather than connect: connman-vpnd is dbus-activated and may not be
	 * running yet, and it must be allowed to restart under us. */
	client->watch_id = g_bus_watch_name(G_BUS_TYPE_SYSTEM, CONNMAN_VPN_SERVICE,
	                                    G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
	                                    on_name_appeared, on_name_vanished, client, NULL);

	return client;
}

void connman_vpn_client_free(struct connman_vpn_client *client)
{
	if (!client)
		return;

	if (client->cancellable) {
		g_cancellable_cancel(client->cancellable);
		g_object_unref(client->cancellable);
	}

	if (client->watch_id)
		g_bus_unwatch_name(client->watch_id);

	if (client->connections)
		g_hash_table_destroy(client->connections);

	if (client->agent)
		vpn_agent_free(client->agent);

	g_clear_object(&client->manager);
	g_clear_object(&client->bus);

	g_free(client);
}

gboolean connman_vpn_client_is_available(const struct connman_vpn_client *client)
{
	return client && client->manager != NULL;
}

GList *connman_vpn_client_get_connections(struct connman_vpn_client *client)
{
	if (!client)
		return NULL;

	return g_hash_table_get_values(client->connections);
}

struct vpn_connection *connman_vpn_client_find_by_path(struct connman_vpn_client *client,
                                                       const char *path)
{
	if (!client || !path)
		return NULL;

	return g_hash_table_lookup(client->connections, path);
}

struct vpn_connection *connman_vpn_client_find_by_name(struct connman_vpn_client *client,
                                                       const char *name)
{
	GHashTableIter iter;
	gpointer value;

	if (!client || !name)
		return NULL;

	g_hash_table_iter_init(&iter, client->connections);

	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		struct vpn_connection *connection = value;
		const char *connection_name = vpn_connection_get_string(connection, "Name");

		if (connection_name && g_strcmp0(connection_name, name) == 0)
			return connection;
	}

	return NULL;
}

const char *vpn_connection_get_path(struct vpn_connection *connection)
{
	return connection ? connection->path : NULL;
}

GVariant *vpn_connection_get_properties(struct vpn_connection *connection)
{
	return connection ? connection->properties : NULL;
}

VpnInterfaceConnection *vpn_connection_get_proxy(struct vpn_connection *connection)
{
	return connection ? connection->proxy : NULL;
}

const char *vpn_connection_get_string(struct vpn_connection *connection, const char *key)
{
	const char *value = NULL;

	if (!connection)
		return NULL;

	if (!gvariant_dict_lookup_string(connection->properties, key, &value))
		return NULL;

	return value;
}

gboolean vpn_connection_get_boolean(struct vpn_connection *connection, const char *key,
                                    gboolean *out)
{
	if (!connection)
		return FALSE;

	return gvariant_dict_lookup_boolean(connection->properties, key, out);
}

const char *vpn_connection_get_connect_state(struct vpn_connection *connection)
{
	const char *state = vpn_connection_get_string(connection, "State");

	if (!state)
		return "unknown";

	if (g_strcmp0(state, "ready") == 0)
		return "connected";

	if (g_strcmp0(state, "configuration") == 0)
		return "connecting";

	if (g_strcmp0(state, "disconnect") == 0)
		return "disconnecting";

	/* "failure" is reported as disconnected; the reason reaches the UI as a
	 * notice on getStatus rather than as a state of its own, which is what the
	 * legacy service did too. */
	if (g_strcmp0(state, "idle") == 0 || g_strcmp0(state, "failure") == 0)
		return "disconnected";

	return "unknown";
}

/* ------------------------------------------------------------------ *
 * Write operations
 * ------------------------------------------------------------------ */

/*
 * error is left exactly as GDBus/GError reported it: stripped of the
 * "GDBus.Error:name: " prefix when it came back as a remote D-Bus error (in
 * which case dbus_error_name_out is also filled in), or the local GLib error
 * text otherwise. Callers own the returned dbus_error_name_out string.
 */
static void extract_dbus_error(GError *error, char **dbus_error_name_out,
                               const char **message_out)
{
	*dbus_error_name_out = NULL;

	if (error && g_dbus_error_is_remote_error(error)) {
		*dbus_error_name_out = g_dbus_error_get_remote_error(error);
		g_dbus_error_strip_remote_error(error);
	}

	*message_out = error ? error->message : "unknown error";
}

struct create_request {
	vpn_client_create_cb callback;
	gpointer user_data;
};

static void on_create_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	struct create_request *req = user_data;
	gchar *path = NULL;
	GError *error = NULL;

	if (!vpn_interface_manager_call_create_finish(VPN_INTERFACE_MANAGER(source), &path,
	                                              result, &error)) {
		char *dbus_error_name;
		const char *message;

		extract_dbus_error(error, &dbus_error_name, &message);
		req->callback(FALSE, NULL, dbus_error_name, message, req->user_data);
		g_free(dbus_error_name);
		g_clear_error(&error);
	} else {
		req->callback(TRUE, path, NULL, NULL, req->user_data);
		g_free(path);
	}

	g_free(req);
}

void connman_vpn_client_create_connection(struct connman_vpn_client *client,
                                          GVariant *properties,
                                          vpn_client_create_cb callback, gpointer user_data)
{
	struct create_request *req;

	if (!callback)
		return;

	if (!client || !client->manager) {
		callback(FALSE, NULL, NULL, "connman-vpnd is not available", user_data);
		return;
	}

	req = g_new0(struct create_request, 1);
	req->callback = callback;
	req->user_data = user_data;

	vpn_interface_manager_call_create(client->manager, properties, client->cancellable,
	                                  on_create_done, req);
}

struct simple_request {
	vpn_client_simple_cb callback;
	gpointer user_data;
};

/*
 * Reports the outcome of one of the four *_finish() calls below and frees
 * req. success/error come from the caller having already called the
 * correctly-typed *_finish() function itself - deliberately not a shared
 * wrapper taking a function pointer: the four *_finish() functions differ in
 * their first argument's type (VpnInterfaceManager* vs
 * VpnInterfaceConnection*), and calling through a function pointer cast to a
 * mismatched parameter type is undefined behaviour in C even when it happens
 * to work on common ABIs.
 */
static void simple_request_done(struct simple_request *req, gboolean success, GError *error)
{
	if (!success) {
		char *dbus_error_name;
		const char *message;

		extract_dbus_error(error, &dbus_error_name, &message);
		req->callback(FALSE, dbus_error_name, message, req->user_data);
		g_free(dbus_error_name);
		g_clear_error(&error);
	} else {
		req->callback(TRUE, NULL, NULL, req->user_data);
	}

	g_free(req);
}

static void on_remove_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	GError *error = NULL;
	gboolean success = vpn_interface_manager_call_remove_finish(
	                        VPN_INTERFACE_MANAGER(source), result, &error);

	simple_request_done(user_data, success, error);
}

void connman_vpn_client_remove_connection(struct connman_vpn_client *client, const char *path,
                                          vpn_client_simple_cb callback, gpointer user_data)
{
	struct simple_request *req;

	if (!callback)
		return;

	if (!client || !client->manager || !path) {
		callback(FALSE, NULL, "connman-vpnd is not available", user_data);
		return;
	}

	req = g_new0(struct simple_request, 1);
	req->callback = callback;
	req->user_data = user_data;

	vpn_interface_manager_call_remove(client->manager, path, client->cancellable,
	                                  on_remove_done, req);
}

static void on_set_property_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	GError *error = NULL;
	gboolean success = vpn_interface_connection_call_set_property_finish(
	                        VPN_INTERFACE_CONNECTION(source), result, &error);

	simple_request_done(user_data, success, error);
}

void connman_vpn_client_set_properties(struct connman_vpn_client *client,
                                       struct vpn_connection *connection,
                                       GVariant *properties,
                                       vpn_client_simple_cb callback, gpointer user_data)
{
	struct simple_request *req;
	GVariant *wrapped;

	if (!callback)
		return;

	if (!client || !connection || !connection->proxy) {
		callback(FALSE, NULL, "connman-vpnd is not available", user_data);
		return;
	}

	req = g_new0(struct simple_request, 1);
	req->callback = callback;
	req->user_data = user_data;

	/*
	 * SetProperty(name, value) takes value as D-Bus type 'v'; the "Properties"
	 * batch form additionally requires the variant's own content to be an
	 * a{sv} (connman's vpn-provider.c set_property() dispatches to the batch
	 * path only when the unwrapped type is DBUS_TYPE_ARRAY). properties is
	 * already a{sv} - one variant wrap gets it to the right shape.
	 */
	wrapped = g_variant_new_variant(properties);

	vpn_interface_connection_call_set_property(connection->proxy, "Properties", wrapped,
	                                           client->cancellable, on_set_property_done,
	                                           req);
}

/*
 * connman-vpnd's own net.connman.vpn.Connection.Connect()/Disconnect() look
 * like the obvious entry point, but calling them directly bypasses connman
 * core (connmand). Core's "vpn" plugin only learns which network service is
 * carrying the tunnel (connection_data->service_ident) as a side effect of
 * routing the connect through its own net.connman.Service.Connect() -
 * connect_provider() in plugins/vpn.c is what sets it, and that function
 * only runs on that path. With service_ident left NULL, core's transport
 * watchdog (vpn_disconnect_check_provider(), reacting to any service state
 * change) reads "no known transport" as "transport gone" and immediately
 * disconnects a tunnel it never finished admitting it should be tracking -
 * usually within the same second the tunnel comes up, well before the
 * OpenVPN management socket can even relay a credential prompt. Routing
 * through core's own Service object keeps its bookkeeping consistent and
 * this watchdog out of the way.
 */
static char *service_path_for_connection(struct vpn_connection *connection)
{
	const char *ident = connection->path ? strrchr(connection->path, '/') : NULL;

	if (!ident || !ident[1])
		return NULL;

	return g_strdup_printf("/net/connman/service/vpn_%s", ident + 1);
}

static void on_core_service_call_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	GError *error = NULL;
	GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);

	if (ret)
		g_variant_unref(ret);

	simple_request_done(user_data, ret != NULL, error);
}

static void core_service_call(struct connman_vpn_client *client, struct vpn_connection *connection,
                              const char *method, vpn_client_simple_cb callback, gpointer user_data)
{
	struct simple_request *req;
	char *service_path;

	if (!callback)
		return;

	if (!client || !client->bus || !connection) {
		callback(FALSE, NULL, "connman-vpnd is not available", user_data);
		return;
	}

	service_path = service_path_for_connection(connection);
	if (!service_path) {
		callback(FALSE, NULL, "invalid connection path", user_data);
		return;
	}

	req = g_new0(struct simple_request, 1);
	req->callback = callback;
	req->user_data = user_data;

	g_dbus_connection_call(client->bus, "net.connman", service_path,
	                       "net.connman.Service", method, NULL, NULL,
	                       G_DBUS_CALL_FLAGS_NONE, -1, client->cancellable,
	                       on_core_service_call_done, req);
	g_free(service_path);
}

void connman_vpn_client_connect(struct connman_vpn_client *client,
                                struct vpn_connection *connection,
                                vpn_client_simple_cb callback, gpointer user_data)
{
	core_service_call(client, connection, "Connect", callback, user_data);
}

void connman_vpn_client_disconnect(struct connman_vpn_client *client,
                                   struct vpn_connection *connection,
                                   vpn_client_simple_cb callback, gpointer user_data)
{
	core_service_call(client, connection, "Disconnect", callback, user_data);
}

void connman_vpn_client_complete_prompt(struct connman_vpn_client *client,
                                        const char *service_path, GVariant *result)
{
	if (!client || !client->agent)
		return;

	vpn_agent_complete_request_input(client->agent, service_path, result);
}

/* vim:ts=4:sw=4:noexpandtab */
