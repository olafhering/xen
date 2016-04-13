/*
 * libxlu_vscsi.c - xl configuration file parsing: setup and helper functions
 *
 * Copyright (C) 2016      SUSE Linux GmbH
 * Author Olaf Hering <olaf@aepfle.de>
 * Author Ondřej Holeček <aaannz@gmail.com>
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
#include <unistd.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "libxlu_internal.h"

#ifdef __linux__
#define LOG(_c, _x, _a...) \
        if((_c) && (_c)->report) fprintf((_c)->report, "%s(%u): " _x "\n", __func__, __LINE__, ##_a)

#define XLU_SYSFS_TARGET_PVSCSI "/sys/kernel/config/target/xen-pvscsi"
#define XLU_WWN_LEN 16
struct xlu__vscsi_target {
    XLU_Config *cfg;
    libxl_vscsi_hctl *pdev_hctl;
    libxl_vscsi_pdev *pdev;
    char path[PATH_MAX];
    char udev_path[PATH_MAX];
    char wwn[XLU_WWN_LEN + 1];
    unsigned long long lun;
};

static int xlu__vscsi_parse_hctl(char *str, libxl_vscsi_hctl *hctl)
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

static char *xlu__vscsi_trim_string(char *s)
{
    size_t len;

    while (isspace(*s))
        s++;
    len = strlen(s);
    while (len-- > 1 && isspace(s[len]))
        s[len] = '\0';
    return s;
}


static int xlu__vscsi_parse_dev(XLU_Config *cfg, char *pdev, libxl_vscsi_hctl *hctl)
{
    struct stat dentry;
    char *sysfs = NULL;
    const char *type;
    int rc, found = 0;
    DIR *dirp;
    struct dirent *de;

    /* stat pdev to get device's sysfs entry */
    if (stat (pdev, &dentry) < 0) {
        LOG(cfg, "%s, device node not found", pdev);
        rc = ERROR_INVAL;
        goto out;
    }

    if (S_ISBLK (dentry.st_mode)) {
        type = "block";
    } else if (S_ISCHR (dentry.st_mode)) {
        type = "char";
    } else {
        LOG(cfg, "%s, device node not a block or char device", pdev);
        rc = ERROR_INVAL;
        goto out;
    }

    /* /sys/dev/type/major:minor symlink added in 2.6.27 */
    if (asprintf(&sysfs, "/sys/dev/%s/%u:%u/device/scsi_device", type,
                 major(dentry.st_rdev), minor(dentry.st_rdev)) < 0) {
        sysfs = NULL;
        rc = ERROR_NOMEM;
        goto out;
    }

    dirp = opendir(sysfs);
    if (!dirp) {
        LOG(cfg, "%s, no major:minor link in sysfs", pdev);
        rc = ERROR_INVAL;
        goto out;
    }

    while ((de = readdir(dirp))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;

        if (xlu__vscsi_parse_hctl(de->d_name, hctl))
            continue;

        found = 1;
        break;
    }
    closedir(dirp);

    if (!found) {
        LOG(cfg, "%s, no h:c:t:l link in sysfs", pdev);
        rc = ERROR_INVAL;
        goto out;
    }

    rc = 0;
out:
    free(sysfs);
    return rc;
}

static bool xlu__vscsi_compare_hctl(libxl_vscsi_hctl *a, libxl_vscsi_hctl *b)
{
    if (a->hst == b->hst &&
        a->chn == b->chn &&
        a->tgt == b->tgt &&
        a->lun == b->lun)
        return true;
    return false;
}

/* Finally at
 * /sys/kernel/config/target/xen-pvscsi/naa.<wwn>/tpgt_1/lun/lun_0/<X>/udev_path
 */
static bool xlu__vscsi_compare_udev(struct xlu__vscsi_target *tgt)
{
    bool ret;
    int fd;
    ssize_t read_sz;
    libxl_vscsi_hctl udev_hctl;

    libxl_vscsi_hctl_init(&udev_hctl);

    fd = open(tgt->path, O_RDONLY);
    if (fd < 0){
        ret = false;
        goto out;
    }
    read_sz = read(fd, &tgt->udev_path, sizeof(tgt->udev_path) - 1);
    close(fd);

    if (read_sz <= 0 || read_sz > sizeof(tgt->udev_path) - 1) {
        ret = false;
        goto out;
    }
    tgt->udev_path[read_sz] = '\0';
    read_sz--;
    if (tgt->udev_path[read_sz] == '\n')
        tgt->udev_path[read_sz] = '\0';

    if (xlu__vscsi_parse_dev(tgt->cfg, tgt->udev_path, &udev_hctl)) {
        ret = false;
        goto out;
    }
    ret = xlu__vscsi_compare_hctl(tgt->pdev_hctl, &udev_hctl);

out:
    libxl_vscsi_hctl_dispose(&udev_hctl);
    return ret;
}

/* /sys/kernel/config/target/xen-pvscsi/naa.<wwn>/tpgt_1/lun/lun_0/<X>/udev_path */
static bool xlu__vscsi_walk_dir_lun(struct xlu__vscsi_target *tgt)
{
    bool found;
    DIR *dirp;
    struct dirent *de;
    size_t path_len = strlen(tgt->path);
    char *subdir = &tgt->path[path_len];

    dirp = opendir(tgt->path);
    if (!dirp)
        return false;

    found = false;
    while ((de = readdir(dirp))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;

        snprintf(subdir, sizeof(tgt->path) - path_len, "/%s/udev_path", de->d_name);

        found = xlu__vscsi_compare_udev(tgt);
        if (found)
            break;

        *subdir = '\0';
    }
    closedir(dirp);
    return found;
}

/* /sys/kernel/config/target/xen-pvscsi/naa.<wwn>/tpgt_1/lun/lun_0 */
static bool xlu__vscsi_walk_dir_luns(struct xlu__vscsi_target *tgt)
{
    bool found;
    DIR *dirp;
    struct dirent *de;
    size_t path_len = strlen(tgt->path);
    char *subdir = &tgt->path[path_len];

    dirp = opendir(tgt->path);
    if (!dirp)
        return false;

    found = false;
    while ((de = readdir(dirp))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;

        if (sscanf(de->d_name, "lun_%llu", &tgt->lun) != 1)
            continue;


        snprintf(subdir, sizeof(tgt->path) - path_len, "/%s", de->d_name);

        found = xlu__vscsi_walk_dir_lun(tgt);
        if (found)
            break;

        *subdir = '\0';
    }
    closedir(dirp);
    return found;
}

/* /sys/kernel/config/target/xen-pvscsi/naa.<wwn>/tpgt_1 */
static bool xlu__vscsi_walk_dir_naa(struct xlu__vscsi_target *tgt)
{
    bool found;
    DIR *dirp;
    struct dirent *de;
    size_t path_len = strlen(tgt->path);
    char *subdir = &tgt->path[path_len];
    unsigned int tpgt;

    dirp = opendir(tgt->path);
    if (!dirp)
        return false;

    found = false;
    while ((de = readdir(dirp))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;

        if (sscanf(de->d_name, "tpgt_%u", &tpgt) != 1)
            continue;

        snprintf(subdir, sizeof(tgt->path) - path_len, "/%s/lun", de->d_name);

        found = xlu__vscsi_walk_dir_luns(tgt);
        if (found)
            break;

        *subdir = '\0';
    }
    closedir(dirp);
    return found;
}

/* /sys/kernel/config/target/xen-pvscsi/naa.<wwn> */
static bool xlu__vscsi_find_target_wwn(struct xlu__vscsi_target *tgt)
{
    bool found;
    DIR *dirp;
    struct dirent *de;
    size_t path_len = strlen(tgt->path);
    char *subdir = &tgt->path[path_len];

    dirp = opendir(tgt->path);
    if (!dirp)
        return false;

    found = false;
    while ((de = readdir(dirp))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;

        if (sscanf(de->d_name, "naa.%16[0-9a-fA-F]", tgt->wwn) != 1)
            continue;

        snprintf(subdir, sizeof(tgt->path) - path_len, "/%s", de->d_name);

        found = xlu__vscsi_walk_dir_naa(tgt);
        if (found)
            break;

        *subdir = '\0';
    }
    closedir(dirp);
    return found;
}

/*
 * Convert pdev from config string into pdev property for backend,
 * which is either h:c:t:l for xenlinux or naa.wwn:lun for pvops
 */
static int xlu__vscsi_dev_to_pdev(XLU_Config *cfg, libxl_ctx *ctx, char *str,
                                  libxl_vscsi_hctl *pdev_hctl,
                                  libxl_vscsi_pdev *pdev)
{
    int rc = ERROR_INVAL;
    struct xlu__vscsi_target *tgt;
    static const char xen_pvscsi[] = XLU_SYSFS_TARGET_PVSCSI;

    /* First get hctl representation of config item */
    if (xlu__vscsi_parse_dev(cfg, str, pdev_hctl))
        goto out;

    /* Check if a SCSI target item exists for the config item */
    if (access(xen_pvscsi, F_OK) == 0) {
        tgt = calloc(1, sizeof(*tgt));
        if (!tgt) {
            rc = ERROR_NOMEM;
            goto out;
        }
        tgt->cfg = cfg;
        tgt->pdev_hctl = pdev_hctl;
        tgt->pdev = pdev;
        snprintf(tgt->path, sizeof(tgt->path), "%s", xen_pvscsi);
        if (xlu__vscsi_find_target_wwn(tgt) == true) {
            LOG(cfg, "'%s' maps to '%s(%s)'", str, tgt->path, tgt->udev_path);
            libxl_vscsi_pdev_init_type(pdev, LIBXL_VSCSI_PDEV_TYPE_WWN);
            if (asprintf(&pdev->u.wwn.m, "naa.%s:%llu", tgt->wwn, tgt->lun) < 0) {
                rc = ERROR_NOMEM;
                goto out;
            }
        }
        free(tgt);
    } else {
        /* Assume xenlinux backend */
        libxl_vscsi_pdev_init_type(pdev, LIBXL_VSCSI_PDEV_TYPE_HCTL);
        libxl_vscsi_hctl_copy(ctx, &pdev->u.hctl.m, pdev_hctl);
    }
    rc = 0;

out:
    return rc;
}

/* WWN as understood by pvops */
static int xlu__vscsi_wwn_to_pdev(XLU_Config *cfg, char *str, libxl_vscsi_pdev *pdev)
{
    int rc = ERROR_INVAL;
    unsigned long long lun;
    char wwn[XLU_WWN_LEN + 1];

    memset(wwn, 0, sizeof(wwn));
    if (sscanf(str, "naa.%16[0-9a-fA-F]:%llu", wwn, &lun) == 2) {
        libxl_vscsi_pdev_init_type(pdev, LIBXL_VSCSI_PDEV_TYPE_WWN);
        pdev->u.wwn.m = strdup(str);
        rc = pdev->u.wwn.m ? 0 : ERROR_NOMEM;
    }
    return rc;
}

static int xlu__vscsi_parse_pdev(XLU_Config *cfg, libxl_ctx *ctx, char *str,
                                 libxl_vscsi_pdev *pdev)
{
    int rc = ERROR_INVAL;
    libxl_vscsi_hctl pdev_hctl;

    libxl_vscsi_hctl_init(&pdev_hctl);
    if (strncmp(str, "/dev/", 5) == 0) {
        rc = xlu__vscsi_dev_to_pdev(cfg, ctx, str, &pdev_hctl, pdev);
    } else if (strncmp(str, "naa.", 4) == 0) {
        rc = xlu__vscsi_wwn_to_pdev(cfg, str, pdev);
    } else if (xlu__vscsi_parse_hctl(str, &pdev_hctl) == 0) {
        /* Either xenlinux, or pvops with properly configured alias in sysfs */
        libxl_vscsi_pdev_init_type(pdev, LIBXL_VSCSI_PDEV_TYPE_HCTL);
        libxl_vscsi_hctl_copy(ctx, &pdev->u.hctl.m, &pdev_hctl);
        rc = 0;
    }

    if (rc == 0) {
            pdev->p_devname = strdup(str);
            if (!pdev->p_devname)
                rc = ERROR_NOMEM;
    }

    libxl_vscsi_hctl_dispose(&pdev_hctl);
    return rc;
}

int xlu_vscsi_parse(XLU_Config *cfg, libxl_ctx *ctx, const char *str,
                    libxl_device_vscsictrl *new_ctrl,
                    libxl_device_vscsidev *new_dev)
{
    int rc;
    char *tmp, *pdev, *vdev, *fhost;

    tmp = strdup(str);
    if (!tmp) {
        rc = ERROR_NOMEM;
        goto out;
    }

    pdev = strtok(tmp, ",");
    vdev = strtok(NULL, ",");
    fhost = strtok(NULL, ",");
    if (!(pdev && vdev)) {
        LOG(cfg, "invalid devspec: '%s'\n", str);
        rc = ERROR_INVAL;
        goto out;
    }

    pdev = xlu__vscsi_trim_string(pdev);
    vdev = xlu__vscsi_trim_string(vdev);

    rc = xlu__vscsi_parse_pdev(cfg, ctx, pdev, &new_dev->pdev);
    if (rc) {
        LOG(cfg, "failed to parse %s, rc == %d", pdev, rc);
        goto out;
    }

    if (xlu__vscsi_parse_hctl(vdev, &new_dev->vdev)) {
        LOG(cfg, "invalid '%s', expecting hst:chn:tgt:lun", vdev);
        rc = ERROR_INVAL;
        goto out;
    }

    new_ctrl->idx = new_dev->vdev.hst;

    if (fhost) {
        fhost = xlu__vscsi_trim_string(fhost);
        if (strcmp(fhost, "feature-host") == 0) {
            libxl_defbool_set(&new_ctrl->scsi_raw_cmds, true);
        } else {
            LOG(cfg, "invalid option '%s', expecting %s", fhost, "feature-host");
            rc = ERROR_INVAL;
            goto out;
        }
    } else
        libxl_defbool_set(&new_ctrl->scsi_raw_cmds, false);
    rc = 0;

out:
    free(tmp);
    return rc;
}

int xlu_vscsi_get_ctrl(XLU_Config *cfg, libxl_ctx *ctx, uint32_t domid,
                       const char *str,
                       libxl_device_vscsictrl *ctrl,
                       libxl_device_vscsidev *dev,
                       libxl_device_vscsictrl *existing,
                       bool *found_existing)
{
    libxl_device_vscsictrl *vscsictrls = NULL, *tmp;
    int rc, found_ctrl = -1, i;
    int num_ctrls;


    rc = xlu_vscsi_parse(cfg, ctx, str, ctrl, dev);
    if (rc)
        goto out;

    /* Look for existing vscsictrl for given domain */
    vscsictrls = libxl_device_vscsictrl_list(ctx, domid, &num_ctrls);
    if (vscsictrls) {
        for (i = 0; i < num_ctrls; ++i) {
            if (vscsictrls[i].idx == dev->vdev.hst) {
                found_ctrl = i;
                break;
            }
        }
    }

    if (found_ctrl == -1) {
        *found_existing = false;
    } else {
        *found_existing = true;
        tmp = vscsictrls + found_ctrl;

        /* Check if the vdev address is already taken */
        for (i = 0; i < tmp->num_vscsidevs; ++i) {
            if (tmp->vscsidevs[i].vdev.chn == dev->vdev.chn &&
                tmp->vscsidevs[i].vdev.tgt == dev->vdev.tgt &&
                tmp->vscsidevs[i].vdev.lun == dev->vdev.lun) {
                unsigned long long lun = dev->vdev.lun;
                LOG(cfg, "vdev '%u:%u:%u:%llu' is already used.\n",
                    dev->vdev.hst, dev->vdev.chn, dev->vdev.tgt, lun);
                rc = ERROR_INVAL;
                goto out;
            }
        }

        if (libxl_defbool_val(ctrl->scsi_raw_cmds) !=
            libxl_defbool_val(tmp->scsi_raw_cmds)) {
            LOG(cfg, "different feature-host setting: "
                      "existing ctrl has it %s, new ctrl has it %s\n",
                libxl_defbool_val(ctrl->scsi_raw_cmds) ? "set" : "unset",
                libxl_defbool_val(tmp->scsi_raw_cmds) ? "set" : "unset");
            rc = ERROR_INVAL;
            goto out;
        }

        libxl_device_vscsictrl_copy(ctx, existing, tmp);
    }

    rc = 0;

out:
    if (vscsictrls) {
        for (i = 0; i < num_ctrls; ++i)
            libxl_device_vscsictrl_dispose(vscsictrls + i);
        free(vscsictrls);
    }
    return rc;
}

int xlu_vscsi_detach(XLU_Config *cfg, libxl_ctx *ctx, uint32_t domid, char *str)
{
    libxl_device_vscsidev dev = { };
    libxl_device_vscsictrl ctrl = { };
    int rc;
    char *tmp = NULL;

    libxl_device_vscsictrl_init(&ctrl);
    libxl_device_vscsidev_init(&dev);

    /* Create a dummy cfg */
    if (asprintf(&tmp, "0:0:0:0,%s", str) < 0) {
        LOG(cfg, "asprintf failed while removing %s from domid %u", str, domid);
        rc = ERROR_FAIL;
        goto out;
    }

    rc = xlu_vscsi_parse(cfg, ctx, tmp, &ctrl, &dev);
    if (rc) goto out;

    rc = libxl_device_vscsidev_remove(ctx, domid, &dev, NULL);
    switch (rc) {
    case ERROR_NOTFOUND:
        LOG(cfg, "detach failed: %s does not exist in domid %u", str, domid);
        break;
    default:
        break;
    }

out:
    free(tmp);
    libxl_device_vscsidev_dispose(&dev);
    libxl_device_vscsictrl_dispose(&ctrl);
    return rc;
}

int xlu_vscsi_config_add(XLU_Config *cfg,
                         libxl_ctx *ctx,
                         const char *str,
                         int *num_vscsis,
                         libxl_device_vscsictrl **vscsis)
{
    int rc, i;
    libxl_device_vscsidev dev = { };
    libxl_device_vscsictrl *tmp_ctrl, ctrl = { };
    bool ctrl_found = false;

    /*
     * #1: parse the devspec and place it in temporary ctrl+dev part
     * #2: find existing vscsictrl with number vdev.hst
     *     if found, append the vscsidev to this vscsictrl
     * #3: otherwise, create new vscsictrl and append vscsidev
     * Note: vdev.hst does not represent the index named "num_vscsis",
     *       it is a private index used just in the config file
     */
    libxl_device_vscsictrl_init(&ctrl);
    libxl_device_vscsidev_init(&dev);

    rc = xlu_vscsi_parse(cfg, ctx, str, &ctrl, &dev);
    if (rc)
        goto out;

    if (*num_vscsis) {
        for (i = 0; i < *num_vscsis; i++) {
            tmp_ctrl = *vscsis + i;
            if (tmp_ctrl->idx == dev.vdev.hst) {
                libxl_device_vscsictrl_append_vscsidev(ctx, tmp_ctrl, &dev);
                ctrl_found = true;
                break;
	           }
        }
    }

    if (!ctrl_found || !*num_vscsis) {
        tmp_ctrl = realloc(*vscsis, sizeof(ctrl) * (*num_vscsis + 1));
        if (!tmp_ctrl) {
            LOG(cfg, "realloc #%d failed", *num_vscsis + 1);
            rc = ERROR_NOMEM;
            goto out;
        }
        *vscsis = tmp_ctrl;
        tmp_ctrl = *vscsis + *num_vscsis;
        libxl_device_vscsictrl_init(tmp_ctrl);

        libxl_device_vscsictrl_copy(ctx, tmp_ctrl, &ctrl);

        libxl_device_vscsictrl_append_vscsidev(ctx, tmp_ctrl, &dev);

        (*num_vscsis)++;
    }

    rc = 0;
out:
    libxl_device_vscsidev_dispose(&dev);
    libxl_device_vscsictrl_dispose(&ctrl);
    return rc;
}
#else /* ! __linux__ */
int xlu_vscsi_get_ctrl(XLU_Config *cfg, libxl_ctx *ctx, uint32_t domid,
                       const char *str,
                       libxl_device_vscsictrl *ctrl,
                       libxl_device_vscsidev *dev,
                       libxl_device_vscsictrl *existing,
                       bool *found_existing)
{
    return ERROR_INVAL;
}

int xlu_vscsi_parse(XLU_Config *cfg,
                    libxl_ctx *ctx,
                    const char *str,
                    libxl_device_vscsictrl *new_ctrl,
                    libxl_device_vscsidev *new_dev)
{
    return ERROR_INVAL;
}

int xlu_vscsi_detach(XLU_Config *cfg,
                     libxl_ctx *ctx,
                     uint32_t domid,
                     char *str)
{
    return ERROR_INVAL;
}

int xlu_vscsi_config_add(XLU_Config *cfg,
                         libxl_ctx *ctx,
                         const char *str,
                         int *num_vscsis,
                         libxl_device_vscsictrl **vscsis)
{
    return ERROR_INVAL;
}
#endif
