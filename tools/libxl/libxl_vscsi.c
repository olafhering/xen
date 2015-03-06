#include "libxl_osdeps.h" /* must come before any other headers */
#include "libxl_internal.h"

int libxl_device_vscsi_parse_hctl(libxl__gc *gc, char *str, libxl_vscsi_hctl *hctl)
{
    unsigned int hst, chn, tgt, lun;

    if (sscanf(str, "%u:%u:%u:%u", &hst, &chn, &tgt, &lun) != 4)
        return ERROR_INVAL;

    hctl->hst = hst;
    hctl->chn = chn;
    hctl->tgt = tgt;
    hctl->lun = lun;
    return 0;
}

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

static bool vscsi_wwn_valid(const char *p)
{
    bool ret = true;
    int i = 0;

    for (i = 0; i < 16; i++, p++) {
        if (*p >= '0' && *p <= '9')
            continue;
        if (*p >= 'a' && *p <= 'f')
            continue;
        if (*p >= 'A' && *p <= 'F')
            continue;
        ret = false;
        break;
    }
    return ret;
}

int libxl_device_vscsi_parse(libxl_ctx *ctx, const char *cfg,
                             libxl_device_vscsi *new_host,
                             libxl_vscsi_dev *new_dev)
{
    GC_INIT(ctx);
    int rc;
    unsigned int lun;
    char wwn[16 + 1], *buf, *pdev, *vdev, *fhost;

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

    new_dev->pdev_type = LIBXL_VSCSI_PDEV_TYPE_INVALID;
    if (strncmp(pdev, "/dev/", 5) == 0) {
        if (libxl_device_vscsi_parse_pdev(gc, pdev, &new_dev->pdev) == 0)
            new_dev->pdev_type = LIBXL_VSCSI_PDEV_TYPE_DEV;
    } else if (strncmp(pdev, "naa.", 4) == 0) {
        memset(wwn, 0, sizeof(wwn));
        if (sscanf(pdev, "naa.%16c:%u", wwn, &lun) == 2 && vscsi_wwn_valid(wwn))
            new_dev->pdev_type = LIBXL_VSCSI_PDEV_TYPE_WWN;
    } else if (libxl_device_vscsi_parse_hctl(gc, pdev, &new_dev->pdev) == 0) {
        new_dev->pdev_type = LIBXL_VSCSI_PDEV_TYPE_HCTL;
    }

    switch (new_dev->pdev_type) {
        case LIBXL_VSCSI_PDEV_TYPE_WWN:
            new_dev->pdev.lun = lun;
            /* Fall through.  */
        case LIBXL_VSCSI_PDEV_TYPE_DEV:
        case LIBXL_VSCSI_PDEV_TYPE_HCTL:
            new_dev->p_devname = libxl__strdup(NOGC, pdev);
            break;
        case LIBXL_VSCSI_PDEV_TYPE_INVALID:
            LOG(ERROR, "vscsi: invalid pdev '%s'", pdev);
            rc = ERROR_INVAL;
            goto out;
    }


    if (libxl_device_vscsi_parse_hctl(gc, vdev, &new_dev->vdev)) {
        LOG(ERROR, "vscsi: invalid '%s', expecting hst:chn:tgt:lun", vdev);
        rc = ERROR_INVAL;
        goto out;
    }

    /* Record group index */
    new_host->v_hst = new_dev->vdev.hst;

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

void libxl_device_vscsi_append_dev(libxl_ctx *ctx, libxl_device_vscsi *hst,
                                   libxl_vscsi_dev *dev)
{
    GC_INIT(ctx);
    hst->vscsi_devs = libxl__realloc(NOGC, hst->vscsi_devs,
                                     sizeof(*dev) * (hst->num_vscsi_devs + 1));
    libxl_vscsi_dev_init(hst->vscsi_devs + hst->num_vscsi_devs);
    dev->vscsi_dev_id = hst->num_vscsi_devs;
    libxl_vscsi_dev_copy(ctx, hst->vscsi_devs + hst->num_vscsi_devs, dev);
    hst->num_vscsi_devs++;
    GC_FREE;
}

int libxl_device_vscsi_get_host(libxl_ctx *ctx, uint32_t domid, const char *cfg, libxl_device_vscsi **vscsi_host)
{
    GC_INIT(ctx);
    libxl_vscsi_dev *new_dev = NULL;
    libxl_device_vscsi *new_host, *vscsi_hosts = NULL, *tmp;
    int rc, found_host = -1, i, j;
    int num_hosts;

    GCNEW(new_host);
    libxl_device_vscsi_init(new_host);

    GCNEW(new_dev);
    libxl_vscsi_dev_init(new_dev);

    if (libxl_device_vscsi_parse(ctx, cfg, new_host, new_dev)) {
        rc = ERROR_INVAL;
        goto out;
    }

    /* FIXME: foreach domain, because pdev is not multiplexed by backend */
    /* FIXME: other device types do not have the multiplexing issue */
    /* FIXME: pci can solve it by unbinding the native driver */

    /* Look for existing vscsi_host for given domain */
    vscsi_hosts = libxl_device_vscsi_list(ctx, domid, &num_hosts);
    if (vscsi_hosts) {
        for (i = 0; i < num_hosts; ++i) {
            for (j = 0; j < vscsi_hosts[i].num_vscsi_devs; j++) {
                if (vscsi_hosts[i].vscsi_devs[j].pdev.hst == new_dev->pdev.hst &&
                    vscsi_hosts[i].vscsi_devs[j].pdev.chn == new_dev->pdev.chn &&
                    vscsi_hosts[i].vscsi_devs[j].pdev.tgt == new_dev->pdev.tgt &&
                    vscsi_hosts[i].vscsi_devs[j].pdev.lun == new_dev->pdev.lun) {
                    LOG(ERROR, "Host device '%u:%u:%u:%u' is already in use"
                        " by guest vscsi specification '%u:%u:%u:%u'.\n",
                        new_dev->pdev.hst, new_dev->pdev.chn, new_dev->pdev.tgt, new_dev->pdev.lun,
                        new_dev->vdev.hst, new_dev->vdev.chn, new_dev->vdev.tgt, new_dev->vdev.lun);
                    rc = ERROR_INVAL;
                    goto out;
                }
            }
            if (vscsi_hosts[i].v_hst == new_host->v_hst) {
                found_host = i;
                break;
            }
        }
    }

    if (found_host == -1) {
        /* Not found, create new host */
        new_host->devid = 0;
        tmp = new_host;
    } else {
        tmp = vscsi_hosts + found_host;

        /* Check if the vdev address is already taken */
        for (i = 0; i < tmp->num_vscsi_devs; ++i) {
            if (tmp->vscsi_devs[i].vdev.chn == new_dev->vdev.chn &&
                tmp->vscsi_devs[i].vdev.tgt == new_dev->vdev.tgt &&
                tmp->vscsi_devs[i].vdev.lun == new_dev->vdev.lun) {
                fprintf(stderr, "Target vscsi specification '%u:%u:%u:%u' is already taken\n",
                        new_dev->vdev.hst, new_dev->vdev.chn, new_dev->vdev.tgt, new_dev->vdev.lun);
                rc = ERROR_INVAL;
                goto out;
            }
        }
    }

    /* The caller gets a copy along with appended new_dev */
    *vscsi_host = libxl__malloc(NOGC, sizeof(*new_host));
    libxl_device_vscsi_init(*vscsi_host);
    libxl_device_vscsi_copy(ctx, *vscsi_host, tmp);
    libxl_device_vscsi_append_dev(ctx, *vscsi_host, new_dev);

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
    char *fe_path, *tmp, *c, *p, *v;
    char **dir, **devs_dir;
    const char *devs_path, *be_path;
    int r;
    bool parsed_ok;
    unsigned int ndirs = 0, ndevs_dirs = 0, i;
    unsigned int vscsi_dev_id;

    fe_path = libxl__sprintf(gc, "%s/device/vscsi", libxl__xs_get_dompath(gc, domid));
    dir = libxl__xs_directory(gc, XBT_NULL, fe_path, &ndirs);
    /* Nothing to do */
    if (!(dir && ndirs))
        goto out;

    /* List of hosts to be returned to the caller */
    vscsi_hosts = libxl__malloc(NOGC, ndirs * sizeof(*vscsi_hosts));

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
            v_hst->vscsi_devs = libxl__malloc(NOGC, ndevs_dirs * sizeof(*v_dev));
            v_hst->num_vscsi_devs = ndevs_dirs;
            /* Fill each device connected to the host */
            for (i = 0; i < ndevs_dirs; i++, devs_dir++) {
                v_dev = &v_hst->vscsi_devs[i];
                libxl_vscsi_dev_init(v_dev);
                parsed_ok = false;
                r = sscanf(*devs_dir, "dev-%u", &vscsi_dev_id);
                if (r == 1) {
                    c = libxl__xs_read(gc, XBT_NULL,
                                         GCSPRINTF("%s/vscsi-devs/dev-%u/p-devname",
                                         be_path, vscsi_dev_id));
                    p = libxl__xs_read(gc, XBT_NULL,
                                         GCSPRINTF("%s/vscsi-devs/dev-%u/p-dev",
                                         be_path, vscsi_dev_id));
                    v = libxl__xs_read(gc, XBT_NULL,
                                          GCSPRINTF("%s/vscsi-devs/dev-%u/v-dev",
                                          be_path, vscsi_dev_id));
                    if (c && p && v) {
                        v_dev->p_devname = libxl__strdup(NOGC, c);
                        if (libxl_device_vscsi_parse_hctl(gc, p, &v_dev->pdev) == 0 &&
                            libxl_device_vscsi_parse_hctl(gc, v, &v_dev->vdev) == 0)
                            parsed_ok = true;
                        v_dev->vscsi_dev_id = vscsi_dev_id;
                        v_hst->v_hst = v_dev->vdev.hst;
                    }
                }

                if (!parsed_ok) {
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
    libxl_vscsi_hctl_copy(ctx, &vscsiinfo->pdev, &vscsi_dev->pdev);
    libxl_vscsi_hctl_copy(ctx, &vscsiinfo->vdev, &vscsi_dev->vdev);

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
