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

#ifndef GVARIANT_JSON_H_
#define GVARIANT_JSON_H_

#include <glib.h>
#include <pbnjson.h>

/*
 * Convert a GVariant coming off the connman-vpn bus into a pbnjson value.
 * Handles the types connman actually uses for VPN connection properties:
 * strings, object paths, booleans, the integer widths, arrays of strings,
 * arrays of dicts (UserRoutes/ServerRoutes) and nested a{sv} dicts (IPv4/IPv6).
 * Anything unrecognised comes back as its g_variant_print() text rather than
 * being dropped, so a new connman property shows up in the API instead of
 * silently vanishing.
 *
 * Returns a new reference the caller owns.
 */
jvalue_ref gvariant_to_jvalue(GVariant *variant);

/* Convenience readers for an a{sv} dict. */
gboolean gvariant_dict_lookup_string(GVariant *dict, const char *key, const char **out);
gboolean gvariant_dict_lookup_boolean(GVariant *dict, const char *key, gboolean *out);
gboolean gvariant_dict_lookup_int(GVariant *dict, const char *key, gint32 *out);

#endif

/* vim:ts=4:sw=4:noexpandtab */
