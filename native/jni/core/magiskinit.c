/* magiskinit.c - Pre-init Magisk support
 *
 * This code has to be compiled statically to work properly.
 *
 * To unify Magisk support for both legacy "normal" devices and new skip_initramfs devices,
 * magisk binary compilation is split into two parts - first part only compiles "magisk".
 * The python build script will load the magisk main binary and compress with lzma2, dumping
 * the results into "dump.h". The "magisk" binary is embedded into this binary, and will
 * get extracted to the overlay folder along with init.magisk.rc.
 *
 * This tool does all pre-init operations to setup a Magisk environment, which pathces rootfs
 * on the fly, providing fundamental support such as init, init.rc, and sepolicy patching.
 *
 * Magiskinit is also responsible for constructing a proper rootfs on skip_initramfs devices.
 * On skip_initramfs devices, it will parse kernel cmdline, mount sysfs, parse through
 * uevent files to make the system (or vendor if available) block device node, then copy
 * rootfs files from system.
 *
 * This tool will be replaced with the real init to continue the boot process, but hardlinks are
 * preserved as it also provides CLI for sepolicy patching (magiskpolicy)
 */


#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <libgen.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/sysmacros.h>

#include <lzma.h>
#include <cil/cil.h>

#include "dump.h"
#include "magiskrc.h"
#include "utils.h"
#include "magiskpolicy.h"
#include "daemon.h"
#include "cpio.h"
#include "magisk.h"

#ifdef MAGISK_DEBUG
	#define VLOG(fmt, ...) printf(fmt, __VA_ARGS__)
#else
	#define VLOG(fmt, ...)
#endif

extern policydb_t *policydb;
int (*init_applet_main[]) (int, char *[]) = { magiskpolicy_main, magiskpolicy_main, NULL };
static char RAND_SOCKET_NAME[sizeof(SOCKET_NAME)];
static int SOCKET_OFF = -1;

struct cmdline {
	char skip_initramfs;
	char slot[3];
};

struct device {
	dev_t major;
	dev_t minor;
	char devname[32];
	char partname[32];
	char path[64];
};

static void parse_cmdline(struct cmdline *cmd)
{
	// cleanup
	memset(cmd, 0, sizeof(&cmd));

	char cmdline[4096];
	mkdir("/proc", 0555);
	xmount("proc", "/proc", "proc", 0, NULL);
	int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
	cmdline[read(fd, cmdline, sizeof(cmdline))] = '\0';
	close(fd);
	umount("/proc");
	for (char *tok = strtok(cmdline, " "); tok; tok = strtok(NULL, " ")) {
		if (strncmp(tok, "androidboot.slot_suffix", 23) == 0) {
			sscanf(tok, "androidboot.slot_suffix=%s", cmd->slot);
		} else if (strncmp(tok, "androidboot.slot", 16) == 0) {
			cmd->slot[0] = '_';
			sscanf(tok, "androidboot.slot=%c", cmd->slot + 1);
		} else if (strcmp(tok, "skip_initramfs") == 0) {
			cmd->skip_initramfs = 1;
		}
	}

	VLOG("cmdline: skip_initramfs[%d] slot[%s]\n", cmd->skip_initramfs, cmd->slot);
}

static void parse_device(struct device *dev, char *uevent)
{
	dev->partname[0] = '\0';
	char *tok;
	tok = strtok(uevent, "\n");
	while (tok != NULL) {
		if (strncmp(tok, "MAJOR", 5) == 0) {
			sscanf(tok, "MAJOR=%ld", (long*) &dev->major);
		} else if (strncmp(tok, "MINOR", 5) == 0) {
			sscanf(tok, "MINOR=%ld", (long*) &dev->minor);
		} else if (strncmp(tok, "DEVNAME", 7) == 0) {
			sscanf(tok, "DEVNAME=%s", dev->devname);
		} else if (strncmp(tok, "PARTNAME", 8) == 0) {
			sscanf(tok, "PARTNAME=%s", dev->partname);
		}
		tok = strtok(NULL, "\n");
	}
	VLOG("%s [%s] (%u, %u)\n", dev->devname, dev->partname, (unsigned) dev->major, (unsigned) dev->minor);
}

static int setup_block(struct device *dev, const char *partname)
{
	char buffer[1024], path[128];
	struct dirent *entry;
	DIR *dir = opendir("/sys/dev/block");
	if (dir == NULL)
		return 1;
	int found = 0;
	while ((entry = readdir(dir))) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		snprintf(path, sizeof(path), "/sys/dev/block/%s/uevent", entry->d_name);
		int fd = open(path, O_RDONLY | O_CLOEXEC);
		ssize_t size = read(fd, buffer, sizeof(buffer));
		buffer[size] = '\0';
		close(fd);
		parse_device(dev, buffer);
		if (strcmp(dev->partname, partname) == 0) {
			snprintf(dev->path, sizeof(dev->path), "/dev/block/%s", dev->devname);
			found = 1;
			break;
		}
	}
	closedir(dir);

	if (!found)
		return 1;

	mkdir("/dev", 0755);
	mkdir("/dev/block", 0755);
	mknod(dev->path, S_IFBLK | 0600, makedev(dev->major, dev->minor));
	return 0;
}

static void patch_ramdisk()
{
	void *addr;
	size_t size;
	mmap_rw("/init", &addr, &size);
	for (int i = 0; i < size; ++i) {
		if (memcmp(addr + i, SPLIT_PLAT_CIL, sizeof(SPLIT_PLAT_CIL) - 1) == 0) {
			memcpy(addr + i + sizeof(SPLIT_PLAT_CIL) - 4, "xxx", 3);
			break;
		}
	}
	munmap(addr, size);

	full_read("/init.rc", &addr, &size);
	patch_init_rc(&addr, &size);
	int fd = creat("/init.rc", 0750);
	write(fd, addr, size);
	close(fd);
	free(addr);
}

static int strend(const char *s1, const char *s2)
{
	size_t l1 = strlen(s1);
	size_t l2 = strlen(s2);
	return strcmp(s1 + l1 - l2, s2);
}

static int compile_cil()
{
	DIR *dir;
	struct dirent *entry;
	char path[128];

	struct cil_db *db = NULL;
	sepol_policydb_t *pdb = NULL;
	void *addr;
	size_t size;

	cil_db_init(&db);
	cil_set_mls(db, 1);
	cil_set_multiple_decls(db, 1);
	cil_set_disable_neverallow(db, 1);
	cil_set_target_platform(db, SEPOL_TARGET_SELINUX);
	cil_set_policy_version(db, POLICYDB_VERSION_XPERMS_IOCTL);
	cil_set_attrs_expand_generated(db, 0);

	// plat
	mmap_ro(SPLIT_PLAT_CIL, &addr, &size);
	VLOG("cil_add[%s]\n", SPLIT_PLAT_CIL);
	cil_add_file(db, SPLIT_PLAT_CIL, addr, size);
	munmap(addr, size);

	// mapping
	char plat[10];
	int fd = open(SPLIT_NONPLAT_VER, O_RDONLY | O_CLOEXEC);
	plat[read(fd, plat, sizeof(plat)) - 1] = '\0';
	snprintf(path, sizeof(path), SPLIT_PLAT_MAPPING, plat);
	mmap_ro(path, &addr, &size);
	VLOG("cil_add[%s]\n", path);
	cil_add_file(db, path, addr, size);
	munmap(addr, size);
	close(fd);

	// nonplat
	dir = opendir(NONPLAT_POLICY_DIR);
	while ((entry = readdir(dir))) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		if (strend(entry->d_name, ".cil") == 0) {
			snprintf(path, sizeof(path), NONPLAT_POLICY_DIR "%s", entry->d_name);
			mmap_ro(path, &addr, &size);
			VLOG("cil_add[%s]\n", path);
			cil_add_file(db, path, addr, size);
			munmap(addr, size);
		}
	}
	closedir(dir);

	cil_compile(db);
	cil_build_policydb(db, &pdb);
	cil_db_destroy(&db);

	policydb = &pdb->p;

	return 0;
}

static int verify_precompiled()
{
	DIR *dir;
	struct dirent *entry;
	int fd;
	char sys_sha[70], ven_sha[70];

	// init the strings with different value
	sys_sha[0] = 0;
	ven_sha[0] = 1;

	dir = opendir(NONPLAT_POLICY_DIR);
	while ((entry = readdir(dir))) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		if (strend(entry->d_name, ".sha256") == 0) {
			fd = openat(dirfd(dir), entry->d_name, O_RDONLY | O_CLOEXEC);
			ven_sha[read(fd, ven_sha, sizeof(ven_sha)) - 1] = '\0';
			close(fd);
			break;
		}
	}
	closedir(dir);
	dir = opendir(PLAT_POLICY_DIR);
	while ((entry = readdir(dir))) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		if (strend(entry->d_name, ".sha256") == 0) {
			fd = openat(dirfd(dir), entry->d_name, O_RDONLY | O_CLOEXEC);
			sys_sha[read(fd, sys_sha, sizeof(sys_sha)) - 1] = '\0';
			close(fd);
			break;
		}
	}
	closedir(dir);
	VLOG("sys_sha[%s]\nven_sha[%s]\n", sys_sha, ven_sha);
	return strcmp(sys_sha, ven_sha) == 0;
}

static int patch_sepolicy()
{
	if (access("/sepolicy", R_OK) == 0)
		load_policydb("/sepolicy");
	else if (access(SPLIT_PRECOMPILE, R_OK) == 0 && verify_precompiled())
		load_policydb(SPLIT_PRECOMPILE);
	else if (access(SPLIT_PLAT_CIL, R_OK) == 0)
		compile_cil();
	else
		return 1;

	sepol_magisk_rules();
	dump_policydb("/sepolicy");

	// Remove the stupid debug sepolicy and use our own
	if (access("/sepolicy_debug", F_OK) == 0) {
		unlink("/sepolicy_debug");
		link("/sepolicy", "/sepolicy_debug");
	}

	return 0;
}

#define BUFSIZE (1 << 20)

static int unxz(const void *buf, size_t size, int fd)
{
	lzma_stream strm = LZMA_STREAM_INIT;
	if (lzma_auto_decoder(&strm, UINT64_MAX, 0) != LZMA_OK)
		return 1;
	lzma_ret ret;
	void *out = malloc(BUFSIZE);
	strm.next_in = buf;
	strm.avail_in = size;
	do {
		strm.next_out = out;
		strm.avail_out = BUFSIZE;
		ret = lzma_code(&strm, LZMA_RUN);
		write(fd, out, BUFSIZE - strm.avail_out);
	} while (strm.avail_out == 0 && ret == LZMA_OK);

	free(out);
	lzma_end(&strm);

	if (ret != LZMA_OK && ret != LZMA_STREAM_END)
		return 1;
	return 0;
}

static int dump_magisk(const char *path, mode_t mode)
{
	unlink(path);
	int fd = creat(path, mode);
	int ret = unxz(magisk_dump, sizeof(magisk_dump), fd);
	close(fd);
	return ret;
}

static int dump_magiskrc(const char *path, mode_t mode)
{
	int fd = creat(path, mode);
	write(fd, magiskrc, sizeof(magiskrc));
	close(fd);
	return 0;
}

static void patch_socket_name(const char *path)
{
	void *buf;
	size_t size;
	mmap_rw(path, &buf, &size);
	if (SOCKET_OFF < 0) {
		for (int i = 0; i < size; ++i) {
			if (memcmp(buf + i, SOCKET_NAME, sizeof(SOCKET_NAME)) == 0) {
				SOCKET_OFF = i;
				break;
			}
		}
	}
	gen_rand_str(RAND_SOCKET_NAME, sizeof(SOCKET_NAME));
	memcpy(buf + SOCKET_OFF, RAND_SOCKET_NAME, sizeof(SOCKET_NAME));
	munmap(buf, size);
}

int main(int argc, char *argv[])
{
	umask(0);

	for (int i = 0; init_applet[i]; ++i) {
		if (strcmp(basename(argv[0]), init_applet[i]) == 0)
			return (*init_applet_main[i])(argc, argv);
	}

	if (argc > 1 && strcmp(argv[1], "-x") == 0) {
		if (strcmp(argv[2], "magisk") == 0)
			return dump_magisk(argv[3], 0755);
		else if (strcmp(argv[2], "magiskrc") == 0)
			return dump_magiskrc(argv[3], 0755);
	}

	// Prevent file descriptor confusion
	mknod("/null", S_IFCHR | 0666, makedev(1, 3));
	int null = open("/null", O_RDWR | O_CLOEXEC);
	unlink("/null");
	dup3(null, STDIN_FILENO, O_CLOEXEC);
	dup3(null, STDOUT_FILENO, O_CLOEXEC);
	dup3(null, STDERR_FILENO, O_CLOEXEC);
	if (null > STDERR_FILENO)
		close(null);

	// Backup self
	link("/init", "/init.bak");

	struct cmdline cmd;
	parse_cmdline(&cmd);

	/* ***********
	 * Initialize
	 * ***********/

	int root = open("/", O_RDONLY | O_CLOEXEC);

	if (cmd.skip_initramfs) {
		// Clear rootfs
		excl_list = (char *[]) { "overlay", ".backup", "init.bak", NULL };
		frm_rf(root);
	} else if (access("/ramdisk.cpio.xz", R_OK) == 0) {
		// High compression mode
		void *addr;
		size_t size;
		mmap_ro("/ramdisk.cpio.xz", &addr, &size);
		int fd = creat("/ramdisk.cpio", 0);
		unxz(addr, size, fd);
		munmap(addr, size);
		close(fd);
		struct vector v;
		vec_init(&v);
		parse_cpio(&v, "/ramdisk.cpio");
		excl_list = (char *[]) { "overlay", ".backup", NULL };
		frm_rf(root);
		chdir("/");
		cpio_extract_all(&v);
		cpio_vec_destroy(&v);
	} else {
		// Revert original init binary
		unlink("/init");
		link("/.backup/init", "/init");
	}

	/* ************
	 * Early Mount
	 * ************/

	// If skip_initramfs or using split policies, we need early mount
	if (cmd.skip_initramfs || access("/sepolicy", R_OK) != 0) {
		char partname[32];
		struct device dev;

		// Mount sysfs
		mkdir("/sys", 0755);
		xmount("sysfs", "/sys", "sysfs", 0, NULL);

		// Mount system
		snprintf(partname, sizeof(partname), "SYSTEM%s", cmd.slot);
		setup_block(&dev, partname);
		if (cmd.skip_initramfs) {
			mkdir("/system_root", 0755);
			xmount(dev.path, "/system_root", "ext4", MS_RDONLY, NULL);
			int system_root = open("/system_root", O_RDONLY | O_CLOEXEC);

			// Clone rootfs except /system
			excl_list = (char *[]) { "system", NULL };
			clone_dir(system_root, root);
			close(system_root);

			mkdir("/system", 0755);
			xmount("/system_root/system", "/system", NULL, MS_BIND, NULL);
		} else {
			xmount(dev.path, "/system", "ext4", MS_RDONLY, NULL);
		}

		// Mount vendor
		snprintf(partname, sizeof(partname), "VENDOR%s", cmd.slot);
		if (setup_block(&dev, partname) == 0)
			xmount(dev.path, "/vendor", "ext4", MS_RDONLY, NULL);
	}

	/* *************
	 * Patch rootfs
	 * *************/

	// Only patch rootfs if not intended to run in recovery
	if (access("/etc/recovery.fstab", F_OK) != 0) {
		int fd;

		// Handle ramdisk overlays
		fd = open("/overlay", O_RDONLY | O_CLOEXEC);
		if (fd >= 0) {
			mv_dir(fd, root);
			close(fd);
			rmdir("/overlay");
		}

		patch_ramdisk();
		patch_sepolicy();

		// Dump binaries
		dump_magiskrc("/init.magisk.rc", 0750);
		dump_magisk("/sbin/magisk", 0755);
		patch_socket_name("/sbin/magisk");
		rename("/init.bak", "/sbin/magiskinit");
	}

	// Clean up
	close(root);
	if (!cmd.skip_initramfs)
		umount("/system");
	umount("/vendor");

	// Finally, give control back!
	execv("/init", argv);
}
