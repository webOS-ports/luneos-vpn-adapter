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

#ifndef VPN_PROVIDERS_H_
#define VPN_PROVIDERS_H_

#include <glib.h>
#include <pbnjson.h>

/*
 * A "provider" here is what the legacy service called an agent: one VPN
 * technology. Legacy webOS discovered these by scanning /usr/lib/vpn/agents for
 * plugin directories with a vpn-plugin-info.json; the equivalent now is which
 * connman VPN plugins are actually installed, so this scans
 * ${libdir}/connman/plugins-vpn for .so files.
 */
struct vpn_provider_info {
	const char *connman_type;	/* Type property value: "openvpn", "vpnc", ... */
	const char *plugin;			/* .so basename in plugins-vpn */
	const char *guid;			/* vpnAgentGuid on the LS2 API */
	const char *label;			/* vpnAgentLabel */
	const char *technology;		/* vpnAgentTechnology, single entry */
	gboolean deprecated;
	const char *import_format;	/* supportsImport entry, or NULL */
};

/* Every provider connman 2.0 can build, whether or not it is installed here. */
const struct vpn_provider_info *vpn_providers_all(guint *count);

/* Installed ones only, in the order of vpn_providers_all(). Free with g_list_free. */
GList *vpn_providers_installed(void);

const struct vpn_provider_info *vpn_provider_find_by_guid(const char *guid);
const struct vpn_provider_info *vpn_provider_find_by_connman_type(const char *type);

/*
 * The blank vpnFormFields descriptor for a provider, read from
 * ${datadir}/luneos-vpn-adapter/formfields/<connman_type>.json. Returns a new
 * reference, or NULL if the file is missing or unparseable.
 */
jvalue_ref vpn_provider_load_form_fields(const struct vpn_provider_info *provider);

#endif

/* vim:ts=4:sw=4:noexpandtab */
