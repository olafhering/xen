#include <arpa/inet.h>

#include <assert.h>

#include "xg_sr_common.h"

/*
 * Read and validate the Image and Domain headers.
 */
static int read_headers(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;
    struct xc_sr_ihdr ihdr;
    struct xc_sr_dhdr dhdr;

    if ( read_exact(ctx->fd, &ihdr, sizeof(ihdr)) )
    {
        PERROR("Failed to read Image Header from stream");
        return -1;
    }

    ihdr.id      = ntohl(ihdr.id);
    ihdr.version = ntohl(ihdr.version);
    ihdr.options = ntohs(ihdr.options);

    if ( ihdr.marker != IHDR_MARKER )
    {
        ERROR("Invalid marker: Got 0x%016"PRIx64, ihdr.marker);
        return -1;
    }

    if ( ihdr.id != IHDR_ID )
    {
        ERROR("Invalid ID: Expected 0x%08x, Got 0x%08x", IHDR_ID, ihdr.id);
        return -1;
    }

    if ( ihdr.version < 2 || ihdr.version > 3 )
    {
        ERROR("Invalid Version: Expected 2 <= ver <= 3, Got %d",
              ihdr.version);
        return -1;
    }

    if ( ihdr.options & IHDR_OPT_BIG_ENDIAN )
    {
        ERROR("Unable to handle big endian streams");
        return -1;
    }

    ctx->restore.format_version = ihdr.version;

    if ( read_exact(ctx->fd, &dhdr, sizeof(dhdr)) )
    {
        PERROR("Failed to read Domain Header from stream");
        return -1;
    }

    ctx->restore.guest_type = dhdr.type;
    ctx->restore.guest_page_size = (1U << dhdr.page_shift);

    if ( dhdr.xen_major == 0 )
    {
        IPRINTF("Found %s domain, converted from legacy stream format",
                dhdr_type_to_str(dhdr.type));
        DPRINTF("  Legacy conversion script version %u", dhdr.xen_minor);
    }
    else
        IPRINTF("Found %s domain from Xen %u.%u",
                dhdr_type_to_str(dhdr.type), dhdr.xen_major, dhdr.xen_minor);
    return 0;
}

static int handle_static_data_end_v2(struct xc_sr_context *ctx)
{
    int rc = 0;

#if defined(__i386__) || defined(__x86_64__)
    xc_interface *xch = ctx->xch;
    /*
     * v2 compatibility only exists for x86 streams.  This is a bit of a
     * bodge, but it is less bad than duplicating handle_page_data() between
     * different architectures.
     */

    /* v2 compat.  Infer the position of STATIC_DATA_END. */
    if ( ctx->restore.format_version < 3 && !ctx->restore.seen_static_data_end )
    {
        rc = handle_static_data_end(ctx);
        if ( rc )
        {
            ERROR("Inferred STATIC_DATA_END record failed");
            goto err;
        }
        rc = -1;
    }

    if ( !ctx->restore.seen_static_data_end )
    {
        ERROR("No STATIC_DATA_END seen");
        goto err;
    }

    rc = 0;
err:
#endif

    return rc;
}

static bool verify_rec_page_hdr(struct xc_sr_context *ctx, uint32_t rec_length,
                                 struct xc_sr_rec_page_data_header *pages)
{
    xc_interface *xch = ctx->xch;
    bool ret = false;

    errno = EINVAL;

    if ( rec_length < sizeof(*pages) )
    {
        ERROR("PAGE_DATA record truncated: length %u, min %zu",
              rec_length, sizeof(*pages));
        goto err;
    }

    if ( !pages->count || pages->count > MAX_BATCH_SIZE )
    {
        ERROR("Unexpected pfn count %u in PAGE_DATA record", pages->count);
        goto err;
    }

    if ( rec_length < sizeof(*pages) + (pages->count * sizeof(uint64_t)) )
    {
        ERROR("PAGE_DATA record (length %u) too short to contain %u"
              " pfns worth of information", rec_length, pages->count);
        goto err;
    }

    ret = true;

err:
    return ret;
}

static bool verify_rec_page_pfns(struct xc_sr_context *ctx, uint32_t rec_length,
                                 struct xc_sr_rec_page_data_header *pages)
{
    xc_interface *xch = ctx->xch;
    uint32_t i, pages_of_data = 0;
    xen_pfn_t pfn;
    uint32_t type;
    bool ret = false;

    for ( i = 0; i < pages->count; ++i )
    {
        pfn = pages->pfn[i] & PAGE_DATA_PFN_MASK;
        if ( !ctx->restore.ops.pfn_is_valid(ctx, pfn) )
        {
            ERROR("pfn %#"PRIpfn" (index %u) outside domain maximum", pfn, i);
            goto err;
        }

        type = (pages->pfn[i] & PAGE_DATA_TYPE_MASK) >> 32;
        if ( !is_known_page_type(type) )
        {
            ERROR("Unknown type %#"PRIx32" for pfn %#"PRIpfn" (index %u)",
                  type, pfn, i);
            goto err;
        }

        if ( page_type_has_stream_data(type) )
            /* NOTAB and all L1 through L4 tables (including pinned) should
             * have a page worth of data in the record. */
            pages_of_data++;

        ctx->restore.pfns[i] = pfn;
        ctx->restore.types[i] = type;
    }

    if ( rec_length != (sizeof(*pages) +
                         (sizeof(uint64_t) * pages->count) +
                         (PAGE_SIZE * pages_of_data)) )
    {
        ERROR("PAGE_DATA record wrong size: length %u, expected "
              "%zu + %zu + %lu", rec_length, sizeof(*pages),
              (sizeof(uint64_t) * pages->count), (PAGE_SIZE * pages_of_data));
        goto err;
    }

    ret = true;

err:
    return ret;
}

/*
 * Populate pfns, if required
 * Fill guest_data with either mapped address or NULL
 * The caller must unmap guest_mapping
 */
static int map_guest_pages(struct xc_sr_context *ctx,
                           struct xc_sr_rec_page_data_header *pages)
{
    xc_interface *xch = ctx->xch;
    uint32_t i, p;
    int rc;

    rc = ctx->restore.ops.populate_pfns(ctx, pages->count, ctx->restore.pfns,
                                        ctx->restore.types);
    if ( rc )
    {
        ERROR("Failed to populate pfns for batch of %u pages", pages->count);
        goto err;
    }

    ctx->restore.nr_mapped_pages = 0;

    for ( i = 0; i < pages->count; i++ )
    {
        ctx->restore.ops.set_page_type(ctx, ctx->restore.pfns[i], ctx->restore.types[i]);

        if ( page_type_has_stream_data(ctx->restore.types[i]) == false )
        {
            ctx->restore.guest_data[i] = NULL;
            continue;
        }

        ctx->restore.mfns[ctx->restore.nr_mapped_pages++] = ctx->restore.ops.pfn_to_gfn(ctx, ctx->restore.pfns[i]);
    }

    /* Nothing to do? */
    if ( ctx->restore.nr_mapped_pages == 0 )
        goto done;

    ctx->restore.guest_mapping = xenforeignmemory_map(xch->fmem, ctx->domid,
            PROT_READ | PROT_WRITE, ctx->restore.nr_mapped_pages,
            ctx->restore.mfns, ctx->restore.map_errs);
    if ( !ctx->restore.guest_mapping )
    {
        rc = -1;
        PERROR("Unable to map %u mfns for %u pages of data",
               ctx->restore.nr_mapped_pages, pages->count);
        goto err;
    }

    /* Verify mapping, and assign address to pfn data */
    for ( i = 0, p = 0; i < pages->count; i++ )
    {
        if ( !page_type_has_stream_data(ctx->restore.types[i]) )
            continue;

        if ( ctx->restore.map_errs[p] == 0 )
        {
            ctx->restore.guest_data[i] = ctx->restore.guest_mapping + (p * PAGE_SIZE);
            p++;
            continue;
        }

        errno = ctx->restore.map_errs[p];
        rc = -1;
        PERROR("Mapping pfn %#"PRIpfn" (mfn %#"PRIpfn", type %#"PRIx32") failed",
              ctx->restore.pfns[i], ctx->restore.mfns[p], ctx->restore.types[i]);
        goto err;
    }

done:
    rc = 0;

err:
    return rc;
}

/*
 * Handle PAGE_DATA record from the stream.
 * Given a list of pfns, their types, and a block of page data from the
 * stream, populate and record their types, map the relevant subset and copy
 * the data into the guest.
 */
static int handle_incoming_page_data(struct xc_sr_context *ctx,
                                     struct xc_sr_rhdr *rhdr)
{
    xc_interface *xch = ctx->xch;
    struct xc_sr_rec_page_data_header *pages = ctx->restore.pages;
    uint64_t *pfn_nums = &pages->pfn[0];
    uint32_t i;
    int rc, iov_idx;

    rc = handle_static_data_end_v2(ctx);
    if ( rc )
        goto err;

    /* First read and verify the header */
    rc = read_exact(ctx->fd, pages, sizeof(*pages));
    if ( rc )
    {
        PERROR("Could not read rec_pfn header");
        goto err;
    }

    if ( !verify_rec_page_hdr(ctx, rhdr->length, pages) )
    {
        rc = -1;
        goto err;
    }

    /* Then read and verify the incoming pfn numbers */
    rc = read_exact(ctx->fd, pfn_nums, sizeof(*pfn_nums) * pages->count);
    if ( rc )
    {
        PERROR("Could not read rec_pfn data");
        goto err;
    }

    if ( !verify_rec_page_pfns(ctx, rhdr->length, pages) )
    {
        rc = -1;
        goto err;
    }

    /* Finally read and verify the incoming pfn data */
    rc = map_guest_pages(ctx, pages);
    if ( rc )
        goto err;

    /* Prepare read buffers, either guest or throw-away memory */
    for ( i = 0, iov_idx = 0; i < pages->count; i++ )
    {
        struct iovec *iov;

        if ( !ctx->restore.guest_data[i] )
            continue;

        iov = &ctx->restore.iov[iov_idx];
        iov->iov_len = PAGE_SIZE;
        if ( ctx->restore.verify )
            iov->iov_base = ctx->restore.verify_buf + (i * PAGE_SIZE);
        else
            iov->iov_base = ctx->restore.guest_data[i];
        iov_idx++;
    }

    if ( !iov_idx )
        goto done;

    rc = readv_exact(ctx->fd, ctx->restore.iov, iov_idx);
    if ( rc )
    {
        PERROR("read of %d pages failed", iov_idx);
        goto err;
    }

    /* Post-processing of pfn data */
    for ( i = 0, iov_idx = 0; i < pages->count; i++ )
    {
        void *addr;

        if ( !ctx->restore.guest_data[i] )
            continue;

        addr = ctx->restore.iov[iov_idx].iov_base;
        rc = ctx->restore.ops.localise_page(ctx, ctx->restore.types[i], addr);
        if ( rc )
        {
            ERROR("Failed to localise pfn %#"PRIpfn" (type %#"PRIx32")",
                  ctx->restore.pfns[i],
                  ctx->restore.types[i] >> XEN_DOMCTL_PFINFO_LTAB_SHIFT);
            goto err;

        }

        if ( ctx->restore.verify )
        {
            if ( memcmp(ctx->restore.guest_data[i], addr, PAGE_SIZE) )
            {
                ERROR("verify pfn %#"PRIpfn" failed (type %#"PRIx32")",
                      ctx->restore.pfns[i],
                      ctx->restore.types[i] >> XEN_DOMCTL_PFINFO_LTAB_SHIFT);
            }
        }

        iov_idx++;
    }

done:
    rc = 0;

err:
    if ( ctx->restore.guest_mapping )
    {
        xenforeignmemory_unmap(xch->fmem, ctx->restore.guest_mapping, ctx->restore.nr_mapped_pages);
        ctx->restore.guest_mapping = NULL;
    }
    return rc;
}

/*
 * Handle PAGE_DATA record from an existing buffer
 * Given a list of pfns, their types, and a block of page data from the
 * stream, populate and record their types, map the relevant subset and copy
 * the data into the guest.
 */
static int handle_buffered_page_data(struct xc_sr_context *ctx,
                                     struct xc_sr_record *rec)
{
    xc_interface *xch = ctx->xch;
    struct xc_sr_rec_page_data_header *pages = rec->data;
    void *p;
    uint32_t i;
    int rc = -1, idx;

    rc = handle_static_data_end_v2(ctx);
    if ( rc )
        goto err;

    /* First read and verify the header */
    if ( !verify_rec_page_hdr(ctx, rec->length, pages) )
    {
        rc = -1;
        goto err;
    }

    /* Then read and verify the pfn numbers */
    if ( !verify_rec_page_pfns(ctx, rec->length, pages) )
    {
        rc = -1;
        goto err;
    }

    /* Map the target pfn */
    rc = map_guest_pages(ctx, pages);
    if ( rc )
        goto err;

    for ( i = 0, idx = 0; i < pages->count; i++ )
    {
        if ( !ctx->restore.guest_data[i] )
            continue;

        p = &pages->pfn[pages->count] + (idx * PAGE_SIZE);
        rc = ctx->restore.ops.localise_page(ctx, ctx->restore.types[i], p);
        if ( rc )
        {
            ERROR("Failed to localise pfn %#"PRIpfn" (type %#"PRIx32")",
                  ctx->restore.pfns[i], ctx->restore.types[i] >> XEN_DOMCTL_PFINFO_LTAB_SHIFT);
            goto err;

        }

        if ( ctx->restore.verify )
        {
            if ( memcmp(ctx->restore.guest_data[i], p, PAGE_SIZE) )
            {
                errno = EIO;
                ERROR("verify pfn %#"PRIpfn" failed (type %#"PRIx32")",
                      ctx->restore.pfns[i], ctx->restore.types[i] >> XEN_DOMCTL_PFINFO_LTAB_SHIFT);
                goto err;
            }
        }
        else
        {
            memcpy(ctx->restore.guest_data[i], p, PAGE_SIZE);
        }

        idx++;
    }

    rc = 0;

 err:
    if ( ctx->restore.guest_mapping )
    {
        xenforeignmemory_unmap(xch->fmem, ctx->restore.guest_mapping, ctx->restore.nr_mapped_pages);
        ctx->restore.guest_mapping = NULL;
    }
    return rc;
}

/*
 * Send checkpoint dirty pfn list to primary.
 */
static int send_checkpoint_dirty_pfn_list(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;
    int rc = -1;
    unsigned int count, written;
    uint64_t i, *pfns = NULL;
    xc_shadow_op_stats_t stats = { 0, ctx->restore.p2m_size };
    struct xc_sr_record rec = {
        .type = REC_TYPE_CHECKPOINT_DIRTY_PFN_LIST,
    };
    struct iovec iov[2] = {
        { &rec, sizeof(rec) },
    };
    DECLARE_HYPERCALL_BUFFER_SHADOW(unsigned long, dirty_bitmap,
                                    &ctx->restore.dirty_bitmap_hbuf);

    if ( xc_logdirty_control(
             xch, ctx->domid, XEN_DOMCTL_SHADOW_OP_CLEAN,
             HYPERCALL_BUFFER(dirty_bitmap), ctx->restore.p2m_size,
             0, &stats) != ctx->restore.p2m_size )
    {
        PERROR("Failed to retrieve logdirty bitmap");
        goto err;
    }

    for ( i = 0, count = 0; i < ctx->restore.p2m_size; i++ )
    {
        if ( test_bit(i, dirty_bitmap) )
            count++;
    }


    pfns = malloc(count * sizeof(*pfns));
    if ( !pfns )
    {
        ERROR("Unable to allocate %zu bytes of memory for dirty pfn list",
              count * sizeof(*pfns));
        goto err;
    }

    for ( i = 0, written = 0; i < ctx->restore.p2m_size; ++i )
    {
        if ( !test_bit(i, dirty_bitmap) )
            continue;

        if ( written > count )
        {
            ERROR("Dirty pfn list exceed");
            goto err;
        }

        pfns[written++] = i;
    }

    rec.length = count * sizeof(*pfns);

    iov[1].iov_base = pfns;
    iov[1].iov_len = rec.length;

    if ( writev_exact(ctx->restore.send_back_fd, iov, ARRAY_SIZE(iov)) )
    {
        PERROR("Failed to write dirty bitmap to stream");
        goto err;
    }

    rc = 0;
 err:
    free(pfns);
    return rc;
}

static int process_buffered_record(struct xc_sr_context *ctx, struct xc_sr_record *rec);
static int handle_checkpoint(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;
    int rc = 0, ret;
    unsigned int i;

    if ( ctx->stream_type == XC_STREAM_PLAIN )
    {
        ERROR("Found checkpoint in non-checkpointed stream");
        rc = -1;
        goto err;
    }

    ret = ctx->restore.callbacks->checkpoint(ctx->restore.callbacks->data);
    switch ( ret )
    {
    case XGR_CHECKPOINT_SUCCESS:
        break;

    case XGR_CHECKPOINT_FAILOVER:
        if ( ctx->restore.buffer_all_records )
            rc = BROKEN_CHANNEL;
        else
            /* We don't have a consistent state */
            rc = -1;
        goto err;

    default: /* Other fatal error */
        rc = -1;
        goto err;
    }

    if ( ctx->restore.buffer_all_records )
    {
        IPRINTF("All records buffered");

        for ( i = 0; i < ctx->restore.buffered_rec_num; i++ )
        {
            rc = process_buffered_record(ctx, &ctx->restore.buffered_records[i]);
            if ( rc )
                goto err;
        }
        ctx->restore.buffered_rec_num = 0;
        IPRINTF("All records processed");
    }
    else
        ctx->restore.buffer_all_records = true;

    if ( ctx->stream_type == XC_STREAM_COLO )
    {
#define HANDLE_CALLBACK_RETURN_VALUE(ret)                   \
    do {                                                    \
        if ( ret == 1 )                                     \
            rc = 0; /* Success */                           \
        else                                                \
        {                                                   \
            if ( ret == 2 )                                 \
                rc = BROKEN_CHANNEL;                        \
            else                                            \
                rc = -1; /* Some unspecified error */       \
            goto err;                                       \
        }                                                   \
    } while (0)

        /* COLO */

        /* We need to resume guest */
        rc = ctx->restore.ops.stream_complete(ctx);
        if ( rc )
            goto err;

        ctx->restore.callbacks->restore_results(ctx->restore.xenstore_gfn,
                                                ctx->restore.console_gfn,
                                                ctx->restore.callbacks->data);

        /* Resume secondary vm */
        ret = ctx->restore.callbacks->postcopy(ctx->restore.callbacks->data);
        HANDLE_CALLBACK_RETURN_VALUE(ret);

        /* Wait for a new checkpoint */
        ret = ctx->restore.callbacks->wait_checkpoint(
            ctx->restore.callbacks->data);
        HANDLE_CALLBACK_RETURN_VALUE(ret);

        /* suspend secondary vm */
        ret = ctx->restore.callbacks->suspend(ctx->restore.callbacks->data);
        HANDLE_CALLBACK_RETURN_VALUE(ret);

#undef HANDLE_CALLBACK_RETURN_VALUE

        rc = send_checkpoint_dirty_pfn_list(ctx);
        if ( rc )
            goto err;
    }

 err:
    return rc;
}

static int buffer_record(struct xc_sr_context *ctx, struct xc_sr_rhdr *rhdr)
{
    xc_interface *xch = ctx->xch;
    unsigned int new_alloc_num;
    struct xc_sr_record rec;
    struct xc_sr_record *p;

    if ( ctx->restore.buffered_rec_num >= ctx->restore.allocated_rec_num )
    {
        new_alloc_num = ctx->restore.allocated_rec_num + DEFAULT_BUF_RECORDS;
        p = realloc(ctx->restore.buffered_records,
                    new_alloc_num * sizeof(struct xc_sr_record));
        if ( !p )
        {
            ERROR("Failed to realloc memory for buffered records");
            return -1;
        }

        ctx->restore.buffered_records = p;
        ctx->restore.allocated_rec_num = new_alloc_num;
    }

    if ( read_record_data(ctx, ctx->fd, rhdr, &rec) )
    {
        return -1;
    }

    memcpy(&ctx->restore.buffered_records[ctx->restore.buffered_rec_num++],
           &rec, sizeof(rec));

    return 0;
}

int handle_static_data_end(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;
    unsigned int missing = 0;
    int rc = 0;

    if ( ctx->restore.seen_static_data_end )
    {
        ERROR("Multiple STATIC_DATA_END records found");
        return -1;
    }

    ctx->restore.seen_static_data_end = true;

    rc = ctx->restore.ops.static_data_complete(ctx, &missing);
    if ( rc )
        return rc;

    if ( ctx->restore.callbacks->static_data_done &&
         (rc = ctx->restore.callbacks->static_data_done(
             missing, ctx->restore.callbacks->data) != 0) )
        ERROR("static_data_done() callback failed: %d\n", rc);

    return rc;
}

static int process_buffered_record(struct xc_sr_context *ctx, struct xc_sr_record *rec)
{
    xc_interface *xch = ctx->xch;
    int rc = 0;

    switch ( rec->type )
    {
    case REC_TYPE_END:
        break;

    case REC_TYPE_PAGE_DATA:
        rc = handle_buffered_page_data(ctx, rec);
        break;

    case REC_TYPE_VERIFY:
        DPRINTF("Verify mode enabled");
        ctx->restore.verify = true;
        if ( !ctx->restore.verify_buf )
        {
            ctx->restore.verify_buf = malloc(MAX_BATCH_SIZE * PAGE_SIZE);
            if ( !ctx->restore.verify_buf )
            {
                PERROR("Unable to allocate verify_buf");
                rc = -1;
            }
        }
        break;

    case REC_TYPE_CHECKPOINT:
        rc = handle_checkpoint(ctx);
        break;

    case REC_TYPE_STATIC_DATA_END:
        rc = handle_static_data_end(ctx);
        break;

    default:
        rc = ctx->restore.ops.process_record(ctx, rec);
        break;
    }

    free(rec->data);
    rec->data = NULL;

    return rc;
}

static int process_incoming_record_header(struct xc_sr_context *ctx, struct xc_sr_rhdr *rhdr)
{
    struct xc_sr_record rec;
    int rc;

    switch ( rhdr->type )
    {
    case REC_TYPE_PAGE_DATA:
        rc = handle_incoming_page_data(ctx, rhdr);
        break;
    default:
        rc = read_record_data(ctx, ctx->fd, rhdr, &rec);
        if ( rc == 0 )
            rc = process_buffered_record(ctx, &rec);;
        break;
    }

    return rc;
}


static int setup(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;
    int rc;
    DECLARE_HYPERCALL_BUFFER_SHADOW(unsigned long, dirty_bitmap,
                                    &ctx->restore.dirty_bitmap_hbuf);

    if ( ctx->stream_type == XC_STREAM_COLO )
    {
        dirty_bitmap = xc_hypercall_buffer_alloc_pages(
            xch, dirty_bitmap, NRPAGES(bitmap_size(ctx->restore.p2m_size)));

        if ( !dirty_bitmap )
        {
            ERROR("Unable to allocate memory for dirty bitmap");
            rc = -1;
            goto err;
        }
    }

    rc = ctx->restore.ops.setup(ctx);
    if ( rc )
        goto err;

    ctx->restore.pfns = malloc(MAX_BATCH_SIZE * sizeof(*ctx->restore.pfns));
    ctx->restore.types = malloc(MAX_BATCH_SIZE * sizeof(*ctx->restore.types));
    ctx->restore.mfns = malloc(MAX_BATCH_SIZE * sizeof(*ctx->restore.mfns));
    ctx->restore.map_errs = malloc(MAX_BATCH_SIZE * sizeof(*ctx->restore.map_errs));
    ctx->restore.pp_pfns = malloc(MAX_BATCH_SIZE * sizeof(*ctx->restore.pp_pfns));
    ctx->restore.pp_mfns = malloc(MAX_BATCH_SIZE * sizeof(*ctx->restore.pp_mfns));
    ctx->restore.guest_data = malloc(MAX_BATCH_SIZE * sizeof(*ctx->restore.guest_data));
    ctx->restore.iov = malloc(MAX_BATCH_SIZE * sizeof(*ctx->restore.iov));
    ctx->restore.pages = malloc(MAX_BATCH_SIZE * sizeof(*ctx->restore.pages->pfn) + sizeof(*ctx->restore.pages));
    if ( !ctx->restore.pfns || !ctx->restore.types || !ctx->restore.mfns ||
         !ctx->restore.map_errs || !ctx->restore.pp_pfns ||
         !ctx->restore.pp_mfns || !ctx->restore.guest_data ||
         !ctx->restore.iov || !ctx->restore.pages )
    {
        ERROR("Unable to allocate memory");
        rc = -1;
        goto err;
    }

    ctx->restore.buffered_records = malloc(
        DEFAULT_BUF_RECORDS * sizeof(struct xc_sr_record));
    if ( !ctx->restore.buffered_records )
    {
        ERROR("Unable to allocate memory for buffered records");
        rc = -1;
        goto err;
    }
    ctx->restore.allocated_rec_num = DEFAULT_BUF_RECORDS;

 err:
    return rc;
}

static void cleanup(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;
    unsigned int i;
    DECLARE_HYPERCALL_BUFFER_SHADOW(unsigned long, dirty_bitmap,
                                    &ctx->restore.dirty_bitmap_hbuf);

    for ( i = 0; i < ctx->restore.buffered_rec_num; i++ )
        free(ctx->restore.buffered_records[i].data);

    if ( ctx->stream_type == XC_STREAM_COLO )
        xc_hypercall_buffer_free_pages(
            xch, dirty_bitmap, NRPAGES(bitmap_size(ctx->restore.p2m_size)));

    free(ctx->restore.buffered_records);
    free(ctx->restore.pages);
    free(ctx->restore.iov);
    free(ctx->restore.guest_data);
    free(ctx->restore.pp_mfns);
    free(ctx->restore.pp_pfns);
    free(ctx->restore.map_errs);
    free(ctx->restore.mfns);
    free(ctx->restore.types);
    free(ctx->restore.pfns);

    if ( ctx->restore.ops.cleanup(ctx) )
        PERROR("Failed to clean up");
}

/*
 * Restore a domain.
 */
static int restore(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;
    struct xc_sr_rhdr rhdr;
    int rc, saved_rc = 0, saved_errno = 0;

    IPRINTF("Restoring domain");

    rc = setup(ctx);
    if ( rc )
        goto err;

    do
    {
        rc = read_record_header(ctx, ctx->fd, &rhdr);
        if ( rc )
        {
            if ( ctx->restore.buffer_all_records )
                goto remus_failover;
            else
                goto err;
        }

        if ( ctx->restore.buffer_all_records &&
             rhdr.type != REC_TYPE_END &&
             rhdr.type != REC_TYPE_CHECKPOINT )
        {
            rc = buffer_record(ctx, &rhdr);
            if ( rc )
                goto err;
        }
        else
        {
            rc = process_incoming_record_header(ctx, &rhdr);
            if ( rc == RECORD_NOT_PROCESSED )
            {
                if ( rhdr.type & REC_TYPE_OPTIONAL )
                    DPRINTF("Ignoring optional record %#x (%s)",
                            rhdr.type, rec_type_to_str(rhdr.type));
                else
                {
                    ERROR("Mandatory record %#x (%s) not handled",
                          rhdr.type, rec_type_to_str(rhdr.type));
                    rc = -1;
                    goto err;
                }
            }
            else if ( rc == BROKEN_CHANNEL )
                goto remus_failover;
            else if ( rc )
                goto err;
        }

    } while ( rhdr.type != REC_TYPE_END );

 remus_failover:
    if ( ctx->stream_type == XC_STREAM_COLO )
    {
        /* With COLO, we have already called stream_complete */
        rc = 0;
        IPRINTF("COLO Failover");
        goto done;
    }

    /*
     * With Remus, if we reach here, there must be some error on primary,
     * failover from the last checkpoint state.
     */
    rc = ctx->restore.ops.stream_complete(ctx);
    if ( rc )
        goto err;

    IPRINTF("Restore successful");
    goto done;

 err:
    saved_errno = errno;
    saved_rc = rc;
    PERROR("Restore failed");

 done:
    cleanup(ctx);

    if ( saved_rc )
    {
        rc = saved_rc;
        errno = saved_errno;
    }

    return rc;
}

int xc_domain_restore(xc_interface *xch, int io_fd, uint32_t dom,
                      unsigned int store_evtchn, unsigned long *store_mfn,
                      uint32_t store_domid, unsigned int console_evtchn,
                      unsigned long *console_gfn, uint32_t console_domid,
                      xc_stream_type_t stream_type,
                      struct restore_callbacks *callbacks, int send_back_fd)
{
    bool hvm;
    xen_pfn_t nr_pfns;
    struct xc_sr_context ctx = {
        .xch = xch,
        .fd = io_fd,
        .stream_type = stream_type,
    };

    /* GCC 4.4 (of CentOS 6.x vintage) can' t initialise anonymous unions. */
    ctx.restore.console_evtchn = console_evtchn;
    ctx.restore.console_domid = console_domid;
    ctx.restore.xenstore_evtchn = store_evtchn;
    ctx.restore.xenstore_domid = store_domid;
    ctx.restore.callbacks = callbacks;
    ctx.restore.send_back_fd = send_back_fd;

    /* Sanity check stream_type-related parameters */
    switch ( stream_type )
    {
    case XC_STREAM_COLO:
        assert(callbacks->suspend &&
               callbacks->postcopy &&
               callbacks->wait_checkpoint &&
               callbacks->restore_results);
        /* Fallthrough */
    case XC_STREAM_REMUS:
        assert(callbacks->checkpoint);
        /* Fallthrough */
    case XC_STREAM_PLAIN:
        break;

    default:
        assert(!"Bad stream_type");
        break;
    }

    if ( xc_domain_getinfo_single(xch, dom, &ctx.dominfo) < 0 )
    {
        PERROR("Failed to get dominfo for dom%u", dom);
        return -1;
    }

    hvm = ctx.dominfo.flags & XEN_DOMINF_hvm_guest;
    DPRINTF("fd %d, dom %u, hvm %u, stream_type %d",
            io_fd, dom, hvm, stream_type);

    ctx.domid = dom;

    if ( read_headers(&ctx) )
        return -1;

    if ( xc_domain_nr_gpfns(xch, dom, &nr_pfns) < 0 )
    {
        PERROR("Unable to obtain the guest p2m size");
        return -1;
    }

    /* See xc_domain_getinfo */
    ctx.restore.max_pages = ctx.dominfo.max_pages;
    ctx.restore.tot_pages = ctx.dominfo.tot_pages;
    ctx.restore.p2m_size = nr_pfns;
    ctx.restore.ops = hvm ? restore_ops_x86_hvm : restore_ops_x86_pv;

    if ( restore(&ctx) )
        return -1;

    IPRINTF("XenStore: mfn %#"PRIpfn", dom %d, evt %u",
            ctx.restore.xenstore_gfn,
            ctx.restore.xenstore_domid,
            ctx.restore.xenstore_evtchn);

    IPRINTF("Console: mfn %#"PRIpfn", dom %d, evt %u",
            ctx.restore.console_gfn,
            ctx.restore.console_domid,
            ctx.restore.console_evtchn);

    *console_gfn = ctx.restore.console_gfn;
    *store_mfn = ctx.restore.xenstore_gfn;

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
