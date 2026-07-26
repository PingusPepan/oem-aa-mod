// jciDbapi plugin ABI.
//
// jciDbapi (/jci/dbapi/svcjcidbapi.so) is the GUI's method-call backend: Opera
// holds a WebSocket to ws://127.0.0.1:2766 and calls
// framework.sendRequestToDbapi(uiaId, cb, api, method, params). It is a plugin
// host — reads /jci/dbapi/plugins.xml and dlopen()s
// /jci/dbapi/plugins/libdbapi<name>.so for each <plugin name="<name>"/>, then
// dlsym()s DBAPI_PluginInit / DBAPI_PluginDestroy. `api` picks the plugin by that
// name, `method` an entry in the table Init returns.
//
// How the host drives a plugin:
//
//   Init          called with a pointer to receive the method table, then six
//                 words: the response callback and five watchdog helpers.
//                 Returns 100 on success; the OEM plugins return 120 when the
//                 callback is NULL.
//   method table  {name, fn} pairs, terminated by a NULL name.
//   method call   fn(reqId, params).
//   response      the callback sends {msgType:"methodResponse", msgContent:{api,
//                 methodName, reqId, params}} to the GUI; it takes its own
//                 reference to params.
//
// Methods run on the host's single plugin-manager thread, one at a time, and only
// after Init succeeded — so blocking I/O is fine and a method may answer inline.

#ifndef OEM_AA_MOD_DBAPI_PLUGIN_H
#define OEM_AA_MOD_DBAPI_PLUGIN_H

#include "jansson_rt.h"

namespace dbapi {

// Init status codes the host checks.
enum Status {
    kOk       = 100,
    kInitFail = 120,   // what the OEM plugins return for a NULL callback
};

// One entry of the table Init hands back. NULL-terminated array.
struct Method {
    const char *name;
    void      (*fn)(int reqId, json_t *params);
};

// The host's "here is your answer" callback. `params` is
// borrowed: the host takes its own reference via json_object_set, so the
// caller still owns — and must release — the object it passed in.
typedef void (*MethodCb)(const char *api, int reqId, const char *methodName,
                         json_t *params);

// Watchdog helpers the host also passes. A plugin with no threads of its own has
// nothing to feed and ignores them, as the OEM plugins do.
typedef void *(*WdmCreateSubscriberFn)(void);
typedef int   (*WdmSubscribeFn)(void *);
typedef int   (*WdmUnsubscribeFn)(void *);
typedef int   (*WdmTickleFn)(void *);
typedef int   (*WdmLocalTickleFn)(void *);

} // namespace dbapi

#endif // OEM_AA_MOD_DBAPI_PLUGIN_H
