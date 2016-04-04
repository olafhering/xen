/*
 * Copyright (C) 2016      SUSE Linux GmbH
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

typedef struct vscsidev_rm {
    libxl_device_vscsictrl *ctrl;
    char *be_path;
    int dev_wait;
    libxl__device dev;
} vscsidev_rm_t;

typedef void (*vscsictrl_add)(libxl__egc *egc,
                              libxl__ao_device *aodev,
                              libxl_device_vscsictrl *vscsictrl,
                              libxl_domain_config *d_config);

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
        if (sscanf(p, "naa.%16[0-9a-fA-F]:%llu", wwn, &lun) == 2) {
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

static bool vscsi_fill_dev(libxl__gc *gc,
                           xs_transaction_t t,
                           const char *devs_path,
                           const char *dev_dir,
                           libxl_device_vscsidev *dev)
{
    char *path, *c, *p, *v, *s;
    unsigned int devid;
    int r;

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

    if (!vscsi_parse_pdev(gc, dev, c, p, v)) {
        LOG(ERROR, "failed to parse %s: %s %s %s %s", path, c, p, v, s);
        return false;
    }

    return true;
}

static bool vscsi_fill_ctrl(libxl__gc *gc,
                            xs_transaction_t t,
                            const char *fe_path,
                            const char *dir,
                            libxl_device_vscsictrl *ctrl)
{
    libxl_device_vscsidev dev;
    char *tmp, *be_path, *devs_path;
    char **dev_dirs;
    unsigned int ndev_dirs, dev_dir;
    bool ok;

    ctrl->devid = atoi(dir);

    be_path = libxl__xs_read(gc, t, GCSPRINTF("%s/%s/backend", fe_path, dir));
    if (!be_path)
        goto out;

    tmp = libxl__xs_read(gc, t, GCSPRINTF("%s/%s/backend-id", fe_path, dir));
    if (!tmp)
        goto out;
    ctrl->backend_domid = atoi(tmp);

    tmp = libxl__xs_read(gc, t, GCSPRINTF("%s/idx", be_path));
    if (!tmp)
        goto out;
    ctrl->idx = atoi(tmp);

    tmp = libxl__xs_read(gc, t, GCSPRINTF("%s/feature-host", be_path));
    if (!tmp)
        goto out;
    ok = atoi(tmp) != 0;
    libxl_defbool_set(&ctrl->scsi_raw_cmds, ok);

    ok = true;
    devs_path = GCSPRINTF("%s/vscsi-devs", be_path);
    dev_dirs = libxl__xs_directory(gc, t, devs_path, &ndev_dirs);
    for (dev_dir = 0; dev_dirs && dev_dir < ndev_dirs; dev_dir++) {
        libxl_device_vscsidev_init(&dev);
        ok = vscsi_fill_dev(gc, t, devs_path, dev_dirs[dev_dir], &dev);
        if (ok == true)
            ok = ctrl->idx == dev.vdev.hst;
        if (ok == true)
            libxl_device_vscsictrl_append_vscsidev(CTX, ctrl, &dev);
        libxl_device_vscsidev_dispose(&dev);
        if (ok == false)
            break;
    }

    return ok;

out:
    libxl_defbool_set(&ctrl->scsi_raw_cmds, false);
    return false;
}

/* return an array of vscsictrls with num elements */
static int vscsi_collect_ctrls(libxl__gc *gc,
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
            if (vscsi_fill_ctrl(gc, t, fe_path, dirs[dir], &ctrl)) {
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

/* Simplified variant of device_addrm_aocomplete */
static void vscsi_aodev_complete(libxl__egc *egc, libxl__ao_device *aodev)
{
    STATE_AO_GC(aodev->ao);
    libxl__ao_complete(egc, ao, aodev->rc);
}

static int libxl__device_from_vscsictrl(libxl__gc *gc, uint32_t domid,
                                        libxl_device_vscsictrl *vscsictrl,
                                        libxl__device *device)
{
    device->backend_devid = vscsictrl->devid;
    device->backend_domid = vscsictrl->backend_domid;
    device->devid         = vscsictrl->devid;
    device->domid         = domid;
    device->backend_kind  = LIBXL__DEVICE_KIND_VSCSI;
    device->kind          = LIBXL__DEVICE_KIND_VSCSI;

    return 0;
}

static int vscsictrl_remove(libxl_ctx *ctx,
                            uint32_t domid,
                            libxl_device_vscsictrl *vscsictrl,
                            const libxl_asyncop_how *ao_how,
                            int force)
{
    AO_CREATE(ctx, domid, ao_how);
    libxl__device *device;
    libxl__ao_device *aodev;
    int rc;

    GCNEW(device);
    rc = libxl__device_from_vscsictrl(gc, domid, vscsictrl, device);
    if (rc != 0) goto out;

    GCNEW(aodev);
    libxl__prepare_ao_device(ao, aodev);
    aodev->action = LIBXL__DEVICE_ACTION_REMOVE;
    aodev->dev = device;
    aodev->callback = vscsi_aodev_complete;
    aodev->force = force;
    libxl__initiate_device_generic_remove(egc, aodev);

out:
    if (rc) return AO_CREATE_FAIL(rc);
    return AO_INPROGRESS;
}

static int vscsidev_be_set_rm(libxl__gc *gc,
                              libxl_device_vscsidev *v,
                              flexarray_t *back)
{
    int rc;
    char *dir;

    dir = GCSPRINTF("vscsi-devs/dev-%u", v->vscsidev_id);
    rc = flexarray_append_pair(back,
                               GCSPRINTF("%s/state", dir),
                               GCSPRINTF("%d", XenbusStateClosing));
    return rc;
}

static int vscsictrl_reconfigure_rm(libxl__ao_device *aodev,
                                    const char *state_path,
                                    int *be_wait)

{
    STATE_AO_GC(aodev->ao);
    vscsidev_rm_t *vscsidev_rm = CONTAINER_OF(aodev->dev, *vscsidev_rm, dev);
    libxl_device_vscsictrl *ctrl = vscsidev_rm->ctrl;
    const char *be_path = vscsidev_rm->be_path;
    int rc, i, be_state;
    char *dev_path, *state_val;
    flexarray_t *back;
    libxl_device_vscsidev *v;
    xs_transaction_t t = XBT_NULL;

    /* Prealloc key+value: 1 toplevel + 1 per device */
    i = 2 * (1 + 1);
    back = flexarray_make(gc, i, 1);

    for (;;) {
        rc = libxl__xs_transaction_start(gc, &t);
        if (rc) goto out;

        state_val = libxl__xs_read(gc, t, state_path);
        LOG(DEBUG, "%s is %s", state_path, state_val);
        if (!state_val) {
            rc = ERROR_NOTFOUND;
            goto out;
        }

        be_state = atoi(state_val);
        switch (be_state) {
        case XenbusStateUnknown:
        case XenbusStateInitialising:
        case XenbusStateClosing:
        case XenbusStateClosed:
        default:
            /* The backend is in a bad state */
            rc = ERROR_FAIL;
            goto out;
        case XenbusStateInitialised:
        case XenbusStateReconfiguring:
        case XenbusStateReconfigured:
            /* Backend is still busy, caller has to retry */
            rc = ERROR_NOT_READY;
            goto out;
        case XenbusStateInitWait:
            /* The frontend did not connect yet */
            *be_wait = XenbusStateInitWait;
            vscsidev_rm->dev_wait = XenbusStateClosing;
            break;
        case XenbusStateConnected:
            /* The backend can handle reconfigure */
            *be_wait = XenbusStateConnected;
            vscsidev_rm->dev_wait = XenbusStateClosed;
            flexarray_append_pair(back, "state",
                                  GCSPRINTF("%d", XenbusStateReconfiguring));
            break;
        }

        /* Append new vscsidev or skip existing  */
        for (i = 0; i < ctrl->num_vscsidevs; i++) {
            unsigned int nb = 0;
            v = ctrl->vscsidevs + i;
            dev_path = GCSPRINTF("%s/vscsi-devs/dev-%u", be_path, v->vscsidev_id);
            if (!libxl__xs_directory(gc, XBT_NULL, dev_path, &nb)) {
                /* FIXME Sanity check */
                LOG(DEBUG, "%s does not exist anymore", dev_path);
                continue;
            }
            rc = vscsidev_be_set_rm(gc, v, back);
            if (rc) goto out;
        }

        libxl__xs_writev(gc, t, be_path,
                         libxl__xs_kvs_of_flexarray(gc, back, back->count));

        rc = libxl__xs_transaction_commit(gc, &t);
        if (!rc) break;
        if (rc < 0) goto out;
    }

    rc = 0;

out:
    libxl__xs_transaction_abort(gc, &t);
    return rc;
}

static void vscsictrl_remove_be_dev(libxl__gc *gc,
                                    libxl_device_vscsidev *v,
                                    xs_transaction_t t,
                                    const char *be_path,
                                    int dev_wait)
{
    char *dir, *path, *val;

    dir = GCSPRINTF("%s/vscsi-devs/dev-%u", be_path, v->vscsidev_id);
    path = GCSPRINTF("%s/state", dir);
    val = libxl__xs_read(gc, t, path);
    LOG(DEBUG, "%s is %s", path, val);
    if (val && strcmp(val, GCSPRINTF("%d", dev_wait)) == 0) {
        xs_rm(CTX->xsh, t, GCSPRINTF("%s/state", dir));
        xs_rm(CTX->xsh, t, GCSPRINTF("%s/p-devname", dir));
        xs_rm(CTX->xsh, t, GCSPRINTF("%s/p-dev", dir));
        xs_rm(CTX->xsh, t, GCSPRINTF("%s/v-dev", dir));
        xs_rm(CTX->xsh, t, dir);
    } else {
        LOG(ERROR, "%s has %s, expected %d", path, val, dev_wait);
    }
}

static void vscsictrl_remove_be_cb(libxl__egc *egc,
                                   libxl__ev_devstate *ds,
                                   int rc)
{
    libxl__ao_device *aodev = CONTAINER_OF(ds, *aodev, backend_ds);
    STATE_AO_GC(aodev->ao);
    vscsidev_rm_t *vscsidev_rm = CONTAINER_OF(aodev->dev, *vscsidev_rm, dev);
    libxl_device_vscsictrl *ctrl = vscsidev_rm->ctrl;
    xs_transaction_t t = XBT_NULL;
    int i;

    for (;;) {
        rc = libxl__xs_transaction_start(gc, &t);
        if (rc) goto out;

        for (i = 0; i < ctrl->num_vscsidevs; i++)
            vscsictrl_remove_be_dev(gc, ctrl->vscsidevs + i, t,
                                    vscsidev_rm->be_path,
                                    vscsidev_rm->dev_wait);

        rc = libxl__xs_transaction_commit(gc, &t);
        if (!rc) break;
        if (rc < 0) goto out;
    }

out:
    aodev->rc = rc;
    aodev->callback(egc, aodev);
}

static void vscsidev__remove(libxl__egc *egc, libxl__ao_device *aodev)
{
    STATE_AO_GC(aodev->ao);
    vscsidev_rm_t *vscsidev_rm = CONTAINER_OF(aodev->dev, *vscsidev_rm, dev);
    char *state_path;
    int rc, be_wait;

    vscsidev_rm->be_path = libxl__device_backend_path(gc, aodev->dev);
    state_path = GCSPRINTF("%s/state", vscsidev_rm->be_path);

    rc = vscsictrl_reconfigure_rm(aodev, state_path, &be_wait);
    if (rc) goto out;

    rc = libxl__ev_devstate_wait(ao, &aodev->backend_ds,
                                 vscsictrl_remove_be_cb,
                                 state_path, be_wait,
                                 LIBXL_DESTROY_TIMEOUT * 1000);
    if (rc) {
        LOG(ERROR, "unable to wait for %s", state_path);
        goto out;
    }

    return;

out:
    aodev->rc = rc;
    /* Notify that this is done */
    aodev->callback(egc, aodev);
}

static int vscsidev_remove(libxl_ctx *ctx,
                           uint32_t domid,
                           libxl_device_vscsictrl *vscsictrl,
                           const libxl_asyncop_how *ao_how)
{
    AO_CREATE(ctx, domid, ao_how);
    libxl__ao_device *aodev;
    vscsidev_rm_t *vscsidev_rm;
    libxl__device *device;
    int rc;

    GCNEW(aodev);

    GCNEW(vscsidev_rm);
    vscsidev_rm->ctrl = vscsictrl;
    device = &vscsidev_rm->dev;

    rc = libxl__device_from_vscsictrl(gc, domid, vscsictrl, device);
    if (rc) goto out;

    libxl__prepare_ao_device(ao, aodev);
    aodev->dev = device;
    aodev->action = LIBXL__DEVICE_ACTION_REMOVE;
    aodev->callback = vscsi_aodev_complete;

    vscsidev__remove(egc, aodev);

out:
    if (rc) AO_CREATE_FAIL(rc);
    return AO_INPROGRESS;
}

static int vscsidev_backend_add(libxl__gc *gc,
                                libxl_device_vscsidev *v,
                                flexarray_t *back)
{
    int rc;
    char *dir;
    unsigned int hst, chn, tgt;
    unsigned long long lun;


    dir = GCSPRINTF("vscsi-devs/dev-%u", v->vscsidev_id);
    switch (v->pdev.type) {
    case LIBXL_VSCSI_PDEV_TYPE_WWN:
        flexarray_append_pair(back,
                              GCSPRINTF("%s/p-dev", dir),
                              v->pdev.u.wwn.m);
        break;
    case LIBXL_VSCSI_PDEV_TYPE_HCTL:
        hst = v->pdev.u.hctl.m.hst;
        chn = v->pdev.u.hctl.m.chn;
        tgt = v->pdev.u.hctl.m.tgt;
        lun = v->pdev.u.hctl.m.lun;
        flexarray_append_pair(back,
                              GCSPRINTF("%s/p-dev", dir),
                              GCSPRINTF("%u:%u:%u:%llu", hst, chn, tgt, lun));
        break;
    case LIBXL_VSCSI_PDEV_TYPE_INVALID:
        /* fallthrough */
    default:
        rc = ERROR_FAIL;
        goto out;
    }
    flexarray_append_pair(back,
                          GCSPRINTF("%s/p-devname", dir),
                          v->pdev.p_devname);
    hst = v->vdev.hst;
    chn = v->vdev.chn;
    tgt = v->vdev.tgt;
    lun = v->vdev.lun;
    flexarray_append_pair(back,
                          GCSPRINTF("%s/v-dev", dir),
                          GCSPRINTF("%u:%u:%u:%llu", hst, chn, tgt, lun));
    flexarray_append_pair(back,
                          GCSPRINTF("%s/state", dir),
                          GCSPRINTF("%d", XenbusStateInitialising));
    rc = 0;
out:
    return rc;
}

static void vscsictrl_new_backend(libxl__egc *egc,
                                  libxl__ao_device *aodev,
                                  libxl_device_vscsictrl *vscsictrl,
                                  libxl_domain_config *d_config)
{
    STATE_AO_GC(aodev->ao);
    int rc, i;
    flexarray_t *back;
    flexarray_t *front;
    libxl_device_vscsidev *v;
    xs_transaction_t t = XBT_NULL;

    /* Prealloc key+value: 4 toplevel + 4 per device */
    i = 2 * (4 + (4 * vscsictrl->num_vscsidevs));
    back = flexarray_make(gc, i, 1);
    front = flexarray_make(gc, 2 * 2, 1);

    flexarray_append_pair(back,
                          "frontend-id",
                          GCSPRINTF("%d", aodev->dev->domid));
    flexarray_append_pair(back, "online", "1");
    flexarray_append_pair(back,
                          "state",
                          GCSPRINTF("%d", XenbusStateInitialising));
    flexarray_append_pair(back,
                          "libxl_ctrl_index",
                          GCSPRINTF("%d", vscsictrl->idx));
    flexarray_append_pair(back, "feature-host",
                          libxl_defbool_val(vscsictrl->scsi_raw_cmds) ?
                          "1" : "0");

    flexarray_append_pair(front,
                          "backend-id",
                          GCSPRINTF("%d", vscsictrl->backend_domid));
    flexarray_append_pair(front,
                          "state",
                          GCSPRINTF("%d", XenbusStateInitialising));

    for (i = 0; i < vscsictrl->num_vscsidevs; i++) {
        v = vscsictrl->vscsidevs + i;
        rc = vscsidev_backend_add(gc, v, back);
        if (rc) goto out;
    }

    for (;;) {
        rc = libxl__xs_transaction_start(gc, &t);
        if (rc) goto out;

        rc = libxl__device_exists(gc, t, aodev->dev);
        if (rc < 0) goto out;
        if (rc == 1) {              /* already exists in xenstore */
            LOG(ERROR, "device already exists in xenstore");
            rc = ERROR_DEVICE_EXISTS;
            goto out;
        }

        if (aodev->update_json) {
            rc = libxl__set_domain_configuration(gc, aodev->dev->domid, d_config);
            if (rc) goto out;
        }

        libxl__device_generic_add(gc, t, aodev->dev,
                                  libxl__xs_kvs_of_flexarray(gc, back,
                                                             back->count),
                                  libxl__xs_kvs_of_flexarray(gc, front,
                                                             front->count),
                                  NULL);

        rc = libxl__xs_transaction_commit(gc, &t);
        if (!rc) break;
        if (rc < 0) goto out;
    }

    libxl__wait_device_connection(egc, aodev);
    return;

out:
    libxl__xs_transaction_abort(gc, &t);
    aodev->rc = rc;
    aodev->callback(egc, aodev);
}

static void vscsictrl_do_reconfigure_add_cb(libxl__egc *egc,
                                            libxl__ev_devstate *ds,
                                            int rc)
{
    libxl__ao_device *aodev = CONTAINER_OF(ds, *aodev, backend_ds);
    STATE_AO_GC(aodev->ao);
    aodev->rc = rc;
    aodev->callback(egc, aodev);
}

static void vscsictrl_do_reconfigure_add(libxl__egc *egc,
                                         libxl__ao_device *aodev,
                                         libxl_device_vscsictrl *vscsictrl,
                                         libxl_domain_config *d_config)
{
    STATE_AO_GC(aodev->ao);
    int rc, i, be_state, be_wait;
    const char *be_path;
    char *dev_path, *state_path, *state_val;
    flexarray_t *back;
    libxl_device_vscsidev *v;
    xs_transaction_t t = XBT_NULL;
    bool do_reconfigure = false;

    /* Prealloc key+value: 1 toplevel + 4 per device */
    i = 2 * (1 + (4 * vscsictrl->num_vscsidevs));
    back = flexarray_make(gc, i, 1);

    be_path = libxl__device_backend_path(gc, aodev->dev);
    state_path = GCSPRINTF("%s/state", be_path);

    for (;;) {
        rc = libxl__xs_transaction_start(gc, &t);
        if (rc) goto out;

        state_val = libxl__xs_read(gc, t, state_path);
        LOG(DEBUG, "%s is %s", state_path, state_val);
        if (!state_val) {
            rc = ERROR_FAIL;
            goto out;
        }

        be_state = atoi(state_val);
        switch (be_state) {
        case XenbusStateUnknown:
        case XenbusStateInitialising:
        case XenbusStateClosing:
        case XenbusStateClosed:
        default:
            /* The backend is in a bad state */
            rc = ERROR_FAIL;
            goto out;
        case XenbusStateInitialised:
        case XenbusStateReconfiguring:
        case XenbusStateReconfigured:
            /* Backend is still busy, caller has to retry */
            rc = ERROR_NOT_READY;
            goto out;
        case XenbusStateInitWait:
            /* The frontend did not connect yet */
            be_wait = XenbusStateInitWait;
            do_reconfigure = false;
            break;
        case XenbusStateConnected:
            /* The backend can handle reconfigure */
            be_wait = XenbusStateConnected;
            flexarray_append_pair(back, "state", GCSPRINTF("%d", XenbusStateReconfiguring));
            do_reconfigure = true;
            break;
        }

        /* Append new vscsidev or skip existing  */
        for (i = 0; i < vscsictrl->num_vscsidevs; i++) {
            unsigned int nb = 0;
            v = vscsictrl->vscsidevs + i;
            dev_path = GCSPRINTF("%s/vscsi-devs/dev-%u", be_path, v->vscsidev_id);
            if (libxl__xs_directory(gc, XBT_NULL, dev_path, &nb)) {
                /* FIXME Sanity check */
                LOG(DEBUG, "%s exists already with %u entries", dev_path, nb);
                continue;
            }
            rc = vscsidev_backend_add(gc, v, back);
            if (rc) goto out;
        }

        if (aodev->update_json) {
            rc = libxl__set_domain_configuration(gc, aodev->dev->domid, d_config);
            if (rc) goto out;
        }

        libxl__xs_writev(gc, t, be_path,
                         libxl__xs_kvs_of_flexarray(gc, back, back->count));

        rc = libxl__xs_transaction_commit(gc, &t);
        if (!rc) break;
        if (rc < 0) goto out;
    }

    if (do_reconfigure) {
        rc = libxl__ev_devstate_wait(ao, &aodev->backend_ds,
                                     vscsictrl_do_reconfigure_add_cb,
                                     state_path, be_wait,
                                     LIBXL_INIT_TIMEOUT * 1000);
        if (rc) goto out;
    }
    return;

out:
    libxl__xs_transaction_abort(gc, &t);
    aodev->rc = rc;
    aodev->callback(egc, aodev);
}

static int vscsictrl_next_vscsidev_id(libxl__gc *gc,
                                      const char *libxl_path,
                                      libxl_devid *vscsidev_id)
{
    const char *val;
    xs_transaction_t t = XBT_NULL;
    unsigned int id;
    int rc;

    for (;;) {
        rc = libxl__xs_transaction_start(gc, &t);
        if (rc) goto out;

        val = libxl__xs_read(gc, t, libxl_path);
        id = val ? strtoul(val, NULL, 10) : 0;

        LOG(DEBUG, "%s = %s vscsidev_id %u", libxl_path, val, id);

        val = GCSPRINTF("%u", id + 1);
        rc = libxl__xs_write_checked(gc, t, libxl_path, val);
        if (rc) goto out;

        rc = libxl__xs_transaction_commit(gc, &t);
        if (!rc) break;
        if (rc < 0) goto out;
    }

    *vscsidev_id = id;
    rc = 0;

out:
    libxl__xs_transaction_abort(gc, &t);
    return rc;
}

static int vscsictrl_assign_vscsidev_ids(libxl__gc *gc,
                                         uint32_t domid,
                                         libxl_device_vscsictrl *vscsictrl)
{
    libxl_device_vscsidev *dev;
    libxl_devid vscsidev_id;
    const char *libxl_path;
    int rc, i;

    libxl_path = GCSPRINTF("%s/vscsi/%u/next_vscsidev_id",
                           libxl__xs_libxl_path(gc, domid),
                           vscsictrl->devid);
    for (i = 0; i < vscsictrl->num_vscsidevs; i++) {
        dev = &vscsictrl->vscsidevs[i];
        if (dev->vscsidev_id >= 0)
            continue;
        rc = vscsictrl_next_vscsidev_id(gc, libxl_path, &vscsidev_id);
        if (rc) {
            LOG(ERROR, "failed to assign vscsidev_id to %s for %s",
                libxl_path, dev->pdev.p_devname);
            goto out;
        }
        dev->vscsidev_id = vscsidev_id;
    }

    rc = 0;
out:
    return rc;
}

static void vscsictrl_update_json(libxl__egc *egc,
                                  libxl__ao_device *aodev,
                                  libxl_device_vscsictrl *vscsictrl,
                                  vscsictrl_add fn)
{
    STATE_AO_GC(aodev->ao);
    int rc;
    uint32_t domid = aodev->dev->domid;
    libxl_device_vscsictrl vscsictrl_saved;
    libxl_domain_config d_config;
    libxl__domain_userdata_lock *lock = NULL;

    libxl_domain_config_init(&d_config);
    libxl_device_vscsictrl_init(&vscsictrl_saved);

    libxl_device_vscsictrl_copy(CTX, &vscsictrl_saved, vscsictrl);

    rc = vscsictrl_assign_vscsidev_ids(gc, domid, &vscsictrl_saved);
    if (rc) goto out;

    if (aodev->update_json) {
        lock = libxl__lock_domain_userdata(gc, domid);
        if (!lock) {
            rc = ERROR_LOCK_FAIL;
            goto out;
        }

        rc = libxl__get_domain_configuration(gc, domid, &d_config);
        if (rc) goto out;

        /* Replace or append the copy to the domain config */
        DEVICE_ADD(vscsictrl, vscsictrls, domid, &vscsictrl_saved, COMPARE_DEVID, &d_config);
    }

    fn(egc, aodev, &vscsictrl_saved, &d_config);

out:
    if (lock) libxl__unlock_domain_userdata(lock);
    libxl_device_vscsictrl_dispose(&vscsictrl_saved);
    libxl_domain_config_dispose(&d_config);
    if (rc) {
        aodev->rc = rc;
        aodev->callback(egc, aodev);
    }
}

static void vscsictrl__reconfigure_add(libxl__egc *egc,
                                       uint32_t domid,
                                       libxl_device_vscsictrl *vscsictrl,
                                       libxl__ao_device *aodev)
{
    STATE_AO_GC(aodev->ao);
    libxl__device *device;
    vscsictrl_add fn;
    int rc;

    GCNEW(device);
    rc = libxl__device_from_vscsictrl(gc, domid, vscsictrl, device);
    if (rc) goto out;
    aodev->dev = device;

    fn = vscsictrl_do_reconfigure_add;
    vscsictrl_update_json(egc, aodev, vscsictrl, fn);
    return;

out:
    aodev->rc = rc;
    aodev->callback(egc, aodev);
}

static int vscsictrl_reconfigure_add(libxl_ctx *ctx,
                                     uint32_t domid,
                                     libxl_device_vscsictrl *vscsictrl,
                                     const libxl_asyncop_how *ao_how)
{
    AO_CREATE(ctx, domid, ao_how);
    libxl__ao_device *aodev;

    GCNEW(aodev);
    libxl__prepare_ao_device(ao, aodev);
    aodev->action = LIBXL__DEVICE_ACTION_ADD;
    aodev->callback = vscsi_aodev_complete;
    aodev->update_json = true;
    vscsictrl__reconfigure_add(egc, domid, vscsictrl, aodev);

    return AO_INPROGRESS;
}

void libxl__device_vscsictrl_add(libxl__egc *egc, uint32_t domid,
                                 libxl_device_vscsictrl *vscsictrl,
                                 libxl__ao_device *aodev)
{
    STATE_AO_GC(aodev->ao);
    libxl__device *device;
    vscsictrl_add fn;
    int rc;

    if (vscsictrl->devid == -1) {
        if ((vscsictrl->devid = libxl__device_nextid(gc, domid, "vscsi")) < 0) {
            rc = ERROR_FAIL;
            goto out;
        }
    }

    GCNEW(device);
    rc = libxl__device_from_vscsictrl(gc, domid, vscsictrl, device);
    if (rc) goto out;
    aodev->dev = device;

    fn = vscsictrl_new_backend;
    vscsictrl_update_json(egc, aodev, vscsictrl, fn);
    return;

out:
    aodev->rc = rc;
    aodev->callback(egc, aodev);
}

int libxl_device_vscsictrl_remove(libxl_ctx *ctx, uint32_t domid,
                                  libxl_device_vscsictrl *vscsictrl,
                                  const libxl_asyncop_how *ao_how)
{
    return vscsictrl_remove(ctx, domid, vscsictrl, ao_how, 0);
}

int libxl_device_vscsictrl_destroy(libxl_ctx *ctx, uint32_t domid,
                                   libxl_device_vscsictrl *vscsictrl,
                                   const libxl_asyncop_how *ao_how)
{
    return vscsictrl_remove(ctx, domid, vscsictrl, ao_how, 1);
}

libxl_device_vscsictrl *libxl_device_vscsictrl_list(libxl_ctx *ctx,
                                                    uint32_t domid,
                                                    int *num)
{
    GC_INIT(ctx);
    libxl_device_vscsictrl *ctrls = NULL;
    int rc, num_ctrls = 0;

    *num = 0;

    rc = vscsi_collect_ctrls(gc, domid, &ctrls, &num_ctrls);
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

    val = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/idx", vscsipath));
    vscsiinfo->idx = val ? strtoul(val, NULL, 10) : -1;

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

int libxl_device_vscsidev_add(libxl_ctx *ctx, uint32_t domid,
                              libxl_device_vscsidev *vscsidev,
                              const libxl_asyncop_how *ao_how)
{
    GC_INIT(ctx);
    libxl_device_vscsictrl *vc, *ctrls = NULL;
    libxl_device_vscsidev *vd;
    int c, d, rc, num_ctrls = 0;
    int duplicate = 0;

    rc = vscsi_collect_ctrls(gc, domid, &ctrls, &num_ctrls);
    if (rc != 0) goto out;


    for (c = 0; c < num_ctrls; ++c) {
        vc = ctrls + c;
        if (vc->idx != vscsidev->vdev.hst)
            continue;

        for (d = 0; d < vc->num_vscsidevs; d++) {
            vd = vc->vscsidevs + d;
            if (vd->vdev.hst == vscsidev->vdev.hst &&
                vd->vdev.chn == vscsidev->vdev.chn &&
                vd->vdev.tgt == vscsidev->vdev.tgt &&
                vd->vdev.lun == vscsidev->vdev.lun) {
                unsigned long long lun = vd->vdev.lun;
                LOG(ERROR, "vdev '%u:%u:%u:%llu' is already used.\n",
                    vd->vdev.hst, vd->vdev.chn, vd->vdev.tgt, lun);
                rc = ERROR_DEVICE_EXISTS;
                duplicate = 1;
                break;
            }
        }

        if (!duplicate) {
            /* Append vscsidev to this vscsictrl, trigger reconfigure */
            libxl_device_vscsictrl_append_vscsidev(ctx, vc, vscsidev);
            rc = vscsictrl_reconfigure_add(ctx, domid, vc, ao_how);
        }
        break;
    }

    for (c = 0; c < num_ctrls; ++c)
        libxl_device_vscsictrl_dispose(ctrls + c);
    free(ctrls);

out:
    GC_FREE;
    return rc;
}

int libxl_device_vscsidev_remove(libxl_ctx *ctx, uint32_t domid,
                                 libxl_device_vscsidev *vscsidev,
                                 const libxl_asyncop_how *ao_how)
{
    GC_INIT(ctx);
    libxl_device_vscsictrl *vc, *ctrls = NULL;
    libxl_device_vscsidev *vd;
    int c, d, rc, num_ctrls = 0;
    int found = 0, idx;
    int head, tail, i;

    rc = vscsi_collect_ctrls(gc, domid, &ctrls, &num_ctrls);
    if (rc != 0) goto out;


    for (c = 0; c < num_ctrls; ++c) {
        vc = ctrls + c;

        for (d = 0; d < vc->num_vscsidevs; d++) {
            vd = vc->vscsidevs + d;
            if (vd->vdev.hst == vscsidev->vdev.hst &&
                vd->vdev.chn == vscsidev->vdev.chn &&
                vd->vdev.tgt == vscsidev->vdev.tgt &&
                vd->vdev.lun == vscsidev->vdev.lun) {
                found = 1;
                idx = d;
                break;
            }
        }

        if (found) {
            if (vc->num_vscsidevs > 1) {
                /* Prepare vscsictrl, leave only desired vscsidev */
                head = idx;
                tail = vc->num_vscsidevs - idx - 1;
                for (i = 0; i < head; i++)
                    libxl_device_vscsictrl_remove_vscsidev(ctx, vc, 0);
                for (i = 0; i < tail; i++)
                    libxl_device_vscsictrl_remove_vscsidev(ctx, vc, 1);

                /* Remove single vscsidev connected to this vscsictrl */
                rc = vscsidev_remove(ctx, domid, vc, ao_how);
            } else {
                /* Wipe entire vscsictrl */;
                rc = vscsictrl_remove(ctx, domid, vc, ao_how, 0);
            }
            break;
        }
    }

    for (c = 0; c < num_ctrls; ++c)
        libxl_device_vscsictrl_dispose(ctrls + c);
    free(ctrls);

    if (!found)
        rc = ERROR_NOTFOUND;

out:
    GC_FREE;
    return rc;
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

void libxl_device_vscsictrl_remove_vscsidev(libxl_ctx *ctx,
                                            libxl_device_vscsictrl *ctrl,
                                            unsigned int idx)
{
    GC_INIT(ctx);
    if (idx >= ctrl->num_vscsidevs)
        return;
    libxl_device_vscsidev_dispose(&ctrl->vscsidevs[idx]);
    if (ctrl->num_vscsidevs > idx + 1)
        memmove(&ctrl->vscsidevs[idx],
                &ctrl->vscsidevs[idx + 1],
                (ctrl->num_vscsidevs - idx - 1) * sizeof(*ctrl->vscsidevs));
    ctrl->vscsidevs = libxl__realloc(NOGC, ctrl->vscsidevs, sizeof(*ctrl->vscsidevs) * (ctrl->num_vscsidevs - 1));
    ctrl->num_vscsidevs--;
    GC_FREE;
}

/*
 * Local variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
