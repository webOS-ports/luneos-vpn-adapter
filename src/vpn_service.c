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

#include <stdio.h>
#include <string.h>
#include <net/if.h>

#include <glib.h>
#include <luna-service2/lunaservice.h>

#include "vpn_service.h"
#include "vpn_errors.h"
#include "vpn_providers.h"
#include "connman_vpn_client.h"
#include "luna_service_utils.h"
#include "gvariant_json.h"
#include "profile_build.h"

#define VPN_SERVICE_NAME	"com.webos.service.vpn"

extern GMainLoop *event_loop;

struct vpn_service {
	LSHandle *handle;
	struct connman_vpn_client *client;

	/*
	 * connman connection path -> struct pending_connect* (below). Lets a
	 * credential prompt raised mid-Connect() preempt the still-open LS2
	 * `connect` call with {returnValue:false, errorCode:-7, promptId} instead
	 * of making the caller wait out however long a human takes to answer it -
	 * see on_agent_prompt() and service_connect_cb().
	 */
	GHashTable *pending_connects;
};

struct pending_connect {
	LSHandle *handle;
	LSMessage *message;	/* reffed */
};

static void pending_connect_free(gpointer data)
{
	struct pending_connect *pending = data;

	if (!pending)
		return;

	LSMessageUnref(pending->message);
	g_free(pending);
}

/*
 * Subscriptions use explicit keys rather than the category/method default,
 * because getProfileDetails and getConnectionDetails are per-profile: a client
 * watching "work" must not be woken for every change to "home".
 */
static char *subscription_key(const char *method, const char *profile_name)
{
	if (profile_name)
		return g_strdup_printf("%s/%s", method, profile_name);

	return g_strdup(method);
}

static bool subscribe_with_key(LSHandle *handle, LSMessage *message, const char *key)
{
	LSError lserror;
	bool subscribed = false;

	if (!LSMessageIsSubscription(message))
		return false;

	LSErrorInit(&lserror);

	if (!LSSubscriptionAdd(handle, key, message, &lserror)) {
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	} else {
		subscribed = true;
	}

	return subscribed;
}

static void post_to_key(LSHandle *handle, const char *key, jvalue_ref payload)
{
	jschema_ref schema;
	LSError lserror;

	LSErrorInit(&lserror);

	schema = jschema_parse(j_cstr_to_buffer("{}"), DOMOPT_NOOPT, NULL);
	if (!schema)
		return;

	if (!LSSubscriptionReply(handle, key, jvalue_tostring(payload, schema), &lserror)) {
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}

	jschema_release(&schema);
}

/* ------------------------------------------------------------------ *
 * Reply builders. Each is shared between the direct reply and the
 * matching subscription update, so the two can never drift apart.
 * ------------------------------------------------------------------ */

static const char *connection_guid(struct vpn_connection *connection)
{
	const char *type = vpn_connection_get_string(connection, "Type");
	const struct vpn_provider_info *provider = vpn_provider_find_by_connman_type(type);

	return provider ? provider->guid : NULL;
}

static jvalue_ref build_profile_entry(struct vpn_connection *connection)
{
	jvalue_ref entry = jobject_create();
	const char *value;
	gboolean flag = FALSE;

	value = vpn_connection_get_string(connection, "Name");
	jobject_put(entry, J_CSTR_TO_JVAL("vpnProfileName"),
	            jstring_create(value ? value : ""));

	value = connection_guid(connection);
	if (value)
		jobject_put(entry, J_CSTR_TO_JVAL("vpnAgentGuid"), jstring_create(value));

	value = vpn_connection_get_string(connection, "Host");
	if (value)
		jobject_put(entry, J_CSTR_TO_JVAL("vpnHost"), jstring_create(value));

	jobject_put(entry, J_CSTR_TO_JVAL("vpnProfileConnectState"),
	            jstring_create(vpn_connection_get_connect_state(connection)));

	/* Immutable connections come from a .config file. The UI must render them
	 * read-only: connman only allows Connect/Disconnect/GetProperties on them. */
	if (vpn_connection_get_boolean(connection, "Immutable", &flag))
		jobject_put(entry, J_CSTR_TO_JVAL("immutable"), jboolean_create(flag));
	else
		jobject_put(entry, J_CSTR_TO_JVAL("immutable"), jboolean_create(false));

	flag = FALSE;
	vpn_connection_get_boolean(connection, "SplitRouting", &flag);
	jobject_put(entry, J_CSTR_TO_JVAL("splitRouting"), jboolean_create(flag));

	return entry;
}

static jvalue_ref build_profile_list(struct vpn_service *service)
{
	jvalue_ref reply = jobject_create();
	jvalue_ref profiles = jarray_create(NULL);
	GList *connections, *item;

	connections = connman_vpn_client_get_connections(service->client);

	for (item = connections; item; item = item->next)
		jarray_append(profiles, build_profile_entry(item->data));

	g_list_free(connections);

	jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply, J_CSTR_TO_JVAL("vpnProfiles"), profiles);

	return reply;
}

static jvalue_ref build_status(struct vpn_service *service)
{
	jvalue_ref reply = jobject_create();
	jvalue_ref active = jarray_create(NULL);
	GList *connections, *item;

	connections = connman_vpn_client_get_connections(service->client);

	for (item = connections; item; item = item->next) {
		const char *state = vpn_connection_get_connect_state(item->data);

		if (g_strcmp0(state, "disconnected") == 0)
			continue;

		jarray_append(active, build_profile_entry(item->data));
	}

	g_list_free(connections);

	jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply, J_CSTR_TO_JVAL("connmanVpnAvailable"),
	            jboolean_create(connman_vpn_client_is_available(service->client)));
	jobject_put(reply, J_CSTR_TO_JVAL("activeProfiles"), active);

	return reply;
}

/*
 * Walk a vpnFormFields descriptor and fill each field's value from the
 * connection's connman properties. Recurses through the container widget types
 * the legacy schema allows, so a grouped descriptor is handled too.
 */
static void fill_form_field_values(jvalue_ref fields, struct vpn_connection *connection)
{
	ssize_t count, i;

	if (!jis_array(fields))
		return;

	count = jarray_size(fields);

	for (i = 0; i < count; i++) {
		jvalue_ref field = jarray_get(fields, i);
		jvalue_ref nested = NULL;
		char *type;
		char *property;
		const char *value;

		if (!jis_object(field))
			continue;

		type = luna_service_object_get_string(field, "type");

		if (g_strcmp0(type, "rowgroup") == 0) {
			if (jobject_get_exists(field, J_CSTR_TO_BUF("vpnFormFields"), &nested))
				fill_form_field_values(nested, connection);
			g_free(type);
			continue;
		}

		if (g_strcmp0(type, "groups") == 0) {
			jvalue_ref groups = NULL;

			if (jobject_get_exists(field, J_CSTR_TO_BUF("groups"), &groups) &&
			    jis_array(groups)) {
				ssize_t group_count = jarray_size(groups);
				ssize_t g;

				for (g = 0; g < group_count; g++) {
					jvalue_ref group = jarray_get(groups, g);

					if (jobject_get_exists(group, J_CSTR_TO_BUF("vpnFormFields"),
					                       &nested))
						fill_form_field_values(nested, connection);
				}
			}
			g_free(type);
			continue;
		}

		property = luna_service_object_get_string(field, "connmanProperty");
		if (!property) {
			g_free(type);
			continue;
		}

		value = vpn_connection_get_string(connection, property);

		/*
		 * connman-vpnd never hands a stored secret back out, so a
		 * passwordfield can only ever report whether one is held. Blanking it
		 * here as well means a descriptor that did somehow carry a secret can
		 * not leak it onto the bus.
		 */
		if (g_strcmp0(type, "passwordfield") == 0) {
			jobject_put(field, J_CSTR_TO_JVAL("value"), jstring_create(""));
			jobject_put(field, J_CSTR_TO_JVAL("hasStoredValue"),
			            jboolean_create(value != NULL));
		} else if (value) {
			jobject_put(field, J_CSTR_TO_JVAL("value"), jstring_create(value));
		}

		g_free(property);
		g_free(type);
	}
}

static jvalue_ref build_profile_details(struct vpn_connection *connection)
{
	jvalue_ref reply = jobject_create();
	jvalue_ref profile = jobject_create();
	jvalue_ref fields;
	const struct vpn_provider_info *provider;
	const char *value;
	gboolean flag = FALSE;

	provider = vpn_provider_find_by_connman_type(
	               vpn_connection_get_string(connection, "Type"));

	jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));

	value = vpn_connection_get_string(connection, "Name");
	jobject_put(reply, J_CSTR_TO_JVAL("vpnProfileName"),
	            jstring_create(value ? value : ""));

	if (provider)
		jobject_put(reply, J_CSTR_TO_JVAL("vpnAgentGuid"),
		            jstring_create(provider->guid));

	if (vpn_connection_get_boolean(connection, "Immutable", &flag))
		jobject_put(reply, J_CSTR_TO_JVAL("immutable"), jboolean_create(flag));

	value = vpn_connection_get_string(connection, "Host");
	if (value)
		jobject_put(profile, J_CSTR_TO_JVAL("vpnHost"), jstring_create(value));

	value = vpn_connection_get_string(connection, "Domain");
	if (value)
		jobject_put(profile, J_CSTR_TO_JVAL("vpnDomain"), jstring_create(value));

	fields = vpn_provider_load_form_fields(provider);
	if (fields) {
		fill_form_field_values(fields, connection);
		jobject_put(profile, J_CSTR_TO_JVAL("vpnFormFields"), fields);
	}

	jobject_put(reply, J_CSTR_TO_JVAL("vpnProfile"), profile);

	return reply;
}

/*
 * Interface counters are not part of the connman VPN API. connman does report
 * the tunnel's ifindex, so read them straight out of sysfs; omit them rather
 * than reporting zero when the interface has gone, because the legacy UI
 * renders a missing value as "Not Available" and a zero as real traffic.
 */
static void add_interface_counters(jvalue_ref reply, gint32 index)
{
	char if_name[IF_NAMESIZE];
	const char *counters[] = { "rx_bytes", "tx_bytes" };
	const char *keys[] = { "bytesRx", "bytesTx" };
	guint i;

	if (index <= 0 || !if_indextoname((unsigned int) index, if_name))
		return;

	jobject_put(reply, J_CSTR_TO_JVAL("ifName"), jstring_create(if_name));

	for (i = 0; i < G_N_ELEMENTS(counters); i++) {
		char *path = g_strdup_printf("/sys/class/net/%s/statistics/%s", if_name,
		                             counters[i]);
		char *contents = NULL;

		if (g_file_get_contents(path, &contents, NULL, NULL)) {
			jobject_put(reply, jstring_create(keys[i]),
			            jnumber_create_i64(g_ascii_strtoll(contents, NULL, 10)));
			g_free(contents);
		}

		g_free(path);
	}
}

static void add_ip_details(jvalue_ref reply, GVariant *properties, const char *family_key)
{
	GVariant *family;
	const char *value = NULL;
	struct {
		const char *connman;
		const char *json;
	} map[] = {
		{ "Address",	 "clientIpAddress" },
		{ "Netmask",	 "netmask" },
		{ "PrefixLength", "prefixLength" },
		{ "Gateway",	 "gateway" },
		{ "Peer",		 "peer" },
	};
	guint i;

	if (!properties)
		return;

	family = g_variant_lookup_value(properties, family_key, G_VARIANT_TYPE_VARDICT);
	if (!family)
		return;

	for (i = 0; i < G_N_ELEMENTS(map); i++) {
		if (gvariant_dict_lookup_string(family, map[i].connman, &value) && value &&
		    *value)
			jobject_put(reply, jstring_create(map[i].json), jstring_create(value));
	}

	g_variant_unref(family);
}

static jvalue_ref build_connection_details(struct vpn_connection *connection)
{
	jvalue_ref reply = jobject_create();
	GVariant *properties = vpn_connection_get_properties(connection);
	GVariant *value;
	const char *string_value;
	gint32 index = 0;
	gboolean flag = FALSE;

	jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply, J_CSTR_TO_JVAL("state"),
	            jstring_create(vpn_connection_get_connect_state(connection)));

	string_value = vpn_connection_get_string(connection, "Type");
	if (string_value)
		jobject_put(reply, J_CSTR_TO_JVAL("tunnelType"), jstring_create(string_value));

	string_value = vpn_connection_get_string(connection, "Host");
	if (string_value)
		jobject_put(reply, J_CSTR_TO_JVAL("serverHostname"), jstring_create(string_value));

	string_value = vpn_connection_get_string(connection, "Domain");
	if (string_value)
		jobject_put(reply, J_CSTR_TO_JVAL("domain"), jstring_create(string_value));

	add_ip_details(reply, properties, "IPv4");
	add_ip_details(reply, properties, "IPv6");

	if (gvariant_dict_lookup_int(properties, "Index", &index)) {
		jobject_put(reply, J_CSTR_TO_JVAL("index"), jnumber_create_i32(index));
		add_interface_counters(reply, index);
	}

	vpn_connection_get_boolean(connection, "SplitRouting", &flag);
	jobject_put(reply, J_CSTR_TO_JVAL("splitRouting"), jboolean_create(flag));

	value = g_variant_lookup_value(properties, "Nameservers", NULL);
	if (value) {
		jobject_put(reply, J_CSTR_TO_JVAL("nameservers"), gvariant_to_jvalue(value));
		g_variant_unref(value);
	}

	value = g_variant_lookup_value(properties, "UserRoutes", NULL);
	if (value) {
		jobject_put(reply, J_CSTR_TO_JVAL("userRoutes"), gvariant_to_jvalue(value));
		g_variant_unref(value);
	}

	value = g_variant_lookup_value(properties, "ServerRoutes", NULL);
	if (value) {
		jobject_put(reply, J_CSTR_TO_JVAL("serverRoutes"), gvariant_to_jvalue(value));
		g_variant_unref(value);
	}

	return reply;
}

/* ------------------------------------------------------------------ *
 * The credential-prompt round trip: net.connman.vpn.Agent.RequestInput's
 * fields dict -> vpnFormFields JSON pushed on getStatus -> the client's
 * uiPromptResponse -> RequestInput's reply dict.
 * ------------------------------------------------------------------ */

/*
 * doc/vpn-agent-api.txt's Type values that matter for choosing a widget.
 * Everything else ("response", plain "string", anything undocumented) is
 * rendered as a textfield - a conservative default that at least lets the
 * value round-trip untouched.
 */
static const char *widget_type_for_agent_type(const char *agent_type)
{
	if (g_strcmp0(agent_type, "password") == 0)
		return "passwordfield";

	if (g_strcmp0(agent_type, "boolean") == 0)
		return "checkbox";

	return "textfield";
}

/*
 * Renders one entry of the agent's fields dict (already converted to pbnjson
 * by gvariant_to_jvalue - agent_field is {"Type":..,"Requirement":..,
 * "Value":..,"Alternates":[..]}) as one vpnFormFields widget, or returns NULL
 * to omit it.
 *
 * "control" fields (AllowStoreCredentials, AllowRetrieveCredentials,
 * KeepCredentials, VpnAgent.AuthFailure) are deliberately dropped rather than
 * rendered: doc/vpn-agent-api.txt says plainly they "will not usually be
 * presented directly to the user, and are not expected to be returned", and
 * acting on them (offering a real "remember credentials" toggle wired to
 * storage policy) is follow-on work, not something to fake with a checkbox
 * that does nothing.
 */
static jvalue_ref build_prompt_field(const char *key, jvalue_ref agent_field)
{
	jvalue_ref field;
	char *requirement = luna_service_object_get_string(agent_field, "Requirement");
	char *agent_type = luna_service_object_get_string(agent_field, "Type");
	jvalue_ref value_field = NULL;
	gboolean has_value = jobject_get_exists(agent_field, J_CSTR_TO_BUF("Value"),
	                                        &value_field);

	if (g_strcmp0(requirement, "control") == 0) {
		g_free(requirement);
		g_free(agent_type);
		return NULL;
	}

	field = jobject_create();
	jobject_put(field, J_CSTR_TO_JVAL("id"), jstring_create(key));

	if (g_strcmp0(requirement, "informational") == 0) {
		/*
		 * Not answerable - shown for context (e.g. which host/account this
		 * login is for) and skipped entirely when the reply is built.
		 */
		jobject_put(field, J_CSTR_TO_JVAL("type"), jstring_create("label"));
		jobject_put(field, J_CSTR_TO_JVAL("label"), jstring_create(key));

		if (has_value && jis_string(value_field)) {
			raw_buffer buf = jstring_get_fast(value_field);

			jobject_put(field, J_CSTR_TO_JVAL("value"),
			            jstring_create(g_strndup(buf.m_str, buf.m_len)));
		}
	} else {
		const char *widget = widget_type_for_agent_type(agent_type);

		jobject_put(field, J_CSTR_TO_JVAL("type"), jstring_create(widget));
		jobject_put(field, J_CSTR_TO_JVAL("label"), jstring_create(key));
		jobject_put(field, J_CSTR_TO_JVAL("required"),
		            jboolean_create(g_strcmp0(requirement, "mandatory") == 0));

		/*
		 * uiPromptResponse's reply builder needs to know the wire type to
		 * send back (D-Bus 'boolean' vs 'string') and this is the only place
		 * that survives until then - the original GVariant is long gone by
		 * the time the user answers.
		 */
		jobject_put(field, J_CSTR_TO_JVAL("promptValueType"),
		            jstring_create(agent_type ? agent_type : "string"));

		if (g_strcmp0(widget, "checkbox") == 0) {
			jobject_put(field, J_CSTR_TO_JVAL("trueValue"), jstring_create("true"));
			jobject_put(field, J_CSTR_TO_JVAL("falseValue"), jstring_create("false"));

			if (has_value && jis_boolean(value_field)) {
				bool bool_value = false;

				jboolean_get(value_field, &bool_value);
				jobject_put(field, J_CSTR_TO_JVAL("value"),
				            jstring_create(bool_value ? "true" : "false"));
			}
		} else if (has_value && jis_string(value_field)) {
			raw_buffer buf = jstring_get_fast(value_field);

			jobject_put(field, J_CSTR_TO_JVAL("value"),
			            jstring_create(g_strndup(buf.m_str, buf.m_len)));
		}
	}

	g_free(requirement);
	g_free(agent_type);

	return field;
}

/*
 * fields is the agent's RequestInput argument, already converted to pbnjson
 * (a JSON object keyed by property name - see on_agent_prompt()). Returns a
 * flat vpnFormFields array; connman's own field set has no grouping concept
 * to preserve.
 */
static jvalue_ref build_prompt_form_fields(jvalue_ref fields)
{
	jvalue_ref array = jarray_create(NULL);
	jobject_iter iter;
	jobject_key_value key_value;

	if (!jis_object(fields) || !jobject_iter_init(&iter, fields))
		return array;

	while (jobject_iter_next(&iter, &key_value)) {
		raw_buffer key_buf = jstring_get_fast(key_value.key);
		char *key = g_strndup(key_buf.m_str, key_buf.m_len);
		jvalue_ref field = build_prompt_field(key, key_value.value);

		if (field)
			jarray_append(array, field);

		g_free(key);
	}

	return array;
}

/*
 * The reverse of build_prompt_form_fields(): the client's uiPromptResponse
 * vpnFormFields array (round-tripped, values filled in) back into the a{sv}
 * RequestInput expects. label/status/button widgets and anything with an
 * empty value are skipped - matching the same "absent, not empty-string"
 * convention profile_build.c uses and consistent with the field never having
 * been meant to be answered in the first place (labels came from
 * "informational"/"control" agent fields, which build_prompt_field() already
 * keeps out of the reply path by either not rendering them (control) or
 * rendering them as non-answerable labels (informational)).
 */
static GVariant *build_agent_reply(jvalue_ref fields)
{
	GVariantBuilder builder;

	g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));

	if (jis_array(fields)) {
		ssize_t count = jarray_size(fields);
		ssize_t i;

		for (i = 0; i < count; i++) {
			jvalue_ref field = jarray_get(fields, i);
			char *id;
			char *type;
			char *value;

			if (!jis_object(field))
				continue;

			type = luna_service_object_get_string(field, "type");
			if (g_strcmp0(type, "label") == 0 || g_strcmp0(type, "status") == 0 ||
			    g_strcmp0(type, "button") == 0) {
				g_free(type);
				continue;
			}
			g_free(type);

			id = luna_service_object_get_string(field, "id");
			value = luna_service_object_get_string(field, "value");

			if (id && value && *value) {
				char *value_type =
				        luna_service_object_get_string(field, "promptValueType");

				if (g_strcmp0(value_type, "boolean") == 0)
					g_variant_builder_add(&builder, "{sv}", id,
					        g_variant_new_boolean(
					            g_strcmp0(value, "true") == 0));
				else
					g_variant_builder_add(&builder, "{sv}", id,
					        g_variant_new_string(value));

				g_free(value_type);
			}

			g_free(id);
			g_free(value);
		}
	}

	return g_variant_builder_end(&builder);
}

/* ------------------------------------------------------------------ *
 * Backend change notifications
 * ------------------------------------------------------------------ */

static void notify_profile_list(struct vpn_service *service)
{
	jvalue_ref payload = build_profile_list(service);

	post_to_key(service->handle, "getProfileList", payload);
	j_release(&payload);
}

static void notify_status(struct vpn_service *service)
{
	jvalue_ref payload = build_status(service);

	post_to_key(service->handle, "getStatus", payload);
	j_release(&payload);
}

static void notify_connection(struct vpn_service *service,
                              struct vpn_connection *connection)
{
	const char *name = vpn_connection_get_string(connection, "Name");
	jvalue_ref payload;
	char *key;

	if (!name)
		return;

	key = subscription_key("getProfileDetails", name);
	payload = build_profile_details(connection);
	post_to_key(service->handle, key, payload);
	j_release(&payload);
	g_free(key);

	key = subscription_key("getConnectionDetails", name);
	payload = build_connection_details(connection);
	post_to_key(service->handle, key, payload);
	j_release(&payload);
	g_free(key);
}

static void available_changed_cb(gboolean available, gpointer user_data)
{
	notify_status(user_data);
}

static void connections_changed_cb(gpointer user_data)
{
	struct vpn_service *service = user_data;

	notify_profile_list(service);
	notify_status(service);
}

static void connection_changed_cb(struct vpn_connection *connection, gpointer user_data)
{
	struct vpn_service *service = user_data;

	/* A State change alters both the list entry and the active set, so all
	 * three feeds are refreshed. */
	notify_profile_list(service);
	notify_status(service);
	notify_connection(service, connection);
}

/*
 * Looks up whatever identity we can find for service_path so prompt/notice
 * pushes can carry vpnProfileName - best effort, since a prompt can in
 * principle be raised for a connection that has not reached our cache yet
 * (Create() beat the ConnectionAdded signal announcing it).
 */
static void find_prompt_identity(struct vpn_service *service, const char *service_path,
                                 const char **name_out, const char **guid_out)
{
	struct vpn_connection *connection =
	        connman_vpn_client_find_by_path(service->client, service_path);

	*name_out = NULL;
	*guid_out = NULL;

	if (!connection)
		return;

	*name_out = vpn_connection_get_string(connection, "Name");
	*guid_out = connection_guid(connection);
}

static void on_agent_prompt(const char *service_path, GVariant *agent_fields,
                            gpointer user_data)
{
	struct vpn_service *service = user_data;
	struct pending_connect *pending;
	jvalue_ref agent_fields_json = gvariant_to_jvalue(agent_fields);
	jvalue_ref form_fields = build_prompt_form_fields(agent_fields_json);
	jvalue_ref payload = jobject_create();
	const char *name, *guid;

	find_prompt_identity(service, service_path, &name, &guid);

	jobject_put(payload, J_CSTR_TO_JVAL("promptId"), jstring_create(service_path));
	jobject_put(payload, J_CSTR_TO_JVAL("promptType"), jstring_create("form"));

	if (name)
		jobject_put(payload, J_CSTR_TO_JVAL("vpnProfileName"), jstring_create(name));
	if (guid)
		jobject_put(payload, J_CSTR_TO_JVAL("vpnAgentGuid"), jstring_create(guid));

	jobject_put(payload, J_CSTR_TO_JVAL("label"),
	            jstring_create(name ? name : "VPN sign-in required"));
	jobject_put(payload, J_CSTR_TO_JVAL("vpnFormFields"), form_fields);

	post_to_key(service->handle, "getStatus", payload);
	j_release(&payload);
	j_release(&agent_fields_json);

	/*
	 * If an LS2 `connect` call is sitting open waiting for exactly this
	 * connection, this prompt is the reason it has not resolved yet - answer
	 * it now rather than making the caller wait out however long the user
	 * takes, per doc/luneos-vpn-api.md's `connect` contract. The Connect() bus
	 * call keeps running in the background; its eventual completion finds
	 * nothing left in pending_connects (see service_connect_cb()'s
	 * on_connect_result) and simply does not reply again.
	 */
	pending = g_hash_table_lookup(service->pending_connects, service_path);
	if (pending) {
		jvalue_ref reply = jobject_create();

		jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(false));
		jobject_put(reply, J_CSTR_TO_JVAL("errorCode"),
		            jnumber_create_i32(VPN_ERR_NEEDS_AUTH));
		jobject_put(reply, J_CSTR_TO_JVAL("errorText"),
		            jstring_create("User authentication required."));
		jobject_put(reply, J_CSTR_TO_JVAL("promptId"), jstring_create(service_path));

		luna_service_message_validate_and_send(pending->handle, pending->message, reply);
		j_release(&reply);

		g_hash_table_remove(service->pending_connects, service_path);
	}
}

static void on_agent_notice(const char *service_path, const char *error, gpointer user_data)
{
	struct vpn_service *service = user_data;
	jvalue_ref payload = jobject_create();
	const char *name, *guid;

	find_prompt_identity(service, service_path, &name, &guid);

	if (name)
		jobject_put(payload, J_CSTR_TO_JVAL("vpnProfileName"), jstring_create(name));

	jobject_put(payload, J_CSTR_TO_JVAL("notice"), jstring_create(error ? error : ""));
	jobject_put(payload, J_CSTR_TO_JVAL("noticeSeverity"), jstring_create("errorNotice"));

	post_to_key(service->handle, "getStatus", payload);
	j_release(&payload);
}

static void on_agent_prompt_cancelled(const char *service_path, gpointer user_data)
{
	struct vpn_service *service = user_data;
	jvalue_ref payload = jobject_create();

	jobject_put(payload, J_CSTR_TO_JVAL("promptId"), jstring_create(service_path));
	jobject_put(payload, J_CSTR_TO_JVAL("promptResolved"), jboolean_create(true));

	post_to_key(service->handle, "getStatus", payload);
	j_release(&payload);
}

/* ------------------------------------------------------------------ *
 * LS2 methods
 * ------------------------------------------------------------------ */

/*
 * Resolve the vpnProfileName in a request to a connection, replying with the
 * right error and returning NULL when that is not possible. Sets *parsed so the
 * caller can release it.
 */
static struct vpn_connection *lookup_requested_connection(struct vpn_service *service,
                                                          LSHandle *handle,
                                                          LSMessage *message,
                                                          jvalue_ref *parsed,
                                                          char **name_out)
{
	struct vpn_connection *connection;
	char *name;

	*parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(message));
	if (!*parsed) {
		luna_service_message_reply_error_bad_json(handle, message);
		return NULL;
	}

	name = luna_service_object_get_string(*parsed, "vpnProfileName");
	if (!name) {
		luna_service_message_reply_error_invalid_params(handle, message);
		return NULL;
	}

	if (!connman_vpn_client_is_available(service->client)) {
		luna_service_message_reply_error_code(handle, message, VPN_ERR_NO_BACKEND,
		                                      "connman-vpnd is not available.");
		g_free(name);
		return NULL;
	}

	connection = connman_vpn_client_find_by_name(service->client, name);
	if (!connection) {
		/*
		 * Not an internal failure: the legacy UI calls this to find out whether
		 * a profile name is still free, and branches on returnValue.
		 */
		luna_service_message_reply_error_code(handle, message, VPN_ERR_PROFILE_NOT_FOUND,
		                                      "Profile not found.");
		g_free(name);
		return NULL;
	}

	*name_out = name;

	return connection;
}

static bool service_get_status_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct vpn_service *service = user_data;
	jvalue_ref reply;
	bool subscribed;

	subscribed = subscribe_with_key(handle, message, "getStatus");

	reply = build_status(service);
	jobject_put(reply, J_CSTR_TO_JVAL("subscribed"), jboolean_create(subscribed));

	luna_service_message_validate_and_send(handle, message, reply);
	j_release(&reply);

	return true;
}

static bool service_get_profile_list_cb(LSHandle *handle, LSMessage *message,
                                        void *user_data)
{
	struct vpn_service *service = user_data;
	jvalue_ref reply;
	bool subscribed;

	subscribed = subscribe_with_key(handle, message, "getProfileList");

	reply = build_profile_list(service);
	jobject_put(reply, J_CSTR_TO_JVAL("subscribed"), jboolean_create(subscribed));

	luna_service_message_validate_and_send(handle, message, reply);
	j_release(&reply);

	return true;
}

static bool service_get_profile_details_cb(LSHandle *handle, LSMessage *message,
                                           void *user_data)
{
	struct vpn_service *service = user_data;
	struct vpn_connection *connection;
	jvalue_ref parsed = NULL;
	jvalue_ref reply;
	char *name = NULL;
	char *key;
	bool subscribed;

	connection = lookup_requested_connection(service, handle, message, &parsed, &name);
	if (!connection) {
		j_release(&parsed);
		return true;
	}

	key = subscription_key("getProfileDetails", name);
	subscribed = subscribe_with_key(handle, message, key);
	g_free(key);

	reply = build_profile_details(connection);
	jobject_put(reply, J_CSTR_TO_JVAL("subscribed"), jboolean_create(subscribed));

	luna_service_message_validate_and_send(handle, message, reply);

	j_release(&reply);
	j_release(&parsed);
	g_free(name);

	return true;
}

static bool service_get_connection_details_cb(LSHandle *handle, LSMessage *message,
                                              void *user_data)
{
	struct vpn_service *service = user_data;
	struct vpn_connection *connection;
	jvalue_ref parsed = NULL;
	jvalue_ref reply;
	char *name = NULL;
	char *key;
	bool subscribed;

	connection = lookup_requested_connection(service, handle, message, &parsed, &name);
	if (!connection) {
		j_release(&parsed);
		return true;
	}

	key = subscription_key("getConnectionDetails", name);
	subscribed = subscribe_with_key(handle, message, key);
	g_free(key);

	reply = build_connection_details(connection);
	jobject_put(reply, J_CSTR_TO_JVAL("subscribed"), jboolean_create(subscribed));

	luna_service_message_validate_and_send(handle, message, reply);

	j_release(&reply);
	j_release(&parsed);
	g_free(name);

	return true;
}

static bool service_get_agents_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	jvalue_ref reply = jobject_create();
	jvalue_ref agents = jarray_create(NULL);
	GList *installed, *item;

	installed = vpn_providers_installed();

	for (item = installed; item; item = item->next) {
		const struct vpn_provider_info *provider = item->data;
		jvalue_ref agent = jobject_create();
		jvalue_ref technology = jarray_create(NULL);

		jobject_put(agent, J_CSTR_TO_JVAL("vpnAgentGuid"), jstring_create(provider->guid));
		jobject_put(agent, J_CSTR_TO_JVAL("vpnAgentLabel"), jstring_create(provider->label));

		jarray_append(technology, jstring_create(provider->technology));
		jobject_put(agent, J_CSTR_TO_JVAL("vpnAgentTechnology"), technology);

		jobject_put(agent, J_CSTR_TO_JVAL("connmanType"),
		            jstring_create(provider->connman_type));

		/*
		 * Emitted for compatibility with UI ported from the legacy service,
		 * which had a whole EULA scene. No connman VPN plugin declares one.
		 */
		jobject_put(agent, J_CSTR_TO_JVAL("vpnAgentIcon"), jstring_create(""));
		jobject_put(agent, J_CSTR_TO_JVAL("vpnAgentEula"), jstring_create(""));

		if (provider->deprecated)
			jobject_put(agent, J_CSTR_TO_JVAL("deprecated"), jboolean_create(true));

		if (provider->import_format) {
			jvalue_ref formats = jarray_create(NULL);

			jarray_append(formats, jstring_create(provider->import_format));
			jobject_put(agent, J_CSTR_TO_JVAL("supportsImport"), formats);
		}

		jarray_append(agents, agent);
	}

	g_list_free(installed);

	jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply, J_CSTR_TO_JVAL("vpnAgents"), agents);

	luna_service_message_validate_and_send(handle, message, reply);
	j_release(&reply);

	return true;
}

static bool service_get_agent_form_fields_cb(LSHandle *handle, LSMessage *message,
                                             void *user_data)
{
	const struct vpn_provider_info *provider;
	jvalue_ref parsed;
	jvalue_ref reply;
	jvalue_ref fields;
	char *guid;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(message));
	if (!parsed) {
		luna_service_message_reply_error_bad_json(handle, message);
		return true;
	}

	guid = luna_service_object_get_string(parsed, "vpnAgentGuid");
	j_release(&parsed);

	if (!guid) {
		luna_service_message_reply_error_invalid_params(handle, message);
		return true;
	}

	provider = vpn_provider_find_by_guid(guid);
	if (!provider) {
		luna_service_message_reply_error_code(handle, message, VPN_ERR_UNSUPPORTED_TYPE,
		                                      "Unknown VPN agent.");
		g_free(guid);
		return true;
	}

	fields = vpn_provider_load_form_fields(provider);
	if (!fields) {
		luna_service_message_reply_error_code(handle, message, VPN_ERR_UNKNOWN,
		                                      "No form field descriptor for this agent.");
		g_free(guid);
		return true;
	}

	reply = jobject_create();
	jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply, J_CSTR_TO_JVAL("vpnAgentGuid"), jstring_create(guid));
	jobject_put(reply, J_CSTR_TO_JVAL("vpnFormFields"), fields);

	luna_service_message_validate_and_send(handle, message, reply);

	j_release(&reply);
	g_free(guid);

	return true;
}

/* ------------------------------------------------------------------ *
 * Write methods: connect, disconnect, addProfile, updateProfile,
 * deleteProfile, uiPromptResponse, acceptEula.
 * ------------------------------------------------------------------ */

/* Generic "keep this LSMessage alive across one async connman call" request,
 * for the write ops that need no extra bookkeeping beyond that. */
struct ls2_request {
	LSHandle *handle;
	LSMessage *message;	/* reffed */
};

static struct ls2_request *ls2_request_new(LSHandle *handle, LSMessage *message)
{
	struct ls2_request *req = g_new0(struct ls2_request, 1);

	req->handle = handle;
	req->message = message;
	LSMessageRef(message);

	return req;
}

static void ls2_request_free(struct ls2_request *req)
{
	if (!req)
		return;

	LSMessageUnref(req->message);
	g_free(req);
}

/*
 * Builds and sends the {returnValue:true} / {returnValue:false, errorCode,
 * errorText} reply for one of connman_vpn_client's simple_cb-shaped results.
 * dbus_error_name ending in ".Canceled" - which is the whole of
 * net.connman.vpn.Agent.Error.Canceled - maps onto the legacy -5 "user
 * cancelled prompt" code the UI already knows to swallow silently; everything
 * else is reported as -6 (agent/provider failure) with connman's message
 * text.
 */
static void reply_simple_result(LSHandle *handle, LSMessage *message, gboolean success,
                                const char *dbus_error_name, const char *error_message)
{
	jvalue_ref reply = jobject_create();

	if (success) {
		jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	} else {
		int code = VPN_ERR_AGENT_FAILURE;

		if (dbus_error_name && g_str_has_suffix(dbus_error_name, ".Canceled"))
			code = VPN_ERR_PROMPT_CANCELLED;

		jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(false));
		jobject_put(reply, J_CSTR_TO_JVAL("errorCode"), jnumber_create_i32(code));
		jobject_put(reply, J_CSTR_TO_JVAL("errorText"),
		            jstring_create(error_message ? error_message : "Unknown error."));
	}

	luna_service_message_validate_and_send(handle, message, reply);
	j_release(&reply);
}

/* Shared completion for disconnect / updateProfile / deleteProfile - each
 * just replies success or failure, with no result payload beyond that. */
static void on_simple_op_done(gboolean success, const char *dbus_error_name,
                              const char *error_message, gpointer user_data)
{
	struct ls2_request *req = user_data;

	reply_simple_result(req->handle, req->message, success, dbus_error_name, error_message);
	ls2_request_free(req);
}

/* For the bulk `disconnect {}` path: nothing to reply to per-connection, only
 * something worth a log line if connman rejected one. */
static void on_fire_and_forget_result(gboolean success, const char *dbus_error_name,
                                      const char *error_message, gpointer user_data)
{
	if (!success)
		g_warning("Disconnect failed: %s", error_message ? error_message : "unknown error");
}

static void on_create_done(gboolean success, const char *path, const char *dbus_error_name,
                           const char *error_message, gpointer user_data)
{
	struct ls2_request *req = user_data;

	if (success) {
		jvalue_ref reply = jobject_create();

		jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
		if (path)
			jobject_put(reply, J_CSTR_TO_JVAL("vpnProfilePath"), jstring_create(path));

		luna_service_message_validate_and_send(req->handle, req->message, reply);
		j_release(&reply);
	} else {
		reply_simple_result(req->handle, req->message, FALSE, dbus_error_name,
		                    error_message);
	}

	ls2_request_free(req);
}

struct connect_completion {
	struct vpn_service *service;
	char *path;
};

static void on_connect_done(gboolean success, const char *dbus_error_name,
                            const char *error_message, gpointer user_data)
{
	struct connect_completion *completion = user_data;
	struct pending_connect *pending =
	        g_hash_table_lookup(completion->service->pending_connects, completion->path);

	if (pending) {
		reply_simple_result(pending->handle, pending->message, success, dbus_error_name,
		                    error_message);
		g_hash_table_remove(completion->service->pending_connects, completion->path);
	}
	/* else: a credential prompt already answered this call with
	 * {errorCode:-7, promptId} back in on_agent_prompt(). The connection's
	 * eventual state still reaches the client through the ordinary
	 * getStatus/getProfileList pushes connection_changed_cb() sends. */

	g_free(completion->path);
	g_free(completion);
}

static bool service_connect_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct vpn_service *service = user_data;
	struct vpn_connection *connection;
	jvalue_ref parsed = NULL;
	char *name = NULL;
	const char *path;
	struct pending_connect *pending;
	struct connect_completion *completion;

	connection = lookup_requested_connection(service, handle, message, &parsed, &name);
	if (!connection) {
		j_release(&parsed);
		return true;
	}

	path = vpn_connection_get_path(connection);

	if (g_hash_table_contains(service->pending_connects, path)) {
		luna_service_message_reply_error_code(handle, message, VPN_ERR_AGENT_FAILURE,
		        "A connection attempt for this profile is already in progress.");
		j_release(&parsed);
		g_free(name);
		return true;
	}

	pending = g_new0(struct pending_connect, 1);
	pending->handle = handle;
	pending->message = message;
	LSMessageRef(message);
	g_hash_table_insert(service->pending_connects, g_strdup(path), pending);

	completion = g_new0(struct connect_completion, 1);
	completion->service = service;
	completion->path = g_strdup(path);

	connman_vpn_client_connect(service->client, connection, on_connect_done, completion);

	j_release(&parsed);
	g_free(name);

	return true;
}

static bool service_disconnect_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct vpn_service *service = user_data;
	jvalue_ref parsed;
	char *name;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(message));
	if (!parsed) {
		luna_service_message_reply_error_bad_json(handle, message);
		return true;
	}

	if (!connman_vpn_client_is_available(service->client)) {
		luna_service_message_reply_error_code(handle, message, VPN_ERR_NO_BACKEND,
		                                      "connman-vpnd is not available.");
		j_release(&parsed);
		return true;
	}

	name = luna_service_object_get_string(parsed, "vpnProfileName");

	if (name) {
		struct vpn_connection *connection =
		        connman_vpn_client_find_by_name(service->client, name);
		struct ls2_request *req;

		if (!connection) {
			luna_service_message_reply_error_code(handle, message,
			        VPN_ERR_PROFILE_NOT_FOUND, "Profile not found.");
			g_free(name);
			j_release(&parsed);
			return true;
		}

		req = ls2_request_new(handle, message);
		connman_vpn_client_disconnect(service->client, connection, on_simple_op_done, req);
	} else {
		/*
		 * No name: best-effort disconnect everything active. Unlike legacy
		 * (one active profile, one clear answer) there can be several
		 * independent tunnels up at once, so this replies as soon as every
		 * Disconnect() has been issued rather than waiting for each to
		 * finish - see doc/luneos-vpn-api.md's `disconnect` section.
		 */
		GList *connections = connman_vpn_client_get_connections(service->client);
		GList *item;

		for (item = connections; item; item = item->next) {
			struct vpn_connection *connection = item->data;
			const char *state = vpn_connection_get_connect_state(connection);

			if (g_strcmp0(state, "disconnected") != 0)
				connman_vpn_client_disconnect(service->client, connection,
				                              on_fire_and_forget_result, NULL);
		}

		g_list_free(connections);
		luna_service_message_reply_success(handle, message);
	}

	g_free(name);
	j_release(&parsed);

	return true;
}

static bool service_add_profile_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct vpn_service *service = user_data;
	jvalue_ref parsed;
	char *name = NULL, *guid = NULL, *host = NULL, *domain = NULL;
	const struct vpn_provider_info *provider;
	jvalue_ref profile = NULL, form_fields = NULL;
	GVariant *properties;
	struct ls2_request *req;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(message));
	if (!parsed) {
		luna_service_message_reply_error_bad_json(handle, message);
		return true;
	}

	if (!connman_vpn_client_is_available(service->client)) {
		luna_service_message_reply_error_code(handle, message, VPN_ERR_NO_BACKEND,
		                                      "connman-vpnd is not available.");
		goto out;
	}

	name = luna_service_object_get_string(parsed, "vpnProfileName");
	guid = luna_service_object_get_string(parsed, "vpnAgentGuid");

	if (!name || !guid) {
		luna_service_message_reply_error_invalid_params(handle, message);
		goto out;
	}

	provider = vpn_provider_find_by_guid(guid);
	if (!provider) {
		luna_service_message_reply_error_code(handle, message, VPN_ERR_UNSUPPORTED_TYPE,
		                                      "Unknown VPN agent.");
		goto out;
	}

	if (connman_vpn_client_find_by_name(service->client, name)) {
		luna_service_message_reply_error_code(handle, message, VPN_ERR_PROFILE_EXISTS,
		                                      "Profile with that name already exists.");
		goto out;
	}

	if (jobject_get_exists(parsed, J_CSTR_TO_BUF("vpnProfile"), &profile) &&
	    jis_object(profile)) {
		host = luna_service_object_get_string(profile, "vpnHost");
		domain = luna_service_object_get_string(profile, "vpnDomain");
		jobject_get_exists(profile, J_CSTR_TO_BUF("vpnFormFields"), &form_fields);
	}

	if (!host) {
		luna_service_message_reply_error_invalid_params(handle, message);
		goto out;
	}

	/*
	 * connman identifies a VPN connection by a hash of (Host, Domain), not by
	 * our Name - see __vpn_provider_create_identifier() in
	 * vpn/vpn-provider.c. Two profiles with different names but the same
	 * Host+Domain collapse onto the same underlying connman connection object
	 * (the second Create() just updates its Name); the PROFILE_EXISTS check
	 * above only catches a name collision, not this. Known limitation - not
	 * solved here.
	 */
	properties = profile_build_properties(provider->connman_type, name, host, domain,
	                                      form_fields);

	req = ls2_request_new(handle, message);
	connman_vpn_client_create_connection(service->client, properties, on_create_done, req);

out:
	g_free(name);
	g_free(guid);
	g_free(host);
	g_free(domain);
	j_release(&parsed);

	return true;
}

static bool service_update_profile_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct vpn_service *service = user_data;
	struct vpn_connection *connection;
	jvalue_ref parsed = NULL;
	char *name = NULL;
	jvalue_ref profile = NULL, form_fields = NULL;
	char *host = NULL, *domain = NULL;
	GVariant *properties;
	struct ls2_request *req;
	gboolean immutable = FALSE;

	connection = lookup_requested_connection(service, handle, message, &parsed, &name);
	if (!connection) {
		j_release(&parsed);
		return true;
	}

	if (vpn_connection_get_boolean(connection, "Immutable", &immutable) && immutable) {
		luna_service_message_reply_error_code(handle, message, VPN_ERR_IMMUTABLE,
		        "This profile is provisioned and cannot be changed.");
		j_release(&parsed);
		g_free(name);
		return true;
	}

	if (jobject_get_exists(parsed, J_CSTR_TO_BUF("vpnProfile"), &profile) &&
	    jis_object(profile)) {
		host = luna_service_object_get_string(profile, "vpnHost");
		domain = luna_service_object_get_string(profile, "vpnDomain");
		jobject_get_exists(profile, J_CSTR_TO_BUF("vpnFormFields"), &form_fields);
	}

	/* Type and Name are deliberately not resent - see profile_build.h. */
	properties = profile_build_properties(NULL, NULL, host, domain, form_fields);

	req = ls2_request_new(handle, message);
	connman_vpn_client_set_properties(service->client, connection, properties,
	                                  on_simple_op_done, req);

	g_free(host);
	g_free(domain);
	j_release(&parsed);
	g_free(name);

	return true;
}

static bool service_delete_profile_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct vpn_service *service = user_data;
	struct vpn_connection *connection;
	jvalue_ref parsed = NULL;
	char *name = NULL;
	gboolean immutable = FALSE;
	struct ls2_request *req;

	connection = lookup_requested_connection(service, handle, message, &parsed, &name);
	if (!connection) {
		j_release(&parsed);
		return true;
	}

	if (vpn_connection_get_boolean(connection, "Immutable", &immutable) && immutable) {
		luna_service_message_reply_error_code(handle, message, VPN_ERR_IMMUTABLE,
		        "This profile is provisioned and cannot be removed.");
		j_release(&parsed);
		g_free(name);
		return true;
	}

	req = ls2_request_new(handle, message);
	connman_vpn_client_remove_connection(service->client, vpn_connection_get_path(connection),
	                                     on_simple_op_done, req);

	j_release(&parsed);
	g_free(name);

	return true;
}

static bool service_ui_prompt_response_cb(LSHandle *handle, LSMessage *message,
                                          void *user_data)
{
	struct vpn_service *service = user_data;
	jvalue_ref parsed;
	char *prompt_id;
	bool cancelled = false;
	jvalue_ref cancelled_val = NULL;
	jvalue_ref fields = NULL;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(message));
	if (!parsed) {
		luna_service_message_reply_error_bad_json(handle, message);
		return true;
	}

	prompt_id = luna_service_object_get_string(parsed, "promptId");
	if (!prompt_id) {
		luna_service_message_reply_error_invalid_params(handle, message);
		j_release(&parsed);
		return true;
	}

	if (jobject_get_exists(parsed, J_CSTR_TO_BUF("cancelled"), &cancelled_val))
		jboolean_get(cancelled_val, &cancelled);

	/* Legacy's cancel signal, still accepted - see doc/luneos-vpn-api.md. */
	if (!cancelled) {
		char *button_id = luna_service_object_get_string(parsed, "buttonId");

		if (g_strcmp0(button_id, "backButton") == 0)
			cancelled = true;

		g_free(button_id);
	}

	if (cancelled) {
		connman_vpn_client_complete_prompt(service->client, prompt_id, NULL);
	} else {
		GVariant *reply;

		jobject_get_exists(parsed, J_CSTR_TO_BUF("vpnFormFields"), &fields);
		reply = build_agent_reply(fields);
		connman_vpn_client_complete_prompt(service->client, prompt_id, reply);
	}

	/*
	 * Fire-and-forget by design, matching the legacy service: the outcome of
	 * whatever this prompt was answering (a connect succeeding, a
	 * ReportError retry) surfaces separately through getStatus, not through
	 * this reply.
	 */
	luna_service_message_reply_success(handle, message);

	g_free(prompt_id);
	j_release(&parsed);

	return true;
}

static bool service_accept_eula_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	/* No shipped connman VPN plugin declares a EULA; kept so UI code ported
	 * from the legacy service does not need a branch for its absence. */
	luna_service_message_reply_success(handle, message);

	return true;
}

static LSMethod vpn_service_methods[] = {
	{ "getStatus", service_get_status_cb },
	{ "getProfileList", service_get_profile_list_cb },
	{ "getProfileDetails", service_get_profile_details_cb },
	{ "getConnectionDetails", service_get_connection_details_cb },
	{ "getAgents", service_get_agents_cb },
	{ "getAgentFormFields", service_get_agent_form_fields_cb },
	{ "connect", service_connect_cb },
	{ "disconnect", service_disconnect_cb },
	{ "addProfile", service_add_profile_cb },
	{ "updateProfile", service_update_profile_cb },
	{ "deleteProfile", service_delete_profile_cb },
	{ "uiPromptResponse", service_ui_prompt_response_cb },
	{ "acceptEula", service_accept_eula_cb },
	{ NULL, NULL },
};

struct vpn_service *vpn_service_create(void)
{
	struct vpn_service *service;
	LSError error;

	LSErrorInit(&error);

	service = g_new0(struct vpn_service, 1);

	if (!LSRegister(VPN_SERVICE_NAME, &service->handle, &error)) {
		g_critical("Failed to register %s: %s", VPN_SERVICE_NAME, error.message);
		LSErrorFree(&error);
		goto failed;
	}

	if (!LSGmainAttach(service->handle, event_loop, &error)) {
		g_critical("Failed to attach %s to the main loop: %s", VPN_SERVICE_NAME,
		           error.message);
		LSErrorFree(&error);
		goto failed;
	}

	if (!LSRegisterCategory(service->handle, "/", vpn_service_methods, NULL, NULL,
	                        &error)) {
		g_critical("Failed to register the service category: %s", error.message);
		LSErrorFree(&error);
		goto failed;
	}

	if (!LSCategorySetData(service->handle, "/", service, &error)) {
		g_critical("Failed to set the service category data: %s", error.message);
		LSErrorFree(&error);
		goto failed;
	}

	/* char* connman connection path -> struct pending_connect*; both owned. */
	service->pending_connects = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                                   pending_connect_free);

	service->client = connman_vpn_client_create(available_changed_cb,
	                                            connections_changed_cb,
	                                            connection_changed_cb,
	                                            on_agent_prompt, on_agent_notice,
	                                            on_agent_prompt_cancelled, service);

	return service;

failed:
	if (service->handle) {
		LSError unregister_error;

		LSErrorInit(&unregister_error);

		if (!LSUnregister(service->handle, &unregister_error))
			LSErrorFree(&unregister_error);
	}

	g_free(service);

	return NULL;
}

void vpn_service_free(struct vpn_service *service)
{
	LSError error;

	if (!service)
		return;

	LSErrorInit(&error);

	if (service->client)
		connman_vpn_client_free(service->client);

	if (service->pending_connects)
		g_hash_table_destroy(service->pending_connects);

	if (service->handle && !LSUnregister(service->handle, &error)) {
		LSErrorPrint(&error, stderr);
		LSErrorFree(&error);
	}

	g_free(service);
}

/* vim:ts=4:sw=4:noexpandtab */
