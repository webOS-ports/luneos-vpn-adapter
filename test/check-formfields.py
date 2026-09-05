#!/usr/bin/env python3
"""
Check every connmanProperty in files/formfields/*.json against the property
names the matching connman VPN plugin actually reads.

A typo here is silent: connman keeps unknown properties in its settings file and
simply never passes them to the client, so the field would appear in the UI, be
saved, and do nothing. That is worth a test.

Usage:
    check-formfields.py <path to connman source tree>

e.g. .../connman/2.0/sources/connman-2.0
"""

import glob
import json
import os
import re
import sys

# Second dots are real: VPNC.IPSec.ID, VPNC.Xauth.Username, VPNC.IKE.DHGroup.
PROPERTY_RE = re.compile(
    r'"((?:OpenVPN|VPNC|L2TP|PPTP|PPPD|WireGuard|OpenConnect|VPN)\.[A-Za-z0-9.]+)"')


def plugin_properties(connman_source, plugin):
    path = os.path.join(connman_source, "vpn", "plugins", plugin + ".c")

    try:
        with open(path) as handle:
            return set(PROPERTY_RE.findall(handle.read()))
    except OSError:
        return None


def collect(fields, found):
    """Walk a vpnFormFields array, including the container widget types."""
    for field in fields:
        if not isinstance(field, dict):
            continue

        if "connmanProperty" in field:
            found.add(field["connmanProperty"])

        # A listselector whose choices map onto more than one property.
        for mapping in field.get("connmanPropertyMap", {}).values():
            found.update(mapping.keys())

        collect(field.get("vpnFormFields", []), found)

        for group in field.get("groups", []):
            collect(group.get("vpnFormFields", []), found)


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    connman_source = sys.argv[1]
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "..", "files", "formfields")

    failures = 0
    checked = 0

    for path in sorted(glob.glob(os.path.join(here, "*.json"))):
        with open(path) as handle:
            descriptor = json.load(handle)

        plugin = descriptor["connmanType"]
        used = set()

        collect(descriptor.get("vpnFormFields", []), used)

        # Credentials the agent supplies at connect time rather than fields the
        # user edits, but they are still real property names.
        for field in descriptor.get("runtimeCredentialFields", []):
            if "connmanProperty" in field:
                used.add(field["connmanProperty"])

        real = plugin_properties(connman_source, plugin)

        if real is None:
            print("%-18s SKIP  no %s.c in %s" %
                  (os.path.basename(path), plugin, connman_source))
            continue

        unknown = sorted(name for name in used if name not in real)
        checked += len(used)

        if unknown:
            failures += len(unknown)
            print("%-18s FAIL  %d/%d unknown" %
                  (os.path.basename(path), len(unknown), len(used)))
            for name in unknown:
                print("                       %s" % name)
        else:
            print("%-18s ok    %d properties" %
                  (os.path.basename(path), len(used)))

    print("\n%d properties checked, %d unknown" % (checked, failures))

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
