/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2020. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "vfs_daemon.h"

#include <ucs/sys/string.h>
#include <ucs/debug/log_def.h>
#include <ucs/debug/memtrack_int.h>
#include <sys/wait.h>
#include <limits.h>
#include <stdlib.h>
#include <assert.h>
#include <ctype.h>


vfs_opts_t g_opts = {
    .action         = VFS_DAEMON_ACTION_START,
    .foreground     = 0,
    .verbose        = 0,
    .mountpoint_dir = VFS_DEFAULT_MOUNTPOINT_DIR,
    .mount_opts     = ""
};

const char *vfs_action_names[] = {
    [UCS_VFS_SOCK_ACTION_STOP]  = "stop",
    [UCS_VFS_SOCK_ACTION_MOUNT] = "mount",
    [VFS_DAEMON_ACTION_START]   = "start"
};

static struct sockaddr_un g_sockaddr;


static int vfs_run_fusermount(char **extra_argv)
{
    char command[128];
    pid_t child_pid;
    int ret, status;
    int devnull_fd;
    char *p, *endp;
    char *argv[16];
    int i, argc;

    argc         = 0;
    argv[argc++] = VFS_FUSE_MOUNT_PROG;
    if (!g_opts.verbose) {
        argv[argc++] = "-q";
    }
    while (*extra_argv != NULL) {
        argv[argc++] = *(extra_argv++);
    }
    argv[argc++] = NULL;
    assert(argc <= ucs_static_array_size(argv));

    /* save the whole command to log */
    p    = command;
    endp = command + sizeof(command);
    for (i = 0; argv[i] != NULL; ++i) {
        snprintf(p, endp - p, "%s ", argv[i]);
        p += strlen(p);
    }
    *(p - 1) = '\0';

    vfs_log("exec '%s'", command);

    child_pid = fork();
    if (child_pid == -1) {
        vfs_error("fork() failed: %m");
        return -1;
    }

    if (child_pid == 0) {
        if (!g_opts.verbose) {
            devnull_fd = open("/dev/null", O_WRONLY);
            if (devnull_fd < 0) {
                vfs_error("failed open /dev/null: %m");
                exit(1);
            }

            dup2(devnull_fd, 1);
            dup2(devnull_fd, 2);
            close(devnull_fd);
        }
        execvp(argv[0], argv);
        vfs_error("failed to execute '%s': %m", command);
        exit(1);
    }

    ret = waitpid(child_pid, &status, 0);
    if (ret < 0) {
        vfs_error("waitpid(%d) failed: %m", child_pid);
        return -errno;
    } else if (WIFEXITED(status) && (WEXITSTATUS(status) != 0)) {
        vfs_error("'%s' exited with status %d", command, WEXITSTATUS(status));
        return -1;
    } else if (!WIFEXITED(status)) {
        vfs_error("'%s' did not exit properly (%d)", command, status);
        return -1;
    }

    return 0;
}

static char *vfs_get_mountpoint(pid_t pid)
{
    ucs_status_t status;
    char *mountpoint;

    status = ucs_string_alloc_formatted_path(&mountpoint, "mountpoint", "%s/%d",
                                             g_opts.mountpoint_dir, pid);
    if (status != UCS_OK) {
        return NULL;
    }

    return mountpoint;
}

static const char *vfs_get_process_name(int pid, char *buf, size_t max_length)
{
    char procfs_comm[NAME_MAX];
    size_t length;
    FILE *file;
    char *p;

    /* open /proc/<pid>/comm to read command name */
    snprintf(procfs_comm, sizeof(procfs_comm), "/proc/%d/comm", pid);
    file = fopen(procfs_comm, "r");
    if (file == NULL) {
        goto err;
    }

    /* read command to buffer */
    if (fgets(buf, max_length, file) == NULL) {
        goto err_close;
    }

    /* remove trailing space/newline */
    length = strlen(buf);
    for (p = &buf[length - 1]; (p >= buf) && isspace(*p); --p) {
        *p = '\0';
        --length;
    }

    /* append process id */
    snprintf(buf + length, max_length - length, "@pid:%d", pid);
    fclose(file);
    goto out;

err_close:
    fclose(file);
err:
    snprintf(buf, max_length, "pid:%d", pid);
out:
    return buf;
}

int vfs_mount(int pid)
{
    char mountopts[1024];
    char name[NAME_MAX];
    char *mountpoint;
    int fuse_fd, ret;

    /* Add common mount options:
     * - File system name (source) : process name and pid
     * - File system type          : ucx_vfs
     * - Enable permissions check  : yes
     * - Direct IO (no caching)    : yes
     */
    ret = snprintf(
            mountopts, sizeof(mountopts),
            "fsname=%s,subtype=ucx_vfs,default_permissions,direct_io%s%s",
            vfs_get_process_name(pid, name, sizeof(name)),
            (strlen(g_opts.mount_opts) > 0) ? "," : "", g_opts.mount_opts);
    if (ret >= sizeof(mountopts)) {
        ret = -ENOMEM;
        goto out;
    }

    /* Create the mount point directory, and ignore "already exists" error */
    mountpoint = vfs_get_mountpoint(pid);
    if (mountpoint == NULL) {
        ret = -ENOMEM;
        goto out;
    }

    ret = mkdir(mountpoint, S_IRWXU);
    if (ret < 0) {
        if (errno == EEXIST) {
            /* Directory already exists */
            ret = 0;
        } else {
            ret = -errno;
            vfs_error("failed to create directory '%s': %m", mountpoint);
            goto out_free_mountpoint;
        }
    }

    /* Mount a new FUSE filesystem in the mount point directory */
    vfs_log("mounting directory '%s' with options '%s'", mountpoint, mountopts);
    ret = fuse_open_channel(mountpoint, mountopts);
    if (ret < 0) {
        vfs_error("fuse_open_channel(%s,opts=%s) failed: %m", mountpoint,
                  mountopts);
        goto out_free_mountpoint;
    }

    fuse_fd = ret;
    vfs_log("mounted directory '%s' with fd %d", mountpoint, fuse_fd);

out_free_mountpoint:
    ucs_free(mountpoint);
out:
    return ret;
}

int vfs_unmount(int pid)
{
    char *mountpoint;
    char *argv[5];
    int ret;

    /* Unmount FUSE file system */
    mountpoint = vfs_get_mountpoint(pid);
    if (mountpoint == NULL) {
        ret = -ENOMEM;
        goto out;
    }

    argv[0] = "-u";
    argv[1] = "-z";
    argv[2] = "--";
    argv[3] = mountpoint;
    argv[4] = NULL;
    ret     = vfs_run_fusermount(argv);
    if (ret < 0) {
        goto out_free_mountpoint;
    }

    /* Remove mount point directory */
    vfs_log("removing directory '%s'", mountpoint);
    ret = rmdir(mountpoint);
    if (ret < 0) {
        vfs_error("failed to remove directory '%s': %m", mountpoint);
    }

out_free_mountpoint:
    ucs_free(mountpoint);
out:
    return ret;
}

static int vfs_unlink_socket(int silent_notexist)
{
    int ret;

    vfs_log("removing existing socket '%s'", g_sockaddr.sun_path);

    ret = unlink(g_sockaddr.sun_path);
    if (ret < 0) {
        ret = -errno;
        if (silent_notexist && (errno == ENOENT)) {
            vfs_log("could not unlink '%s': %m", g_sockaddr.sun_path);
        } else {
            vfs_error("could not unlink '%s': %m", g_sockaddr.sun_path);
        }
        return ret;
    }

    return 0;
}

/* return 0 or the (negative) value of errno in case of error */
static int vfs_listen(int silent_addinuse_err)
{
    int listen_fd, ret;

    ret = umask(~S_IRWXU);
    if (ret < 0) {
        ret = -errno;
        vfs_error("failed to set umask permissions: %m");
        goto out;
    }

    ret = ucs_vfs_sock_mkdir(g_sockaddr.sun_path, UCS_LOG_LEVEL_ERROR);
    if (ret != 0) {
        goto out;
    }

    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        ret = -errno;
        vfs_error("failed to create listening socket: %m");
        goto out;
    }

    ret = bind(listen_fd, (const struct sockaddr*)&g_sockaddr,
               sizeof(g_sockaddr));
    if (ret < 0) {
        ret = -errno;
        if ((errno != EADDRINUSE) || !silent_addinuse_err) {
            vfs_error("bind(%s) failed: %m", g_sockaddr.sun_path);
        }
        goto out_close;
    }

    ret = listen(listen_fd, 128);
    if (ret < 0) {
        ret = -errno;
        vfs_error("listen() failed: %m");
        goto out_unlink;
    }

    vfs_log("listening for connections on '%s'", g_sockaddr.sun_path);
    ret = vfs_server_loop(listen_fd);

out_unlink:
    vfs_unlink_socket(0);
out_close:
    close(listen_fd);
out:
    return ret;
}

/* return 0 or the (negative) value of errno in case of error */
static int vfs_connect_and_act()
{
    ucs_vfs_sock_message_t vfs_msg_out;
    int connfd;
    int ret;

    vfs_log("connecting to '%s'", g_sockaddr.sun_path);

    connfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connfd < 0) {
        ret = -errno;
        vfs_error("failed to create connection socket: %m");
        goto out;
    }

    ret = connect(connfd, (const struct sockaddr*)&g_sockaddr,
                  sizeof(g_sockaddr));
    if (ret < 0) {
        ret = -errno;
        if (errno == ECONNREFUSED) {
            vfs_log("connect(%s) failed: %m", g_sockaddr.sun_path);
        } else {
            vfs_error("connect(%s) failed: %m", g_sockaddr.sun_path);
        }
        goto out_close;
    }

    if (g_opts.action < UCS_VFS_SOCK_ACTION_LAST) {
        vfs_log("sending action '%s'", vfs_action_names[g_opts.action]);

        /* send action */
        vfs_msg_out.action = g_opts.action;
        ret                = ucs_vfs_sock_send(connfd, &vfs_msg_out);
        if (ret < 0) {
            vfs_error("failed to send: %d", ret);
            goto out_close;
        }

        ret = 0;
    }

out_close:
    close(connfd);
out:
    return ret;
}

/* return 0 or negative value in case of error */
int vfs_start()
{
    int mountpoint_dir_created, ret, rmdir_ret;

    mountpoint_dir_created = !mkdir(g_opts.mountpoint_dir, S_IRWXU);
    if (!mountpoint_dir_created && (errno != EEXIST)) {
        vfs_error("could not create directory '%s': %m", g_opts.mountpoint_dir);
        return -1;
    }

    ret = vfs_listen(1);
    if (ret != -EADDRINUSE) {
        goto out;
    }

    /* Failed to listen because 'socket_name' path already exists - try to
     * connect */
    ret = vfs_connect_and_act();
    if (ret != -ECONNREFUSED) {
        goto out;
    }

    /* Could not connect to the socket because no one is listening - remove the
     * socket file and try listening again */
    ret = vfs_unlink_socket(0);
    if (ret < 0) {
        goto out;
    }

    ret = vfs_listen(0);

out:
    if (mountpoint_dir_created) {
        rmdir_ret = rmdir(g_opts.mountpoint_dir);
        if (rmdir_ret < 0) {
            vfs_error("failed to remove directory '%s': %m",
                      g_opts.mountpoint_dir);

            if (ret >= 0) {
                ret = rmdir_ret;
            }
        }
    }

    return ret;
}

static void vfs_usage()
{
    printf("Usage:   ucx_vfs [options]  [action]\n");
    printf("\n");
    printf("Options:\n");
    printf("  -d <dir>   Set parent directory for mount points (default: %s)\n",
           g_opts.mountpoint_dir);
    printf("  -o <opts>  Pass these mount options to mount.fuse\n");
    printf("  -f         Do not daemonize; run in foreground\n");
    printf("  -v         Enable verbose logging (requires -f)\n");
    printf("\n");
    printf("Actions:\n");
    printf("   start     Run the daemon and listen for connection from UCX\n");
    printf("             If a daemon is already running, do nothing\n");
    printf("             This is the default action.\n");
    printf("   stop      Stop the running daemon\n");
    printf("\n");
}

static int vfs_parse_args(int argc, char **argv)
{
    const char *action_str;
    int c, i;

    while ((c = getopt(argc, argv, "d:o:vfh")) != -1) {
        switch (c) {
        case 'd':
            g_opts.mountpoint_dir = optarg;
            break;
        case 'o':
            g_opts.mount_opts = optarg;
            break;
        case 'v':
            ++g_opts.verbose;
            break;
        case 'f':
            g_opts.foreground = 1;
            break;
        case 'h':
        default:
            vfs_usage();
            return -127;
        }
    }

    if (g_opts.verbose && !g_opts.foreground) {
        vfs_error("Option -v requires -f");
        vfs_usage();
        return -1;
    }

    if (optind < argc) {
        action_str    = argv[optind];
        g_opts.action = UCS_VFS_SOCK_ACTION_LAST;
        for (i = 0; i < ucs_static_array_size(vfs_action_names); ++i) {
            if ((vfs_action_names[i] != NULL) &&
                !strcmp(action_str, vfs_action_names[i])) {
                g_opts.action = i;
            }
        }
        if (g_opts.action == UCS_VFS_SOCK_ACTION_LAST) {
            vfs_error("invalid action '%s'", action_str);
            vfs_usage();
            return 0;
        }
        ++optind;
    }

    if (optind < argc) {
        vfs_error("only one action can be specified");
        vfs_usage();
        return -1;
    }

    return 0;
}

static int vfs_test_fuse()
{
    char *argv[] = {"-V", NULL};
    return vfs_run_fusermount(argv);
}

int main(int argc, char **argv)
{
    int ret;

    ret = vfs_parse_args(argc, argv);
    if (ret < 0) {
        return -1;
    }

    ret = vfs_test_fuse();
    if (ret < 0) {
        return -1;
    }

    if (!g_opts.foreground) {
        fuse_daemonize(0);
    }

    ucs_vfs_sock_get_address(&g_sockaddr);

    switch (g_opts.action) {
    case VFS_DAEMON_ACTION_START:
        return vfs_start();
    case UCS_VFS_SOCK_ACTION_STOP:
        return vfs_connect_and_act();
    default:
        vfs_error("unexpected action %d", g_opts.action);
        return -1;
    }
}
