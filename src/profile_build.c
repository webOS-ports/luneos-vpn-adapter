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

#include "profile_build.h"
#include "luna_service_utils.h"

static void add_string(GVariantBuilder *builder, const char *key, const char *value)
{
	if (!key || !value)
		return;

	g_variant_builder_add(builder, "{sv}", key, g_variant_new_string(value));
}

/*
 * A field's plain (non-mapped) value is skipped when empty rather than sent as
 * "". On the SetProperty("Properties", ...) path (updateProfile) an explicit
 * empty string clears the property - not the same thing as "the user left an
 * optional field untouched", which is what an empty value on a fresh form
 * normally means. On the Create() path (addProfile) it does not even have
 * that meaning: Create()'s second pass writes every string-valued dict entry
 * verbatim via vpn_provider_set_string(), with no "" -> unset conversion, so
 * sending "" there would store a literal empty string instead of leaving the
 * property genuinely absent. Skipping empty values gives the same, correct
 * "unset" outcome on both paths.
 */
static void add_field_value(GVariantBuilder *builder, const char *key, const char *value)
{
	if (!key || !value || !*value)
		return;

	add_string(builder, key, value);
}

/*
 * connmanPropertyMap is only used by vpnc's encryption-method selector, where
 * an empty per-key value is a deliberate "clear this flag for the selected
 * option" instruction, not an unfilled field. Those are sent as-is, including
 * empty strings - see profile_build.h.
 */
static void add_property_map_entry(GVariantBuilder *builder, jvalue_ref map,
                                   const char *selected_value)
{
	jvalue_ref sub_map = NULL;
	jobject_iter iter;
	jobject_key_value key_value;

	if (!jis_object(map) || !selected_value)
		return;

	if (!jobject_get_exists(map, j_cstr_to_buffer(selected_value), &sub_map))
		return;

	if (!jis_object(sub_map))
		return;

	if (!jobject_iter_init(&iter, sub_map))
		return;

	while (jobject_iter_next(&iter, &key_value)) {
		raw_buffer key_buf = jstring_get_fast(key_value.key);
		char *key = g_strndup(key_buf.m_str, key_buf.m_len);
		char *value = NULL;

		if (jis_string(key_value.value)) {
			raw_buffer value_buf = jstring_get_fast(key_value.value);

			value = g_strndup(value_buf.m_str, value_buf.m_len);
		}

		add_string(builder, key, value ? value : "");

		g_free(key);
		g_free(value);
	}
}

static void walk_form_fields(GVariantBuilder *builder, jvalue_ref fields);

static void walk_one_field(GVariantBuilder *builder, jvalue_ref field)
{
	char *type;
	char *value;
	jvalue_ref nested = NULL;
	jvalue_ref property_map = NULL;

	if (!jis_object(field))
		return;

	type = luna_service_object_get_string(field, "type");

	if (g_strcmp0(type, "rowgroup") == 0) {
		if (jobject_get_exists(field, J_CSTR_TO_BUF("vpnFormFields"), &nested))
			walk_form_fields(builder, nested);
		g_free(type);
		return;
	}

	if (g_strcmp0(type, "groups") == 0) {
		jvalue_ref groups = NULL;

		if (jobject_get_exists(field, J_CSTR_TO_BUF("groups"), &groups) &&
		    jis_array(groups)) {
			ssize_t count = jarray_size(groups);
			ssize_t i;

			for (i = 0; i < count; i++) {
				jvalue_ref group = jarray_get(groups, i);

				if (jobject_get_exists(group, J_CSTR_TO_BUF("vpnFormFields"),
				                       &nested))
					walk_form_fields(builder, nested);
			}
		}
		g_free(type);
		return;
	}

	/* Every other widget type (textfield, passwordfield, checkbox,
	 * listselector, button, label, status) either carries a value worth
	 * sending or does not; button/label/status never have a connmanProperty
	 * and fall out of both branches below harmlessly. */
	value = luna_service_object_get_string(field, "value");

	if (jobject_get_exists(field, J_CSTR_TO_BUF("connmanPropertyMap"), &property_map)) {
		add_property_map_entry(builder, property_map, value);
	} else {
		char *property = luna_service_object_get_string(field, "connmanProperty");

		add_field_value(builder, property, value);
		g_free(property);
	}

	g_free(value);
	g_free(type);
}

static void walk_form_fields(GVariantBuilder *builder, jvalue_ref fields)
{
	ssize_t count, i;

	if (!jis_array(fields))
		return;

	count = jarray_size(fields);

	for (i = 0; i < count; i++)
		walk_one_field(builder, jarray_get(fields, i));
}

GVariant *profile_build_properties(const char *type, const char *name,
                                   const char *host, const char *domain,
                                   jvalue_ref vpn_form_fields)
{
	GVariantBuilder builder;

	g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));

	add_string(&builder, "Type", type);
	add_string(&builder, "Name", name);
	add_string(&builder, "Host", host);
	add_string(&builder, "Domain", domain);

	if (vpn_form_fields)
		walk_form_fields(&builder, vpn_form_fields);

	return g_variant_builder_end(&builder);
}

/* vim:ts=4:sw=4:noexpandtab */
