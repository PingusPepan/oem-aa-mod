// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef LIBPATCH_AAP_SERVICE_NET_SOCKBUF_H
#define LIBPATCH_AAP_SERVICE_NET_SOCKBUF_H

namespace aap_service_net {

// Logs that the fix is active. The fix itself is the setsockopt() override.
void init();

} // namespace aap_service_net

#endif // LIBPATCH_AAP_SERVICE_NET_SOCKBUF_H
