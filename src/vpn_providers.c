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
#include <stdlib.h>

#include "vpn_providers.h"

/*
 * Kept in the order they should be offered to the user: the two that are both
 * modern and well supported first, the IPsec family next, PPTP last because it
 * is broken and only there for reachability of ancient gateways.
 */
static const struct vpn_provider_info providers[] = {
	{ "wireguard",   "wireguard.so",   "com.webos.vpn.wireguard",
	  "WireGuard",              "WireGuard", FALSE, "wg-conf" },
	{ "openvpn",     "openvpn.so",     "com.webos.vpn.openvpn",
	  "OpenVPN",                "ssl",       FALSE, "ovpn" },
	{ "openconnect", "openconnect.so", "com.webos.vpn.openconnect",
	  "OpenConnect (AnyConnect)", "ssl",     FALSE, NULL },
	{ "vpnc",        "vpnc.so",        "com.webos.vpn.vpnc",
	  "Cisco IPsec",            "IPSec",     FALSE, NULL },
	{ "l2tp",        "l2tp.so",        "com.webos.vpn.l2tp",
	  "L2TP/IPsec",             "L2TP",      FALSE, NULL },
	{ "pptp",        "pptp.so",        "com.webos.vpn.pptp",
	  "PPTP",                   "PPTP",      TRUE,  NULL },
};

const struct vpn_provider_info *vpn_providers_all(guint *count)
{
	if (count)
		*count = G_N_ELEMENTS(providers);

	return providers;
}

/*
 * CONNMAN_VPN_PLUGIN_DIR and LUNEOS_VPN_DATA_DIR are compiled in from CMake but
 * overridable from the environment, so the service can be exercised on a host
 * without installing it.
 */
static const char *plugin_dir(void)
{
	const char *dir = g_getenv("LUNEOS_VPN_PLUGIN_DIR");

	return (dir && *dir) ? dir : CONNMAN_VPN_PLUGIN_DIR;
}

static const char *data_dir(void)
{
	const char *dir = g_getenv("LUNEOS_VPN_DATA_DIR");

	return (dir && *dir) ? dir : LUNEOS_VPN_DATA_DIR;
}

GList *vpn_providers_installed(void)
{
	GList *installed = NULL;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(providers); i++) {
		char *path = g_build_filename(plugin_dir(), providers[i].plugin, NULL);

		if (g_file_test(path, G_FILE_TEST_EXISTS))
			installed = g_list_append(installed, (gpointer) &providers[i]);

		g_free(path);
	}

	return installed;
}

const struct vpn_provider_info *vpn_provider_find_by_guid(const char *guid)
{
	guint i;

	if (!guid)
		return NULL;

	for (i = 0; i < G_N_ELEMENTS(providers); i++) {
		if (g_strcmp0(providers[i].guid, guid) == 0)
			return &providers[i];
	}

	return NULL;
}

const struct vpn_provider_info *vpn_provider_find_by_connman_type(const char *type)
{
	guint i;

	if (!type)
		return NULL;

	/* connman reports Type lowercased ("openvpn"), but a .config file may have
	 * written it as "OpenVPN", so compare case-insensitively. */
	for (i = 0; i < G_N_ELEMENTS(providers); i++) {
		if (g_ascii_strcasecmp(providers[i].connman_type, type) == 0)
			return &providers[i];
	}

	return NULL;
}

jvalue_ref vpn_provider_load_form_fields(const struct vpn_provider_info *provider)
{
	char *path;
	char *contents = NULL;
	gsize length = 0;
	GError *error = NULL;
	jvalue_ref parsed;
	jvalue_ref fields = NULL;
	jschema_ref schema;
	JSchemaInfo schema_info;

	if (!provider)
		return NULL;

	path = g_build_filename(data_dir(), "formfields", NULL);
	{
		char *name = g_strdup_printf("%s.json", provider->connman_type);
		char *full = g_build_filename(path, name, NULL);

		g_free(path);
		g_free(name);
		path = full;
	}

	if (!g_file_get_contents(path, &contents, &length, &error)) {
		g_warning("No form field descriptor for provider %s at %s: %s",
		          provider->connman_type, path,
		          error ? error->message : "unknown error");
		g_clear_error(&error);
		g_free(path);
		return NULL;
	}

	g_free(path);

	schema = jschema_parse(j_cstr_to_buffer("{}"), DOMOPT_NOOPT, NULL);
	if (!schema) {
		g_free(contents);
		return NULL;
	}

	jschema_info_init(&schema_info, schema, NULL, NULL);
	parsed = jdom_parse(j_str_to_buffer(contents, length), DOMOPT_NOOPT, &schema_info);

	jschema_release(&schema);
	g_free(contents);

	if (jis_null(parsed)) {
		g_warning("Could not parse the form field descriptor for provider %s",
		          provider->connman_type);
		j_release(&parsed);
		return NULL;
	}

	/* The file is the whole descriptor; the API only returns its vpnFormFields
	 * array, so lift that out and drop the rest. */
	if (jobject_get_exists(parsed, J_CSTR_TO_BUF("vpnFormFields"), &fields))
		fields = jvalue_duplicate(fields);
	else
		fields = NULL;

	j_release(&parsed);

	return fields;
}

/* vim:ts=4:sw=4:noexpandtab */
