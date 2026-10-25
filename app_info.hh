#pragma once

// Program name and version of this fork, and where it keeps its settings. The fork
// installs and stores settings under its own name so it can sit beside upstream modem73.
// APP_VERSION and MODEM73_VERSION (the upstream base) are normally set by the Makefile.

#ifndef APP_NAME
#define APP_NAME "modem73fm"
#endif
#ifndef APP_TITLE
#define APP_TITLE "MODEM73FM"
#endif
#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif
#ifndef MODEM73_VERSION
#define MODEM73_VERSION "dev"
#endif

// "modem73fm 0.1 (modem73 2.4.2)"
#define APP_VERSION_STRING APP_NAME " " APP_VERSION " (modem73 " MODEM73_VERSION ")"

// Settings directory, relative to $HOME
#define APP_CONFIG_SUBDIR "/.config/" APP_NAME
