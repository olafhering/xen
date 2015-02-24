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

/* FIXME proper log target */
int libxl_device_vscsi_parse(char *buf, libxl_device_vscsi *new_host,
                              libxl_vscsi_dev *new_dev)
{
    char *pdev, *vdev, *fhost;
    unsigned int hst, chn, tgt, lun;

    pdev = strtok(buf, ",");
    vdev = strtok(NULL, ",");
    fhost = strtok(NULL, ",");
    if (!(pdev && vdev)) {
        fprintf(stderr, "invalid vscsi= devspec: '%s'\n", buf);
        return -1;
    }

    pdev = vscsi_trim_string(pdev);
    vdev = vscsi_trim_string(vdev);

    if (strncmp(pdev, "/dev/", 5) == 0) {
        struct stat pdev_stat;
        char pdev_sysfs_path[PATH_MAX];
        const char *type;
        int result = 0;
        DIR *dirp;
        struct dirent *de;

        /* stat pdev to get device's sysfs entry */
        if (stat (pdev, &pdev_stat) == -1) {
            fprintf(stderr, "vscsi: invalid %s '%s' in vscsi= devspec: '%s', device not found or cannot be read\n", "pdev", pdev, buf);
            return -1;
        }

        if (S_ISBLK (pdev_stat.st_mode)) {
            type = "block";
        } else if (S_ISCHR (pdev_stat.st_mode)) {
            type = "char";
        } else {
            fprintf(stderr, "vscsi: invalid %s '%s' in vscsi= devspec: '%s', not a valid block or char device\n", "pdev", pdev, buf);
            return -1;
        }

        /* get pdev scsi address - subdir of scsi_device sysfs entry */
        snprintf(pdev_sysfs_path, sizeof(pdev_sysfs_path),
                 "/sys/dev/%s/%u:%u/device/scsi_device",
                 type,
                 major(pdev_stat.st_rdev),
                 minor(pdev_stat.st_rdev));

        dirp = opendir(pdev_sysfs_path);
        if (!dirp) {
            fprintf(stderr, "vscsi: invalid %s '%s' in vscsi= devspec: '%s', cannot find scsi device\n", "pdev", pdev, buf);
            return -1;
        }

        while ((de = readdir(dirp))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;

            if (sscanf(de->d_name, "%u:%u:%u:%u", &hst, &chn, &tgt, &lun) != 4) {
                fprintf(stderr, "vscsi: ignoring unknown devspec '%s' for device '%s'\n",
                        de->d_name, pdev);
                continue;
            }
            result = 1;
            break;
        }
        closedir(dirp);

        if (!result) {
            fprintf(stderr, "vscsi: invalid %s '%s' in vscsi= devspec: '%s', cannot find scsi device in sysfs\n", "pdev", pdev, buf);
            return -1;
        }
    } else if (sscanf(pdev, "%u:%u:%u:%u", &hst, &chn, &tgt, &lun) != 4) {
        fprintf(stderr, "vscsi: invalid %s '%s' in vscsi= devspec: '%s', expecting hst:chn:tgt:lun\n", "pdev", pdev, buf);
        return -1;
    }

    new_dev->p_hst = hst;
    new_dev->p_chn = chn;
    new_dev->p_tgt = tgt;
    new_dev->p_lun = lun;

    if (sscanf(vdev, "%u:%u:%u:%u", &hst, &chn, &tgt, &lun) != 4) {
        fprintf(stderr, "vscsi: invalid %s '%s' in vscsi= devspec: '%s', expecting hst:chn:tgt:lun\n", "vdev", vdev, buf);
        return -1;
    }

    new_host->v_hst = hst;
    new_dev->v_chn = chn;
    new_dev->v_tgt = tgt;
    new_dev->v_lun = lun;

    if (fhost) {
        fhost = vscsi_trim_string(fhost);
        new_host->feature_host = strcmp(fhost, "feature-host") == 0;
        if (!new_host->feature_host) {
            fprintf(stderr, "vscsi: invalid option '%s' in vscsi= devspec: '%s', expecting %s\n", fhost, buf, "feature-host");
            return -1;
        }
    }
    return 0;
}

int libxl_device_vscsi_get(libxl_ctx *ctx, uint32_t domid, const char *cfg, libxl_device_vscsi **vscsi_host)
{
    GC_INIT(ctx);
    libxl_vscsi_dev *new_dev = NULL;
    libxl_device_vscsi *new_host, *vscsi_hosts = NULL, *tmp;
    char *buf;
    int rc = -1, found_host = -1, i, j;
    int num_hosts;

    GCNEW(new_host);
    libxl_device_vscsi_init(new_host);

    GCNEW(new_dev);
    libxl_vscsi_dev_init(new_dev);

    buf = libxl__strdup(gc, cfg);

    if (libxl_device_vscsi_parse(buf, new_host, new_dev))
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

    /* Not found, create new host */
    if (found_host == -1) {
        /* FIXME proper alloc/free ? */
        tmp = malloc(sizeof(*new_host));
        if (!tmp)
            goto out;
        *tmp = *new_host;
        tmp->vscsi_devs = malloc(sizeof(*new_dev));
        if (!tmp->vscsi_devs) {
            free(tmp);
            goto out;
        }
        tmp->devid = 0;
        new_dev->vscsi_dev_id = 0;
        *tmp->vscsi_devs = *new_dev;
        tmp->num_vscsi_devs = 1;
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
        /* FIXME proper alloc/free ? */
        /* The caller gets a copy along with appended new_dev */
        tmp = malloc(sizeof(*new_host));
        if (!tmp)
            goto out;
        memcpy(tmp, vscsi_hosts + found_host, sizeof(*new_host));
        tmp->vscsi_devs = calloc(sizeof(libxl_vscsi_dev), tmp->num_vscsi_devs + 1);
        if (!tmp->vscsi_devs) {
            free(tmp);
            goto out;
        }
        memcpy(tmp->vscsi_devs, (vscsi_hosts + found_host)->vscsi_devs, sizeof(libxl_vscsi_dev) * tmp->num_vscsi_devs);
        new_dev->vscsi_dev_id = tmp->num_vscsi_devs;
        memcpy(tmp->vscsi_devs + tmp->num_vscsi_devs, new_dev, sizeof(*new_dev));
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
    GC_FREE;
    return rc;
}

libxl_device_vscsi *libxl_device_vscsi_list(libxl_ctx *ctx, uint32_t domid, int *num)
{
    GC_INIT(ctx);
    libxl_vscsi_dev *v_dev;
    libxl_device_vscsi *v_hst, *vscsi_hosts = NULL;
    char *fe_path, *tmp, *p, *v;
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
                    p = libxl__xs_read(gc, XBT_NULL,
                                         GCSPRINTF("%s/vscsi-devs/dev-%u/p-dev",
                                         be_path, vscsi_dev_id));
                    v = libxl__xs_read(gc, XBT_NULL,
                                          GCSPRINTF("%s/vscsi-devs/dev-%u/v-dev",
                                          be_path, vscsi_dev_id));
                    if (p && v) {
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
