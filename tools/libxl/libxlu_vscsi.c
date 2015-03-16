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

static int xlu__vscsi_parse_pdev(XLU_Config *cfg, char *pdev, libxl_vscsi_dev *new_dev)
{
    int rc = 0;
    unsigned int lun;
    char wwn[16 + 1];

    if (strncmp(pdev, "/dev/", 5) == 0) {
        /* Either xenlinux or pvops with properly configured alias in sysfs */
        if (xlu__vscsi_parse_dev(cfg, pdev, &new_dev->pdev) == 0)
            new_dev->pdev_type = LIBXL_VSCSI_PDEV_TYPE_DEV;
    } else if (strncmp(pdev, "naa.", 4) == 0) {
        /* WWN as understood by pvops */
        memset(wwn, 0, sizeof(wwn));
        if (sscanf(pdev, "naa.%16c:%u", wwn, &lun) == 2 && xlu__vscsi_wwn_valid(wwn)) {
            new_dev->pdev_type = LIBXL_VSCSI_PDEV_TYPE_WWN;
            new_dev->pdev.lun = lun;
        }
    } else if (xlu__vscsi_parse_hctl(pdev, &new_dev->pdev) == 0) {
        /* Either xenlinux or pvops with properly configured alias in sysfs */
        new_dev->pdev_type = LIBXL_VSCSI_PDEV_TYPE_HCTL;
    } else
        rc = ERROR_INVAL;

    return rc;
}

int xlu_vscsi_parse(XLU_Config *cfg, const char *str,
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
        LOG(cfg, "invalid vscsi= devspec: '%s'\n", str);
        rc = ERROR_INVAL;
        goto out;
    }

    pdev = xlu__vscsi_trim_string(pdev);
    vdev = xlu__vscsi_trim_string(vdev);

    new_dev->pdev_type = LIBXL_VSCSI_PDEV_TYPE_INVALID;
    rc = xlu__vscsi_parse_pdev(cfg, pdev, new_dev);
    switch (rc) {
        case ERROR_NOPARAVIRT:
            LOG(cfg, "vscsi: not supported");
            goto out;
        default:
            LOG(cfg, "vscsi: failed to parse %s", pdev);
            goto out;
    }

    switch (new_dev->pdev_type) {
        case LIBXL_VSCSI_PDEV_TYPE_WWN:
        case LIBXL_VSCSI_PDEV_TYPE_DEV:
        case LIBXL_VSCSI_PDEV_TYPE_HCTL:
            new_dev->p_devname = strdup(pdev);
            if (!new_dev->p_devname) {
                rc = ERROR_NOMEM;
                goto out;
            }
            break;
        case LIBXL_VSCSI_PDEV_TYPE_INVALID:
            LOG(cfg, "vscsi: invalid pdev '%s'", pdev);
            rc = ERROR_INVAL;
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
        new_host->feature_host = strcmp(fhost, "feature-host") == 0;
        if (!new_host->feature_host) {
            LOG(cfg, "vscsi: invalid option '%s', expecting %s", fhost, "feature-host");
            rc = ERROR_INVAL;
            goto out;
        }
    }
    rc = 0;

out:
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
    dev->vscsi_dev_id = hst->num_vscsi_devs;
    libxl_vscsi_dev_copy(ctx, hst->vscsi_devs + hst->num_vscsi_devs, dev);
    hst->num_vscsi_devs++;
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

    if (xlu_vscsi_parse(cfg, str, new_host, new_dev)) {
        rc = ERROR_INVAL;
        goto out;
    }

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
            if (tmp->vscsi_devs[i].vdev.chn == new_dev->vdev.chn &&
                tmp->vscsi_devs[i].vdev.tgt == new_dev->vdev.tgt &&
                tmp->vscsi_devs[i].vdev.lun == new_dev->vdev.lun) {
                LOG(cfg, "Target vscsi specification '%u:%u:%u:%u' is already taken\n",
                    new_dev->vdev.hst, new_dev->vdev.chn, new_dev->vdev.tgt, new_dev->vdev.lun);
                rc = ERROR_INVAL;
                goto out;
            }
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
    libxl_device_vscsi_dispose(new_host);
    libxl_vscsi_dev_dispose(new_dev);
    return rc;
}

