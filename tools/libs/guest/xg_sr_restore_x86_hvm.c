#include <assert.h>
#include <arpa/inet.h>

#include "xg_sr_common_x86.h"

/*
 * Process an HVM_CONTEXT record from the stream.
 */
static int handle_hvm_context(struct xc_sr_context *ctx,
                              struct xc_sr_record *rec)
{
    xc_interface *xch = ctx->xch;
    int rc = update_blob(&ctx->x86.hvm.restore.context, rec->data, rec->length);

    if ( rc )
        ERROR("Unable to allocate %u bytes for hvm context", rec->length);

    return rc;
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

        case HVM_PARAM_PAE_ENABLED:
            /*
             * This HVM_PARAM only ever existed to pass data into
             * xc_cpuid_apply_policy().  The function has now been updated to
             * use a normal calling convention, making the param obsolete.
             *
             * Discard if we find it in an old migration stream.
             */
            continue;
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

static bool x86_hvm_expand_sp_bitmaps(struct xc_sr_context *ctx, unsigned long max_pfn)
{
    struct sr_bitmap *bm;

    bm = &ctx->x86.hvm.restore.attempted_1g;
    if ( !sr_bitmap_expand(bm, max_pfn >> SUPERPAGE_1GB_SHIFT) )
        return false;

    bm = &ctx->x86.hvm.restore.attempted_2m;
    if ( !sr_bitmap_expand(bm, max_pfn >> SUPERPAGE_2MB_SHIFT) )
        return false;

    bm = &ctx->x86.hvm.restore.allocated_pfns;
    if ( !sr_bitmap_expand(bm, max_pfn) )
        return false;

    return true;
}

static void x86_hvm_no_superpage(struct xc_sr_context *ctx, unsigned long addr)
{
    unsigned long pfn = addr >> XC_PAGE_SHIFT;

    sr_set_bit(pfn >> SUPERPAGE_1GB_SHIFT, &ctx->x86.hvm.restore.attempted_1g);
    sr_set_bit(pfn >> SUPERPAGE_2MB_SHIFT, &ctx->x86.hvm.restore.attempted_2m);
}

/*
 * restore_ops function. Confirms the stream matches the domain.
 */
static int x86_hvm_setup(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;
    unsigned long max_pfn;

    if ( ctx->restore.guest_type != DHDR_TYPE_X86_HVM )
    {
        ERROR("Unable to restore %s domain into an x86 HVM domain",
              dhdr_type_to_str(ctx->restore.guest_type));
        return -1;
    }

    if ( ctx->restore.guest_page_size != PAGE_SIZE )
    {
        ERROR("Invalid page size %u for x86 HVM domains",
              ctx->restore.guest_page_size);
        return -1;
    }

#ifdef __i386__
    /* Very large domains (> 1TB) will exhaust virtual address space. */
    if ( ctx->restore.p2m_size > 0x0fffffff )
    {
        errno = E2BIG;
        PERROR("Cannot restore this big a guest");
        return -1;
    }
#endif

    max_pfn = max(ctx->restore.p2m_size, ctx->dominfo.max_pages);
    if ( !sr_bitmap_expand(&ctx->restore.populated_pfns, max_pfn) )
        goto out;

    if ( !x86_hvm_expand_sp_bitmaps(ctx, max_pfn) )
        goto out;

    /* FIXME: distinguish between PVH and HVM */
    /* No superpage in 1st 2MB due to VGA hole */
    x86_hvm_no_superpage(ctx, 0xA0000u);
#define LAPIC_BASE_ADDRESS  0xfee00000u
#define ACPI_INFO_PHYSICAL_ADDRESS 0xfc000000u
    x86_hvm_no_superpage(ctx, LAPIC_BASE_ADDRESS);
    x86_hvm_no_superpage(ctx, ACPI_INFO_PHYSICAL_ADDRESS);

    return 0;

out:
    PERROR("Unable to allocate memory for pfn bitmaps");
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
    case REC_TYPE_X86_TSC_INFO:
        return handle_x86_tsc_info(ctx, rec);

    case REC_TYPE_HVM_CONTEXT:
        return handle_hvm_context(ctx, rec);

    case REC_TYPE_HVM_PARAMS:
        return handle_hvm_params(ctx, rec);

    case REC_TYPE_X86_CPUID_POLICY:
        return handle_x86_cpuid_policy(ctx, rec);

    case REC_TYPE_X86_MSR_POLICY:
        return handle_x86_msr_policy(ctx, rec);

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
                                  ctx->x86.hvm.restore.context.ptr,
                                  ctx->x86.hvm.restore.context.size);
    if ( rc < 0 )
    {
        PERROR("Unable to restore HVM context");
        return rc;
    }

    rc = xc_dom_gnttab_seed(xch, ctx->domid, true,
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
    sr_bitmap_free(&ctx->restore.populated_pfns);
    sr_bitmap_free(&ctx->x86.hvm.restore.attempted_1g);
    sr_bitmap_free(&ctx->x86.hvm.restore.attempted_2m);
    sr_bitmap_free(&ctx->x86.hvm.restore.allocated_pfns);
    free(ctx->x86.hvm.restore.context.ptr);

    free(ctx->x86.restore.cpuid.ptr);
    free(ctx->x86.restore.msr.ptr);

    return 0;
}

/*
 * Set a range of pfns as allocated
 */
static void pfn_set_long_allocated(struct xc_sr_context *ctx, xen_pfn_t base_pfn)
{
    sr_set_long_bit(base_pfn, &ctx->x86.hvm.restore.allocated_pfns);
}

static void pfn_set_allocated(struct xc_sr_context *ctx, xen_pfn_t pfn)
{
    sr_set_bit(pfn, &ctx->x86.hvm.restore.allocated_pfns);
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
    unsigned int order;
    int i, done;
    xen_pfn_t extent;

    /* Only one attempt to avoid overlapping allocation */
    if ( sr_test_and_set_bit(sp->index, &ctx->x86.hvm.restore.attempted_1g) )
        return false;

    order = SUPERPAGE_1GB_SHIFT;
    sp->count = SUPERPAGE_1GB_NR_PFNS;

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

    DPRINTF("1G %" PRI_xen_pfn "\n", sp->base_pfn);

    /* Mark all 2MB pages as done to avoid overlapping allocation */
    for ( i = 0; i < (SUPERPAGE_1GB_NR_PFNS/SUPERPAGE_2MB_NR_PFNS); i++ )
        sr_set_bit((sp->base_pfn >> SUPERPAGE_2MB_SHIFT) + i, &ctx->x86.hvm.restore.attempted_2m);

    return true;
}

/* Allocate a 2MB page if x86_hvm_alloc_1g failed, avoid Over-allocation. */
static bool x86_hvm_alloc_2m(struct xc_sr_context *ctx, struct x86_hvm_sp *sp)
{
    xc_interface *xch = ctx->xch;
    unsigned int order;
    int done;
    xen_pfn_t extent;

    /* Only one attempt to avoid overlapping allocation */
    if ( sr_test_and_set_bit(sp->index, &ctx->x86.hvm.restore.attempted_2m) )
        return false;

    order = SUPERPAGE_2MB_SHIFT;
    sp->count = SUPERPAGE_2MB_NR_PFNS;

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

    DPRINTF("2M %" PRI_xen_pfn "\n", sp->base_pfn);
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
    sp->count = 1UL;

    /* Allocate only if there is room for another page */
    if ( ctx->restore.tot_pages + sp->count > ctx->restore.max_pages ) {
        errno = E2BIG;
        return false;
    }

    extent = sp->base_pfn = (sp->pfn >> order) << order;
    done = xc_domain_populate_physmap(xch, ctx->domid, 1, order, 0, &extent);
    if ( done < 0 ) {
        PERROR("populate_physmap failed.");
        return false;
    }
    if ( done == 0 ) {
        errno = ENOMEM;
        return false;
    }

    DPRINTF("4K %" PRI_xen_pfn "\n", sp->base_pfn);
    return true;
}
/*
 * Attempt to allocate a superpage where the pfn resides.
 */
static int x86_hvm_allocate_pfn(struct xc_sr_context *ctx, xen_pfn_t pfn)
{
    bool success;
    unsigned long idx_1g, idx_2m;
    struct x86_hvm_sp sp = {
        .pfn = pfn
    };

    if ( sr_test_bit(pfn, &ctx->x86.hvm.restore.allocated_pfns) )
        return 0;

    idx_1g = pfn >> SUPERPAGE_1GB_SHIFT;
    idx_2m = pfn >> SUPERPAGE_2MB_SHIFT;

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

    if ( success == false )
        return -1;

    do {
        if ( sp.count >= BITS_PER_LONG && (sp.count % BITS_PER_LONG) == 0 ) {
            sp.count -= BITS_PER_LONG;
            ctx->restore.tot_pages += BITS_PER_LONG;
            pfn_set_long_allocated(ctx, sp.base_pfn + sp.count);
        } else {
            sp.count--;
            ctx->restore.tot_pages++;
            pfn_set_allocated(ctx, sp.base_pfn + sp.count);
        }
    } while ( sp.count );

    return 0;
}

/*
 * Deallocate memory.
 * There was likely an optimistic superpage allocation.
 * This means more pages may have been allocated past gap_end.
 * This range is not freed now. Incoming higher pfns will release it.
 */
static int x86_hvm_punch_hole(struct xc_sr_context *ctx,
                               xen_pfn_t gap_start, xen_pfn_t gap_end)
{
    xc_interface *xch = ctx->xch;
    xen_pfn_t _pfn, pfn;
    uint32_t domid, freed = 0;
    int rc;

    pfn = gap_start >> SUPERPAGE_1GB_SHIFT;
    do
    {
        sr_set_bit(pfn, &ctx->x86.hvm.restore.attempted_1g);
    } while (++pfn <= gap_end >> SUPERPAGE_1GB_SHIFT);

    pfn = gap_start >> SUPERPAGE_2MB_SHIFT;
    do
    {
        sr_set_bit(pfn, &ctx->x86.hvm.restore.attempted_2m);
    } while (++pfn <= gap_end >> SUPERPAGE_2MB_SHIFT);

    pfn = gap_start;

    while ( pfn <= gap_end )
    {
        if ( sr_test_and_clear_bit(pfn, &ctx->x86.hvm.restore.allocated_pfns) )
        {
            domid = ctx->domid;
            _pfn = pfn;
            rc = xc_domain_decrease_reservation_exact(xch, domid, 1, 0, &_pfn);
            if ( rc )
            {
                PERROR("Failed to release pfn %" PRI_xen_pfn, pfn);
                return -1;
            }
            ctx->restore.tot_pages--;
            freed++;
        }
        pfn++;
    }
    if ( freed )
        DPRINTF("freed %u between %" PRI_xen_pfn " %" PRI_xen_pfn "\n",
                freed, gap_start, gap_end);
    return 0;
}

static int x86_hvm_unpopulate_page(struct xc_sr_context *ctx, xen_pfn_t pfn)
{
    sr_clear_bit(pfn, &ctx->restore.populated_pfns);
    return x86_hvm_punch_hole(ctx, pfn, pfn);
}

static int x86_hvm_populate_page(struct xc_sr_context *ctx, xen_pfn_t pfn)
{
    xen_pfn_t gap_start, gap_end;
    bool has_gap, first_iteration;
    int rc;

    /*
     * Check for a gap between the previous populated pfn and this pfn.
     * In case a gap exists, it is required to punch a hole to release memory,
     * starting after the previous pfn and before this pfn.
     *
     * But: this can be done only during the first iteration, which is the
     * only place where superpage allocations are attempted. All following
     * iterations lack the info to properly maintain prev_populated_pfn.
     */
    has_gap = ctx->x86.hvm.restore.prev_populated_pfn + 1 < pfn;
    first_iteration = ctx->x86.hvm.restore.iteration == 0;
    if ( has_gap && first_iteration )
    {
        gap_start = ctx->x86.hvm.restore.prev_populated_pfn + 1;
        gap_end = pfn - 1;

        rc = x86_hvm_punch_hole(ctx, gap_start, gap_end);
        if ( rc )
            goto err;
    }

    rc = x86_hvm_allocate_pfn(ctx, pfn);
    if ( rc )
        goto err;
    pfn_set_populated(ctx, pfn);
    ctx->x86.hvm.restore.prev_populated_pfn = pfn;

    rc = 0;
err:
    return rc;
}

/*
 * Try to allocate superpages.
 * This works without memory map because the pfns arrive in incremental order.
 * All pfn numbers and their type are submitted.
 * Only pfns with data will have also pfn content transmitted.
 */
static int x86_hvm_populate_pfns(struct xc_sr_context *ctx, unsigned count,
                                 const xen_pfn_t *original_pfns,
                                 const uint32_t *types)
{
    xc_interface *xch = ctx->xch;
    xen_pfn_t pfn, min_pfn, max_pfn;
    bool to_populate, populated;
    unsigned i = count;
    int rc = 0;

    min_pfn = count ? original_pfns[0] : 0;
    max_pfn = count ? original_pfns[count - 1] : 0;
    DPRINTF("batch of %u pfns between %" PRI_xen_pfn " %" PRI_xen_pfn "\n",
            count, min_pfn, max_pfn);

    if ( !x86_hvm_expand_sp_bitmaps(ctx, max_pfn) )
    {
        ERROR("Unable to allocate memory for pfn bitmaps");
        return -1;
    }

    /*
     * There is no indicator for a new iteration.
     * Simulate it by checking if a lower pfn is coming in.
     * In the end it matters only to know if this iteration is the first one.
     */
    if ( min_pfn < ctx->x86.hvm.restore.iteration_tracker_pfn )
        ctx->x86.hvm.restore.iteration++;
    ctx->x86.hvm.restore.iteration_tracker_pfn = min_pfn;

    for ( i = 0; i < count; ++i )
    {
        pfn = original_pfns[i];

        to_populate = page_type_to_populate(types[i]);
        populated = pfn_is_populated(ctx, pfn);

        /*
         * page has data, pfn populated: nothing to do
         * page has data, pfn not populated: likely never seen before
         * page has no data, pfn populated: likely ballooned out during migration
         * page has no data, pfn not populated: nothing to do
         */
        if ( to_populate && !populated )
        {
            rc = x86_hvm_populate_page(ctx, pfn);
        } else if ( !to_populate && populated )
        {
            rc = x86_hvm_unpopulate_page(ctx, pfn);
        }
        if ( rc )
            break;
    }

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
    .static_data_complete = x86_static_data_complete,
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
