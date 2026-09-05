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

#include "gvariant_json.h"

static jvalue_ref array_to_jvalue(GVariant *variant)
{
	jvalue_ref array = jarray_create(NULL);
	GVariantIter iter;
	GVariant *child;

	g_variant_iter_init(&iter, variant);

	while ((child = g_variant_iter_next_value(&iter))) {
		jarray_append(array, gvariant_to_jvalue(child));
		g_variant_unref(child);
	}

	return array;
}

static jvalue_ref dict_to_jvalue(GVariant *variant)
{
	jvalue_ref object = jobject_create();
	GVariantIter iter;
	const gchar *key;
	GVariant *value;

	g_variant_iter_init(&iter, variant);

	while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
		jobject_put(object, jstring_create(key), gvariant_to_jvalue(value));
		g_variant_unref(value);
	}

	return object;
}

jvalue_ref gvariant_to_jvalue(GVariant *variant)
{
	const GVariantType *type;

	if (!variant)
		return jnull();

	/* A variant wrapping a variant: unwrap and try again. Every value in an
	 * a{sv} arrives like this. */
	if (g_variant_is_of_type(variant, G_VARIANT_TYPE_VARIANT)) {
		GVariant *inner = g_variant_get_variant(variant);
		jvalue_ref result = gvariant_to_jvalue(inner);

		g_variant_unref(inner);
		return result;
	}

	type = g_variant_get_type(variant);

	if (g_variant_type_equal(type, G_VARIANT_TYPE_STRING) ||
	    g_variant_type_equal(type, G_VARIANT_TYPE_OBJECT_PATH) ||
	    g_variant_type_equal(type, G_VARIANT_TYPE_SIGNATURE))
		return jstring_create(g_variant_get_string(variant, NULL));

	if (g_variant_type_equal(type, G_VARIANT_TYPE_BOOLEAN))
		return jboolean_create(g_variant_get_boolean(variant));

	if (g_variant_type_equal(type, G_VARIANT_TYPE_BYTE))
		return jnumber_create_i32(g_variant_get_byte(variant));

	if (g_variant_type_equal(type, G_VARIANT_TYPE_INT16))
		return jnumber_create_i32(g_variant_get_int16(variant));

	if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT16))
		return jnumber_create_i32(g_variant_get_uint16(variant));

	if (g_variant_type_equal(type, G_VARIANT_TYPE_INT32))
		return jnumber_create_i32(g_variant_get_int32(variant));

	if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT32))
		return jnumber_create_i64(g_variant_get_uint32(variant));

	if (g_variant_type_equal(type, G_VARIANT_TYPE_INT64))
		return jnumber_create_i64(g_variant_get_int64(variant));

	if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT64))
		return jnumber_create_i64((gint64) g_variant_get_uint64(variant));

	if (g_variant_type_equal(type, G_VARIANT_TYPE_DOUBLE))
		return jnumber_create_f64(g_variant_get_double(variant));

	if (g_variant_is_of_type(variant, G_VARIANT_TYPE_VARDICT))
		return dict_to_jvalue(variant);

	if (g_variant_type_is_array(type) || g_variant_type_is_tuple(type))
		return array_to_jvalue(variant);

	/* Unrecognised: keep the printed form rather than losing the value. */
	{
		gchar *text = g_variant_print(variant, FALSE);
		jvalue_ref result = jstring_create(text);

		g_free(text);
		return result;
	}
}

gboolean gvariant_dict_lookup_string(GVariant *dict, const char *key, const char **out)
{
	GVariant *value;

	if (!dict || !g_variant_is_of_type(dict, G_VARIANT_TYPE_VARDICT))
		return FALSE;

	value = g_variant_lookup_value(dict, key, G_VARIANT_TYPE_STRING);
	if (!value)
		return FALSE;

	/* The dict keeps the string alive; callers only read it. */
	*out = g_variant_get_string(value, NULL);
	g_variant_unref(value);

	return TRUE;
}

gboolean gvariant_dict_lookup_boolean(GVariant *dict, const char *key, gboolean *out)
{
	GVariant *value;

	if (!dict || !g_variant_is_of_type(dict, G_VARIANT_TYPE_VARDICT))
		return FALSE;

	value = g_variant_lookup_value(dict, key, G_VARIANT_TYPE_BOOLEAN);
	if (!value)
		return FALSE;

	*out = g_variant_get_boolean(value);
	g_variant_unref(value);

	return TRUE;
}

gboolean gvariant_dict_lookup_int(GVariant *dict, const char *key, gint32 *out)
{
	GVariant *value;

	if (!dict || !g_variant_is_of_type(dict, G_VARIANT_TYPE_VARDICT))
		return FALSE;

	value = g_variant_lookup_value(dict, key, G_VARIANT_TYPE_INT32);
	if (!value)
		return FALSE;

	*out = g_variant_get_int32(value);
	g_variant_unref(value);

	return TRUE;
}

/* vim:ts=4:sw=4:noexpandtab */
