// dbapi-plugin logging shim — sets the library name and pulls in the shared
// logging helper (patches/common/log.h). Per-TU includes are `#include
// "log.h"`; set LOG_TAG before including to label the line.
//
// This one is not a libpatch-*.so preload but a jciDbapi plugin
// (libdbapiaamod.so). It keeps the same helper so its lines land in the
// device log in the same greppable shape as the shims'; stderr of the
// jciDbapi launcher PID is captured by sm exactly like the others.

#ifndef LIBPATCH_DBAPI_AAMOD_LOG_H
#define LIBPATCH_DBAPI_AAMOD_LOG_H

#define LIBPATCH_NAME "dbapiaamod"
#include "common/log.h"

#endif // LIBPATCH_DBAPI_AAMOD_LOG_H
