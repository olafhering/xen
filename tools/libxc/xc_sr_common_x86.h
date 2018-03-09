#ifndef __COMMON_X86__H
#define __COMMON_X86__H

#include "xc_sr_common.h"

/*
 * Obtains a domains TSC information from Xen and writes a TSC_INFO
 * and a record VTSC_TOLERANCE_KHZ into the stream.
 */
int write_tsc(struct xc_sr_context *ctx);

/*
 * Parses a VTSC_TOLERANCE_KHZ record and applies the result to the domain.
 */
int handle_vtsc_tolerance_khz(struct xc_sr_context *ctx, struct xc_sr_record *rec);

/*
 * Parses a TSC_INFO record and applies the result to the domain.
 */
int handle_tsc_info(struct xc_sr_context *ctx, struct xc_sr_record *rec);

#endif
/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
