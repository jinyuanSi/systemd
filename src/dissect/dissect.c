/* SPDX-License-Identifier: LGPL-2.1+ */

#include <fcntl.h>
#include <getopt.h>
#include <linux/loop.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mount.h>

#include "architecture.h"
#include "copy.h"
#include "dissect-image.h"
#include "fd-util.h"
#include "format-util.h"
#include "fs-util.h"
#include "hexdecoct.h"
#include "log.h"
#include "loop-util.h"
#include "main-func.h"
#include "mkdir.h"
#include "mount-util.h"
#include "namespace-util.h"
#include "parse-util.h"
#include "path-util.h"
#include "pretty-print.h"
#include "stat-util.h"
#include "string-util.h"
#include "strv.h"
#include "terminal-util.h"
#include "tmpfile-util.h"
#include "user-util.h"
#include "util.h"

static enum {
        ACTION_DISSECT,
        ACTION_MOUNT,
        ACTION_COPY_FROM,
        ACTION_COPY_TO,
} arg_action = ACTION_DISSECT;
static const char *arg_image = NULL;
static const char *arg_path = NULL;
static const char *arg_source = NULL;
static const char *arg_target = NULL;
static DissectImageFlags arg_flags = DISSECT_IMAGE_REQUIRE_ROOT|DISSECT_IMAGE_DISCARD_ON_LOOP|DISSECT_IMAGE_RELAX_VAR_CHECK|DISSECT_IMAGE_FSCK;
static void *arg_root_hash = NULL;
static char *arg_verity_data = NULL;
static size_t arg_root_hash_size = 0;
static char *arg_root_hash_sig_path = NULL;
static void *arg_root_hash_sig = NULL;
static size_t arg_root_hash_sig_size = 0;

STATIC_DESTRUCTOR_REGISTER(arg_root_hash, freep);
STATIC_DESTRUCTOR_REGISTER(arg_verity_data, freep);
STATIC_DESTRUCTOR_REGISTER(arg_root_hash_sig_path, freep);
STATIC_DESTRUCTOR_REGISTER(arg_root_hash_sig, freep);

static int help(void) {
        _cleanup_free_ char *link = NULL;
        int r;

        r = terminal_urlify_man("systemd-dissect", "1", &link);
        if (r < 0)
                return log_oom();

        printf("%1$s [OPTIONS...] IMAGE\n"
               "%1$s [OPTIONS...] --mount IMAGE PATH\n"
               "%1$s [OPTIONS...] --copy-from IMAGE PATH [TARGET]\n"
               "%1$s [OPTIONS...] --copy-to IMAGE [SOURCE] PATH\n\n"
               "%5$sDissect a file system OS image.%6$s\n\n"
               "%3$sOptions:%4$s\n"
               "  -r --read-only          Mount read-only\n"
               "     --fsck=BOOL          Run fsck before mounting\n"
               "     --mkdir              Make mount directory before mounting, if missing\n"
               "     --discard=MODE       Choose 'discard' mode (disabled, loop, all, crypto)\n"
               "     --root-hash=HASH     Specify root hash for verity\n"
               "     --root-hash-sig=SIG  Specify pkcs7 signature of root hash for verity\n"
               "                          as a DER encoded PKCS7, either as a path to a file\n"
               "                          or as an ASCII base64 encoded string prefixed by\n"
               "                          'base64:'\n"
               "     --verity-data=PATH   Specify data file with hash tree for verity if it is\n"
               "                          not embedded in IMAGE\n"
               "\n%3$sCommands:%4$s\n"
               "  -h --help               Show this help\n"
               "     --version            Show package version\n"
               "  -m --mount              Mount the image to the specified directory\n"
               "  -M                      Shortcut for --mount --mkdir\n"
               "  -x --copy-from          Copy files from image to host\n"
               "  -a --copy-to            Copy files from host to image\n"
               "\nSee the %2$s for details.\n"
               , program_invocation_short_name
               , link
               , ansi_underline(), ansi_normal()
               , ansi_highlight(), ansi_normal());

        return 0;
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
                ARG_DISCARD,
                ARG_ROOT_HASH,
                ARG_FSCK,
                ARG_VERITY_DATA,
                ARG_ROOT_HASH_SIG,
                ARG_MKDIR,
        };

        static const struct option options[] = {
                { "help",          no_argument,       NULL, 'h'               },
                { "version",       no_argument,       NULL, ARG_VERSION       },
                { "mount",         no_argument,       NULL, 'm'               },
                { "read-only",     no_argument,       NULL, 'r'               },
                { "discard",       required_argument, NULL, ARG_DISCARD       },
                { "root-hash",     required_argument, NULL, ARG_ROOT_HASH     },
                { "fsck",          required_argument, NULL, ARG_FSCK          },
                { "verity-data",   required_argument, NULL, ARG_VERITY_DATA   },
                { "root-hash-sig", required_argument, NULL, ARG_ROOT_HASH_SIG },
                { "mkdir",         no_argument,       NULL, ARG_MKDIR         },
                { "copy-from",     no_argument,       NULL, 'x'               },
                { "copy-to",       no_argument,       NULL, 'a'               },
                {}
        };

        int c, r;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "hmrMxa", options, NULL)) >= 0) {

                switch (c) {

                case 'h':
                        return help();

                case ARG_VERSION:
                        return version();

                case 'm':
                        arg_action = ACTION_MOUNT;
                        break;

                case ARG_MKDIR:
                        arg_flags |= DISSECT_IMAGE_MKDIR;
                        break;

                case 'M':
                        /* Shortcut combination of the above two */
                        arg_action = ACTION_MOUNT;
                        arg_flags |= DISSECT_IMAGE_MKDIR;
                        break;

                case 'x':
                        arg_action = ACTION_COPY_FROM;
                        arg_flags |= DISSECT_IMAGE_READ_ONLY;
                        break;

                case 'a':
                        arg_action = ACTION_COPY_TO;
                        break;

                case 'r':
                        arg_flags |= DISSECT_IMAGE_READ_ONLY;
                        break;

                case ARG_DISCARD: {
                        DissectImageFlags flags;

                        if (streq(optarg, "disabled"))
                                flags = 0;
                        else if (streq(optarg, "loop"))
                                flags = DISSECT_IMAGE_DISCARD_ON_LOOP;
                        else if (streq(optarg, "all"))
                                flags = DISSECT_IMAGE_DISCARD_ON_LOOP | DISSECT_IMAGE_DISCARD;
                        else if (streq(optarg, "crypt"))
                                flags = DISSECT_IMAGE_DISCARD_ANY;
                        else if (streq(optarg, "list")) {
                                puts("disabled\n"
                                     "all\n"
                                     "crypt\n"
                                     "loop");
                                return 0;
                        } else
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "Unknown --discard= parameter: %s",
                                                       optarg);
                        arg_flags = (arg_flags & ~DISSECT_IMAGE_DISCARD_ANY) | flags;

                        break;
                }

                case ARG_ROOT_HASH: {
                        void *p;
                        size_t l;

                        r = unhexmem(optarg, strlen(optarg), &p, &l);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse root hash '%s': %m", optarg);
                        if (l < sizeof(sd_id128_t)) {
                                log_error("Root hash must be at least 128bit long: %s", optarg);
                                free(p);
                                return -EINVAL;
                        }

                        free(arg_root_hash);
                        arg_root_hash = p;
                        arg_root_hash_size = l;
                        break;
                }

                case ARG_VERITY_DATA:
                        r = parse_path_argument_and_warn(optarg, false, &arg_verity_data);
                        if (r < 0)
                                return r;
                        break;

                case ARG_ROOT_HASH_SIG: {
                        char *value;

                        if ((value = startswith(optarg, "base64:"))) {
                                void *p;
                                size_t l;

                                r = unbase64mem(value, strlen(value), &p, &l);
                                if (r < 0)
                                        return log_error_errno(r, "Failed to parse root hash signature '%s': %m", optarg);

                                free_and_replace(arg_root_hash_sig, p);
                                arg_root_hash_sig_size = l;
                                arg_root_hash_sig_path = mfree(arg_root_hash_sig_path);
                        } else {
                                r = parse_path_argument_and_warn(optarg, false, &arg_root_hash_sig_path);
                                if (r < 0)
                                        return r;
                                arg_root_hash_sig = mfree(arg_root_hash_sig);
                                arg_root_hash_sig_size = 0;
                        }

                        break;
                }

                case ARG_FSCK:
                        r = parse_boolean(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse --fsck= parameter: %s", optarg);

                        SET_FLAG(arg_flags, DISSECT_IMAGE_FSCK, r);
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }

        }

        switch (arg_action) {

        case ACTION_DISSECT:
                if (optind + 1 != argc)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "Expected an image file path as only argument.");

                arg_image = argv[optind];
                arg_flags |= DISSECT_IMAGE_READ_ONLY;
                break;

        case ACTION_MOUNT:
                if (optind + 2 != argc)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "Expected an image file path and mount point path as only arguments.");

                arg_image = argv[optind];
                arg_path = argv[optind + 1];
                break;

        case ACTION_COPY_FROM:
                if (argc < optind + 2 || argc > optind + 3)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "Expected an image file path, a source path and an optional destination path as only arguments.");

                arg_image = argv[optind];
                arg_source = argv[optind + 1];
                arg_target = argc > optind + 2 ? argv[optind + 2] : "-" /* this means stdout */ ;

                arg_flags |= DISSECT_IMAGE_READ_ONLY;
                break;

        case ACTION_COPY_TO:
                if (argc < optind + 2 || argc > optind + 3)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "Expected an image file path, an optional source path and a destination path as only arguments.");

                arg_image = argv[optind];

                if (argc > optind + 2) {
                        arg_source = argv[optind + 1];
                        arg_target = argv[optind + 2];
                } else {
                        arg_source = "-"; /* this means stdin */
                        arg_target = argv[optind + 1];
                }

                break;

        default:
                assert_not_reached("Unknown action.");
        }

        return 1;
}

static int run(int argc, char *argv[]) {
        _cleanup_(loop_device_unrefp) LoopDevice *d = NULL;
        _cleanup_(decrypted_image_unrefp) DecryptedImage *di = NULL;
        _cleanup_(dissected_image_unrefp) DissectedImage *m = NULL;
        int r;

        log_parse_environment();
        log_open();

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r;

        r = loop_device_make_by_path(arg_image, (arg_flags & DISSECT_IMAGE_READ_ONLY) ? O_RDONLY : O_RDWR, LO_FLAGS_PARTSCAN, &d);
        if (r < 0)
                return log_error_errno(r, "Failed to set up loopback device: %m");

        r = verity_metadata_load(arg_image, NULL, arg_root_hash ? NULL : &arg_root_hash, &arg_root_hash_size,
                           arg_verity_data ? NULL : &arg_verity_data,
                           arg_root_hash_sig_path || arg_root_hash_sig ? NULL : &arg_root_hash_sig_path);
        if (r < 0)
                return log_error_errno(r, "Failed to read verity artefacts for %s: %m", arg_image);
        arg_flags |= arg_verity_data ? DISSECT_IMAGE_NO_PARTITION_TABLE : 0;

        r = dissect_image_and_warn(d->fd, arg_image, arg_root_hash, arg_root_hash_size, arg_verity_data, NULL, arg_flags, &m);
        if (r < 0)
                return r;

        switch (arg_action) {

        case ACTION_DISSECT: {
                uint64_t size;
                unsigned i;

                for (i = 0; i < _PARTITION_DESIGNATOR_MAX; i++) {
                        DissectedPartition *p = m->partitions + i;

                        if (!p->found)
                                continue;

                        printf("Found %s '%s' partition",
                               p->rw ? "writable" : "read-only",
                               partition_designator_to_string(i));

                        if (!sd_id128_is_null(p->uuid))
                                printf(" (UUID " SD_ID128_FORMAT_STR ")", SD_ID128_FORMAT_VAL(p->uuid));

                        if (p->fstype)
                                printf(" of type %s", p->fstype);

                        if (p->architecture != _ARCHITECTURE_INVALID)
                                printf(" for %s", architecture_to_string(p->architecture));

                        if (dissected_image_can_do_verity(m, i))
                                printf(" %s verity", dissected_image_has_verity(m, i) ? "with" : "without");

                        if (p->partno >= 0)
                                printf(" on partition #%i", p->partno);

                        if (p->node)
                                printf(" (%s)", p->node);

                        putchar('\n');
                }

                printf("      Name: %s\n", basename(arg_image));

                if (ioctl(d->fd, BLKGETSIZE64, &size) < 0)
                        log_debug_errno(errno, "Failed to query size of loopback device: %m");
                else {
                        char t[FORMAT_BYTES_MAX];
                        printf("      Size: %s\n", format_bytes(t, sizeof(t), size));
                }

                r = dissected_image_acquire_metadata(m);
                if (r < 0)
                        return log_error_errno(r, "Failed to acquire image metadata: %m");

                if (m->hostname)
                        printf("  Hostname: %s\n", m->hostname);

                if (!sd_id128_is_null(m->machine_id))
                        printf("Machine ID: " SD_ID128_FORMAT_STR "\n", SD_ID128_FORMAT_VAL(m->machine_id));

                if (!strv_isempty(m->machine_info)) {
                        char **p, **q;

                        STRV_FOREACH_PAIR(p, q, m->machine_info)
                                printf("%s %s=%s\n",
                                       p == m->machine_info ? "Mach. Info:" : "           ",
                                       *p, *q);
                }

                if (!strv_isempty(m->os_release)) {
                        char **p, **q;

                        STRV_FOREACH_PAIR(p, q, m->os_release)
                                printf("%s %s=%s\n",
                                       p == m->os_release ? "OS Release:" : "           ",
                                       *p, *q);
                }

                break;
        }

        case ACTION_MOUNT:
                r = dissected_image_decrypt_interactively(
                                m, NULL,
                                arg_root_hash, arg_root_hash_size,
                                arg_verity_data,
                                arg_root_hash_sig_path, arg_root_hash_sig, arg_root_hash_sig_size,
                                arg_flags,
                                &di);
                if (r < 0)
                        return r;

                r = dissected_image_mount(m, arg_path, UID_INVALID, arg_flags);
                if (r == -EUCLEAN)
                        return log_error_errno(r, "File system check on image failed: %m");
                if (r < 0)
                        return log_error_errno(r, "Failed to mount image: %m");

                if (di) {
                        r = decrypted_image_relinquish(di);
                        if (r < 0)
                                return log_error_errno(r, "Failed to relinquish DM devices: %m");
                }

                loop_device_relinquish(d);
                break;

        case ACTION_COPY_FROM:
        case ACTION_COPY_TO: {
                _cleanup_(umount_and_rmdir_and_freep) char *mounted_dir = NULL;
                _cleanup_(rmdir_and_freep) char *created_dir = NULL;
                _cleanup_free_ char *temp = NULL;

                r = dissected_image_decrypt_interactively(
                                m, NULL,
                                arg_root_hash, arg_root_hash_size,
                                arg_verity_data,
                                arg_root_hash_sig_path, arg_root_hash_sig, arg_root_hash_sig_size,
                                arg_flags,
                                &di);
                if (r < 0)
                        return r;

                r = detach_mount_namespace();
                if (r < 0)
                        return log_error_errno(r, "Failed to detach mount namespace: %m");

                r = tempfn_random_child(NULL, program_invocation_short_name, &temp);
                if (r < 0)
                        return log_error_errno(r, "Failed to generate temporary mount directory: %m");

                r = mkdir_p(temp, 0700);
                if (r < 0)
                        return log_error_errno(r, "Failed to create mount point: %m");

                created_dir = TAKE_PTR(temp);

                r = dissected_image_mount(m, created_dir, UID_INVALID, arg_flags);
                if (r == -EUCLEAN)
                        return log_error_errno(r, "File system check on image failed: %m");
                if (r < 0)
                        return log_error_errno(r, "Failed to mount image: %m");

                mounted_dir = TAKE_PTR(created_dir);

                if (di) {
                        r = decrypted_image_relinquish(di);
                        if (r < 0)
                                return log_error_errno(r, "Failed to relinquish DM devices: %m");
                }

                loop_device_relinquish(d);

                if (arg_action == ACTION_COPY_FROM) {
                        _cleanup_close_ int source_fd = -1, target_fd = -1;

                        source_fd = chase_symlinks_and_open(arg_source, mounted_dir, CHASE_PREFIX_ROOT|CHASE_WARN, O_RDONLY|O_CLOEXEC|O_NOCTTY, NULL);
                        if (source_fd < 0)
                                return log_error_errno(source_fd, "Failed to open source path '%s' in image '%s': %m", arg_source, arg_image);

                        /* Copying to stdout? */
                        if (streq(arg_target, "-")) {
                                r = copy_bytes(source_fd, STDOUT_FILENO, (uint64_t) -1, COPY_REFLINK);
                                if (r < 0)
                                        return log_error_errno(r, "Failed to copy bytes from %s in mage '%s' to stdout: %m", arg_source, arg_image);

                                /* When we copy to stdou we don't copy any attributes (i.e. no access mode, no ownership, no xattr, no times) */
                                break;
                        }

                        /* Try to copy as directory? */
                        r = copy_directory_fd(source_fd, arg_target, COPY_REFLINK|COPY_MERGE_EMPTY|COPY_SIGINT);
                        if (r >= 0)
                                break;
                        if (r != -ENOTDIR)
                                return log_error_errno(r, "Failed to copy %s in image '%s' to '%s': %m", arg_source, arg_image, arg_target);

                        r = fd_verify_regular(source_fd);
                        if (r == -EISDIR)
                                return log_error_errno(r, "Target '%s' exists already and is not a directory.", arg_target);
                        if (r < 0)
                                return log_error_errno(r, "Source path %s in image '%s' is neither regular file nor directory, refusing: %m", arg_source, arg_image);

                        /* Nah, it's a plain file! */
                        target_fd = open(arg_target, O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOCTTY|O_NOFOLLOW, 0600);
                        if (target_fd < 0)
                                return log_error_errno(errno, "Failed to create regular file at target path '%s': %m", arg_target);

                        r = copy_bytes(source_fd, target_fd, (uint64_t) -1, COPY_REFLINK);
                        if (r < 0)
                                return log_error_errno(r, "Failed to copy bytes from %s in mage '%s' to '%s': %m", arg_source, arg_image, arg_target);

                        (void) copy_xattr(source_fd, target_fd);
                        (void) copy_access(source_fd, target_fd);
                        (void) copy_times(source_fd, target_fd, 0);

                        /* When this is a regular file we don't copy ownership! */

                } else {
                        _cleanup_close_ int source_fd = -1, target_fd = -1;
                        _cleanup_close_ int dfd = -1;
                        _cleanup_free_ char *dn = NULL;

                        assert(arg_action == ACTION_COPY_TO);

                        dn = dirname_malloc(arg_target);
                        if (!dn)
                                return log_oom();

                        r = chase_symlinks(dn, mounted_dir, CHASE_PREFIX_ROOT|CHASE_WARN, NULL, &dfd);
                        if (r < 0)
                                return log_error_errno(r, "Failed to open '%s': %m", dn);

                        /* Are we reading from stdin? */
                        if (streq(arg_source, "-")) {
                                target_fd = openat(dfd, basename(arg_target), O_WRONLY|O_CREAT|O_CLOEXEC|O_NOCTTY|O_EXCL, 0644);
                                if (target_fd < 0)
                                        return log_error_errno(errno, "Failed to open target file '%s': %m", arg_target);

                                r = copy_bytes(STDIN_FILENO, target_fd, (uint64_t) -1, COPY_REFLINK);
                                if (r < 0)
                                        return log_error_errno(r, "Failed to copy bytes from stdin to '%s' in image '%s': %m", arg_target, arg_image);

                                /* When we copy from stdin we don't copy any attributes (i.e. no access mode, no ownership, no xattr, no times) */
                                break;
                        }

                        source_fd = open(arg_source, O_RDONLY|O_CLOEXEC|O_NOCTTY);
                        if (source_fd < 0)
                                return log_error_errno(source_fd, "Failed to open source path '%s': %m", arg_source);

                        r = fd_verify_regular(source_fd);
                        if (r < 0) {
                                if (r != -EISDIR)
                                        return log_error_errno(r, "Source '%s' is neither regular file nor directory: %m", arg_source);

                                /* We are looking at a directory. */

                                target_fd = openat(dfd, basename(arg_target), O_RDONLY|O_DIRECTORY|O_CLOEXEC);
                                if (target_fd < 0) {
                                        if (errno != ENOENT)
                                                return log_error_errno(errno, "Failed to open destination '%s': %m", arg_target);

                                        r = copy_tree_at(source_fd, ".", dfd, basename(arg_target), UID_INVALID, GID_INVALID, COPY_REFLINK|COPY_REPLACE|COPY_SIGINT);
                                } else
                                        r = copy_tree_at(source_fd, ".", target_fd, ".", UID_INVALID, GID_INVALID, COPY_REFLINK|COPY_REPLACE|COPY_SIGINT);
                                if (r < 0)
                                        return log_error_errno(r, "Failed to copy '%s' to '%s' in image '%s': %m", arg_source, arg_target, arg_image);

                                break;
                        }

                        /* We area looking at a regular file */
                        target_fd = openat(dfd, basename(arg_target), O_WRONLY|O_CREAT|O_CLOEXEC|O_NOCTTY|O_EXCL, 0600);
                        if (target_fd < 0)
                                return log_error_errno(errno, "Failed to open target file '%s': %m", arg_target);

                        r = copy_bytes(source_fd, target_fd, (uint64_t) -1, COPY_REFLINK);
                        if (r < 0)
                                return log_error_errno(r, "Failed to copy bytes from '%s' to '%s' in image '%s': %m", arg_source, arg_target, arg_image);

                        (void) copy_xattr(source_fd, target_fd);
                        (void) copy_access(source_fd, target_fd);
                        (void) copy_times(source_fd, target_fd, 0);

                        /* When this is a regular file we don't copy ownership! */
                }

                break;
        }

        default:
                assert_not_reached("Unknown action.");
        }

        return 0;
}

DEFINE_MAIN_FUNCTION(run);
