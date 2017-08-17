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
    xc_sr_set_bit(0, &ctx->x86_hvm.restore.attempted_1g);
    xc_sr_set_bit(0, &ctx->x86_hvm.restore.attempted_2m);

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

    if ( !xc_sr_set_bit(pfn, &ctx->x86_hvm.restore.allocated_pfns) )
    {
        ERROR("Failed to realloc allocated_pfns bitmap");
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

struct x86_hvm_sp {
    xen_pfn_t pfn;
    xen_pfn_t base_pfn;
    unsigned long index;
    unsigned long count;
};

/*
 * Try to allocate a 1GB page for this pfn, but avoid Over-allocation.
 * If this succeeds, mark the range of 2MB pages as busy.
 */
static bool x86_hvm_alloc_1g(struct xc_sr_context *ctx, struct x86_hvm_sp *sp)
{
    xc_interface *xch = ctx->xch;
    struct xc_sr_bitmap *bm;
    unsigned int order, shift;
    int i, done;
    xen_pfn_t extent;

    bm = &ctx->x86_hvm.restore.attempted_1g;

    /* Only one attempt to avoid overlapping allocation */
    if ( xc_sr_test_and_set_bit(sp->index, bm) )
        return false;

    order = SUPERPAGE_1GB_SHIFT;
    sp->count = 1ULL << order;

    /* Allocate only if there is room for another superpage */
    if ( ctx->restore.tot_pages + sp->count > ctx->restore.max_pages )
        return false;

    extent = sp->base_pfn = (sp->pfn >> order) << order;
    done = xc_domain_populate_physmap(xch, ctx->domid, 1, order, 0, &extent);
    if ( done < 0 ) {
        PERROR("populate_physmap failed.");
        return false;
    }
    if ( done == 0 )
        return false;

    DPRINTF("1G base_pfn %" PRI_xen_pfn "\n", sp->base_pfn);

    /* Mark all 2MB pages as done to avoid overlapping allocation */
    bm = &ctx->x86_hvm.restore.attempted_2m;
    shift = SUPERPAGE_1GB_SHIFT - SUPERPAGE_2MB_SHIFT;
    for ( i = 0; i < (sp->count >> shift); i++ )
        xc_sr_set_bit((sp->base_pfn >> SUPERPAGE_2MB_SHIFT) + i, bm);

    return true;
}

/* Allocate a 2MB page if x86_hvm_alloc_1g failed, avoid Over-allocation. */
static bool x86_hvm_alloc_2m(struct xc_sr_context *ctx, struct x86_hvm_sp *sp)
{
    xc_interface *xch = ctx->xch;
    struct xc_sr_bitmap *bm;
    unsigned int order;
    int done;
    xen_pfn_t extent;

    bm = &ctx->x86_hvm.restore.attempted_2m;

    /* Only one attempt to avoid overlapping allocation */
    if ( xc_sr_test_and_set_bit(sp->index, bm) )
        return false;

    order = SUPERPAGE_2MB_SHIFT;
    sp->count = 1ULL << order;

    /* Allocate only if there is room for another superpage */
    if ( ctx->restore.tot_pages + sp->count > ctx->restore.max_pages )
        return false;

    extent = sp->base_pfn = (sp->pfn >> order) << order;
    done = xc_domain_populate_physmap(xch, ctx->domid, 1, order, 0, &extent);
    if ( done < 0 ) {
        PERROR("populate_physmap failed.");
        return false;
    }
    if ( done == 0 )
        return false;

    DPRINTF("2M base_pfn %" PRI_xen_pfn "\n", sp->base_pfn);
    return true;
}

/* Allocate a single page if x86_hvm_alloc_2m failed. */
static bool x86_hvm_alloc_4k(struct xc_sr_context *ctx, struct x86_hvm_sp *sp)
{
    xc_interface *xch = ctx->xch;
    unsigned int order;
    int done;
    xen_pfn_t extent;

    order = 0;
    sp->count = 1ULL << order;

    /* Allocate only if there is room for another page */
    if ( ctx->restore.tot_pages + sp->count > ctx->restore.max_pages )
        return false;

    extent = sp->base_pfn = (sp->pfn >> order) << order;
    done = xc_domain_populate_physmap(xch, ctx->domid, 1, order, 0, &extent);
    if ( done < 0 ) {
        PERROR("populate_physmap failed.");
        return false;
    }
    if ( done == 0 )
        return false;

    DPRINTF("4K base_pfn %" PRI_xen_pfn "\n", sp->base_pfn);
    return true;
}
/*
 * Attempt to allocate a superpage where the pfn resides.
 */
static int x86_hvm_allocate_pfn(struct xc_sr_context *ctx, xen_pfn_t pfn)
{
    xc_interface *xch = ctx->xch;
    bool success;
    int rc = -1;
    unsigned long idx_1g, idx_2m;
    struct x86_hvm_sp sp = {
        .pfn = pfn
    };

    if ( xc_sr_test_bit(pfn, &ctx->x86_hvm.restore.allocated_pfns) )
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

    sp.index = idx_1g;
    success = x86_hvm_alloc_1g(ctx, &sp);

    if ( success == false ) {
        sp.index = idx_2m;
        success = x86_hvm_alloc_2m(ctx, &sp);
    }

    if ( success == false ) {
        sp.index = 0;
        success = x86_hvm_alloc_4k(ctx, &sp);
    }

    if ( success == true ) {
        do {
            sp.count--;
            ctx->restore.tot_pages++;
            rc = pfn_set_allocated(ctx, sp.base_pfn + sp.count);
            if ( rc )
                break;
        } while ( sp.count );
    }
    return rc;
}

static int x86_hvm_populate_pfns(struct xc_sr_context *ctx, unsigned count,
                                 const xen_pfn_t *original_pfns,
                                 const uint32_t *types)
{
    xc_interface *xch = ctx->xch;
    xen_pfn_t pfn, min_pfn = original_pfns[0], max_pfn = original_pfns[0];
    unsigned i, freed = 0, order;
    int rc = -1;

    for ( i = 0; i < count; ++i )
    {
        if ( original_pfns[i] < min_pfn )
            min_pfn = original_pfns[i];
        if ( original_pfns[i] > max_pfn )
            max_pfn = original_pfns[i];
    }
    DPRINTF("batch of %u pfns between %" PRI_xen_pfn " %" PRI_xen_pfn "\n",
            count, min_pfn, max_pfn);

    for ( i = 0; i < count; ++i )
    {
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

    /*
     * Scan the entire superpage because several batches will fit into
     * a superpage, and it is unknown which pfn triggered the allocation.
     */
    order = SUPERPAGE_1GB_SHIFT;
    pfn = min_pfn = (min_pfn >> order) << order;

    while ( pfn <= max_pfn )
    {
        struct xc_sr_bitmap *bm;
        bm = &ctx->x86_hvm.restore.allocated_pfns;
        if ( !xc_sr_bitmap_resize(bm, pfn) )
        {
            PERROR("Failed to realloc allocated_pfns %" PRI_xen_pfn, pfn);
            goto err;
        }
        if ( !pfn_is_populated(ctx, pfn) &&
            xc_sr_test_and_clear_bit(pfn, bm) ) {
            xen_pfn_t p = pfn;
            rc = xc_domain_decrease_reservation_exact(xch, ctx->domid, 1, 0, &p);
            if ( rc )
            {
                PERROR("Failed to release pfn %" PRI_xen_pfn, pfn);
                goto err;
            }
            ctx->restore.tot_pages--;
            freed++;
        }
        pfn++;
    }
    if ( freed )
        DPRINTF("freed %u between %" PRI_xen_pfn " %" PRI_xen_pfn "\n",
                freed, min_pfn, max_pfn);

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
