#include "libxl_osdeps.h" /* must come before any other headers */
#include "libxl_internal.h"

static int vscsi_parse_hctl(libxl__gc *gc, char *str, libxl_vscsi_hctl *hctl)
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
                        /* FIXME pdev not always in hctl format with pvops */
                        if (vscsi_parse_hctl(gc, p, &v_dev->pdev) == 0 &&
                            vscsi_parse_hctl(gc, v, &v_dev->vdev) == 0)
                            parsed_ok = true;
                        v_dev->vscsi_dev_id = vscsi_dev_id;
                        if (vscsi_dev_id > v_hst->next_vscsi_dev_id)
                            v_hst->next_vscsi_dev_id = vscsi_dev_id + 1;
                        v_hst->v_hst = v_dev->vdev.hst;
                    }
                }

                if (!parsed_ok) {
                    /* FIXME what if xenstore is broken? */
                    LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "%s/vscsi-devs/%s failed to parse", be_path, *devs_dir);
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
