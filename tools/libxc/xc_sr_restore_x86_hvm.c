#include <assert.h>
#include <arpa/inet.h>

#include "xc_sr_common_x86.h"

/*
 * Process an HVM_CONTEXT record from the stream.
 */
static int handle_hvm_context(struct xc_sr_context *ctx,
                              struct xc_sr_record *rec)
{
    xc_interface *xch = ctx->xch;
    void *p;

    p = malloc(rec->length);
    if ( !p )
    {
        ERROR("Unable to allocate %u bytes for hvm context", rec->length);
        return -1;
    }

    free(ctx->x86_hvm.restore.context);

    ctx->x86_hvm.restore.context = memcpy(p, rec->data, rec->length);
    ctx->x86_hvm.restore.contextsz = rec->length;

    return 0;
}

/*
 * Process an HVM_PARAMS record from the stream.
 */
static int handle_hvm_params(struct xc_sr_context *ctx,
                             struct xc_sr_record *rec)
{
    xc_interface *xch = ctx->xch;
    struct xc_sr_rec_hvm_params *hdr = rec->data;
    struct xc_sr_rec_hvm_params_entry *entry = hdr->param;
    unsigned int i;
    int rc;

    if ( rec->length < sizeof(*hdr) )
    {
        ERROR("HVM_PARAMS record truncated: length %u, header size %zu",
              rec->length, sizeof(*hdr));
        return -1;
    }

    if ( rec->length != (sizeof(*hdr) + hdr->count * sizeof(*entry)) )
    {
        ERROR("HVM_PARAMS record truncated: header %zu, count %u, "
              "expected len %zu, got %u",
              sizeof(*hdr), hdr->count, hdr->count * sizeof(*entry),
              rec->length);
        return -1;
    }

    /*
     * Tolerate empty records.  Older sending sides used to accidentally
     * generate them.
     */
    if ( hdr->count == 0 )
    {
        DBGPRINTF("Skipping empty HVM_PARAMS record\n");
        return 0;
    }

    for ( i = 0; i < hdr->count; i++, entry++ )
    {
        switch ( entry->index )
        {
        case HVM_PARAM_CONSOLE_PFN:
            ctx->restore.console_gfn = entry->value;
            xc_clear_domain_page(xch, ctx->domid, entry->value);
            break;
        case HVM_PARAM_STORE_PFN:
            ctx->restore.xenstore_gfn = entry->value;
            xc_clear_domain_page(xch, ctx->domid, entry->value);
            break;
        case HVM_PARAM_IOREQ_PFN:
        case HVM_PARAM_BUFIOREQ_PFN:
            xc_clear_domain_page(xch, ctx->domid, entry->value);
            break;
        }

        rc = xc_hvm_param_set(xch, ctx->domid, entry->index, entry->value);
        if ( rc < 0 )
        {
            PERROR("set HVM param %"PRId64" = 0x%016"PRIx64,
                   entry->index, entry->value);
            return rc;
        }
    }
    return 0;
}

/* restore_ops function. */
static bool x86_hvm_pfn_is_valid(const struct xc_sr_context *ctx, xen_pfn_t pfn)
{
    return true;
}

/* restore_ops function. */
static xen_pfn_t x86_hvm_pfn_to_gfn(const struct xc_sr_context *ctx,
                                    xen_pfn_t pfn)
{
    return pfn;
}

/* restore_ops function. */
static void x86_hvm_set_gfn(struct xc_sr_context *ctx, xen_pfn_t pfn,
                            xen_pfn_t gfn)
{
    /* no op */
}

/* restore_ops function. */
static void x86_hvm_set_page_type(struct xc_sr_context *ctx,
                                  xen_pfn_t pfn, xen_pfn_t type)
{
    /* no-op */
}

/* restore_ops function. */
static int x86_hvm_localise_page(struct xc_sr_context *ctx,
                                 uint32_t type, void *page)
{
    /* no-op */
    return 0;
}

/*
 * restore_ops function. Confirms the stream matches the domain.
 */
static int x86_hvm_setup(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;
    struct xc_sr_bitmap *bm;
    unsigned long bits;

    if ( ctx->restore.guest_type != DHDR_TYPE_X86_HVM )
    {
        ERROR("Unable to restore %s domain into an x86_hvm domain",
              dhdr_type_to_str(ctx->restore.guest_type));
        return -1;
    }
    else if ( ctx->restore.guest_page_size != PAGE_SIZE )
    {
        ERROR("Invalid page size %u for x86_hvm domains",
              ctx->restore.guest_page_size);
        return -1;
    }

    bm = &ctx->x86_hvm.restore.attempted_1g;
    bits = (ctx->restore.p2m_size >> SUPERPAGE_1GB_SHIFT) + 1;
    if ( xc_sr_bitmap_resize(bm, bits) == false )
        goto out;

    bm = &ctx->x86_hvm.restore.attempted_2m;
    bits = (ctx->restore.p2m_size >> SUPERPAGE_2MB_SHIFT) + 1;
    if ( xc_sr_bitmap_resize(bm, bits) == false )
        goto out;

    bm = &ctx->x86_hvm.restore.allocated_pfns;
    bits = ctx->restore.p2m_size + 1;
    if ( xc_sr_bitmap_resize(bm, bits) == false )
        goto out;

    /* No superpage in 1st 2MB due to VGA hole */
    xc_sr_set(0, &ctx->x86_hvm.restore.attempted_1g);
    xc_sr_set(0, &ctx->x86_hvm.restore.attempted_2m);

    return 0;

out:
    ERROR("Unable to allocate memory for pfn bitmaps");
    return -1;
}

/*
 * restore_ops function.
 */
static int x86_hvm_process_record(struct xc_sr_context *ctx,
                                  struct xc_sr_record *rec)
{
    switch ( rec->type )
    {
    case REC_TYPE_TSC_INFO:
        return handle_tsc_info(ctx, rec);

    case REC_TYPE_HVM_CONTEXT:
        return handle_hvm_context(ctx, rec);

    case REC_TYPE_HVM_PARAMS:
        return handle_hvm_params(ctx, rec);

    default:
        return RECORD_NOT_PROCESSED;
    }
}

/*
 * restore_ops function.  Sets extra hvm parameters and seeds the grant table.
 */
static int x86_hvm_stream_complete(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;
    int rc;

    rc = xc_hvm_param_set(xch, ctx->domid, HVM_PARAM_STORE_EVTCHN,
                          ctx->restore.xenstore_evtchn);
    if ( rc )
    {
        PERROR("Failed to set HVM_PARAM_STORE_EVTCHN");
        return rc;
    }

    rc = xc_hvm_param_set(xch, ctx->domid, HVM_PARAM_CONSOLE_EVTCHN,
                          ctx->restore.console_evtchn);
    if ( rc )
    {
        PERROR("Failed to set HVM_PARAM_CONSOLE_EVTCHN");
        return rc;
    }

    rc = xc_domain_hvm_setcontext(xch, ctx->domid,
                                  ctx->x86_hvm.restore.context,
                                  ctx->x86_hvm.restore.contextsz);
    if ( rc < 0 )
    {
        PERROR("Unable to restore HVM context");
        return rc;
    }

    rc = xc_dom_gnttab_hvm_seed(xch, ctx->domid,
                                ctx->restore.console_gfn,
                                ctx->restore.xenstore_gfn,
                                ctx->restore.console_domid,
                                ctx->restore.xenstore_domid);
    if ( rc )
    {
        PERROR("Failed to seed grant table");
        return rc;
    }

    return rc;
}

static int x86_hvm_cleanup(struct xc_sr_context *ctx)
{
    free(ctx->x86_hvm.restore.context);
    xc_sr_bitmap_free(&ctx->x86_hvm.restore.attempted_1g);
    xc_sr_bitmap_free(&ctx->x86_hvm.restore.attempted_2m);
    xc_sr_bitmap_free(&ctx->x86_hvm.restore.allocated_pfns);

    return 0;
}

/*
 * Set a pfn as allocated, expanding the tracking structures if needed.
 */
static int pfn_set_allocated(struct xc_sr_context *ctx, xen_pfn_t pfn)
{
    xc_interface *xch = ctx->xch;

    if ( !xc_sr_set(pfn, &ctx->x86_hvm.restore.allocated_pfns) )
    {
        ERROR("Failed to realloc allocated_pfns bitmap");
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

static bool x86_hvm_do_superpage(struct xc_sr_context *ctx, unsigned int order)
{
    unsigned long count = 1ULL << order;
    return ctx->restore.tot_pages + count <= ctx->restore.max_pages;
}

/*
 * Attempt to allocate a superpage where the pfn resides.
 */
static int x86_hvm_allocate_pfn(struct xc_sr_context *ctx, xen_pfn_t pfn)
{
    xc_interface *xch = ctx->xch;
    struct xc_sr_bitmap *bm;
    bool success = false;
    int rc = -1, done;
    unsigned int order, shift;
    unsigned long i;
    unsigned long idx_1g, idx_2m;
    unsigned long count;
    xen_pfn_t base_pfn = 0, extnt;

    if ( xc_sr_test(pfn, &ctx->x86_hvm.restore.allocated_pfns) )
        return 0;

    idx_1g = pfn >> SUPERPAGE_1GB_SHIFT;
    idx_2m = pfn >> SUPERPAGE_2MB_SHIFT;
    if ( !xc_sr_bitmap_resize(&ctx->x86_hvm.restore.attempted_1g, idx_1g) )
    {
        PERROR("Failed to realloc attempted_1g");
        return -1;
    }
    if ( !xc_sr_bitmap_resize(&ctx->x86_hvm.restore.attempted_2m, idx_2m) )
    {
        PERROR("Failed to realloc attempted_2m");
        return -1;
    }

    /*
     * Try to allocate a 1GB page for this pfn, but avoid Over-allocation.
     * If this succeeds, mark the range of 2MB pages as busy.
     */
    bm = &ctx->x86_hvm.restore.attempted_1g;
    order = SUPERPAGE_1GB_SHIFT;
    if ( !xc_sr_test_and_set(idx_1g, bm) && x86_hvm_do_superpage(ctx, order) ) {
        count = 1UL << order;
        base_pfn = (pfn >> order) << order;
        extnt = base_pfn;
        done = xc_domain_populate_physmap(xch, ctx->domid, 1, order, 0, &extnt);
        if ( done > 0 ) {
            DPRINTF("1G base_pfn %" PRI_xen_pfn "\n", base_pfn);
            success = true;
            bm = &ctx->x86_hvm.restore.attempted_2m;
            shift = SUPERPAGE_1GB_SHIFT - SUPERPAGE_2MB_SHIFT;
            for ( i = 0; i < (count >> shift); i++ )
                xc_sr_set((base_pfn >> SUPERPAGE_2MB_SHIFT) + i, bm);
        } else if ( done < 0 ) {
            PERROR("populate_physmap failed.");
            return -1;
        }
    }

    /* Allocate a 2MB page if the above failed, avoid Over-allocation. */
    bm = &ctx->x86_hvm.restore.attempted_2m;
    order = SUPERPAGE_2MB_SHIFT;
    if ( !xc_sr_test_and_set(idx_2m, bm) && x86_hvm_do_superpage(ctx, order) ) {
        count = 1UL << order;
        base_pfn = (pfn >> order) << order;
        extnt = base_pfn;
        done = xc_domain_populate_physmap(xch, ctx->domid, 1, order, 0, &extnt);
        if ( done > 0 ) {
            DPRINTF("2M base_pfn %" PRI_xen_pfn "\n", base_pfn);
            success = true;
        } else if ( done < 0 ) {
            PERROR("populate_physmap failed.");
            return -1;
        }
    }
    if ( success == false ) {
        count = 1;
        extnt = base_pfn = pfn;
        done = xc_domain_populate_physmap(xch, ctx->domid, count, 0, 0, &extnt);
        if ( done > 0 ) {
            DPRINTF("4K pfn %" PRI_xen_pfn "\n", pfn);
            success = true;
        } else if ( done < 0 ) {
            PERROR("populate_physmap failed.");
            return -1;
        }
    }
    if ( success == true ) {
        do {
            count--;
            ctx->restore.tot_pages++;
            rc = pfn_set_allocated(ctx, base_pfn + count);
            if ( rc )
                break;
        } while ( count );
    }
    return rc;
}

static int x86_hvm_populate_pfns(struct xc_sr_context *ctx, unsigned count,
                                 const xen_pfn_t *original_pfns,
                                 const uint32_t *types)
{
    xc_interface *xch = ctx->xch;
    xen_pfn_t min_pfn = original_pfns[0], max_pfn = original_pfns[0];
    unsigned i;
    int rc = -1;

    for ( i = 0; i < count; ++i )
    {
        if ( original_pfns[i] < min_pfn )
            min_pfn = original_pfns[i];
        if ( original_pfns[i] > max_pfn )
            max_pfn = original_pfns[i];
        if ( (types[i] != XEN_DOMCTL_PFINFO_XTAB &&
              types[i] != XEN_DOMCTL_PFINFO_BROKEN) &&
             !pfn_is_populated(ctx, original_pfns[i]) )
        {
            rc = x86_hvm_allocate_pfn(ctx, original_pfns[i]);
            if ( rc )
                goto err;
            rc = pfn_set_populated(ctx, original_pfns[i]);
            if ( rc )
                goto err;
        }
    }

    while ( min_pfn < max_pfn )
    {
        if ( !xc_sr_bitmap_resize(&ctx->x86_hvm.restore.allocated_pfns, min_pfn) )
        {
            PERROR("Failed to realloc allocated_pfns %" PRI_xen_pfn, min_pfn);
            goto err;
        }
        if ( !pfn_is_populated(ctx, min_pfn) &&
            xc_sr_test_and_clear(min_pfn, &ctx->x86_hvm.restore.allocated_pfns) ) {
            xen_pfn_t pfn = min_pfn;
            rc = xc_domain_decrease_reservation_exact(xch, ctx->domid, 1, 0, &pfn);
            if ( rc )
            {
                PERROR("Failed to release pfn %" PRI_xen_pfn, min_pfn);
                goto err;
            }
            ctx->restore.tot_pages--;
        }
        min_pfn++;
    }

    rc = 0;

 err:
    return rc;
}


struct xc_sr_restore_ops restore_ops_x86_hvm =
{
    .pfn_is_valid    = x86_hvm_pfn_is_valid,
    .pfn_to_gfn      = x86_hvm_pfn_to_gfn,
    .set_gfn         = x86_hvm_set_gfn,
    .set_page_type   = x86_hvm_set_page_type,
    .localise_page   = x86_hvm_localise_page,
    .setup           = x86_hvm_setup,
    .populate_pfns   = x86_hvm_populate_pfns,
    .process_record  = x86_hvm_process_record,
    .stream_complete = x86_hvm_stream_complete,
    .cleanup         = x86_hvm_cleanup,
};

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
