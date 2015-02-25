#include "libxl_osdeps.h" /* must come before any other headers */
#include "libxl_internal.h"

static char *vscsi_trim_string(char *s)
{
    unsigned int len;

    while (isspace(*s))
        s++;
    len = strlen(s);
    while (len-- > 1 && isspace(s[len]))
        s[len] = '\0';
    return s;
}

int libxl_device_vscsi_parse(libxl_ctx *ctx, const char *cfg,
                             libxl_device_vscsi *new_host,
                             libxl_vscsi_dev *new_dev)
{
    GC_INIT(ctx);
    int rc;
    char *buf, *pdev, *vdev, *fhost;
    unsigned int hst, chn, tgt, lun;

    buf = libxl__strdup(gc, cfg);

    pdev = strtok(buf, ",");
    vdev = strtok(NULL, ",");
    fhost = strtok(NULL, ",");
    if (!(pdev && vdev)) {
        LOG(ERROR, "invalid vscsi= devspec: '%s'\n", cfg);
        rc = ERROR_INVAL;
        goto out;
    }

    pdev = vscsi_trim_string(pdev);
    vdev = vscsi_trim_string(vdev);

    if (strncmp(pdev, "/dev/", 5) == 0) {
        if (libxl_device_vscsi_parse_pdev(gc, pdev, &hst, &chn, &tgt, &lun)) {
            LOG(ERROR, "vscsi: invalid pdev '%s'", pdev);
            rc = ERROR_INVAL;
            goto out;
        }
    } else if (sscanf(pdev, "%u:%u:%u:%u", &hst, &chn, &tgt, &lun) != 4) {
        LOG(ERROR, "vscsi: invalid pdev '%s', expecting hst:chn:tgt:lun", pdev);
        rc = ERROR_INVAL;
        goto out;
    }

    new_dev->p_devname = libxl__strdup(gc, pdev);
    new_dev->p_hst = hst;
    new_dev->p_chn = chn;
    new_dev->p_tgt = tgt;
    new_dev->p_lun = lun;

    if (sscanf(vdev, "%u:%u:%u:%u", &hst, &chn, &tgt, &lun) != 4) {
        LOG(ERROR, "vscsi: invalid vdev '%s', expecting hst:chn:tgt:lun", pdev);
        rc = ERROR_INVAL;
        goto out;
    }

    new_host->v_hst = hst;
    new_dev->v_chn = chn;
    new_dev->v_tgt = tgt;
    new_dev->v_lun = lun;

    if (fhost) {
        fhost = vscsi_trim_string(fhost);
        new_host->feature_host = strcmp(fhost, "feature-host") == 0;
        if (!new_host->feature_host) {
            LOG(ERROR, "vscsi: invalid option '%s', expecting %s", fhost, "feature-host");
            rc = ERROR_INVAL;
            goto out;
        }
    }
    rc = 0;

out:
    GC_FREE;
    return rc;
}

int libxl_device_vscsi_get_host(libxl_ctx *ctx, uint32_t domid, const char *cfg, libxl_device_vscsi **vscsi_host)
{
    GC_INIT(ctx);
    libxl_vscsi_dev *new_dev = NULL;
    libxl_device_vscsi *new_host, *vscsi_hosts = NULL, *tmp;
    int rc = -1, found_host = -1, i, j;
    int num_hosts;

    GCNEW(new_host);
    libxl_device_vscsi_init(new_host);

    GCNEW(new_dev);
    libxl_vscsi_dev_init(new_dev);

    if (libxl_device_vscsi_parse(ctx, cfg, new_host, new_dev))
        goto out;

    /* FIXME: foreach domain, because pdev is not multiplexed by backend */
    /* FIXME: other device types do not have the multiplexing issue */
    /* FIXME: pci can solve it by unbinding the native driver */

    /* Look for existing vscsi_host for given domain */
    vscsi_hosts = libxl_device_vscsi_list(ctx, domid, &num_hosts);
    if (vscsi_hosts) {
        for (i = 0; i < num_hosts; ++i) {
            for (j = 0; j < vscsi_hosts[i].num_vscsi_devs; j++) {
                if (vscsi_hosts[i].vscsi_devs[j].p_hst == new_dev->p_hst &&
                    vscsi_hosts[i].vscsi_devs[j].p_chn == new_dev->p_chn &&
                    vscsi_hosts[i].vscsi_devs[j].p_tgt == new_dev->p_tgt &&
                    vscsi_hosts[i].vscsi_devs[j].p_lun == new_dev->p_lun) {
                    /* FIXME proper log target */
                    fprintf(stderr, "Host device '%u:%u:%u:%u' is already in use"
                            " by guest vscsi specification '%u:%u:%u:%u'.\n",
                            new_dev->p_hst, new_dev->p_chn, new_dev->p_tgt, new_dev->p_lun,
                            vscsi_hosts[i].v_hst, new_dev->v_chn, new_dev->v_tgt, new_dev->v_lun);
                    goto out;
                }
            }
            if (vscsi_hosts[i].v_hst == new_host->v_hst) {
                found_host = i;
                break;
            }
        }
    }

    /* The caller gets a copy along with appended new_dev */
    /* Rely on the _copy helper to do all the allocation work */
    if (found_host == -1) {
        /* Not found, create new host */
        new_host->devid = 0;
        new_host->num_vscsi_devs = 1;
        new_host->vscsi_devs = new_dev;
        new_dev->vscsi_dev_id = 0;

        tmp = malloc(sizeof(*new_host));
        if (!tmp)
            goto out;
        libxl_device_vscsi_init(tmp);
        libxl_device_vscsi_copy(ctx, tmp, new_host);

        /* Clear for _dispose */
        new_host->num_vscsi_devs = 0;
        new_host->vscsi_devs = NULL;
    } else {
        /* Check if the vdev address is already taken */
        tmp = vscsi_hosts + found_host;
        for (i = 0; i < tmp->num_vscsi_devs; ++i) {
            if (tmp->vscsi_devs[i].v_chn == new_dev->v_chn &&
                tmp->vscsi_devs[i].v_tgt == new_dev->v_tgt &&
                tmp->vscsi_devs[i].v_lun == new_dev->v_lun) {
                fprintf(stderr, "Target vscsi specification '%u:%u:%u:%u' is already taken\n",
                        tmp->v_hst, new_dev->v_chn, new_dev->v_tgt, new_dev->v_lun);
                goto out;
            }
        }
        tmp = malloc(sizeof(*new_host));
        if (!tmp)
            goto out;
        libxl_device_vscsi_init(tmp);
        libxl_device_vscsi_copy(ctx, tmp, vscsi_hosts + found_host);

        /* Now append the new device to the existing host */
        tmp->vscsi_devs = libxl__realloc(NOGC, tmp->vscsi_devs, sizeof(libxl_vscsi_dev) * (tmp->num_vscsi_devs + 1));

        new_dev->vscsi_dev_id = tmp->num_vscsi_devs;
        libxl_vscsi_dev_copy(ctx, tmp->vscsi_devs + tmp->num_vscsi_devs, new_dev);

        tmp->num_vscsi_devs++;
    }

    *vscsi_host = tmp;
    rc = 0;

out:
    if (vscsi_hosts) {
        for (i = 0; i < num_hosts; ++i){
            libxl_device_vscsi_dispose(&vscsi_hosts[i]);
        }
        free(vscsi_hosts);
    }
    libxl_device_vscsi_dispose(new_host);
    libxl_vscsi_dev_dispose(new_dev);
    GC_FREE;
    return rc;
}

libxl_device_vscsi *libxl_device_vscsi_list(libxl_ctx *ctx, uint32_t domid, int *num)
{
    GC_INIT(ctx);
    libxl_vscsi_dev *v_dev;
    libxl_device_vscsi *v_hst, *vscsi_hosts = NULL;
    char *fe_path, *tmp, *d, *p, *v;
    char **dir, **devs_dir;
    const char *devs_path, *be_path;
    int r;
    unsigned int ndirs = 0, ndevs_dirs = 0, i;
    unsigned int vscsi_dev_id, parsed_ok;

    fe_path = libxl__sprintf(gc, "%s/device/vscsi", libxl__xs_get_dompath(gc, domid));
    dir = libxl__xs_directory(gc, XBT_NULL, fe_path, &ndirs);
    /* Nothing to do */
    if (!(dir && ndirs))
        goto out;

    /* List of hosts to be returned to the caller */
    vscsi_hosts = calloc(ndirs, sizeof(*vscsi_hosts));
    /* FIXME how to handle OOM? */
    if (!vscsi_hosts)
        goto out;

    /* Fill each host */
    for (v_hst = vscsi_hosts; v_hst < vscsi_hosts + ndirs; ++v_hst, ++dir) {
        libxl_device_vscsi_init(v_hst);

        v_hst->devid = atoi(*dir);

        tmp = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/%s/backend-id",
                             fe_path, *dir));
        /* FIXME what if xenstore is broken? */
        if (tmp)
            v_hst->backend_domid = atoi(tmp);

        be_path = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/%s/backend",
                                 fe_path, *dir));
        /* FIXME what if xenstore is broken? */
        if (be_path) {
            devs_path = libxl__sprintf(gc, "%s/vscsi-devs", be_path);
            devs_dir = libxl__xs_directory(gc, XBT_NULL, devs_path, &ndevs_dirs);
        } else {
            devs_dir = NULL;
        }

        if (devs_dir && ndevs_dirs) {
            v_hst->vscsi_devs = calloc(ndevs_dirs, sizeof(*v_dev));
            /* FIXME how to handle OOM? */
            if (!v_hst->vscsi_devs)
                continue;

            v_hst->num_vscsi_devs = ndevs_dirs;
            /* Fill each device connected to the host */
            for (i = 0; i < ndevs_dirs; i++, devs_dir++) {
                v_dev = &v_hst->vscsi_devs[i];
                libxl_vscsi_dev_init(v_dev);
                parsed_ok = 0;
                r = sscanf(*devs_dir, "dev-%u", &vscsi_dev_id);
                if (r == 1) {
                    parsed_ok += 1;
                    d = libxl__xs_read(gc, XBT_NULL,
                                         GCSPRINTF("%s/vscsi-devs/dev-%u/p-devname",
                                         be_path, vscsi_dev_id));
                    p = libxl__xs_read(gc, XBT_NULL,
                                         GCSPRINTF("%s/vscsi-devs/dev-%u/p-dev",
                                         be_path, vscsi_dev_id));
                    v = libxl__xs_read(gc, XBT_NULL,
                                          GCSPRINTF("%s/vscsi-devs/dev-%u/v-dev",
                                          be_path, vscsi_dev_id));
                    if (d && p && v) {
                        v_dev->p_devname = libxl__strdup(NOGC, d);
                        r = sscanf(p, "%u:%u:%u:%u", &v_dev->p_hst,
                                   &v_dev->p_chn, &v_dev->p_tgt, &v_dev->p_lun);
                        if (r == 4)
                            parsed_ok += 4;
                        r = sscanf(v, "%u:%u:%u:%u", &v_hst->v_hst,
                                   &v_dev->v_chn, &v_dev->v_tgt, &v_dev->v_lun);
                        if (r == 4)
                            parsed_ok += 4;
                        v_dev->vscsi_dev_id = vscsi_dev_id;
                    }
                }

                /* One dev-N + (2 * four per [pv]-dev) */
                if (parsed_ok != 9) {
                    /* FIXME what if xenstore is broken? */
                    LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "%s/scsi-devs/%s failed to parse", be_path, *devs_dir);
                    continue;
                }
            }
        }
    }

out:
    *num = ndirs;

    GC_FREE;
    return vscsi_hosts;
}

int libxl_device_vscsi_getinfo(libxl_ctx *ctx, uint32_t domid,
                               libxl_device_vscsi *vscsi_host,
                               libxl_vscsi_dev *vscsi_dev,
                               libxl_vscsiinfo *vscsiinfo)
{
    GC_INIT(ctx);
    char *dompath, *vscsipath;
    char *val;
    int rc = ERROR_FAIL;

    libxl_vscsiinfo_init(vscsiinfo);
    dompath = libxl__xs_get_dompath(gc, domid);
    vscsiinfo->devid = vscsi_host->devid;
    vscsiinfo->p_hst = vscsi_dev->p_hst;
    vscsiinfo->p_chn = vscsi_dev->p_chn;
    vscsiinfo->p_tgt = vscsi_dev->p_tgt;
    vscsiinfo->p_lun = vscsi_dev->p_lun;
    vscsiinfo->v_hst = vscsi_host->v_hst;
    vscsiinfo->v_chn = vscsi_dev->v_chn;
    vscsiinfo->v_tgt = vscsi_dev->v_tgt;
    vscsiinfo->v_lun = vscsi_dev->v_lun;

    vscsipath = GCSPRINTF("%s/device/vscsi/%d", dompath, vscsiinfo->devid);
    vscsiinfo->backend = xs_read(ctx->xsh, XBT_NULL,
                                 GCSPRINTF("%s/backend", vscsipath), NULL);
    if (!vscsiinfo->backend)
        goto out;
    if(!libxl__xs_read(gc, XBT_NULL, vscsiinfo->backend))
        goto out;

    val = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/backend-id", vscsipath));
    vscsiinfo->backend_id = val ? strtoul(val, NULL, 10) : -1;

    val = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/state", vscsipath));
    vscsiinfo->vscsi_host_state = val ? strtoul(val, NULL, 10) : -1;

    val = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/event-channel", vscsipath));
    vscsiinfo->evtch = val ? strtoul(val, NULL, 10) : -1;

    val = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/ring-ref", vscsipath));
    vscsiinfo->rref = val ? strtoul(val, NULL, 10) : -1;

    vscsiinfo->frontend = xs_read(ctx->xsh, XBT_NULL,
                                  GCSPRINTF("%s/frontend", vscsiinfo->backend), NULL);

    val = libxl__xs_read(gc, XBT_NULL,
                         GCSPRINTF("%s/frontend-id", vscsiinfo->backend));
    vscsiinfo->frontend_id = val ? strtoul(val, NULL, 10) : -1;

    val = libxl__xs_read(gc, XBT_NULL,
                         GCSPRINTF("%s/vscsi-devs/dev-%u/state",
                         vscsiinfo->backend, vscsi_dev->vscsi_dev_id));
    vscsiinfo->vscsi_dev_state = val ? strtoul(val, NULL, 10) : -1;

    rc = 0;
out:
    GC_FREE;
    return rc;
}



/*
 * Local variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
