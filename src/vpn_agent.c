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

#include "vpn_agent.h"
#include "connman-vpn-agent-interface.h"

#define VPN_AGENT_OBJECT_PATH	"/VpnAgent"
#define VPN_AGENT_CANCELED_ERROR	"net.connman.vpn.Agent.Error.Canceled"

struct vpn_agent {
	GDBusConnection *bus;		/* borrowed */
	VpnInterfaceAgent *skeleton;

	/* service object path -> GDBusMethodInvocation*, reffed for us. There is
	 * at most one entry in practice (connman serialises RequestInput calls
	 * per agent), but keying by service path rather than assuming a single
	 * global slot costs nothing and does not rely on that being permanent. */
	GHashTable *pending_inputs;

	vpn_agent_request_input_cb request_input_cb;
	vpn_agent_report_error_cb report_error_cb;
	vpn_agent_cancel_cb cancel_cb;
	gpointer user_data;
};

/*
 * Destroy-notify for pending_inputs: only ever runs on vpn_agent_free() for a
 * RequestInput call nobody completed (the peer bus connection is about to go
 * away regardless, but replying is cheap and correct). Every ordinary
 * completion path removes its entry with g_hash_table_steal() first, so this
 * never double-completes an invocation.
 */
static void pending_input_destroy(gpointer data)
{
	GDBusMethodInvocation *invocation = data;

	g_dbus_method_invocation_return_dbus_error(invocation,
	        VPN_AGENT_CANCELED_ERROR, "Agent shutting down");
}

static gboolean on_handle_release(VpnInterfaceAgent *object,
                                  GDBusMethodInvocation *invocation,
                                  gpointer user_data)
{
	g_message("VPN agent released by connman-vpnd");

	vpn_interface_agent_complete_release(object, invocation);

	return TRUE;
}

static gboolean on_handle_report_error(VpnInterfaceAgent *object,
                                       GDBusMethodInvocation *invocation,
                                       const gchar *arg_service,
                                       const gchar *arg_error,
                                       gpointer user_data)
{
	struct vpn_agent *agent = user_data;

	if (agent->report_error_cb)
		agent->report_error_cb(arg_service, arg_error, agent->user_data);

	/*
	 * net.connman.vpn.Agent.Error.Retry is documented as a way to ask connman
	 * to retry the failed transaction. Not implemented: every ReportError is
	 * acknowledged plainly, which is the same as always declining the retry.
	 */
	vpn_interface_agent_complete_report_error(object, invocation);

	return TRUE;
}

static gboolean on_handle_request_input(VpnInterfaceAgent *object,
                                        GDBusMethodInvocation *invocation,
                                        const gchar *arg_service,
                                        GVariant *arg_fields,
                                        gpointer user_data)
{
	struct vpn_agent *agent = user_data;

	if (!arg_service) {
		g_dbus_method_invocation_return_dbus_error(invocation,
		        "net.connman.vpn.Agent.Error.Canceled", "No service path");
		return TRUE;
	}

	/* Kept alive past this call; vpn_agent_complete_request_input() owns the
	 * matching unref via whichever completion path it takes. */
	g_hash_table_replace(agent->pending_inputs, g_strdup(arg_service),
	                     g_object_ref(invocation));

	if (agent->request_input_cb)
		agent->request_input_cb(arg_service, arg_fields, agent->user_data);

	return TRUE;
}

/*
 * Shared by on_handle_cancel() (connman-vpnd asked us to cancel) and
 * vpn_agent_cancel_all() (connman-vpnd went away, so it never will).
 *
 * g_hash_table_iter_steal(), not _remove(): pending_inputs' value destroy
 * function (pending_input_destroy) also completes the invocation, with a
 * Canceled error - the same reply this loop already sends by hand. Using
 * _remove() here would fire that destroy function too and send a second
 * reply to a GDBusMethodInvocation already completed by the line above, which
 * is a use-after-complete. _steal() removes the entry without invoking it.
 *
 * cancel_cb must not call back into this module (in particular
 * vpn_agent_complete_request_input(), which touches the same table) - this
 * loop is still iterating it via `iter`.
 */
static void cancel_all_pending(struct vpn_agent *agent, const char *reason)
{
	GHashTableIter iter;
	gpointer key, value;

	g_hash_table_iter_init(&iter, agent->pending_inputs);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		char *service_path = key;
		GDBusMethodInvocation *pending = value;

		g_dbus_method_invocation_return_dbus_error(pending,
		        VPN_AGENT_CANCELED_ERROR, reason);

		if (agent->cancel_cb)
			agent->cancel_cb(service_path, agent->user_data);

		g_hash_table_iter_steal(&iter);
		g_free(service_path);
	}
}

static gboolean on_handle_cancel(VpnInterfaceAgent *object,
                                 GDBusMethodInvocation *invocation,
                                 gpointer user_data)
{
	struct vpn_agent *agent = user_data;

	/*
	 * No argument identifies which call to cancel because connman only ever
	 * has one RequestInput outstanding against a given agent (see the header
	 * comment on vpn_agent_cancel_cb). Cancel whatever is pending; in the
	 * expected case that is zero or one entries.
	 */
	cancel_all_pending(agent, "Cancelled by connman-vpnd");

	vpn_interface_agent_complete_cancel(object, invocation);

	return TRUE;
}

void vpn_agent_cancel_all(struct vpn_agent *agent)
{
	if (!agent)
		return;

	cancel_all_pending(agent, "connman-vpnd is no longer available");
}

struct vpn_agent *vpn_agent_create(GDBusConnection *bus,
                                   vpn_agent_request_input_cb request_input_cb,
                                   vpn_agent_report_error_cb report_error_cb,
                                   vpn_agent_cancel_cb cancel_cb,
                                   gpointer user_data)
{
	struct vpn_agent *agent;
	GError *error = NULL;

	agent = g_new0(struct vpn_agent, 1);
	agent->bus = bus;
	agent->request_input_cb = request_input_cb;
	agent->report_error_cb = report_error_cb;
	agent->cancel_cb = cancel_cb;
	agent->user_data = user_data;
	agent->pending_inputs = g_hash_table_new_full(g_str_hash, g_str_equal,
	                            g_free, pending_input_destroy);

	agent->skeleton = vpn_interface_agent_skeleton_new();

	g_signal_connect(agent->skeleton, "handle-release",
	                 G_CALLBACK(on_handle_release), agent);
	g_signal_connect(agent->skeleton, "handle-report-error",
	                 G_CALLBACK(on_handle_report_error), agent);
	g_signal_connect(agent->skeleton, "handle-request-input",
	                 G_CALLBACK(on_handle_request_input), agent);
	g_signal_connect(agent->skeleton, "handle-cancel",
	                 G_CALLBACK(on_handle_cancel), agent);

	if (!g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(agent->skeleton),
	                                      bus, VPN_AGENT_OBJECT_PATH, &error)) {
		g_critical("Failed to export the VPN agent at %s: %s",
		           VPN_AGENT_OBJECT_PATH,
		           error ? error->message : "unknown error");
		g_clear_error(&error);
		g_object_unref(agent->skeleton);
		g_hash_table_destroy(agent->pending_inputs);
		g_free(agent);
		return NULL;
	}

	return agent;
}

void vpn_agent_free(struct vpn_agent *agent)
{
	if (!agent)
		return;

	if (agent->skeleton) {
		g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(agent->skeleton));
		g_object_unref(agent->skeleton);
	}

	/* Destroy-notify replies Canceled to whatever is still outstanding. */
	if (agent->pending_inputs)
		g_hash_table_destroy(agent->pending_inputs);

	g_free(agent);
}

const char *vpn_agent_get_object_path(struct vpn_agent *agent)
{
	return agent ? VPN_AGENT_OBJECT_PATH : NULL;
}

void vpn_agent_complete_request_input(struct vpn_agent *agent,
                                      const char *service_path, GVariant *result)
{
	gpointer stolen_key = NULL;
	gpointer stolen_value = NULL;
	GDBusMethodInvocation *invocation;

	if (!agent || !service_path)
		return;

	/*
	 * steal_extended, not lookup+remove: as in on_handle_cancel() above,
	 * removing through the ordinary path would run pending_input_destroy()
	 * and complete this same invocation a second time. Stealing hands back
	 * the exact key/value pointers that were stored (the g_strdup'd path and
	 * the ref taken in on_handle_request_input()) without invoking it.
	 */
	if (!g_hash_table_steal_extended(agent->pending_inputs, service_path,
	                                 &stolen_key, &stolen_value))
		return;

	g_free(stolen_key);
	invocation = stolen_value;

	if (result)
		vpn_interface_agent_complete_request_input(agent->skeleton, invocation,
		                                           result);
	else
		g_dbus_method_invocation_return_dbus_error(invocation,
		        VPN_AGENT_CANCELED_ERROR, "Cancelled");
}

/* vim:ts=4:sw=4:noexpandtab */
