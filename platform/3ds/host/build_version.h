#pragma once

// The port's version: in the log's first lines (main.c), and the CIA's version, read by the
// developer's CIA script and the builder (tools/beta/galaxyctr_builder.py, port_version).
// Raised for every CIA.
#define SMG_CIA_VERSION_MAJOR 0
#define SMG_CIA_VERSION_MINOR 21
#define SMG_CIA_VERSION_MICRO 4
#define SMG_STRINGIFY_INNER(value) #value
#define SMG_STRINGIFY(value) SMG_STRINGIFY_INNER(value)
#define SMG_CIA_VERSION SMG_STRINGIFY(SMG_CIA_VERSION_MAJOR) "." SMG_STRINGIFY(SMG_CIA_VERSION_MINOR) "." SMG_STRINGIFY(SMG_CIA_VERSION_MICRO)
