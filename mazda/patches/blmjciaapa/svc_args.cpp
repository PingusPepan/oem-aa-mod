// SPDX-License-Identifier: AGPL-3.0-or-later
//
// libpatch-blmjciaapa.so — SM_SVC_GetServiceArgs override (wireless Android Auto).
//
// blmjciaapa.so reads its launch arguments with SM_SVC_GetServiceArgs and
// selects its attribute/certificate set on "new_hw". The symbol is imported,
// so this preloaded definition replaces it. The logic is in common/svc_args.h.
// With `wireless = false` it returns the real arguments unchanged.

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE          // dladdr() (used by config::load via svc_args.h)
#endif

#include "log.h"
#include "common/svc_args.h"

extern "C" PRELOAD_EXPORT const char *SM_SVC_GetServiceArgs(void *ctx)
{
    return libpatch_svcargs::get_service_args(
        ctx, reinterpret_cast<void *>(&SM_SVC_GetServiceArgs));
}
