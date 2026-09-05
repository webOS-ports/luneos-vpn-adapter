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

#ifndef PROFILE_BUILD_H_
#define PROFILE_BUILD_H_

#include <glib.h>
#include <pbnjson.h>

/*
 * Builds the a{sv} property dict connman's Manager.Create() / one round of
 * Connection.SetProperty("Properties", ...) expects, from an addProfile /
 * updateProfile request payload.
 *
 * type may be NULL (updateProfile does not resend Type - connman treats it as
 * immutable in practice and vpn_provider_set_string() re-setting it on update
 * would be a pointless write, so it is deliberately left out of the batch
 * path). host/domain/name may be NULL if the caller does not want to
 * (re)send them. vpn_form_fields is the request's vpnProfile.vpnFormFields
 * array, or NULL.
 *
 * Every property connman actually reads for a VPN connection is D-Bus type
 * string, per vpn/vpn-provider.c's set_provider_property() - the two
 * exceptions (SplitRouting: boolean, UserRoutes: array) are not part of
 * vpnFormFields and are handled separately by setSplitRouting. So every
 * connmanProperty field value is added here as a plain string; a field whose
 * value is empty or absent is skipped rather than sent as "" (an explicit
 * empty string clears the property on the SetProperty path, which is not
 * always what an unfilled optional field means - see the .c file for the
 * full reasoning).
 *
 * Returns a floating a{sv} GVariant reference the caller passes straight to
 * connman_vpn_client_create_connection() / _set_properties(), or NULL if
 * nothing at all was set for the field or you passed a form fields array
 * where connmanProperty entries were unrecognised (see the .c file - this
 * can't happen from a well-formed request, so it is not treated as an error).
 */
GVariant *profile_build_properties(const char *type, const char *name,
                                   const char *host, const char *domain,
                                   jvalue_ref vpn_form_fields);

#endif

/* vim:ts=4:sw=4:noexpandtab */
