/*
 * Copyright (C) 2015      SUSE Linux GmbH
 * Author Olaf Hering <olaf@aepfle.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; version 2.1 only. with the special
 * exception on linking described in file LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */
#include "libxl_osdeps.h" /* must come before any other headers */
#include "libxl_internal.h"

#define XLU_WWN_LEN 16

static int vscsi_parse_hctl(char *str, libxl_vscsi_hctl *hctl)
{
    unsigned int hst, chn, tgt;
    unsigned long long lun;

    if (sscanf(str, "%u:%u:%u:%llu", &hst, &chn, &tgt, &lun) != 4)
        return ERROR_INVAL;

    hctl->hst = hst;
    hctl->chn = chn;
    hctl->tgt = tgt;
    hctl->lun = lun;
    return 0;
}

static bool vscsi_wwn_valid(const char *p)
{
    bool ret = true;
    int i = 0;

    for (i = 0; i < XLU_WWN_LEN; i++, p++) {
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

/* Translate p-dev back into pdev.type */
static bool vscsi_parse_pdev(libxl__gc *gc, libxl_device_vscsidev *dev,
                             char *c, char *p, char *v)
{
    libxl_vscsi_hctl hctl;
    unsigned long long lun;
    char wwn[XLU_WWN_LEN + 1];
    bool parsed_ok = false;

    libxl_vscsi_hctl_init(&hctl);

    dev->pdev.p_devname = libxl__strdup(NOGC, c);

    if (strncmp(p, "naa.", 4) == 0) {
        /* WWN as understood by pvops */
        memset(wwn, 0, sizeof(wwn));
        if (sscanf(p, "naa.%16c:%llu", wwn, &lun) == 2 && vscsi_wwn_valid(wwn)) {
            libxl_vscsi_pdev_init_type(&dev->pdev, LIBXL_VSCSI_PDEV_TYPE_WWN);
            dev->pdev.u.wwn.m = libxl__strdup(NOGC, p);
            parsed_ok = true;
        }
    } else if (vscsi_parse_hctl(p, &hctl) == 0) {
        /* Either xenlinux, or pvops with properly configured alias in sysfs */
        libxl_vscsi_pdev_init_type(&dev->pdev, LIBXL_VSCSI_PDEV_TYPE_HCTL);
        libxl_vscsi_hctl_copy(CTX, &dev->pdev.u.hctl.m, &hctl);
        parsed_ok = true;
    }

    if (parsed_ok && vscsi_parse_hctl(v, &dev->vdev) != 0)
        parsed_ok = false;

    libxl_vscsi_hctl_dispose(&hctl);

    return parsed_ok;
}

void libxl_device_vscsictrl_append_vscsidev(libxl_ctx *ctx,
                                            libxl_device_vscsictrl *ctrl,
                                            libxl_device_vscsidev *dev)
{
    GC_INIT(ctx);
    ctrl->vscsidevs = libxl__realloc(NOGC, ctrl->vscsidevs, sizeof(*dev) * (ctrl->num_vscsidevs + 1));
    libxl_device_vscsidev_init(ctrl->vscsidevs + ctrl->num_vscsidevs);
    libxl_device_vscsidev_copy(CTX, ctrl->vscsidevs + ctrl->num_vscsidevs, dev);
    ctrl->num_vscsidevs++;
    GC_FREE;
}

static bool libxl__vscsi_fill_dev(libxl__gc *gc,
                                  xs_transaction_t t,
                                  const char *devs_path,
                                  const char *dev_dir,
                                  libxl_device_vscsidev *dev)
{
    char *path, *c, *p, *v, *s;
    unsigned int devid;
    int r;
    bool parsed_ok;

    r = sscanf(dev_dir, "dev-%u", &devid);
    if (r != 1) {
        LOG(ERROR, "expected dev-N, got '%s'", dev_dir);
        return false;
    }
    dev->vscsidev_id = devid;

    path = GCSPRINTF("%s/%s", devs_path, dev_dir);
    c = libxl__xs_read(gc, t, GCSPRINTF("%s/p-devname", path));
    p = libxl__xs_read(gc, t, GCSPRINTF("%s/p-dev", path));
    v = libxl__xs_read(gc, t, GCSPRINTF("%s/v-dev", path));
    s = libxl__xs_read(gc, t, GCSPRINTF("%s/state", path));
    LOG(DEBUG, "%s/state is %s", path, s);
    if (!(c && p && v && s)) {
        LOG(ERROR, "p-devname '%s' p-dev '%s' v-dev '%s'", c, p, v);
        return false;
    }

    parsed_ok = vscsi_parse_pdev(gc, dev, c, p, v);
    if (!parsed_ok) {
        LOG(ERROR, "failed to parse %s: %s %s %s %s", path, c, p, v, s);
        return false;
    }

    switch (atoi(s)) {
        case XenbusStateUnknown:
        case XenbusStateInitialising:
        case XenbusStateInitWait:
        case XenbusStateInitialised:
        case XenbusStateConnected:
        case XenbusStateReconfiguring:
        case XenbusStateReconfigured:
            parsed_ok = true;
            break;
        case XenbusStateClosing:
        case XenbusStateClosed:
            parsed_ok = false;
            break;
    }

    if (!parsed_ok)
        LOG(ERROR, "unexpected state in %s: %s", path, s);

    return parsed_ok;
}

static bool libxl__vscsi_fill_ctrl(libxl__gc *gc,
                                   xs_transaction_t t,
                                   const char *fe_path,
                                   const char *dir,
                                   libxl_device_vscsictrl *ctrl)
{
    libxl_device_vscsidev dev;
    char *tmp, *be_path, *devs_path;
    char **dev_dirs;
    unsigned int ndev_dirs, dev_dir;
    bool parsed_ok;

    ctrl->devid = atoi(dir);

    be_path = libxl__xs_read(gc, t, GCSPRINTF("%s/%s/backend", fe_path, dir));
    /* FIXME what if xenstore is broken? */
    if (!be_path) {
        libxl_defbool_set(&ctrl->scsi_raw_cmds, false);
        return false;
    }

    tmp = libxl__xs_read(gc, t, GCSPRINTF("%s/%s/backend-id", fe_path, dir));
    /* FIXME what if xenstore is broken? */
    if (tmp)
        ctrl->backend_domid = atoi(tmp);

    parsed_ok = false;
    tmp = libxl__xs_read(gc, t, GCSPRINTF("%s/feature-host", be_path));
    if (tmp)
        parsed_ok = atoi(tmp) != 0;
    libxl_defbool_set(&ctrl->scsi_raw_cmds, parsed_ok);

    parsed_ok = true;
    devs_path = GCSPRINTF("%s/vscsi-devs", be_path);
    dev_dirs = libxl__xs_directory(gc, t, devs_path, &ndev_dirs);
    for (dev_dir = 0; dev_dirs && dev_dir < ndev_dirs; dev_dir++) {
        libxl_device_vscsidev_init(&dev);
        parsed_ok = libxl__vscsi_fill_dev(gc, t, devs_path, dev_dirs[dev_dir], &dev);
        if (parsed_ok == true)
            libxl_device_vscsictrl_append_vscsidev(CTX, ctrl, &dev);
        libxl_device_vscsidev_dispose(&dev);
        if (parsed_ok == false)
            break;
    }

    return parsed_ok;
}

int libxl__vscsi_collect_ctrls(libxl__gc *gc,
                               uint32_t domid,
                               libxl_device_vscsictrl **ctrls,
                               int *num)
{
    xs_transaction_t t = XBT_NULL;
    libxl_device_vscsictrl ctrl;
	char *fe_path;
    char **dirs;
    unsigned int ndirs = 0, dir;
    int rc;

    fe_path = GCSPRINTF("%s/device/vscsi", libxl__xs_get_dompath(gc, domid));

    for (;;) {
        *num = 0;

        rc = libxl__xs_transaction_start(gc, &t);
        if (rc) goto out;

        dirs = libxl__xs_directory(gc, t, fe_path, &ndirs);
        /* Nothing to do */
        if (!(dirs && ndirs))
            break;

        /* List of ctrls to be returned to the caller */
        *ctrls = libxl__malloc(NOGC, ndirs * sizeof(**ctrls));

        for (dir = 0; dir < ndirs; dir++) {
            libxl_device_vscsictrl_init(*ctrls + dir);

            libxl_device_vscsictrl_init(&ctrl);
            if (libxl__vscsi_fill_ctrl(gc, t, fe_path, dirs[dir], &ctrl)) {
                libxl_device_vscsictrl_copy(CTX, *ctrls + *num, &ctrl);
                (*num)++;
            }
            libxl_device_vscsictrl_dispose(&ctrl);
        }

        rc = libxl__xs_transaction_commit(gc, &t);
        if (!rc) break;

        if (rc < 0) {
            for (dir = 0; dir < ndirs; dir++)
                libxl_device_vscsictrl_dispose(*ctrls + dir);
            free(*ctrls);
            *ctrls = NULL;
            *num = 0;
            goto out;
        }
    }

out:
    libxl__xs_transaction_abort(gc, &t);
    return rc;
}

libxl_device_vscsictrl *libxl_device_vscsictrl_list(libxl_ctx *ctx,
                                                    uint32_t domid,
                                                    int *num)
{
    GC_INIT(ctx);
    libxl_device_vscsictrl *ctrls = NULL;
    int rc, num_ctrls = 0;

    *num = 0;

    rc = libxl__vscsi_collect_ctrls(gc, domid, &ctrls, &num_ctrls);
    if (rc == 0)
        *num = num_ctrls;

    GC_FREE;
    return ctrls;
}

int libxl_device_vscsictrl_getinfo(libxl_ctx *ctx, uint32_t domid,
                                   libxl_device_vscsictrl *vscsictrl,
                                   libxl_device_vscsidev *vscsidev,
                                   libxl_vscsiinfo *vscsiinfo)
{
    GC_INIT(ctx);
    char *dompath, *vscsipath;
    char *val;
    int rc = ERROR_FAIL;

    libxl_vscsiinfo_init(vscsiinfo);
    dompath = libxl__xs_get_dompath(gc, domid);
    vscsiinfo->devid = vscsictrl->devid;
    vscsiinfo->vscsidev_id = vscsidev->vscsidev_id;
    libxl_vscsi_pdev_copy(ctx, &vscsiinfo->pdev, &vscsidev->pdev);
    libxl_vscsi_hctl_copy(ctx, &vscsiinfo->vdev, &vscsidev->vdev);

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
    vscsiinfo->vscsictrl_state = val ? strtoul(val, NULL, 10) : -1;

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
                         vscsiinfo->backend, vscsidev->vscsidev_id));
    vscsiinfo->vscsidev_state = val ? strtoul(val, NULL, 10) : -1;

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
