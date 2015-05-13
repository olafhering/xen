/*
 * libxlu_vscsi.c - xl configuration file parsing: setup and helper functions
 *
 * Copyright (C) 2015      SUSE Linux GmbH
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
    unsigned int lun;
};

static int xlu__vscsi_parse_hctl(char *str, libxl_vscsi_hctl *hctl)
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

static bool xlu__vscsi_wwn_valid(const char *p)
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

    if (read_sz < 0 || read_sz > sizeof(tgt->udev_path) - 1) {
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

        if (sscanf(de->d_name, "lun_%u", &tgt->lun) != 1)
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

        if (sscanf(de->d_name, "naa.%16c", tgt->wwn) != 1)
            continue;
        if (!xlu__vscsi_wwn_valid(tgt->wwn))
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
            if (asprintf(&pdev->u.wwn.m, "naa.%s:%u", tgt->wwn, tgt->lun) < 0) {
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
    unsigned int lun;
    char wwn[XLU_WWN_LEN + 1];

    memset(wwn, 0, sizeof(wwn));
    if (sscanf(str, "naa.%16c:%u", wwn, &lun) == 2 && xlu__vscsi_wwn_valid(wwn)) {
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
                             libxl_device_vscsi *new_host,
                             libxl_vscsi_dev *new_dev)
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

    /* Record group index */
    new_host->v_hst = new_dev->vdev.hst;

    if (fhost) {
        fhost = xlu__vscsi_trim_string(fhost);
        if (strcmp(fhost, "feature-host") == 0) {
            libxl_defbool_set(&new_host->scsi_raw_cmds, true);
        } else {
            LOG(cfg, "invalid option '%s', expecting %s", fhost, "feature-host");
            rc = ERROR_INVAL;
            goto out;
        }
    } else
        libxl_defbool_set(&new_host->scsi_raw_cmds, false);
    rc = 0;

out:
    free(tmp);
    return rc;
}


static int xlu_vscsi_append_dev(libxl_ctx *ctx, libxl_device_vscsi *hst,
                                libxl_vscsi_dev *dev)
{
    int rc, num;
    libxl_vscsi_dev *devs, *tmp;
    libxl_devid next_vscsi_dev_id = 0;

    for (num = 0; num < hst->num_vscsi_devs; num++) {
        tmp = hst->vscsi_devs + num;
        if (next_vscsi_dev_id <= tmp->vscsi_dev_id)
            next_vscsi_dev_id = tmp->vscsi_dev_id + 1;
    }

    devs = realloc(hst->vscsi_devs, sizeof(*dev) * (hst->num_vscsi_devs + 1));
    if (!devs) {
        rc = ERROR_NOMEM;
        goto out;
    }

    hst->vscsi_devs = devs;
    libxl_vscsi_dev_init(hst->vscsi_devs + hst->num_vscsi_devs);
    dev->vscsi_dev_id = next_vscsi_dev_id;
    libxl_vscsi_dev_copy(ctx, hst->vscsi_devs + hst->num_vscsi_devs, dev);
    hst->num_vscsi_devs++;
    rc = 0;
out:
    return rc;
}

int xlu_vscsi_get_host(XLU_Config *cfg, libxl_ctx *ctx, uint32_t domid,
                       const char *str, libxl_device_vscsi *vscsi_host)
{
    libxl_vscsi_dev *new_dev = NULL;
    libxl_device_vscsi *new_host, *vscsi_hosts = NULL, *tmp;
    int rc, found_host = -1, i;
    int num_hosts;

    new_host = malloc(sizeof(*new_host));
    new_dev = malloc(sizeof(*new_dev));
    if (!(new_host && new_dev)) {
        rc = ERROR_NOMEM;
        goto out;
    }
    libxl_device_vscsi_init(new_host);
    libxl_vscsi_dev_init(new_dev);

    rc = xlu_vscsi_parse(cfg, ctx, str, new_host, new_dev);
    if (rc)
        goto out;

    /* Look for existing vscsi_host for given domain */
    vscsi_hosts = libxl_device_vscsi_list(ctx, domid, &num_hosts);
    if (vscsi_hosts) {
        for (i = 0; i < num_hosts; ++i) {
            if (vscsi_hosts[i].v_hst == new_host->v_hst) {
                found_host = i;
                break;
            }
        }
    }

    if (found_host == -1) {
        /* Not found, create new host */
        tmp = new_host;
    } else {
        tmp = vscsi_hosts + found_host;

        /* Check if the vdev address is already taken */
        for (i = 0; i < tmp->num_vscsi_devs; ++i) {
            if (tmp->vscsi_devs[i].vscsi_dev_id != -1 &&
                tmp->vscsi_devs[i].vdev.chn == new_dev->vdev.chn &&
                tmp->vscsi_devs[i].vdev.tgt == new_dev->vdev.tgt &&
                tmp->vscsi_devs[i].vdev.lun == new_dev->vdev.lun) {
                LOG(cfg, "vdev '%u:%u:%u:%u' is already used.\n",
                    new_dev->vdev.hst, new_dev->vdev.chn, new_dev->vdev.tgt, new_dev->vdev.lun);
                rc = ERROR_INVAL;
                goto out;
            }
        }

        if (libxl_defbool_val(new_host->scsi_raw_cmds) !=
            libxl_defbool_val(tmp->scsi_raw_cmds)) {
            LOG(cfg, "different feature-host setting: "
                      "existing host has it %s, new host has it %s\n",
                libxl_defbool_val(new_host->scsi_raw_cmds) ? "set" : "unset",
                libxl_defbool_val(tmp->scsi_raw_cmds) ? "set" : "unset");
            rc = ERROR_INVAL;
            goto out;
        }
    }

    libxl_device_vscsi_copy(ctx, vscsi_host, tmp);
    rc = xlu_vscsi_append_dev(ctx, vscsi_host, new_dev);
    if (rc)
        goto out;

    rc = 0;

out:
    if (vscsi_hosts) {
        for (i = 0; i < num_hosts; ++i)
            libxl_device_vscsi_dispose(&vscsi_hosts[i]);
        free(vscsi_hosts);
    }
    libxl_vscsi_dev_dispose(new_dev);
    libxl_device_vscsi_dispose(new_host);
    free(new_dev);
    free(new_host);
    return rc;
}

int xlu_vscsi_detach(XLU_Config *cfg, libxl_ctx *ctx, uint32_t domid, char *str)
{
    libxl_vscsi_dev v_dev = { }, *vd;
    libxl_device_vscsi v_hst = { }, *vh, *vscsi_hosts;
    int num_hosts, h, d, found = 0;
    char *tmp = NULL;

    libxl_device_vscsi_init(&v_hst);
    libxl_vscsi_dev_init(&v_dev);

    /* Create a dummy cfg */
    if (asprintf(&tmp, "0:0:0:0,%s", str) < 0) {
        LOG(cfg, "asprintf failed while removing %s from domid %u", str, domid);
        goto out;
    }

    if (xlu_vscsi_parse(cfg, ctx, tmp, &v_hst, &v_dev))
        goto out;

    vscsi_hosts = libxl_device_vscsi_list(ctx, domid, &num_hosts);
    if (!vscsi_hosts)
        goto out;

    for (h = 0; h < num_hosts; ++h) {
        vh = vscsi_hosts + h;
        for (d = 0; d < vh->num_vscsi_devs; d++) {
            vd = vh->vscsi_devs + d;
#define CMP(member) (vd->vdev.member == v_dev.vdev.member)
            if (!found && vd->vscsi_dev_id != -1 &&
                CMP(hst) && CMP(chn) && CMP(tgt) && CMP(lun)) {
                vd->remove = true;
                libxl_device_vscsi_remove(ctx, domid, vh, NULL);
                found = 1;
            }
#undef CMP
            libxl_vscsi_dev_dispose(vd);
        }
        libxl_device_vscsi_dispose(vh);
    }
    free(vscsi_hosts);

out:
    free(tmp);
    libxl_vscsi_dev_dispose(&v_dev);
    libxl_device_vscsi_dispose(&v_hst);
    return found;
}

int xlu_vscsi_config_add(XLU_Config *cfg,
                         libxl_ctx *ctx,
                         const char *str,
                         int *num_vscsis,
                         libxl_device_vscsi **vscsis)
{
    int rc, i;
    libxl_vscsi_dev v_dev = { };
    libxl_device_vscsi *tmp, v_hst = { };
    bool hst_found = false;

    /*
     * #1: parse the devspec and place it in temporary host+dev part
     * #2: find existing vscsi_host with number v_hst
     *     if found, append the vscsi_dev to this vscsi_host
     * #3: otherwise, create new vscsi_host and append vscsi_dev
     * Note: v_hst does not represent the index named "num_vscsis",
     *       it is a private index used just in the config file
     */
    libxl_device_vscsi_init(&v_hst);
    libxl_vscsi_dev_init(&v_dev);

    rc = xlu_vscsi_parse(cfg, ctx, str, &v_hst, &v_dev);
    if (rc)
        goto out;

    if (*num_vscsis) {
        for (i = 0; i < *num_vscsis; i++) {
            tmp = *vscsis + i;
            if (tmp->v_hst == v_hst.v_hst) {
                rc = xlu_vscsi_append_dev(ctx, tmp, &v_dev);
                if (rc) {
                    LOG(cfg, "xlu_vscsi_append_dev failed: %d\n", rc);
                    goto out;
                }
                hst_found = true;
                break;
	           }
        }
    }

    if (!hst_found || !*num_vscsis) {
        tmp = realloc(*vscsis, sizeof(v_hst) * (*num_vscsis + 1));
        if (!tmp) {
            LOG(cfg, "realloc #%d failed", *num_vscsis + 1);
            rc = ERROR_NOMEM;
            goto out;
        }
        *vscsis = tmp;
        tmp = *vscsis + *num_vscsis;
        libxl_device_vscsi_init(tmp);

        v_hst.devid = *num_vscsis;
        libxl_device_vscsi_copy(ctx, tmp, &v_hst);

        rc = xlu_vscsi_append_dev(ctx, tmp, &v_dev);
        if (rc) {
            LOG(cfg, "xlu_vscsi_append_dev failed: %d\n", rc);
            goto out;
        }

        (*num_vscsis)++;
    }

    rc = 0;
out:
    libxl_vscsi_dev_dispose(&v_dev);
    libxl_device_vscsi_dispose(&v_hst);
    return rc;
}
#else /* ! __linux__ */
int xlu_vscsi_get_host(XLU_Config *config,
                       libxl_ctx *ctx,
                       uint32_t domid,
                       const char *str,
                       libxl_device_vscsi *vscsi_host)
{
    return ERROR_INVAL;
}

int xlu_vscsi_parse(XLU_Config *cfg,
                    libxl_ctx *ctx,
                    const char *str,
                    libxl_device_vscsi *new_host,
                    libxl_vscsi_dev *new_dev)
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
                         libxl_device_vscsi **vscsis)
{
    return ERROR_INVAL;
}
#endif
