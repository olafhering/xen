#include "xc_sr_common_x86.h"

static int write_vtsc_tolerance_khz(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;
    struct xc_sr_rec_vtsc_tolerance_khz tsc = { 0 };
    struct xc_sr_record rec =
    {
        .type = REC_TYPE_VTSC_TOLERANCE_KHZ,
        .length = sizeof(tsc),
        .data = &tsc
    };

    if ( xc_domain_get_vtsc_tolerance_khz(xch, ctx->domid, &tsc.tolerance) < 0 )
    {
        PERROR("Unable to obtain vTSC information");
        return -1;
    }

    return write_record(ctx, &rec);
}

static int write_tsc_info(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;
    struct xc_sr_rec_tsc_info tsc = { 0 };
    struct xc_sr_record rec =
    {
        .type = REC_TYPE_TSC_INFO,
        .length = sizeof(tsc),
        .data = &tsc
    };

    if ( xc_domain_get_tsc_info(xch, ctx->domid, &tsc.mode,
                                &tsc.nsec, &tsc.khz, &tsc.incarnation) < 0 )
    {
        PERROR("Unable to obtain TSC information");
        return -1;
    }

    return write_record(ctx, &rec);
}

int write_tsc(struct xc_sr_context *ctx)
{
    int rc;

    /* Send tolerance before tsc_info */
    rc = write_vtsc_tolerance_khz(ctx);
    if ( rc )
        return rc;

    return write_tsc_info(ctx);
}

int handle_tsc_info(struct xc_sr_context *ctx, struct xc_sr_record *rec)
{
    xc_interface *xch = ctx->xch;
    struct xc_sr_rec_tsc_info *tsc = rec->data;

    if ( rec->length != sizeof(*tsc) )
    {
        ERROR("TSC_INFO record wrong size: length %u, expected %zu",
              rec->length, sizeof(*tsc));
        return -1;
    }

    if ( xc_domain_set_tsc_info(xch, ctx->domid, tsc->mode,
                                tsc->nsec, tsc->khz, tsc->incarnation) )
    {
        PERROR("Unable to set TSC information");
        return -1;
    }

    return 0;
}

int handle_vtsc_tolerance_khz(struct xc_sr_context *ctx, struct xc_sr_record *rec)
{
    xc_interface *xch = ctx->xch;
    struct xc_sr_rec_vtsc_tolerance_khz *tsc = rec->data;

    if ( rec->length != sizeof(*tsc) )
    {
        ERROR("VTSC_TOLERANCE_KHZ record wrong size: length %u, expected %zu",
              rec->length, sizeof(*tsc));
        return -1;
    }

    if ( xc_domain_set_vtsc_tolerance_khz(xch, ctx->domid, tsc->tolerance) )
    {
        PERROR("Unable to set vTSC information");
        return -1;
    }

    return 0;
}


/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
