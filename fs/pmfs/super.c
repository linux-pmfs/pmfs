/*
 * BRIEF DESCRIPTION
 *
 * Super block operations.
 *
 * Copyright 2012-2013 Intel Corporation
 * Copyright 2009-2011 Marco Stornelli <marco.stornelli@gmail.com>
 * Copyright 2003 Sony Corporation
 * Copyright 2003 Matsushita Electric Industrial Co., Ltd.
 * 2003-2004 (c) MontaVista Software, Inc. , Steve Longerbeam
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/module.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/parser.h>
#include <linux/vfs.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/seq_file.h>
#include <linux/mount.h>
#include <linux/mm.h>
#include <linux/ctype.h>
#include <linux/bitops.h>
#include <linux/magic.h>
#include <linux/exportfs.h>
#include <linux/random.h>
#include <linux/cred.h>
#include <linux/backing-dev.h>
#include <linux/list.h>
#include "pmfs.h"

static struct super_operations pmfs_sops;
static const struct export_operations pmfs_export_ops;
static struct kmem_cache *pmfs_inode_cachep;
static struct kmem_cache *pmfs_blocknode_cachep;
static struct kmem_cache *pmfs_transaction_cachep;
/* FIXME: should the following variable be one per PMFS instance? */
unsigned int pmfs_dbgmask = 0;

#ifdef CONFIG_PMFS_TEST
static void *first_pmfs_super;

struct pmfs_super_block *get_pmfs_super(void)
{
	return (struct pmfs_super_block *)first_pmfs_super;
}
EXPORT_SYMBOL(get_pmfs_super);
#endif

void pmfs_error_mng(struct super_block *sb, const char *fmt, ...)
{
	va_list args;

	printk("pmfs error: ");
	va_start(args, fmt);
	vprintk(fmt, args);
	va_end(args);

	if (test_opt(sb, ERRORS_PANIC))
		panic("pmfs: panic from previous error\n");
	if (test_opt(sb, ERRORS_RO)) {
		printk(KERN_CRIT "pmfs err: remounting filesystem read-only");
		sb->s_flags |= MS_RDONLY;
	}
}

static void pmfs_set_blocksize(struct super_block *sb, unsigned long size)
{
	int bits;

	/*
	 * We've already validated the user input and the value here must be
	 * between PMFS_MAX_BLOCK_SIZE and PMFS_MIN_BLOCK_SIZE
	 * and it must be a power of 2.
	 */
	bits = fls(size) - 1;
	sb->s_blocksize_bits = bits;
	sb->s_blocksize = (1 << bits);
}

static inline int pmfs_has_huge_ioremap(struct super_block *sb)
{
	struct pmfs_sb_info *sbi = (struct pmfs_sb_info *)sb->s_fs_info;

	return sbi->s_mount_opt & PMFS_MOUNT_HUGEIOREMAP;
}

void *pmfs_ioremap(struct super_block *sb, phys_addr_t phys_addr, ssize_t size)
{
	void *retval;
	int protect, hugeioremap;

	if (sb) {
		protect = pmfs_is_wprotected(sb);
		hugeioremap = pmfs_has_huge_ioremap(sb);
	} else {
		protect = 0;
		hugeioremap = 1;
	}

	/*
	 * NOTE: Userland may not map this resource, we will mark the region so
	 * /dev/mem and the sysfs MMIO access will not be allowed. This
	 * restriction depends on STRICT_DEVMEM option. If this option is
	 * disabled or not available we mark the region only as busy.
	 */
	retval = request_mem_region_exclusive(phys_addr, size, "pmfs");
	if (!retval)
		goto fail;

	if (protect) {
		if (hugeioremap)
			retval = ioremap_hpage_cache_ro(phys_addr, size);
		else
			retval = ioremap_cache_ro(phys_addr, size);
	} else {
		if (hugeioremap)
			retval = ioremap_hpage_cache(phys_addr, size);
		else
			retval = ioremap_cache(phys_addr, size);
	}

fail:
	return retval;
}

static inline int pmfs_iounmap(void *virt_addr, ssize_t size, int protected)
{
	iounmap(virt_addr);
	return 0;
}

static loff_t pmfs_max_size(int bits)
{
	loff_t res;

	res = (1ULL << (3 * 9 + bits)) - 1;

	if (res > MAX_LFS_FILESIZE)
		res = MAX_LFS_FILESIZE;

	pmfs_dbg_verbose("max file size %llu bytes\n", res);
	return res;
}

enum {
	Opt_addr, Opt_bpi, Opt_size, Opt_jsize,
	Opt_num_inodes, Opt_mode, Opt_uid,
	Opt_gid, Opt_blocksize, Opt_wprotect, Opt_wprotectold,
	Opt_err_cont, Opt_err_panic, Opt_err_ro,
	Opt_backing, Opt_backing_opt,
	Opt_hugemmap, Opt_nohugeioremap, Opt_dbgmask, Opt_err
};

static const match_table_t tokens = {
	{ Opt_addr,	     "physaddr=%x"	  },
	{ Opt_bpi,	     "bpi=%u"		  },
	{ Opt_size,	     "init=%s"		  },
	{ Opt_jsize,     "jsize=%s"		  },
	{ Opt_num_inodes,"num_inodes=%u"  },
	{ Opt_mode,	     "mode=%o"		  },
	{ Opt_uid,	     "uid=%u"		  },
	{ Opt_gid,	     "gid=%u"		  },
	{ Opt_wprotect,	     "wprotect"		  },
	{ Opt_wprotectold,   "wprotectold"	  },
	{ Opt_err_cont,	     "errors=continue"	  },
	{ Opt_err_panic,     "errors=panic"	  },
	{ Opt_err_ro,	     "errors=remount-ro"  },
	{ Opt_backing,	     "backing=%s"	  },
	{ Opt_backing_opt,   "backing_opt=%u"	  },
	{ Opt_hugemmap,	     "hugemmap"		  },
	{ Opt_nohugeioremap, "nohugeioremap"	  },
	{ Opt_dbgmask,	     "dbgmask=%u"	  },
	{ Opt_err,	     NULL		  },
};

static phys_addr_t get_phys_addr(void **data)
{
	phys_addr_t phys_addr;
	char *options = (char *)*data;

	if (!options || strncmp(options, "physaddr=", 9) != 0)
		return (phys_addr_t)ULLONG_MAX;
	options += 9;
	phys_addr = (phys_addr_t)simple_strtoull(options, &options, 0);
	if (*options && *options != ',') {
		printk(KERN_ERR "Invalid phys addr specification: %s\n",
		       (char *)*data);
		return (phys_addr_t)ULLONG_MAX;
	}
	if (phys_addr & (PAGE_SIZE - 1)) {
		printk(KERN_ERR "physical address 0x%16llx for pmfs isn't "
		       "aligned to a page boundary\n", (u64)phys_addr);
		return (phys_addr_t)ULLONG_MAX;
	}
	if (*options == ',')
		options++;
	*data = (void *)options;
	return phys_addr;
}

static int pmfs_parse_options(char *options, struct pmfs_sb_info *sbi,
			       bool remount)
{
	char *p, *rest;
	substring_t args[MAX_OPT_ARGS];
	int option;

	if (!options)
		return 0;

	while ((p = strsep(&options, ",")) != NULL) {
		int token;
		if (!*p)
			continue;

		token = match_token(p, tokens, args);
		switch (token) {
		case Opt_addr:
			if (remount)
				goto bad_opt;
			/* physaddr managed in get_phys_addr() */
			break;
		case Opt_bpi:
			if (remount)
				goto bad_opt;
			if (match_int(&args[0], &option))
				goto bad_val;
			sbi->bpi = option;
			break;
		case Opt_uid:
			if (remount)
				goto bad_opt;
			if (match_int(&args[0], &option))
				goto bad_val;
			sbi->uid = make_kuid(current_user_ns(), option);
			break;
		case Opt_gid:
			if (match_int(&args[0], &option))
				goto bad_val;
			sbi->gid = make_kgid(current_user_ns(), option);
			break;
		case Opt_mode:
			if (match_octal(&args[0], &option))
				goto bad_val;
			sbi->mode = option & 01777U;
			break;
		case Opt_size:
			if (remount)
				goto bad_opt;
			/* memparse() will accept a K/M/G without a digit */
			if (!isdigit(*args[0].from))
				goto bad_val;
			sbi->initsize = memparse(args[0].from, &rest);
			set_opt(sbi->s_mount_opt, FORMAT);
			break;
		case Opt_jsize:
			if (remount)
				goto bad_opt;
			/* memparse() will accept a K/M/G without a digit */
			if (!isdigit(*args[0].from))
				goto bad_val;
			sbi->jsize = memparse(args[0].from, &rest);
			/* make sure journal size is integer power of 2 */
			if (sbi->jsize & (sbi->jsize - 1) ||
				sbi->jsize < PMFS_MINIMUM_JOURNAL_SIZE) {
				pmfs_dbg("Invalid jsize: "
					"must be whole power of 2 & >= 64KB\n");
				goto bad_val;
			}
			break;
		case Opt_num_inodes:
			if (remount)
				goto bad_opt;
			if (match_int(&args[0], &option))
				goto bad_val;
			sbi->num_inodes = option;
			break;
		case Opt_err_panic:
			clear_opt(sbi->s_mount_opt, ERRORS_CONT);
			clear_opt(sbi->s_mount_opt, ERRORS_RO);
			set_opt(sbi->s_mount_opt, ERRORS_PANIC);
			break;
		case Opt_err_ro:
			clear_opt(sbi->s_mount_opt, ERRORS_CONT);
			clear_opt(sbi->s_mount_opt, ERRORS_PANIC);
			set_opt(sbi->s_mount_opt, ERRORS_RO);
			break;
		case Opt_err_cont:
			clear_opt(sbi->s_mount_opt, ERRORS_RO);
			clear_opt(sbi->s_mount_opt, ERRORS_PANIC);
			set_opt(sbi->s_mount_opt, ERRORS_CONT);
			break;
		case Opt_wprotect:
			if (remount)
				goto bad_opt;
			set_opt(sbi->s_mount_opt, PROTECT);
			pmfs_info
				("PMFS: Enabling new Write Protection (CR0.WP)\n");
			break;
		case Opt_wprotectold:
			if (remount)
				goto bad_opt;
			set_opt(sbi->s_mount_opt, PROTECT_OLD);
			pmfs_info
				("PMFS: Enabling old Write Protection (PAGE RW Bit)\n");
			break;
		case Opt_hugemmap:
			if (remount)
				goto bad_opt;
			set_opt(sbi->s_mount_opt, HUGEMMAP);
			pmfs_info("PMFS: Enabling huge mappings for mmap\n");
			break;
		case Opt_nohugeioremap:
			if (remount)
				goto bad_opt;
			clear_opt(sbi->s_mount_opt, HUGEIOREMAP);
			pmfs_info("PMFS: Disabling huge ioremap\n");
			break;
		case Opt_dbgmask:
			if (match_int(&args[0], &option))
				goto bad_val;
			pmfs_dbgmask = option;
			break;
		case Opt_backing:
			strncpy(sbi->pmfs_backing_file, args[0].from, 255);
			break;
		case Opt_backing_opt:
			if (match_int(&args[0], &option))
				goto bad_val;
			sbi->pmfs_backing_option = option;
			break;
		default: {
			goto bad_opt;
		}
		}
	}

	return 0;

bad_val:
	printk(KERN_INFO "Bad value '%s' for mount option '%s'\n", args[0].from,
	       p);
	return -EINVAL;
bad_opt:
	printk(KERN_INFO "Bad mount option: \"%s\"\n", p);
	return -EINVAL;
}

static bool pmfs_check_size (struct super_block *sb, unsigned long size)
{
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	unsigned long minimum_size, num_blocks;

	/* space required for super block and root directory */
	minimum_size = 2 << sb->s_blocksize_bits;

	/* space required for inode table */
	if (sbi->num_inodes > 0)
		num_blocks = (sbi->num_inodes >>
			(sb->s_blocksize_bits - PMFS_INODE_BITS)) + 1;
	else
		num_blocks = 1;
	minimum_size += (num_blocks << sb->s_blocksize_bits);
	/* space required for journal */
	minimum_size += sbi->jsize;

	if (size < minimum_size)
	    return false;

	return true;
}


static struct pmfs_inode *pmfs_init(struct super_block *sb,
				      unsigned long size)
{
	unsigned long blocksize;
	u64 journal_meta_start, journal_data_start, inode_table_start;
	struct pmfs_inode *root_i;
	struct pmfs_super_block *super;
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	struct pmfs_direntry *de;
	unsigned long blocknr;

	pmfs_info("creating an empty pmfs of size %lu\n", size);
	sbi->virt_addr = pmfs_ioremap(sb, sbi->phys_addr, size);
	sbi->block_start = (unsigned long)0;
	sbi->block_end = ((unsigned long)(size) >> PAGE_SHIFT);
	sbi->num_free_blocks = ((unsigned long)(size) >> PAGE_SHIFT);

	if (!sbi->virt_addr) {
		printk(KERN_ERR "ioremap of the pmfs image failed(1)\n");
		return ERR_PTR(-EINVAL);
	}
#ifdef CONFIG_PMFS_TEST
	if (!first_pmfs_super)
		first_pmfs_super = sbi->virt_addr;
#endif

	pmfs_dbg_verbose("pmfs: Default block size set to 4K\n");
	blocksize = sbi->blocksize = PMFS_DEF_BLOCK_SIZE_4K;

	pmfs_set_blocksize(sb, blocksize);
	blocksize = sb->s_blocksize;

	if (sbi->blocksize && sbi->blocksize != blocksize)
		sbi->blocksize = blocksize;

	if (!pmfs_check_size(sb, size)) {
		pmfs_dbg("Specified PMFS size too small 0x%lx. Either increase"
			" PMFS size, or reduce num. of inodes (minimum 32)" 
			" or journal size (minimum 64KB)\n", size);
		return ERR_PTR(-EINVAL);
	}

	journal_meta_start = sizeof(struct pmfs_super_block);
	journal_meta_start = (journal_meta_start + CACHELINE_SIZE - 1) &
		~(CACHELINE_SIZE - 1);
	inode_table_start = journal_meta_start + sizeof(pmfs_journal_t);
	inode_table_start = (inode_table_start + CACHELINE_SIZE - 1) &
		~(CACHELINE_SIZE - 1);

	if ((inode_table_start + sizeof(struct pmfs_inode)) > PMFS_SB_SIZE) {
		pmfs_dbg("PMFS super block defined too small. defined 0x%x, "
				"required 0x%llx\n", PMFS_SB_SIZE,
			inode_table_start + sizeof(struct pmfs_inode));
		return ERR_PTR(-EINVAL);
	}

	journal_data_start = PMFS_SB_SIZE * 2;
	journal_data_start = (journal_data_start + blocksize - 1) &
		~(blocksize - 1);

	pmfs_dbg_verbose("journal meta start %llx data start 0x%llx, "
		"journal size 0x%x, inode_table 0x%llx\n", journal_meta_start,
		journal_data_start, sbi->jsize, inode_table_start);
	pmfs_dbg_verbose("max file name len %d\n", (unsigned int)PMFS_NAME_LEN);

	super = pmfs_get_super(sb);
	pmfs_memunlock_range(sb, super, journal_data_start);

	/* clear out super-block and inode table */
	memset_nt(super, 0, journal_data_start);
	super->s_size = cpu_to_le64(size);
	super->s_blocksize = cpu_to_le32(blocksize);
	super->s_magic = cpu_to_le16(PMFS_SUPER_MAGIC);
	super->s_journal_offset = cpu_to_le64(journal_meta_start);
	super->s_inode_table_offset = cpu_to_le64(inode_table_start);

	pmfs_init_blockmap(sb, journal_data_start + sbi->jsize);
	pmfs_memlock_range(sb, super, journal_data_start);

	if (pmfs_journal_hard_init(sb, journal_data_start, sbi->jsize) < 0) {
		printk(KERN_ERR "Journal hard initialization failed\n");
		return ERR_PTR(-EINVAL);
	}

	if (pmfs_init_inode_table(sb) < 0)
		return ERR_PTR(-EINVAL);

	pmfs_memunlock_range(sb, super, PMFS_SB_SIZE*2);
	pmfs_sync_super(super);
	pmfs_memlock_range(sb, super, PMFS_SB_SIZE*2);

	pmfs_flush_buffer(super, PMFS_SB_SIZE, false);
	pmfs_flush_buffer((char *)super + PMFS_SB_SIZE, sizeof(*super), false);

	pmfs_new_block(sb, &blocknr, PMFS_BLOCK_TYPE_4K, 1);

	root_i = pmfs_get_inode(sb, PMFS_ROOT_INO);

	pmfs_memunlock_inode(sb, root_i);
	root_i->i_mode = cpu_to_le16(sbi->mode | S_IFDIR);
	root_i->i_uid = cpu_to_le32(from_kuid(&init_user_ns, sbi->uid));
	root_i->i_gid = cpu_to_le32(from_kgid(&init_user_ns, sbi->gid));
	root_i->i_links_count = cpu_to_le16(2);
	root_i->i_blk_type = PMFS_BLOCK_TYPE_4K;
	root_i->i_flags = 0;
	root_i->i_blocks = cpu_to_le64(1);
	root_i->i_size = cpu_to_le64(sb->s_blocksize);
	root_i->i_atime = root_i->i_mtime = root_i->i_ctime =
		cpu_to_le32(get_seconds());
	root_i->root = cpu_to_le64(pmfs_get_block_off(sb, blocknr,
						       PMFS_BLOCK_TYPE_4K));
	root_i->height = 0;
	/* pmfs_sync_inode(root_i); */
	pmfs_memlock_inode(sb, root_i);
	pmfs_flush_buffer(root_i, sizeof(*root_i), false);
	de = (struct pmfs_direntry *)
		pmfs_get_block(sb, pmfs_get_block_off(sb, blocknr, PMFS_BLOCK_TYPE_4K));

	pmfs_memunlock_range(sb, de, sb->s_blocksize);
	de->ino = cpu_to_le64(PMFS_ROOT_INO);
	de->name_len = 1;
	de->de_len = cpu_to_le16(PMFS_DIR_REC_LEN(de->name_len));
	strcpy(de->name, ".");
	de = (struct pmfs_direntry *)((char *)de + le16_to_cpu(de->de_len));
	de->ino = cpu_to_le64(PMFS_ROOT_INO);
	de->de_len = cpu_to_le16(sb->s_blocksize - PMFS_DIR_REC_LEN(1));
	de->name_len = 2;
	strcpy(de->name, "..");
	pmfs_memlock_range(sb, de, sb->s_blocksize);
	pmfs_flush_buffer(de, PMFS_DIR_REC_LEN(2), false);
	PERSISTENT_MARK();
	PERSISTENT_BARRIER();
	return root_i;
}

static inline void set_default_opts(struct pmfs_sb_info *sbi)
{
	/* set_opt(sbi->s_mount_opt, PROTECT); */
	set_opt(sbi->s_mount_opt, HUGEIOREMAP);
	set_opt(sbi->s_mount_opt, ERRORS_CONT);
	sbi->pmfs_backing_file[0] = '\0';
	sbi->pmfs_backing_option = 0;
	sbi->jsize = PMFS_DEFAULT_JOURNAL_SIZE;
}

static void pmfs_root_check(struct super_block *sb, struct pmfs_inode *root_pi)
{
/*
 *      if (root_pi->i_d.d_next) {
 *              pmfs_warn("root->next not NULL, trying to fix\n");
 *              goto fail1;
 *      }
 */
	if (!S_ISDIR(le16_to_cpu(root_pi->i_mode)))
		pmfs_warn("root is not a directory!\n");
#if 0
	if (pmfs_calc_checksum((u8 *)root_pi, PMFS_INODE_SIZE)) {
		pmfs_dbg("checksum error in root inode, trying to fix\n");
		goto fail3;
	}
#endif
}

int pmfs_check_integrity(struct super_block *sb,
			  struct pmfs_super_block *super)
{
	struct pmfs_super_block *super_redund;

	super_redund =
		(struct pmfs_super_block *)((char *)super + PMFS_SB_SIZE);

	/* Do sanity checks on the superblock */
	if (le16_to_cpu(super->s_magic) != PMFS_SUPER_MAGIC) {
		if (le16_to_cpu(super_redund->s_magic) != PMFS_SUPER_MAGIC) {
			printk(KERN_ERR "Can't find a valid pmfs partition\n");
			goto out;
		} else {
			pmfs_warn
				("Error in super block: try to repair it with "
				"the redundant copy");
			/* Try to auto-recover the super block */
			if (sb)
				pmfs_memunlock_super(sb, super);
			memcpy(super, super_redund,
				sizeof(struct pmfs_super_block));
			if (sb)
				pmfs_memlock_super(sb, super);
			pmfs_flush_buffer(super, sizeof(*super), false);
			pmfs_flush_buffer((char *)super + PMFS_SB_SIZE,
				sizeof(*super), false);

		}
	}

	/* Read the superblock */
	if (pmfs_calc_checksum((u8 *)super, PMFS_SB_STATIC_SIZE(super))) {
		if (pmfs_calc_checksum((u8 *)super_redund,
					PMFS_SB_STATIC_SIZE(super_redund))) {
			printk(KERN_ERR "checksum error in super block\n");
			goto out;
		} else {
			pmfs_warn
				("Error in super block: try to repair it with "
				"the redundant copy");
			/* Try to auto-recover the super block */
			if (sb)
				pmfs_memunlock_super(sb, super);
			memcpy(super, super_redund,
				sizeof(struct pmfs_super_block));
			if (sb)
				pmfs_memlock_super(sb, super);
			pmfs_flush_buffer(super, sizeof(*super), false);
			pmfs_flush_buffer((char *)super + PMFS_SB_SIZE,
				sizeof(*super), false);
		}
	}

	return 1;
out:
	return 0;
}

static void pmfs_recover_truncate_list(struct super_block *sb)
{
	struct pmfs_inode_truncate_item *head = pmfs_get_truncate_list_head(sb);
	u64 ino_next = le64_to_cpu(head->i_next_truncate);
	struct pmfs_inode *pi;
	struct pmfs_inode_truncate_item *li;
	struct inode *inode;

	if (ino_next == 0)
		return;

	while (ino_next != 0) {
		pi = pmfs_get_inode(sb, ino_next);
		li = (struct pmfs_inode_truncate_item *)(pi + 1);
		inode = pmfs_iget(sb, ino_next);
		if (IS_ERR(inode))
			break;
		pmfs_dbg("Recover ino %llx nlink %d sz %llx:%llx\n", ino_next,
			inode->i_nlink, pi->i_size, li->i_truncatesize);
		if (inode->i_nlink) {
			/* set allocation hint */
			pmfs_set_blocksize_hint(sb, pi, 
					le64_to_cpu(li->i_truncatesize));
			pmfs_setsize(inode, le64_to_cpu(li->i_truncatesize));
			pmfs_update_isize(inode, pi);
		} else {
			/* free the inode */
			pmfs_dbg("deleting unreferenced inode %lx\n",
				inode->i_ino);
		}
		iput(inode);
		pmfs_flush_buffer(pi, CACHELINE_SIZE, false);
		ino_next = le64_to_cpu(li->i_next_truncate);
	}
	PERSISTENT_MARK();
	PERSISTENT_BARRIER();
	/* reset the truncate_list */
	pmfs_memunlock_range(sb, head, sizeof(*head));
	head->i_next_truncate = 0;
	pmfs_memlock_range(sb, head, sizeof(*head));
	pmfs_flush_buffer(head, sizeof(*head), false);
	PERSISTENT_MARK();
	PERSISTENT_BARRIER();
}

static int pmfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct pmfs_super_block *super;
	struct pmfs_inode *root_pi;
	struct pmfs_sb_info *sbi = NULL;
	struct inode *root_i = NULL;
	unsigned long blocksize, initsize = 0;
	u32 random = 0;
	int retval = -EINVAL;

	BUILD_BUG_ON(sizeof(struct pmfs_super_block) > PMFS_SB_SIZE);
	BUILD_BUG_ON(sizeof(struct pmfs_inode) > PMFS_INODE_SIZE);

	sbi = kzalloc(sizeof(struct pmfs_sb_info), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	sb->s_fs_info = sbi;

	set_default_opts(sbi);

	sbi->phys_addr = get_phys_addr(&data);
	if (sbi->phys_addr == (phys_addr_t)ULLONG_MAX)
		goto out;

	get_random_bytes(&random, sizeof(u32));
	atomic_set(&sbi->next_generation, random);

	/* Init with default values */
	INIT_LIST_HEAD(&sbi->block_inuse_head);
	sbi->mode = (S_IRUGO | S_IXUGO | S_IWUSR);
	sbi->uid = current_fsuid();
	sbi->gid = current_fsgid();
	set_opt(sbi->s_mount_opt, XIP);
	clear_opt(sbi->s_mount_opt, PROTECT);
	set_opt(sbi->s_mount_opt, HUGEIOREMAP);

	INIT_LIST_HEAD(&sbi->s_truncate);
	mutex_init(&sbi->s_truncate_lock);
	mutex_init(&sbi->inode_table_mutex);
	mutex_init(&sbi->s_lock);

	if (pmfs_parse_options(data, sbi, 0))
		goto out;

	set_opt(sbi->s_mount_opt, MOUNTING);
	initsize = sbi->initsize;

	/* Init a new pmfs instance */
	if (initsize) {
		root_pi = pmfs_init(sb, initsize);

		if (IS_ERR(root_pi))
			goto out;

		super = pmfs_get_super(sb);

		goto setup_sb;
	} else {
		pmfs_load_from_file(sb);
	}
	pmfs_dbg_verbose("checking physical address 0x%016llx for pmfs image\n",
		  (u64)sbi->phys_addr);

	/* Map only one page for now. Will remap it when fs size is known. */
	initsize = PAGE_SIZE;
	sbi->virt_addr = pmfs_ioremap(sb, sbi->phys_addr, initsize);
	if (!sbi->virt_addr) {
		printk(KERN_ERR "ioremap of the pmfs image failed(2)\n");
		goto out;
	}

	super = pmfs_get_super(sb);

	initsize = le64_to_cpu(super->s_size);
	sbi->initsize = initsize;
	pmfs_dbg_verbose("pmfs image appears to be %lu KB in size\n",
		   initsize >> 10);

	pmfs_iounmap(sbi->virt_addr, PAGE_SIZE, pmfs_is_wprotected(sb));

	/* Remap the whole filesystem now */
	release_mem_region(sbi->phys_addr, PAGE_SIZE);
	/* FIXME: Remap the whole filesystem in pmfs virtual address range. */
	sbi->virt_addr = pmfs_ioremap(sb, sbi->phys_addr, initsize);
	if (!sbi->virt_addr) {
		printk(KERN_ERR "ioremap of the pmfs image failed(3)\n");
		goto out;
	}

	super = pmfs_get_super(sb);

	if (pmfs_journal_soft_init(sb)) {
		retval = -EINVAL;
		printk(KERN_ERR "Journal initialization failed\n");
		goto out;
	}
	if (pmfs_recover_journal(sb)) {
		retval = -EINVAL;
		printk(KERN_ERR "Journal recovery failed\n");
		goto out;
	}

	if (pmfs_check_integrity(sb, super) == 0) {
		pmfs_dbg("Memory contains invalid pmfs %x:%x\n",
				le16_to_cpu(super->s_magic), PMFS_SUPER_MAGIC);
		goto out;
	}

	blocksize = le32_to_cpu(super->s_blocksize);
	pmfs_set_blocksize(sb, blocksize);

	pmfs_dbg_verbose("blocksize %lu\n", blocksize);

	/* Read the root inode */
	root_pi = pmfs_get_inode(sb, PMFS_ROOT_INO);

	/* Check that the root inode is in a sane state */
	pmfs_root_check(sb, root_pi);

#ifdef CONFIG_PMFS_TEST
	if (!first_pmfs_super)
		first_pmfs_super = sbi->virt_addr;
#endif

	/* Set it all up.. */
setup_sb:
	sb->s_magic = le16_to_cpu(super->s_magic);
	sb->s_op = &pmfs_sops;
	sb->s_maxbytes = pmfs_max_size(sb->s_blocksize_bits);
	sb->s_time_gran = 1;
	sb->s_export_op = &pmfs_export_ops;
	sb->s_xattr = NULL;
	sb->s_flags |= MS_NOSEC;
	root_i = pmfs_iget(sb, PMFS_ROOT_INO);
	if (IS_ERR(root_i)) {
		retval = PTR_ERR(root_i);
		goto out;
	}

	sb->s_root = d_make_root(root_i);
	if (!sb->s_root) {
		printk(KERN_ERR "get pmfs root inode failed\n");
		retval = -ENOMEM;
		goto out;
	}

	pmfs_recover_truncate_list(sb);
	/* If the FS was not formatted on this mount, scan the meta-data after
	 * truncate list has been processed */
	if ((sbi->s_mount_opt & PMFS_MOUNT_FORMAT) == 0)
		pmfs_setup_blocknode_map(sb);

	if (!(sb->s_flags & MS_RDONLY)) {
		u64 mnt_write_time;
		/* update mount time and write time atomically. */
		mnt_write_time = (get_seconds() & 0xFFFFFFFF);
		mnt_write_time = mnt_write_time | (mnt_write_time << 32);

		pmfs_memunlock_range(sb, &super->s_mtime, 8);
		pmfs_memcpy_atomic(&super->s_mtime, &mnt_write_time, 8);
		pmfs_memlock_range(sb, &super->s_mtime, 8);

		pmfs_flush_buffer(&super->s_mtime, 8, false);
		PERSISTENT_MARK();
		PERSISTENT_BARRIER();
	}

	clear_opt(sbi->s_mount_opt, MOUNTING);
	retval = 0;
	return retval;
out:
	if (sbi->virt_addr) {
		pmfs_iounmap(sbi->virt_addr, initsize, pmfs_is_wprotected(sb));
		release_mem_region(sbi->phys_addr, initsize);
	}

	kfree(sbi);
	return retval;
}

int pmfs_statfs(struct dentry *d, struct kstatfs *buf)
{
	struct super_block *sb = d->d_sb;
	unsigned long count = 0;
	struct pmfs_sb_info *sbi = (struct pmfs_sb_info *)sb->s_fs_info;

	buf->f_type = PMFS_SUPER_MAGIC;
	buf->f_bsize = sb->s_blocksize;

	count = sbi->block_end;
	buf->f_blocks = sbi->block_end;
	buf->f_bfree = buf->f_bavail = pmfs_count_free_blocks(sb);
	buf->f_files = (sbi->s_inodes_count);
	buf->f_ffree = (sbi->s_free_inodes_count);
	buf->f_namelen = PMFS_NAME_LEN;
	pmfs_dbg_verbose("pmfs_stats: total 4k free blocks 0x%llx\n",
		buf->f_bfree);
	pmfs_dbg_verbose("total inodes 0x%x, free inodes 0x%x, "
		"blocknodes 0x%lx\n", (sbi->s_inodes_count),
		(sbi->s_free_inodes_count), (sbi->num_blocknode_allocated));
	return 0;
}

static int pmfs_show_options(struct seq_file *seq, struct dentry *root)
{
	struct pmfs_sb_info *sbi = PMFS_SB(root->d_sb);

	seq_printf(seq, ",physaddr=0x%016llx", (u64)sbi->phys_addr);
	if (sbi->initsize)
		seq_printf(seq, ",init=%luk", sbi->initsize >> 10);
	if (sbi->blocksize)
		seq_printf(seq, ",bs=%lu", sbi->blocksize);
	if (sbi->bpi)
		seq_printf(seq, ",bpi=%lu", sbi->bpi);
	if (sbi->num_inodes)
		seq_printf(seq, ",N=%lu", sbi->num_inodes);
	if (sbi->mode != (S_IRWXUGO | S_ISVTX))
		seq_printf(seq, ",mode=%03o", sbi->mode);
	if (uid_valid(sbi->uid))
		seq_printf(seq, ",uid=%u", from_kuid(&init_user_ns, sbi->uid));
	if (gid_valid(sbi->gid))
		seq_printf(seq, ",gid=%u", from_kgid(&init_user_ns, sbi->gid));
	if (test_opt(root->d_sb, ERRORS_RO))
		seq_puts(seq, ",errors=remount-ro");
	if (test_opt(root->d_sb, ERRORS_PANIC))
		seq_puts(seq, ",errors=panic");
	/* memory protection disabled by default */
	if (test_opt(root->d_sb, PROTECT))
		seq_puts(seq, ",wprotect");
	if (test_opt(root->d_sb, HUGEMMAP))
		seq_puts(seq, ",hugemmap");
	if (test_opt(root->d_sb, HUGEIOREMAP))
		seq_puts(seq, ",hugeioremap");
	/* xip not enabled by default */
	if (test_opt(root->d_sb, XIP))
		seq_puts(seq, ",xip");

	return 0;
}

int pmfs_remount(struct super_block *sb, int *mntflags, char *data)
{
	unsigned long old_sb_flags;
	unsigned long old_mount_opt;
	struct pmfs_super_block *ps;
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	int ret = -EINVAL;

	/* Store the old options */
	mutex_lock(&sbi->s_lock);
	old_sb_flags = sb->s_flags;
	old_mount_opt = sbi->s_mount_opt;

	if (pmfs_parse_options(data, sbi, 1))
		goto restore_opt;

	sb->s_flags = (sb->s_flags & ~MS_POSIXACL) |
		      ((sbi->s_mount_opt & PMFS_MOUNT_POSIX_ACL) ? MS_POSIXACL : 0);

	if ((*mntflags & MS_RDONLY) != (sb->s_flags & MS_RDONLY)) {
		u64 mnt_write_time;
		ps = pmfs_get_super(sb);
		/* update mount time and write time atomically. */
		mnt_write_time = (get_seconds() & 0xFFFFFFFF);
		mnt_write_time = mnt_write_time | (mnt_write_time << 32);

		pmfs_memunlock_range(sb, &ps->s_mtime, 8);
		pmfs_memcpy_atomic(&ps->s_mtime, &mnt_write_time, 8);
		pmfs_memlock_range(sb, &ps->s_mtime, 8);

		pmfs_flush_buffer(&ps->s_mtime, 8, false);
		PERSISTENT_MARK();
		PERSISTENT_BARRIER();
	}

	mutex_unlock(&sbi->s_lock);
	ret = 0;
	return ret;

restore_opt:
	sb->s_flags = old_sb_flags;
	sbi->s_mount_opt = old_mount_opt;
	mutex_unlock(&sbi->s_lock);
	return ret;
}

static void pmfs_put_super(struct super_block *sb)
{
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	struct pmfs_super_block *ps = pmfs_get_super(sb);
	u64 size = le64_to_cpu(ps->s_size);
	struct pmfs_blocknode *i;
	struct list_head *head = &(sbi->block_inuse_head);

#ifdef CONFIG_PMFS_TEST
	if (first_pmfs_super == sbi->virt_addr)
		first_pmfs_super = NULL;
#endif

	/* It's unmount time, so unmap the pmfs memory */
	if (sbi->virt_addr) {
		pmfs_save_blocknode_mappings(sb);
		pmfs_journal_uninit(sb);
		pmfs_store_to_file(sb);
		pmfs_iounmap(sbi->virt_addr, size, pmfs_is_wprotected(sb));
		sbi->virt_addr = NULL;
		release_mem_region(sbi->phys_addr, size);
	}

	/* Free all the pmfs_blocknodes */
	while (!list_empty(head)) {
		i = list_first_entry(head, struct pmfs_blocknode, link);
		list_del(&i->link);
		pmfs_free_blocknode(sb, i);
	}
	sb->s_fs_info = NULL;
	pmfs_dbgmask = 0;
	kfree(sbi);
}

inline void pmfs_free_transaction(pmfs_transaction_t *trans)
{
	kmem_cache_free(pmfs_transaction_cachep, trans);
}

void __pmfs_free_blocknode(struct pmfs_blocknode *bnode)
{
	kmem_cache_free(pmfs_blocknode_cachep, bnode);
}

void pmfs_free_blocknode(struct super_block *sb, struct pmfs_blocknode *bnode)
{
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	sbi->num_blocknode_allocated--;
	__pmfs_free_blocknode(bnode);
}

inline pmfs_transaction_t *pmfs_alloc_transaction(void)
{
	return (pmfs_transaction_t *)
		kmem_cache_alloc(pmfs_transaction_cachep, GFP_NOFS);
}

struct pmfs_blocknode *pmfs_alloc_blocknode(struct super_block *sb)
{
	struct pmfs_blocknode *p;
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	p = (struct pmfs_blocknode *)
		kmem_cache_alloc(pmfs_blocknode_cachep, GFP_NOFS);
	if (p) {
		sbi->num_blocknode_allocated++;
	}
	return p;
}

static struct inode *pmfs_alloc_inode(struct super_block *sb)
{
	struct pmfs_inode_vfs *vi = (struct pmfs_inode_vfs *)
				     kmem_cache_alloc(pmfs_inode_cachep, GFP_NOFS);

	if (!vi)
		return NULL;
	vi->vfs_inode.i_version = 1;
	return &vi->vfs_inode;
}

static void pmfs_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);

	kmem_cache_free(pmfs_inode_cachep, PMFS_I(inode));
}

static void pmfs_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, pmfs_i_callback);
}

static void init_once(void *foo)
{
	struct pmfs_inode_vfs *vi = (struct pmfs_inode_vfs *)foo;

	vi->i_dir_start_lookup = 0;
	INIT_LIST_HEAD(&vi->i_truncated);
	inode_init_once(&vi->vfs_inode);
}


static int __init init_blocknode_cache(void)
{
	pmfs_blocknode_cachep = kmem_cache_create("pmfs_blocknode_cache",
					sizeof(struct pmfs_blocknode),
					0, (SLAB_RECLAIM_ACCOUNT |
                                        SLAB_MEM_SPREAD), NULL);
	if (pmfs_blocknode_cachep == NULL)
		return -ENOMEM;
	return 0;
}


static int __init init_inodecache(void)
{
	pmfs_inode_cachep = kmem_cache_create("pmfs_inode_cache",
					       sizeof(struct pmfs_inode_vfs),
					       0, (SLAB_RECLAIM_ACCOUNT |
						   SLAB_MEM_SPREAD), init_once);
	if (pmfs_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static int __init init_transaction_cache(void)
{
	pmfs_transaction_cachep = kmem_cache_create("pmfs_journal_transaction",
			sizeof(pmfs_transaction_t), 0, (SLAB_RECLAIM_ACCOUNT |
			SLAB_MEM_SPREAD), NULL);
	if (pmfs_transaction_cachep == NULL) {
		pmfs_dbg("PMFS: failed to init transaction cache\n");
		return -ENOMEM;
	}
	return 0;
}

static void destroy_transaction_cache(void)
{
	if (pmfs_transaction_cachep)
		kmem_cache_destroy(pmfs_transaction_cachep);
	pmfs_transaction_cachep = NULL;
}

static void destroy_inodecache(void)
{
	kmem_cache_destroy(pmfs_inode_cachep);
}

static void destroy_blocknode_cache(void)
{
	kmem_cache_destroy(pmfs_blocknode_cachep);
}

/*
 * the super block writes are all done "on the fly", so the
 * super block is never in a "dirty" state, so there's no need
 * for write_super.
 */
static struct super_operations pmfs_sops = {
	.alloc_inode	= pmfs_alloc_inode,
	.destroy_inode	= pmfs_destroy_inode,
	.write_inode	= pmfs_write_inode,
	.dirty_inode	= pmfs_dirty_inode,
	.evict_inode	= pmfs_evict_inode,
	.put_super	= pmfs_put_super,
	.statfs		= pmfs_statfs,
	.remount_fs	= pmfs_remount,
	.show_options	= pmfs_show_options,
};

static struct dentry *pmfs_mount(struct file_system_type *fs_type,
				  int flags, const char *dev_name, void *data)
{
	return mount_nodev(fs_type, flags, data, pmfs_fill_super);
}

static struct file_system_type pmfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "pmfs",
	.mount		= pmfs_mount,
	.kill_sb	= kill_anon_super,
};

static struct inode *pmfs_nfs_get_inode(struct super_block *sb,
					 u64 ino, u32 generation)
{
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	struct inode *inode;

	if (ino < PMFS_ROOT_INO)
		return ERR_PTR(-ESTALE);

	if ((ino >> PMFS_INODE_BITS) > (sbi->s_inodes_count))
		return ERR_PTR(-ESTALE);

	inode = pmfs_iget(sb, ino);
	if (IS_ERR(inode))
		return ERR_CAST(inode);

	if (generation && inode->i_generation != generation) {
		/* we didn't find the right inode.. */
		iput(inode);
		return ERR_PTR(-ESTALE);
	}

	return inode;
}

static struct dentry *pmfs_fh_to_dentry(struct super_block *sb,
					 struct fid *fid, int fh_len,
					 int fh_type)
{
	return generic_fh_to_dentry(sb, fid, fh_len, fh_type,
				    pmfs_nfs_get_inode);
}

static struct dentry *pmfs_fh_to_parent(struct super_block *sb,
					 struct fid *fid, int fh_len,
					 int fh_type)
{
	return generic_fh_to_parent(sb, fid, fh_len, fh_type,
				    pmfs_nfs_get_inode);
}

static const struct export_operations pmfs_export_ops = {
	.fh_to_dentry	= pmfs_fh_to_dentry,
	.fh_to_parent	= pmfs_fh_to_parent,
	.get_parent	= pmfs_get_parent,
};

static int __init init_pmfs_fs(void)
{
	int rc = 0;

	rc = init_blocknode_cache();
	if (rc)
		return rc;

	rc = init_transaction_cache();
	if (rc)
		goto out1;

	rc = init_inodecache();
	if (rc)
		goto out2;

	rc = bdi_init(&pmfs_backing_dev_info);
	if (rc)
		goto out3;

	rc = register_filesystem(&pmfs_fs_type);
	if (rc)
		goto out4;

	return 0;

out4:
	bdi_destroy(&pmfs_backing_dev_info);
out3:
	destroy_inodecache();
out2:
	destroy_transaction_cache();
out1:
	destroy_blocknode_cache();
	return rc;
}

static void __exit exit_pmfs_fs(void)
{
	unregister_filesystem(&pmfs_fs_type);
	bdi_destroy(&pmfs_backing_dev_info);
	destroy_inodecache();
	destroy_blocknode_cache();
	destroy_transaction_cache();
}

MODULE_AUTHOR("Intel Corporation <linux-pmfs@intel.com>");
MODULE_DESCRIPTION("Persistent Memory File System");
MODULE_LICENSE("GPL");

module_init(init_pmfs_fs)
module_exit(exit_pmfs_fs)
