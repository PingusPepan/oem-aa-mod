// The slice of jansson we need, bound at runtime.
//
// The plugin exchanges json_t with jciDbapi, but the m3-toolchain sysroot has no
// libjansson to link against — the device has it (/usr/lib/libjansson.so.0) and
// svcjcidbapi.so imports it, so the code is mapped in the process; the only
// question is whether we can see it.
//
// dlsym(RTLD_DEFAULT) alone is not enough: it searches the global scope, and
// sm_svclauncher dlopen()s svcjcidbapi.so with RTLD_LOCAL, so libjansson sits in a
// local scope. api() therefore falls back to dlopen()ing the soname — see the
// note there; it is load-bearing.
//
// Nothing is linked, so the .so keeps no undefined jansson symbols and still
// builds with -Wl,--no-undefined.

#ifndef OEM_AA_MOD_JANSSON_RT_H
#define OEM_AA_MOD_JANSSON_RT_H

#include "log.h"
#include <dlfcn.h>
#include <stddef.h>

// json_decref() is a static inline in jansson.h, so libjansson does not export it.
// json_t is { uint32_t type; size_t refcount; }, with (size_t)-1 marking an
// immortal object (json_true/json_false return those, so that check matters).
// decref() below mirrors jansson's inline json_decref().
struct json_t {
    unsigned int type;
    size_t       refcount;
};

namespace jansson {

// jansson 1.x signatures (the device ships libjansson.so.0, i.e. 1.x: no
// json_pack/json_loadb, and json_integer takes an int); only what the plugin calls.
struct Api {
    json_t     *(*object)(void);
    json_t     *(*array)(void);
    json_t     *(*string)(const char *);
    json_t     *(*integer)(int);
    json_t     *(*boolean_true)(void);
    json_t     *(*boolean_false)(void);
    int         (*object_set_new)(json_t *, const char *, json_t *);
    int         (*array_append_new)(json_t *, json_t *);
    json_t     *(*object_get)(const json_t *, const char *);
    const char *(*string_value)(const json_t *);
    void        (*delete_)(json_t *);   // json_delete: the decref primitive
};

// Bound once, all-or-nothing: a missing symbol leaves the API unusable and the
// plugin refuses to initialise (see DBAPI_PluginInit).
inline const Api *api()
{
    static Api  a;
    static bool tried  = false;
    static bool usable = false;

    if (tried) {
        return usable ? &a : NULL;
    }
    tried = true;

    struct Bind { void **slot; const char *sym; };
    const Bind binds[] = {
        { (void **)&a.object,           "json_object"           },
        { (void **)&a.array,            "json_array"            },
        { (void **)&a.string,           "json_string"           },
        { (void **)&a.integer,          "json_integer"          },
        { (void **)&a.boolean_true,     "json_true"             },
        { (void **)&a.boolean_false,    "json_false"            },
        { (void **)&a.object_set_new,   "json_object_set_new"   },
        { (void **)&a.array_append_new, "json_array_append_new" },
        { (void **)&a.object_get,       "json_object_get"       },
        { (void **)&a.string_value,     "json_string_value"     },
        { (void **)&a.delete_,          "json_delete"           },
    };

    // One scope for all symbols, never a mix.
    const size_t nbinds = sizeof(binds) / sizeof(binds[0]);
    struct Try {
        static bool all(void *scope, const Bind *b, size_t n)
        {
            for (size_t i = 0; i < n; ++i) {
                *b[i].slot = dlsym(scope, b[i].sym);
                if (*b[i].slot == NULL) {
                    LOGD("jansson: %s not in this scope", b[i].sym);
                    return false;
                }
            }
            return true;
        }
    };

    // 1. The global scope as it stands (works if jansson is already global).
    if (Try::all(RTLD_DEFAULT, binds, nbinds)) {
        LOGD("jansson: bound from the existing global scope");
        usable = true;
        return &a;
    }

    // 2. dlopen the soname: finds the copy already mapped (same soname =>
    //    refcount++) and RTLD_GLOBAL promotes it into the global scope.
    //    LOAD-BEARING — inside jciDbapi step 1 fails and this is what works.
    //    Do not drop it because step 1 succeeded somewhere.
    void *h = dlopen("libjansson.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (h == NULL) {
        LOGD("jansson: dlopen(libjansson.so.0) failed: %s", dlerror());
        h = dlopen("libjansson.so", RTLD_NOW | RTLD_GLOBAL);   // tree with a dev symlink
        if (h == NULL) {
            LOGD("jansson: dlopen(libjansson.so) failed: %s", dlerror());
        }
    }
    if (h != NULL && Try::all(h, binds, nbinds)) {
        LOGD("jansson: bound via dlopen(libjansson.so.0)");
        usable = true;
        return &a;
    }

    LOGE("jansson: not resolvable in this process (tried the global scope and "
         "dlopen of libjansson.so.0) — plugin inert");
    return NULL;
}

// json_object_set_new() steals the value reference while the host's
// json_object_set() takes its own, so after handing a tree to the callback we
// still own exactly one reference — to the root, and only the root.
inline void decref(json_t *j)
{
    if (j == NULL) {
        return;
    }
    if (j->refcount == (size_t)-1) {
        return;   // immortal singleton (json_true/json_false/json_null)
    }
    if (--j->refcount == 0) {
        const Api *a = api();
        if (a != NULL) {
            a->delete_(j);
        }
    }
}

} // namespace jansson

#endif // OEM_AA_MOD_JANSSON_RT_H
