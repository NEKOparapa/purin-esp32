# Opus dependency

This component uses `78/esp-opus` version 1.0.5 through ESP-IDF Component Manager.
The library contains the Xiph.Org Opus implementation. Its original `COPYING`
file and patent-license references remain in the managed dependency; consult
that file for redistribution terms. The dependency is not copied into this
component, and its license is not replaced by the registry's package label.

The Ogg writer in this directory is project code implementing RFC 3533 and
RFC 7845. Encoder delay is queried at runtime, not assumed from a particular
Opus version.
