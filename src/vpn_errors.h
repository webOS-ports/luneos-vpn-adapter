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

#ifndef VPN_ERRORS_H_
#define VPN_ERRORS_H_

/*
 * Error codes are carried over verbatim from the legacy webOS com.palm.vpn
 * service. The 2011 VPN app branched on -5 and -7 to decide *not* to show an
 * error dialog, and any UI ported from it will do the same, so the numbering is
 * part of the contract rather than an implementation detail.
 */
#define VPN_ERR_UNKNOWN				(-1)	/* internal / unclassified */
#define VPN_ERR_INVALID_PARAMS		(-2)
#define VPN_ERR_PROFILE_NOT_FOUND	(-3)
#define VPN_ERR_PROFILE_EXISTS		(-4)
#define VPN_ERR_PROMPT_CANCELLED	(-5)	/* net.connman.vpn.Agent.Error.Canceled */
#define VPN_ERR_AGENT_FAILURE		(-6)
#define VPN_ERR_NEEDS_AUTH			(-7)	/* prompt raised, connect unresolved */
#define VPN_ERR_NO_BACKEND			(-8)	/* connman-vpnd is not on the bus */
#define VPN_ERR_UNSUPPORTED_TYPE	(-9)	/* no such connman VPN plugin installed */
#define VPN_ERR_IMMUTABLE			(-10)	/* provisioned by a .config file */

#endif

/* vim:ts=4:sw=4:noexpandtab */
