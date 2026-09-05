luneos-vpn-adapter
==================

Summary
-------
Bridges connman-vpnd onto the luna-service2 bus as `com.webos.service.vpn`.

Description
-----------

LuneOS VPN service. Bridges `connman-vpnd` (`net.connman.vpn` on the system bus)
onto the luna-service2 bus as `com.webos.service.vpn`, with `com.palm.vpn` kept
as an alias.

Why this exists
---------------

Legacy webOS shipped a VPN framework daemon, `PmVpnDaemon`, with a `dlopen`
plugin ABI and two plugins: vpnc and a proprietary Cisco AnyConnect blob. It
hand-rolled everything downstream of the tunnel - `netroute/addNetIf`,
`firewall/ctrl`, DNS, crash recovery - because webOS had no connection manager
underneath it.

LuneOS has one. connman-vpnd owns the provider configuration, spawns the
transport, and hands the tunnel to connmand as an ordinary service of type
`vpn`, so routing, DNS, teardown on carrier loss and reconnect all come for
free. This service is only the LS2 face of that: it translates, and holds no
state of its own.

The API deliberately keeps the legacy method names and error numbering, because
they map onto connman's VPN D-Bus API almost one for one and because UI ported
from the 2011 app branches on the error codes. See the `VPN/` design notes.

What it does not do
-------------------

It stores nothing. connman-vpnd already persists provider settings under
`/var/lib/connman-vpn/<ident>/settings` (root, 0600) and re-supplies saved
credentials itself, so there is no second secret store here and no equivalent
of the legacy `com.palm.vpn.crypt` keymanager path.

Layout
------

    src/connman_vpn_client.c   net.connman.vpn: name watch, connection cache, signals
    src/vpn_service.c          the LS2 methods
    src/vpn_providers.c        which connman VPN plugins are installed, and their form fields
    src/gvariant_json.c        GVariant -> pbnjson
    files/xml/connman-vpn.xml  D-Bus introspection, fed to gdbus-codegen
    files/formfields/*.json    per-provider vpnFormFields descriptors, installed as data

The form field descriptors are data rather than compiled-in tables so a field
can be corrected without rebuilding the service.

Status
------

Read-only so far: `getStatus`, `getProfileList`, `getProfileDetails`,
`getConnectionDetails`, `getAgents`, `getAgentFormFields`, all subscribable
where it makes sense. The write path (`connect`, `disconnect`, `addProfile`,
`updateProfile`, `deleteProfile`) and the `net.connman.vpn.Agent` credential
prompt flow behind `uiPromptResponse` are next.
