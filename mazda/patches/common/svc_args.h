// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Wireless Android Auto: service launch arguments.
//
// The service manager starts each service through sm_svclauncher, and the
// service reads its launch arguments with SM_SVC_GetServiceArgs(ctx). Units
// with factory wireless projection boot /jci/sm/sm_WCP.conf instead of
// sm.conf, which gives jciAAPA the argument "new_hw": load
// /etc/aap_system_attributes.xml, the wireless-capable attribute/certificate
// set. With `wireless = true`, the override returns "new_hw" for jciAAPA when
// the OEM passed no arguments, so sm.conf does not have to be changed.
//
// sm_WCP.conf also sets "wcp" on blm_nms_svc, which makes NMS run its own
// access point. That is not needed: start-aa-ap.sh runs the AP.
//
// Header-only. The exported SM_SVC_GetServiceArgs must exist once per .so, so
// it lives in a per-patch .cpp (patches/blmjciaapa/svc_args.cpp). Include the
// patch's own log.h first, and define _GNU_SOURCE before the first system
// header (config::load() uses dladdr()).

#ifndef LIBPATCH_COMMON_SVC_ARGS_H
#define LIBPATCH_COMMON_SVC_ARGS_H

#include "log.h"
#include "common/config.h"
#include "common/preload.h"

#include <string.h>

// Fixed log tag for the inline bodies (ODR-safe), restored at the end. Same as
// common/config.h.
#pragma push_macro("LOG_TAG")
#undef LOG_TAG
#define LOG_TAG "WIRELESS"

namespace libpatch_svcargs {

// Argument to supply per service, or nullptr to leave the service alone. The
// stock sm.conf gives jciAAPA no arguments, so no merging is needed.
inline const char *arg_for_service(const char *name)
{
    if (name == nullptr) {
        return nullptr;
    }
    if (strcmp(name, "jciAAPA") == 0) {
        return "new_hw";
    }
    return nullptr;
}

// SM_SVC_GetServiceArgs / SM_SVC_GetServiceName.
typedef const char *(*sm_str_fn)(void *ctx);

// Body of the override. `own_shim` is the exported function's address, so
// resolve_real_symbol never resolves to ourselves.
//
// Returns the real arguments unless wireless is enabled, the real arguments
// are empty, and the service is listed in arg_for_service(). The real function
// is in the launcher's global scope, so RTLD_NEXT finds it. If it cannot be
// resolved, the arguments are treated as empty (the stock value).
inline const char *get_service_args(void *ctx, void *own_shim)
{
    // Idempotent; finds libpatch.conf next to this .so.
    libpatch_config::load(own_shim);

    static void *args_cache = nullptr;
    sm_str_fn real_args = reinterpret_cast<sm_str_fn>(resolve_real_symbol(
        "SM_SVC_GetServiceArgs", nullptr, nullptr, own_shim, &args_cache));
    const char *real = real_args ? real_args(ctx) : nullptr;

    if (!libpatch_config::wireless_enabled()) {
        return real;
    }
    if (real && *real) {
        return real;                                  // never override OEM args
    }

    // Not overridden by us, so no self-exclusion is needed.
    static void *name_cache = nullptr;
    sm_str_fn real_name = reinterpret_cast<sm_str_fn>(resolve_real_symbol(
        "SM_SVC_GetServiceName", nullptr, nullptr, nullptr, &name_cache));
    const char *name = real_name ? real_name(ctx) : nullptr;

    const char *inj = arg_for_service(name);
    if (inj == nullptr) {
        return real;
    }

    if (real_args == nullptr) {
        LOGE("svc_args: real SM_SVC_GetServiceArgs unresolved; returning \"%s\" "
             "for \"%s\"", inj, name ? name : "(null)");
    } else {
        LOGE("svc_args: returning \"%s\" for \"%s\"", inj, name ? name : "(null)");
    }
    return inj;
}

} // namespace libpatch_svcargs

#pragma pop_macro("LOG_TAG")

#endif  // LIBPATCH_COMMON_SVC_ARGS_H
