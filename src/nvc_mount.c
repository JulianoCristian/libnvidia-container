/*
 * Copyright (c) 2017-2018, NVIDIA CORPORATION. All rights reserved.
 */

#include <sys/sysmacros.h>
#include <sys/mount.h>
#include <sys/types.h>

#include <errno.h>
#include <libgen.h>
#undef basename /* Use the GNU version of basename. */
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sched.h>
#include <unistd.h>

#include "nvc_internal.h"

#include "error.h"
#include "options.h"
#include "utils.h"
#include "xfuncs.h"

static char **mount_files(struct error *, const struct nvc_container *, const char *, char *[], size_t);
static char *mount_device(struct error *, const struct nvc_container *, const char *);
static char *mount_ipc(struct error *, const struct nvc_container *, const char *);
static char *mount_procfs(struct error *, const struct nvc_container *);
static char *mount_procfs_gpu(struct error *, const struct nvc_container *, const char *);
static char *mount_app_profile(struct error *, const struct nvc_container *);
static int  update_app_profile(struct error *, const struct nvc_container *, dev_t);
static void unmount(const char *);
static int  setup_cgroup(struct error *, const char *, dev_t);
static int  symlink_library(struct error *, const char *, const char *, const char *, uid_t, gid_t);
static int  symlink_libraries(struct error *, const struct nvc_container *, const char * const [], size_t);

static char **
mount_files(struct error *err, const struct nvc_container *cnt, const char *dir, char *paths[], size_t size)
{
        char path[PATH_MAX];
        mode_t mode;
        char *end, *file;
        char **mnt, **ptr;

        if (path_resolve(err, path, cnt->cfg.rootfs, dir) < 0)
                return (NULL);
        if (file_create(err, path, NULL, cnt->uid, cnt->gid, MODE_DIR(0755)) < 0)
                return (NULL);

        end = path + strlen(path);
        mnt = ptr = array_new(err, size + 1); /* NULL terminated. */
        if (mnt == NULL)
                return (NULL);

        for (size_t i = 0; i < size; ++i) {
                file = basename(paths[i]);
                if (!match_binary_flags(file, cnt->flags) && !match_library_flags(file, cnt->flags))
                        continue;
                if (path_append(err, path, file) < 0)
                        goto fail;
                if (file_mode(err, paths[i], &mode) < 0)
                        goto fail;
                if (file_create(err, path, NULL, cnt->uid, cnt->gid, mode) < 0)
                        goto fail;

                log_infof("mounting %s at %s", paths[i], path);
                if (xmount(err, paths[i], path, NULL, MS_BIND, NULL) < 0)
                        goto fail;
                if (xmount(err, NULL, path, NULL, MS_BIND|MS_REMOUNT | MS_RDONLY|MS_NODEV|MS_NOSUID, NULL) < 0)
                        goto fail;
                if ((*ptr++ = xstrdup(err, path)) == NULL)
                        goto fail;
                *end = '\0';
        }
        return (mnt);

 fail:
        for (size_t i = 0; i < size; ++i)
                unmount(mnt[i]);
        array_free(mnt, size);
        return (NULL);
}

static char *
mount_device(struct error *err, const struct nvc_container *cnt, const char *dev)
{
        char path[PATH_MAX];
        mode_t mode;
        char *mnt;

        if (path_resolve(err, path, cnt->cfg.rootfs, dev) < 0)
                return (NULL);
        if (file_mode(err, dev, &mode) < 0)
                return (NULL);
        if (file_create(err, path, NULL, cnt->uid, cnt->gid, mode) < 0)
                return (NULL);

        log_infof("mounting %s at %s", dev, path);
        if (xmount(err, dev, path, NULL, MS_BIND, NULL) < 0)
                goto fail;
        if (xmount(err, NULL, path, NULL, MS_BIND|MS_REMOUNT | MS_RDONLY|MS_NOSUID|MS_NOEXEC, NULL) < 0)
                goto fail;
        if ((mnt = xstrdup(err, path)) == NULL)
                goto fail;
        return (mnt);

 fail:
        unmount(path);
        return (NULL);
}

static char *
mount_ipc(struct error *err, const struct nvc_container *cnt, const char *ipc)
{
        char path[PATH_MAX];
        mode_t mode;
        char *mnt;

        if (path_resolve(err, path, cnt->cfg.rootfs, ipc) < 0)
                return (NULL);
        if (file_mode(err, ipc, &mode) < 0)
                return (NULL);
        if (file_create(err, path, NULL, cnt->uid, cnt->gid, mode) < 0)
                return (NULL);

        log_infof("mounting %s at %s", ipc, path);
        if (xmount(err, ipc, path, NULL, MS_BIND, NULL) < 0)
                goto fail;
        if (xmount(err, NULL, path, NULL, MS_BIND|MS_REMOUNT | MS_NODEV|MS_NOSUID|MS_NOEXEC, NULL) < 0)
                goto fail;
        if ((mnt = xstrdup(err, path)) == NULL)
                goto fail;
        return (mnt);

 fail:
        unmount(path);
        return (NULL);
}

static char *
mount_app_profile(struct error *err, const struct nvc_container *cnt)
{
        char path[PATH_MAX];
        char *mnt;

        if (path_resolve(err, path, cnt->cfg.rootfs, NV_APP_PROFILE_DIR) < 0)
                return (NULL);
        if (file_create(err, path, NULL, cnt->uid, cnt->gid, MODE_DIR(0555)) < 0)
                goto fail;

        log_infof("mounting tmpfs at %s", path);
        if (xmount(err, "tmpfs", path, "tmpfs", 0, "mode=0555") < 0)
                goto fail;
        /* XXX Some kernels require MS_BIND in order to remount within a userns */
        if (xmount(err, NULL, path, NULL, MS_BIND|MS_REMOUNT | MS_NODEV|MS_NOSUID|MS_NOEXEC, NULL) < 0)
                goto fail;
        if ((mnt = xstrdup(err, path)) == NULL)
                goto fail;
        return (mnt);

 fail:
        unmount(path);
        return (NULL);
}

static int
update_app_profile(struct error *err, const struct nvc_container *cnt, dev_t id)
{
        char path[PATH_MAX];
        char *buf = NULL;
        char *ptr;
        uintmax_t n;
        uint64_t dev;
        int rv = -1;

#define profile quote_str({\
        "profiles": [{"name": "_container_", "settings": ["EGLVisibleDGPUDevices", 0x%lx]}],\
        "rules": [{"pattern": [], "profile": "_container_"}]\
})

        dev = 1ull << minor(id);
        if (path_resolve(err, path, cnt->cfg.rootfs, NV_APP_PROFILE_DIR "/10-container.conf") < 0)
                return (-1);
        if (file_read_text(err, path, &buf) < 0) {
                if (err->code != ENOENT)
                        goto fail;
                if (xasprintf(err, &buf, profile, dev) < 0)
                        goto fail;
        } else {
                if ((ptr = strstr(buf, "0x")) == NULL ||
                    (n = strtoumax(ptr, NULL, 16)) == UINTMAX_MAX) {
                        error_setx(err, "invalid application profile: %s", path);
                        goto fail;
                }
                free(buf), buf = NULL;
                if (xasprintf(err, &buf, profile, (uint64_t)n|dev) < 0)
                        goto fail;
        }
        if (file_create(err, path, buf, cnt->uid, cnt->gid, MODE_REG(0555)) < 0)
                goto fail;
        rv = 0;

#undef profile

 fail:
        free(buf);
        return (rv);
}

static char *
mount_procfs(struct error *err, const struct nvc_container *cnt)
{
        char path[PATH_MAX];
        char *ptr, *mnt, *param;
        mode_t mode;
        char *buf = NULL;
        const char *files[] = {
                NV_PROC_DRIVER "/params",
                NV_PROC_DRIVER "/version",
                NV_PROC_DRIVER "/registry",
        };

        if (path_resolve(err, path, cnt->cfg.rootfs, NV_PROC_DRIVER) < 0)
                return (NULL);
        log_infof("mounting tmpfs at %s", path);
        if (xmount(err, "tmpfs", path, "tmpfs", 0, "mode=0555") < 0)
                return (NULL);

        ptr = path + strlen(path);

        for (size_t i = 0; i < nitems(files); ++i) {
                if (file_mode(err, files[i], &mode) < 0) {
                        if (err->code == ENOENT)
                                continue;
                        goto fail;
                }
                if (file_read_text(err, files[i], &buf) < 0)
                        goto fail;
                /* Prevent NVRM from ajusting the device nodes. */
                if (i == 0 && (param = strstr(buf, "ModifyDeviceFiles: 1")) != NULL)
                        param[19] = '0';
                if (path_append(err, path, basename(files[i])) < 0)
                        goto fail;
                if (file_create(err, path, buf, cnt->uid, cnt->gid, mode) < 0)
                        goto fail;
                *ptr = '\0';
                free(buf);
                buf = NULL;
        }
        /* XXX Some kernels require MS_BIND in order to remount within a userns */
        if (xmount(err, NULL, path, NULL, MS_BIND|MS_REMOUNT | MS_NODEV|MS_NOSUID|MS_NOEXEC, NULL) < 0)
                goto fail;
        if ((mnt = xstrdup(err, path)) == NULL)
                goto fail;
        return (mnt);

 fail:
        *ptr = '\0';
        free(buf);
        unmount(path);
        return (NULL);
}

static char *
mount_procfs_gpu(struct error *err, const struct nvc_container *cnt, const char *busid)
{
        char path[PATH_MAX] = {0};
        char *gpu = NULL;
        char *mnt = NULL;
        mode_t mode;

        /* XXX The driver procfs uses 16-bit PCI domain */
        if (xasprintf(err, &gpu, "%s/gpus/%s", NV_PROC_DRIVER, busid + 4) < 0)
                return (NULL);
        if (file_mode(err, gpu, &mode) < 0)
                goto fail;
        if (path_resolve(err, path, cnt->cfg.rootfs, gpu) < 0)
                goto fail;
        if (file_create(err, path, NULL, cnt->uid, cnt->gid, mode) < 0)
                goto fail;

        log_infof("mounting %s at %s", gpu, path);
        if (xmount(err, gpu, path, NULL, MS_BIND, NULL) < 0)
                goto fail;
        if (xmount(err, NULL, path, NULL, MS_BIND|MS_REMOUNT | MS_RDONLY|MS_NODEV|MS_NOSUID|MS_NOEXEC, NULL) < 0)
                goto fail;
        if ((mnt = xstrdup(err, path)) == NULL)
                goto fail;

 fail:
        if (mnt == NULL)
                unmount(path);
        free(gpu);
        return (mnt);
}

static void
unmount(const char *path)
{
        if (path == NULL || strempty(path))
                return;
        umount2(path, MNT_DETACH);
        file_remove(NULL, path);
}

static int
setup_cgroup(struct error *err, const char *cgroup, dev_t id)
{
        char path[PATH_MAX];
        FILE *fs;
        int rv = -1;

        if (path_join(err, path, cgroup, "devices.allow") < 0)
                return (-1);
        if ((fs = xfopen(err, path, "a")) == NULL)
                return (-1);

        log_infof("whitelisting device node %u:%u", major(id), minor(id));
        /* XXX dprintf doesn't seem to catch the write errors, flush the stream explicitly instead. */
        if (fprintf(fs, "c %u:%u rw", major(id), minor(id)) < 0 || fflush(fs) == EOF || ferror(fs)) {
                error_set(err, "write error: %s", path);
                goto fail;
        }
        rv = 0;

 fail:
        fclose(fs);
        return (rv);
}

static int
symlink_library(struct error *err, const char *src, const char *target, const char *linkname, uid_t uid, gid_t gid)
{
        char path[PATH_MAX];
        char *tmp;
        int rv = -1;

        if ((tmp = xstrdup(err, src)) == NULL)
                return (-1);
        if (path_join(err, path, dirname(tmp), linkname) < 0)
                goto fail;

        log_infof("creating symlink %s -> %s", path, target);
        if (file_create(err, path, target, uid, gid, MODE_LNK(0777)) < 0)
                goto fail;
        rv = 0;

 fail:
        free(tmp);
        return (rv);
}

static int
symlink_libraries(struct error *err, const struct nvc_container *cnt, const char * const paths[], size_t size)
{
        char *lib;

        for (size_t i = 0; i < size; ++i) {
                lib = basename(paths[i]);
                if (!strpcmp(lib, "libcuda.so")) {
                        /* XXX Many applications wrongly assume that libcuda.so exists (e.g. with dlopen). */
                        if (symlink_library(err, paths[i], lib, "libcuda.so", cnt->uid, cnt->gid) < 0)
                                return (-1);
                } else if (!strpcmp(lib, "libGLX_nvidia.so")) {
                        /* XXX GLVND requires this symlink for indirect GLX support. */
                        if (symlink_library(err, paths[i], lib, "libGLX_indirect.so.0", cnt->uid, cnt->gid) < 0)
                                return (-1);
                }
        }
        return (0);
}

int
nvc_driver_mount(struct nvc_context *ctx, const struct nvc_container *cnt, const struct nvc_driver_info *info)
{
        const char **mnt, **ptr, **tmp;
        size_t nmnt;
        int rv = -1;

        if (validate_context(ctx) < 0)
                return (-1);
        if (validate_args(ctx, cnt != NULL && info != NULL) < 0)
                return (-1);

        if (nsenter(&ctx->err, cnt->mnt_ns, CLONE_NEWNS) < 0)
                return (-1);

        nmnt = 2 + info->nbins + info->nlibs + info->nlibs32 + info->nipcs + info->ndevs;
        mnt = ptr = (const char **)array_new(&ctx->err, nmnt);
        if (mnt == NULL)
                goto fail;

        /* Procfs mount */
        if ((*ptr++ = mount_procfs(&ctx->err, cnt)) == NULL)
                goto fail;
        /* Application profile mount */
        if (cnt->flags & OPT_GRAPHICS_LIBS) {
                if ((*ptr++ = mount_app_profile(&ctx->err, cnt)) == NULL)
                        goto fail;
        }
        /* Binary and library mounts */
        if (info->bins != NULL && info->nbins > 0) {
                if ((tmp = (const char **)mount_files(&ctx->err, cnt, cnt->cfg.bins_dir, info->bins, info->nbins)) == NULL)
                        goto fail;
                ptr = array_append(ptr, tmp, array_size(tmp));
                free(tmp);
        }
        if (info->libs != NULL && info->nlibs > 0) {
                if ((tmp = (const char **)mount_files(&ctx->err, cnt, cnt->cfg.libs_dir, info->libs, info->nlibs)) == NULL)
                        goto fail;
                ptr = array_append(ptr, tmp, array_size(tmp));
                free(tmp);
        }
        if ((cnt->flags & OPT_COMPAT32) && info->libs32 != NULL && info->nlibs32 > 0) {
                if ((tmp = (const char **)mount_files(&ctx->err, cnt, cnt->cfg.libs32_dir, info->libs32, info->nlibs32)) == NULL)
                        goto fail;
                ptr = array_append(ptr, tmp, array_size(tmp));
                free(tmp);
        }
        if (symlink_libraries(&ctx->err, cnt, mnt, (size_t)(ptr - mnt)) < 0)
                goto fail;
        /* IPC mounts */
        for (size_t i = 0; i < info->nipcs; ++i) {
                /* XXX Only utility libraries require persistenced IPC, everything else is compute only. */
                if (!strrcmp(NV_PERSISTENCED_SOCKET, info->ipcs[i])) {
                        if (!(cnt->flags & OPT_UTILITY_LIBS))
                                continue;
                } else if (!(cnt->flags & OPT_COMPUTE_LIBS))
                        continue;
                if ((*ptr++ = mount_ipc(&ctx->err, cnt, info->ipcs[i])) == NULL)
                        goto fail;
        }
        /* Device mounts */
        for (size_t i = 0; i < info->ndevs; ++i) {
                /* XXX Only compute libraries require specific devices (e.g. UVM). */
                if (!(cnt->flags & OPT_COMPUTE_LIBS) && major(info->devs[i].id) != NV_DEVICE_MAJOR)
                        continue;
                if (!(cnt->flags & OPT_NO_DEVBIND)) {
                        if ((*ptr++ = mount_device(&ctx->err, cnt, info->devs[i].path)) == NULL)
                                goto fail;
                }
                if (!(cnt->flags & OPT_NO_CGROUPS)) {
                        if (setup_cgroup(&ctx->err, cnt->dev_cg, info->devs[i].id) < 0)
                                goto fail;
                }
        }
        rv = 0;

 fail:
        if (rv < 0) {
                for (size_t i = 0; mnt != NULL && i < nmnt; ++i)
                        unmount(mnt[i]);
                assert_func(nsenterat(NULL, ctx->mnt_ns, CLONE_NEWNS));
        } else {
                rv = nsenterat(&ctx->err, ctx->mnt_ns, CLONE_NEWNS);
        }

        array_free((char **)mnt, nmnt);
        return (rv);
}

int
nvc_device_mount(struct nvc_context *ctx, const struct nvc_container *cnt, const struct nvc_device *dev)
{
        char *dev_mnt = NULL;
        char *proc_mnt = NULL;
        struct stat s;
        int rv = -1;

        if (validate_context(ctx) < 0)
                return (-1);
        if (validate_args(ctx, cnt != NULL && dev != NULL) < 0)
                return (-1);

        if (nsenter(&ctx->err, cnt->mnt_ns, CLONE_NEWNS) < 0)
                return (-1);

        if (!(cnt->flags & OPT_NO_DEVBIND)) {
                if (xstat(&ctx->err, dev->node.path, &s) < 0)
                        return (-1);
                if (s.st_rdev != dev->node.id) {
                        error_setx(&ctx->err, "invalid device node: %s", dev->node.path);
                        return (-1);
                }
                if ((dev_mnt = mount_device(&ctx->err, cnt, dev->node.path)) == NULL)
                        goto fail;
        }
        if ((proc_mnt = mount_procfs_gpu(&ctx->err, cnt, dev->busid)) == NULL)
                goto fail;
        if (cnt->flags & OPT_GRAPHICS_LIBS) {
                if (update_app_profile(&ctx->err, cnt, dev->node.id) < 0)
                        goto fail;
        }
        if (!(cnt->flags & OPT_NO_CGROUPS)) {
                if (setup_cgroup(&ctx->err, cnt->dev_cg, dev->node.id) < 0)
                        goto fail;
        }
        rv = 0;

 fail:
        if (rv < 0) {
                unmount(proc_mnt);
                unmount(dev_mnt);
                assert_func(nsenterat(NULL, ctx->mnt_ns, CLONE_NEWNS));
        } else {
                rv = nsenterat(&ctx->err, ctx->mnt_ns, CLONE_NEWNS);
        }

        free(proc_mnt);
        free(dev_mnt);
        return (rv);
}
