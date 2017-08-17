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
    void *p;
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

    /*
     * Preallocate array for holes.
     * Any size will do, the sender is free to send batches of arbitrary length.
     */
    bits = 16;
    p = calloc(bits, sizeof(*ctx->x86_hvm.restore.extents));
    if ( !p )
        goto out;
    ctx->x86_hvm.restore.extents = p;
    ctx->x86_hvm.restore.max_extents = bits;

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
    free(ctx->x86_hvm.restore.extents);
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

/* track allocation of a superpage */
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
    if ( done < 0 )
    {
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
    if ( done < 0 )
    {
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
    if ( done < 0 )
    {
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
        PERROR("Failed to realloc attempted_1g for pfn %" PRI_xen_pfn, pfn );
        return -1;
    }
    if ( !xc_sr_bitmap_resize(&ctx->x86_hvm.restore.attempted_2m, idx_2m) )
    {
        PERROR("Failed to realloc attempted_2m for pfn %" PRI_xen_pfn, pfn );
        return -1;
    }

    sp.index = idx_1g;
    success = x86_hvm_alloc_1g(ctx, &sp);

    if ( success == false )
    {
        sp.index = idx_2m;
        success = x86_hvm_alloc_2m(ctx, &sp);
    }

    if ( success == false )
    {
        sp.index = 0;
        success = x86_hvm_alloc_4k(ctx, &sp);
    }

    if ( success == true )
    {
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

/* Keep track of pfns that need to be released. */
static bool x86_hvm_stash_pfn(struct xc_sr_context *ctx, xen_pfn_t pfn)
{
    xc_interface *xch = ctx->xch;
    unsigned long idx = ctx->x86_hvm.restore.nr_extents;

    if ( idx > ctx->x86_hvm.restore.max_extents )
    {
        unsigned long max_extents = ctx->x86_hvm.restore.max_extents * 2;
        size_t size = sizeof(*ctx->x86_hvm.restore.extents) * max_extents;
        void *p = realloc(ctx->x86_hvm.restore.extents, size);

        if ( !p )
        {
            PERROR("Failed to realloc extents array %lu", max_extents);
            return false;
        }
        ctx->x86_hvm.restore.extents = p;
        ctx->x86_hvm.restore.max_extents = max_extents;
    }

    ctx->x86_hvm.restore.extents[idx] = pfn;
    ctx->x86_hvm.restore.nr_extents++;
    return true;
}

/*
 * Check if a range of pfns represents a contiguous superpage
 * Returns the number of contiguous pages
 */
static unsigned long x86_hvm_scan_2m(xen_pfn_t *pfns, unsigned long idx,
                                     unsigned long max)
{
    xen_pfn_t val = pfns[idx];
    unsigned long i = 0;

    /* First pfn was already checked */
    do {
        val++;
        i++;
        if ( pfns[idx + i] != val )
            break;
    } while ( i < SUPERPAGE_2MB_NR_PFNS );

    return i;
}

static bool x86_hvm_release_2m_sp(struct xc_sr_context *ctx, unsigned long idx)
{
    xc_interface *xch = ctx->xch;
    int rc;
    uint32_t domid = ctx->domid;
    unsigned int order = SUPERPAGE_2MB_SHIFT;
    xen_pfn_t base_pfn = ctx->x86_hvm.restore.extents[idx];

    DPRINTF("releasing 2mb at %" PRI_xen_pfn, base_pfn);
    rc = xc_domain_decrease_reservation_exact(xch, domid, 1, order, &base_pfn);
    if ( rc )
    {
        PERROR("Failed to release 2mb at %lx", idx);
        return false;
    }
    return true;
}

static bool x86_hvm_release_pages(struct xc_sr_context *ctx,
                                  unsigned long start,
                                  unsigned long idx)
{
    xc_interface *xch = ctx->xch;
    int rc;
    uint32_t domid = ctx->domid;
    unsigned int order = 0;
    unsigned long num = idx - start;
    xen_pfn_t *pfns = &ctx->x86_hvm.restore.extents[start];

    DPRINTF("releasing %lu 4k pages", num);
    rc = xc_domain_decrease_reservation_exact(xch, domid, num, order, pfns);
    if ( rc )
    {
        PERROR("Failed to release %lu pfns", num);
        return false;
    }
    return true;
}

/* Release pfns which are not populated. */
static bool x86_hvm_free_pfns(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;
    xen_pfn_t *pfns = ctx->x86_hvm.restore.extents;
    xen_pfn_t mask;
    unsigned long idx, start, num, max;

    max = ctx->x86_hvm.restore.nr_extents;
    if ( !max )
        return true;

    mask = (1UL << SUPERPAGE_2MB_SHIFT) - 1;
    idx = 0;
    start = 0;
    while ( idx < max )
    {
        /* This is the start of a 2M range, release as a single superpage */
        if ( (pfns[idx] & mask ) == 0 &&
             idx + SUPERPAGE_2MB_NR_PFNS <= max )
        {
            num = x86_hvm_scan_2m(pfns, idx, max);
            DPRINTF("found %lu pfns at %" PRI_xen_pfn, num, pfns[idx]);
            if ( num == SUPERPAGE_2MB_NR_PFNS )
            {
                /* Release range before this superpage */
                if ( (idx - start) > 0 &&
                     x86_hvm_release_pages(ctx, start, idx) == false )
                    return false;
                if ( x86_hvm_release_2m_sp(ctx, idx) == false )
                    return false;
                start = idx + num;
            }
            idx += num;
        }
        else
        {
            idx++;
        }
    }

    /* Release remaining pages, or everything if no superpage was found */
    if ( (idx - start) > 0 && x86_hvm_release_pages(ctx, start, idx) == false )
            return false;

    ctx->x86_hvm.restore.nr_extents = 0;
    return true;
}

static bool x86_hvm_punch_hole(struct xc_sr_context *ctx, xen_pfn_t max_pfn)
{
    xc_interface *xch = ctx->xch;
    struct xc_sr_bitmap *bm = &ctx->x86_hvm.restore.allocated_pfns;
    xen_pfn_t pfn, start_pfn;
    unsigned int freed = 0, order;

    /* Expand the bitmap to allow clearing bits up to max_pfn */
    if ( !xc_sr_bitmap_resize(bm, max_pfn) )
    {
        PERROR("Failed to realloc allocated_pfns %" PRI_xen_pfn, max_pfn);
        return false;
    }
    /*
     * Scan the entire superpage because several batches will fit into
     * a superpage, and it is unknown which pfn triggered the allocation.
     */
    order = SUPERPAGE_1GB_SHIFT;
    pfn = start_pfn = (max_pfn >> order) << order;

    while ( pfn <= max_pfn )
    {
        if ( !pfn_is_populated(ctx, pfn) &&
            xc_sr_test_and_clear_bit(pfn, bm) )
        {
            if ( x86_hvm_stash_pfn(ctx, pfn) == false )
                return false;
            ctx->restore.tot_pages--;
            freed++;
        }
        pfn++;
    }

    if ( freed )
    {
        DPRINTF("%u pages to be freed between %" PRI_xen_pfn " %" PRI_xen_pfn,
                freed, start_pfn, max_pfn);
        if ( x86_hvm_free_pfns(ctx) == false )
            return false;
    }

    return true;
}

/* Avoid allocating a superpage if a hole exists */
static bool x86_hvm_mark_hole_in_sp(struct xc_sr_context *ctx, xen_pfn_t pfn)
{
    xc_interface *xch = ctx->xch;
    struct xc_sr_bitmap *bm;
    unsigned long idx_1g, idx_2m;

    idx_1g = pfn >> SUPERPAGE_1GB_SHIFT;
    idx_2m = pfn >> SUPERPAGE_2MB_SHIFT;

    bm = &ctx->x86_hvm.restore.attempted_1g;
    if ( xc_sr_set_bit(idx_1g, bm) == false )
    {
        PERROR("Failed to realloc attempted_1g for pfn %" PRI_xen_pfn, pfn );
        return false;
    }

    bm = &ctx->x86_hvm.restore.attempted_2m;
    if ( xc_sr_set_bit(idx_2m, bm) == false )
    {
        PERROR("Failed to realloc attempted_2m for pfn %" PRI_xen_pfn, pfn );
        return false;
    }
    return true;
}

/*
 * Try to allocate superpages.
 * This works without memory map only if the pfns arrive in incremental order.
 */
static int x86_hvm_populate_pfns(struct xc_sr_context *ctx, unsigned int count,
                                 const xen_pfn_t *pfns, const uint32_t *types)
{
    xc_interface *xch = ctx->xch;
    xen_pfn_t pfn, min_pfn = pfns[0], max_pfn = pfns[0];
    xen_pfn_t idx1G, idx2M;
    unsigned int i, order;
    int rc = -1;

    /*
     * Analyze the array:
     * - to show statistics
     * - to indicate holes to the superpage allocator
     *   this would be more efficient with batches for 1G instead of 4M
     *   with 4M batches a 1G superpage might be allocated before a hole is seen
     */
    for ( i = 0; i < count; ++i )
    {
        if ( pfns[i] < min_pfn )
            min_pfn = pfns[i];
        if ( pfns[i] > max_pfn )
            max_pfn = pfns[i];

        switch (types[i]) {
            case XEN_DOMCTL_PFINFO_XTAB:
            case XEN_DOMCTL_PFINFO_BROKEN:
                if ( x86_hvm_mark_hole_in_sp(ctx, pfns[i]) == false )
                    goto err;
                break;
            default:
                break;
        }
    }
    DPRINTF("batch of %u pfns between %" PRI_xen_pfn " %" PRI_xen_pfn "\n",
            count, min_pfn, max_pfn);

    for ( i = 0; i < count; ++i )
    {
        pfn = pfns[i];
        idx1G = pfn >> SUPERPAGE_1GB_SHIFT;
        idx2M = pfn >> SUPERPAGE_2MB_SHIFT;

        /*
         * Handle batches smaller than 1GB.
         * If this pfn is in another 2MB superpage it is required to punch holes
         * to release memory, starting from the 1GB boundary up to the highest
         * pfn within the previous 2MB superpage.
         */
        if ( ctx->x86_hvm.restore.idx1G_prev == idx1G &&
             ctx->x86_hvm.restore.idx2M_prev == idx2M )
        {
            /* Same 2MB superpage, nothing to do */
        }
        else
        {
            /*
             * If this next pfn is within another 1GB or 2MB superpage it is
             * required to scan the entire previous superpage because there
             * might be holes between the last pfn and the end of the superpage
             * containing that pfn.
             */
            if ( ctx->x86_hvm.restore.idx1G_prev != idx1G )
            {
                order = SUPERPAGE_1GB_SHIFT;
                max_pfn = ((ctx->x86_hvm.restore.idx1G_prev + 1) << order) - 1;
            }
            else
            {
                order = SUPERPAGE_2MB_SHIFT;
                max_pfn = ((ctx->x86_hvm.restore.idx2M_prev + 1) << order) - 1;
            }

            if ( x86_hvm_punch_hole(ctx, max_pfn) == false )
                goto err;
        }

        if ( (types[i] != XEN_DOMCTL_PFINFO_XTAB &&
              types[i] != XEN_DOMCTL_PFINFO_BROKEN) &&
             !pfn_is_populated(ctx, pfn) )
        {
            rc = x86_hvm_allocate_pfn(ctx, pfn);
            if ( rc )
                goto err;
            rc = pfn_set_populated(ctx, pfn);
            if ( rc )
                goto err;
        }
        ctx->x86_hvm.restore.idx1G_prev = idx1G;
        ctx->x86_hvm.restore.idx2M_prev = idx2M;
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
