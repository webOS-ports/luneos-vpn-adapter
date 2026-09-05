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

#ifndef VPN_AGENT_H_
#define VPN_AGENT_H_

#include <glib.h>
#include <gio/gio.h>

struct vpn_agent;

/*
 * connman-vpnd is asking for credentials for the connection at service_path.
 * fields is the a{sv} dict documented in doc/vpn-agent-api.txt (borrowed -
 * valid only for the duration of the call). The reply, when the caller has
 * one, comes back later through vpn_agent_complete_request_input() keyed by
 * the same service_path - connman serialises requests per agent (see
 * files/xml/connman-vpn-agent.xml), so at most one of these is outstanding at
 * a time.
 */
typedef void (*vpn_agent_request_input_cb)(const char *service_path,
                                           GVariant *fields, gpointer user_data);

/* A notice to show the user; no reply expected. */
typedef void (*vpn_agent_report_error_cb)(const char *service_path,
                                          const char *error, gpointer user_data);

/*
 * connman-vpnd is withdrawing whichever RequestInput call is currently
 * outstanding (there is at most one). The agent has already replied
 * net.connman.vpn.Agent.Error.Canceled on connman's behalf by the time this
 * fires; the callback only needs to clean up local state (close a UI prompt,
 * drop bookkeeping keyed by that service path).
 */
typedef void (*vpn_agent_cancel_cb)(const char *service_path, gpointer user_data);

/*
 * Exports net.connman.vpn.Agent on bus (borrowed - must outlive the agent) and
 * returns its object path via vpn_agent_get_object_path(), ready to hand to
 * net.connman.vpn.Manager.RegisterAgent(). Returns NULL if the skeleton could
 * not be exported.
 */
struct vpn_agent *vpn_agent_create(GDBusConnection *bus,
                                   vpn_agent_request_input_cb request_input_cb,
                                   vpn_agent_report_error_cb report_error_cb,
                                   vpn_agent_cancel_cb cancel_cb,
                                   gpointer user_data);
void vpn_agent_free(struct vpn_agent *agent);

const char *vpn_agent_get_object_path(struct vpn_agent *agent);

/*
 * Completes the RequestInput call for service_path. result is the a{sv} reply
 * dict (floating reference; consumed) on success, or NULL to reply
 * net.connman.vpn.Agent.Error.Canceled (the user dismissed the prompt, or it
 * was answered too late). A service_path with no matching pending call is a
 * harmless no-op - the request may already have been withdrawn by
 * vpn_agent_cancel_cb.
 */
void vpn_agent_complete_request_input(struct vpn_agent *agent,
                                      const char *service_path, GVariant *result);

/*
 * Withdraws every currently outstanding RequestInput prompt: each one is
 * completed net.connman.vpn.Agent.Error.Canceled and reported through
 * cancel_cb, exactly as if connman-vpnd itself had called Cancel() on it. For
 * use when connman-vpnd goes away and will never call Cancel() for prompts it
 * raised - without this a dialog raised before the backend vanished would
 * never close.
 */
void vpn_agent_cancel_all(struct vpn_agent *agent);

#endif

/* vim:ts=4:sw=4:noexpandtab */
