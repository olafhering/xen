#include "libxl_osdeps.h" /* must come before any other headers */
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include "libxlu_internal.h"

#define LOG(_c, _x, _a...) \
        if((_c) && (_c)->report) fprintf((_c)->report, _x, ##_a)

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
    unsigned int len;

    while (isspace(*s))
        s++;
    len = strlen(s);
    while (len-- > 1 && isspace(s[len]))
        s[len] = '\0';
    return s;
}


#ifdef __linux__
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
        LOG(cfg, "vscsi: %s, device node not found", pdev);
        rc = ERROR_INVAL;
        goto out;
    }

    if (S_ISBLK (dentry.st_mode)) {
        type = "block";
    } else if (S_ISCHR (dentry.st_mode)) {
        type = "char";
    } else {
        LOG(cfg, "vscsi: %s, device node not a block or char device", pdev);
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
        LOG(cfg, "vscsi: %s, no major:minor link in sysfs", pdev);
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
        LOG(cfg, "vscsi: %s, no h:c:t:l link in sysfs", pdev);
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

static int xlu__vscsi_parse_pdev(XLU_Config *cfg, libxl_ctx *ctx, char *str,
                                 libxl_vscsi_pdev *pdev)
{
    int rc = 0;
    unsigned int lun;
    char wwn[16 + 1];
    libxl_vscsi_hctl hctl;

    libxl_vscsi_hctl_init(&hctl);
    if (strncmp(str, "/dev/", 5) == 0) {
        /* Either xenlinux, or pvops with properly configured alias in sysfs */
        if (xlu__vscsi_parse_dev(cfg, str, &hctl) == 0) {
            libxl_vscsi_pdev_init_type(pdev, LIBXL_VSCSI_PDEV_TYPE_DEV);
            libxl_vscsi_hctl_copy(ctx, &pdev->u.dev.m, &hctl);
        }
    } else if (strncmp(str, "naa.", 4) == 0) {
        /* WWN as understood by pvops */
        memset(wwn, 0, sizeof(wwn));
        if (sscanf(str, "naa.%16c:%u", wwn, &lun) == 2 && xlu__vscsi_wwn_valid(wwn)) {
            libxl_vscsi_pdev_init_type(pdev, LIBXL_VSCSI_PDEV_TYPE_WWN);
            pdev->u.wwn.m = strdup(str);
            if (!pdev->u.wwn.m)
                rc = ERROR_NOMEM;
        }
    } else if (xlu__vscsi_parse_hctl(str, &hctl) == 0) {
        /* Either xenlinux, or pvops with properly configured alias in sysfs */
        libxl_vscsi_pdev_init_type(pdev, LIBXL_VSCSI_PDEV_TYPE_HCTL);
        libxl_vscsi_hctl_copy(ctx, &pdev->u.dev.m, &hctl);
    } else
        rc = ERROR_INVAL;

    if (rc == 0) {
            pdev->p_devname = strdup(str);
            if (!pdev->p_devname)
                rc = ERROR_NOMEM;
    }

    libxl_vscsi_hctl_dispose(&hctl);
    return rc;
}
#else /* ! __linux__ */
static int xlu__vscsi_parse_pdev(XLU_Config *cfg, libxl_ctx *ctx, char *str,
                                 libxl_vscsi_pdev *pdev)
{
    return ERROR_FAIL;
}
#endif

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
        LOG(cfg, "vscsi: invalid devspec: '%s'\n", str);
        rc = ERROR_INVAL;
        goto out;
    }

    pdev = xlu__vscsi_trim_string(pdev);
    vdev = xlu__vscsi_trim_string(vdev);

    rc = xlu__vscsi_parse_pdev(cfg, ctx, pdev, &new_dev->pdev);
    if (rc) {
        LOG(cfg, "vscsi: failed to parse %s, rc == %d", pdev, rc);
        goto out;
    }

    if (xlu__vscsi_parse_hctl(vdev, &new_dev->vdev)) {
        LOG(cfg, "vscsi: invalid '%s', expecting hst:chn:tgt:lun", vdev);
        rc = ERROR_INVAL;
        goto out;
    }

    /* Record group index */
    new_host->v_hst = new_dev->vdev.hst;

    if (fhost) {
        fhost = xlu__vscsi_trim_string(fhost);
        if (strcmp(fhost, "feature-host") == 0) {
            libxl_defbool_set(&new_host->feature_host, true);
        } else {
            LOG(cfg, "vscsi: invalid option '%s', expecting %s", fhost, "feature-host");
            rc = ERROR_INVAL;
            goto out;
        }
    } else
        libxl_defbool_set(&new_host->feature_host, false);
    rc = 0;

out:
    free(tmp);
    return rc;
}


int xlu_vscsi_append_dev(libxl_ctx *ctx, libxl_device_vscsi *hst,
                                   libxl_vscsi_dev *dev)
{
    int rc;
    libxl_vscsi_dev *devs;

    devs = realloc(hst->vscsi_devs, sizeof(*dev) * (hst->num_vscsi_devs + 1));
    if (!devs) {
        rc = ERROR_NOMEM;
        goto out;
    }

    hst->vscsi_devs = devs;
    libxl_vscsi_dev_init(hst->vscsi_devs + hst->num_vscsi_devs);
    dev->vscsi_dev_id = hst->next_vscsi_dev_id;
    libxl_vscsi_dev_copy(ctx, hst->vscsi_devs + hst->num_vscsi_devs, dev);
    hst->num_vscsi_devs++;
    hst->next_vscsi_dev_id++;
    rc = 0;
out:
    return rc;
}

int xlu_vscsi_get_host(XLU_Config *cfg, libxl_ctx *ctx, uint32_t domid, const char *str, libxl_device_vscsi *vscsi_host)
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
        new_host->next_vscsi_dev_id = 0;
        tmp = new_host;
    } else {
        tmp = vscsi_hosts + found_host;

        /* Check if the vdev address is already taken */
        for (i = 0; i < tmp->num_vscsi_devs; ++i) {
            if (tmp->vscsi_devs[i].vdev.chn == new_dev->vdev.chn &&
                tmp->vscsi_devs[i].vdev.tgt == new_dev->vdev.tgt &&
                tmp->vscsi_devs[i].vdev.lun == new_dev->vdev.lun) {
                LOG(cfg, "vscsi: vdev '%u:%u:%u:%u' is already used.\n",
                    new_dev->vdev.hst, new_dev->vdev.chn, new_dev->vdev.tgt, new_dev->vdev.lun);
                rc = ERROR_INVAL;
                goto out;
            }
        }

        if (libxl_defbool_val(new_host->feature_host) !=
            libxl_defbool_val(tmp->feature_host)) {
            LOG(cfg, "vscsi: different feature-host setting: "
                      "existing host has it %s, new has it %s\n",
                libxl_defbool_val(new_host->feature_host) ? "set" : "unset",
                libxl_defbool_val(tmp->feature_host) ? "set" : "unset");
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

