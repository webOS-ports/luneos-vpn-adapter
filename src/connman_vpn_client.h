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

#ifndef CONNMAN_VPN_CLIENT_H_
#define CONNMAN_VPN_CLIENT_H_

#include <glib.h>
#include <gio/gio.h>

struct connman_vpn_client;
struct vpn_connection;

/* connman-vpnd came or went on the system bus. */
typedef void (*vpn_client_available_cb)(gboolean available, gpointer user_data);
/* A connection was added or removed - the profile list changed. */
typedef void (*vpn_client_connections_cb)(gpointer user_data);
/* One connection's properties changed (State, IPv4, ...). */
typedef void (*vpn_client_connection_cb)(struct vpn_connection *connection,
                                         gpointer user_data);

/*
 * connman-vpnd is asking for credentials for the connection at service_path,
 * via our exported net.connman.vpn.Agent. fields is the a{sv} dict from
 * doc/vpn-agent-api.txt (borrowed - valid only for the duration of the call).
 * Answer with connman_vpn_client_complete_prompt().
 */
typedef void (*vpn_client_prompt_cb)(const char *service_path, GVariant *fields,
                                     gpointer user_data);
/* A notice to show the user (Agent.ReportError); no reply expected. */
typedef void (*vpn_client_notice_cb)(const char *service_path, const char *error,
                                     gpointer user_data);
/* connman-vpnd withdrew the prompt at service_path (Agent.Cancel) before it was
 * answered; drop any local bookkeeping/UI for it. */
typedef void (*vpn_client_prompt_cancel_cb)(const char *service_path,
                                            gpointer user_data);

struct connman_vpn_client *connman_vpn_client_create(vpn_client_available_cb available_cb,
                                                     vpn_client_connections_cb connections_cb,
                                                     vpn_client_connection_cb connection_cb,
                                                     vpn_client_prompt_cb prompt_cb,
                                                     vpn_client_notice_cb notice_cb,
                                                     vpn_client_prompt_cancel_cb prompt_cancel_cb,
                                                     gpointer user_data);
void connman_vpn_client_free(struct connman_vpn_client *client);

gboolean connman_vpn_client_is_available(const struct connman_vpn_client *client);

/* Borrowed list of struct vpn_connection*, owned by the client. Do not free. */
GList *connman_vpn_client_get_connections(struct connman_vpn_client *client);

struct vpn_connection *connman_vpn_client_find_by_name(struct connman_vpn_client *client,
                                                       const char *name);
struct vpn_connection *connman_vpn_client_find_by_path(struct connman_vpn_client *client,
                                                       const char *path);

const char *vpn_connection_get_path(struct vpn_connection *connection);
/* Borrowed a{sv} of every property connman reports for this connection. */
GVariant *vpn_connection_get_properties(struct vpn_connection *connection);

/* Borrowed; valid until the connection's properties next change. */
const char *vpn_connection_get_string(struct vpn_connection *connection, const char *key);
gboolean vpn_connection_get_boolean(struct vpn_connection *connection, const char *key,
                                    gboolean *out);

/*
 * connman's State, mapped onto the legacy vpnProfileConnectState vocabulary the
 * UI speaks: idle->disconnected, configuration->connecting, ready->connected,
 * disconnect->disconnecting, failure->disconnected. Never returns NULL.
 */
const char *vpn_connection_get_connect_state(struct vpn_connection *connection);

/*
 * Write operations. All are asynchronous; the callback fires exactly once,
 * either synchronously (backend unavailable / bad connection handle) or later
 * off the GDBus completion. success is FALSE on failure, in which case
 * dbus_error_name is the D-Bus error name (e.g.
 * "net.connman.vpn.Agent.Error.Canceled", "net.connman.Error.AlreadyExists")
 * when the failure came back as a remote D-Bus error, or NULL for a local/
 * transport failure - callers map this to their own error vocabulary.
 * error_message is always non-NULL on failure and is never used verbatim as
 * an LS2 error string (it can be very D-Bus-specific).
 */
typedef void (*vpn_client_create_cb)(gboolean success, const char *path,
                                     const char *dbus_error_name,
                                     const char *error_message, gpointer user_data);
void connman_vpn_client_create_connection(struct connman_vpn_client *client,
                                          GVariant *properties,
                                          vpn_client_create_cb callback, gpointer user_data);

typedef void (*vpn_client_simple_cb)(gboolean success, const char *dbus_error_name,
                                     const char *error_message, gpointer user_data);
void connman_vpn_client_remove_connection(struct connman_vpn_client *client,
                                          const char *path,
                                          vpn_client_simple_cb callback, gpointer user_data);

/* properties is an a{sv} (not pre-wrapped in a variant); sets every entry via
 * connman's SetProperty("Properties", ...) batch path in one round trip. */
void connman_vpn_client_set_properties(struct connman_vpn_client *client,
                                       struct vpn_connection *connection,
                                       GVariant *properties,
                                       vpn_client_simple_cb callback, gpointer user_data);

/*
 * Connect()/Disconnect() have no bounded timeout on the wire - Connect() in
 * particular blocks connman-vpnd until the tunnel is up, which can include an
 * arbitrarily long wait on our own agent's RequestInput reply. The connection
 * proxy's default GDBusProxy timeout is set to unbounded to match; callers
 * that want a user-facing time limit (this service does, via the -7/promptId
 * early-reply path) must impose it themselves.
 */
void connman_vpn_client_connect(struct connman_vpn_client *client,
                                struct vpn_connection *connection,
                                vpn_client_simple_cb callback, gpointer user_data);
void connman_vpn_client_disconnect(struct connman_vpn_client *client,
                                   struct vpn_connection *connection,
                                   vpn_client_simple_cb callback, gpointer user_data);

/*
 * Completes the RequestInput prompt for service_path with result (an a{sv}
 * reply dict, floating reference, consumed), or - if result is NULL - replies
 * net.connman.vpn.Agent.Error.Canceled. A service_path with no outstanding
 * prompt is a harmless no-op.
 */
void connman_vpn_client_complete_prompt(struct connman_vpn_client *client,
                                        const char *service_path, GVariant *result);

#endif

/* vim:ts=4:sw=4:noexpandtab */
