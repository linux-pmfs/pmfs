/*
 * BRIEF DESCRIPTION
 *
 * XIP operations.
 *
 * Copyright 2012-2013 Intel Corporation
 * Copyright 2009-2011 Marco Stornelli <marco.stornelli@gmail.com>
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <asm/cpufeature.h>
#include <asm/pgtable.h>
#include "pmfs.h"
#include "xip.h"

/*
 * Wrappers. We need to use the rcu read lock to avoid
 * concurrent truncate operation. No problem for write because we held
 * i_mutex.
 */
ssize_t pmfs_xip_file_read(struct file *filp, char __user *buf,
			    size_t len, loff_t *ppos)
{
	ssize_t res;

	rcu_read_lock();
	res = xip_file_read(filp, buf, len, ppos);
	rcu_read_unlock();
	return res;
}

static inline void pmfs_flush_edge_cachelines(loff_t pos, ssize_t len,
	void *start_addr)
{
	if (unlikely(pos & 0x7))
		pmfs_flush_buffer(start_addr, 1, false);
	if (unlikely(((pos + len) & 0x7) && ((pos & (CACHELINE_SIZE - 1)) !=
			((pos + len) & (CACHELINE_SIZE - 1)))))
		pmfs_flush_buffer(start_addr + len, 1, false);
}

static ssize_t
__pmfs_xip_file_write(struct address_space *mapping, const char __user *buf,
          size_t count, loff_t pos, loff_t *ppos)
{
	struct inode    *inode = mapping->host;
	struct super_block *sb = inode->i_sb;
	long        status = 0;
	size_t      bytes;
	ssize_t     written = 0;
	struct pmfs_inode *pi;

	pi = pmfs_get_inode(sb, inode->i_ino);
	do {
		unsigned long index;
		unsigned long offset;
		size_t copied;
		void *xmem;
		unsigned long xpfn;

		offset = (pos & (sb->s_blocksize - 1)); /* Within page */
		index = pos >> sb->s_blocksize_bits;
		bytes = sb->s_blocksize - offset;
		if (bytes > count)
			bytes = count;

		status = pmfs_get_xip_mem(mapping, index, 1, &xmem, &xpfn);
		if (status)
			break;
		pmfs_xip_mem_protect(sb, xmem + offset, bytes, 1);
		copied = bytes -
		__copy_from_user_inatomic_nocache(xmem + offset, buf, bytes);
		pmfs_xip_mem_protect(sb, xmem + offset, bytes, 0);

		/* if start or end dest address is not 8 byte aligned, 
	 	 * __copy_from_user_inatomic_nocache uses cacheable instructions
	 	 * (instead of movnti) to write. So flush those cachelines. */
		pmfs_flush_edge_cachelines(pos, copied, xmem + offset);

        	if (likely(copied > 0)) {
			status = copied;

			if (status >= 0) {
				written += status;
				count -= status;
				pos += status;
				buf += status;
			}
		}
		if (unlikely(copied != bytes))
			if (status >= 0)
				status = -EFAULT;
		if (status < 0)
			break;
	} while (count);
	*ppos = pos;
	/*
 	* No need to use i_size_read() here, the i_size
 	* cannot change under us because we hold i_mutex.
 	*/
	if (pos > inode->i_size) {
		i_size_write(inode, pos);
		pmfs_update_isize(inode, pi);
	}

	return written ? written : status;
}

/* optimized path for file write that doesn't require a transaction. In this
 * path we don't need to allocate any new data blocks. So the only meta-data
 * modified in path is inode's i_size, i_ctime, and i_mtime fields */
static ssize_t pmfs_file_write_fast(struct super_block *sb, struct inode *inode,
	struct pmfs_inode *pi, const char __user *buf, size_t count, loff_t pos,
	loff_t *ppos, u64 block)
{
	void *xmem = pmfs_get_block(sb, block);
	size_t copied, ret = 0, offset;

	offset = pos & (sb->s_blocksize - 1);

	pmfs_xip_mem_protect(sb, xmem + offset, count, 1);
	copied = count - __copy_from_user_inatomic_nocache(xmem
		+ offset, buf, count);
	pmfs_xip_mem_protect(sb, xmem + offset, count, 0);

	pmfs_flush_edge_cachelines(pos, copied, xmem + offset);

	if (likely(copied > 0)) {
		pos += copied;
		ret = copied;
	}
	if (unlikely(copied != count && copied == 0))
		ret = -EFAULT;
	*ppos = pos;
	inode->i_ctime = inode->i_mtime = CURRENT_TIME_SEC;
	if (pos > inode->i_size) {
		/* make sure written data is persistent before updating
	 	* time and size */
		PERSISTENT_MARK();
		i_size_write(inode, pos);
		PERSISTENT_BARRIER();
		pmfs_memunlock_inode(sb, pi);
		pmfs_update_time_and_size(inode, pi);
		pmfs_memlock_inode(sb, pi);
	} else {
		u64 c_m_time;
		/* update c_time and m_time atomically. We don't need to make the data
		 * persistent because the expectation is that the close() or an explicit
		 * fsync will do that. */
		c_m_time = (inode->i_ctime.tv_sec & 0xFFFFFFFF);
		c_m_time = c_m_time | (c_m_time << 32);
		pmfs_memunlock_inode(sb, pi);
		pmfs_memcpy_atomic(&pi->i_ctime, &c_m_time, 8);
		pmfs_memlock_inode(sb, pi);
	}
	pmfs_flush_buffer(pi, 1, false);
	return ret;
}

static inline void pmfs_clear_edge_blk (struct super_block *sb, struct
	pmfs_inode *pi, bool new_blk, unsigned long block)
{
	void *ptr;
	unsigned long blknr;

	if (new_blk) {
		blknr = block >> (pmfs_inode_blk_shift(pi) -
			sb->s_blocksize_bits);
		ptr = pmfs_get_block(sb, __pmfs_find_data_block(sb, pi, blknr));
		if (ptr != NULL) {
			pmfs_memunlock_range(sb, ptr,  pmfs_inode_blk_size(pi));
			memset_nt(ptr, 0, pmfs_inode_blk_size(pi));
			pmfs_memlock_range(sb, ptr,  pmfs_inode_blk_size(pi));
		}
	}
}

ssize_t pmfs_xip_file_write(struct file *filp, const char __user *buf,
          size_t len, loff_t *ppos)
{
	struct address_space *mapping = filp->f_mapping;
	struct inode    *inode = mapping->host;
	struct super_block *sb = inode->i_sb;
	pmfs_transaction_t *trans;
	struct pmfs_inode *pi;
	ssize_t     written = 0;
	loff_t pos;
	u64 block;
	bool new_sblk = false, new_eblk = false;
	size_t count, offset, ret;
	unsigned long start_blk, num_blocks, max_logentries;

	sb_start_write(inode->i_sb);
	mutex_lock(&inode->i_mutex);

	if (!access_ok(VERIFY_READ, buf, len)) {
		ret = -EFAULT;
		goto out;
	}
	pos = *ppos;
	count = len;

	/* We can write back this queue in page reclaim */
	current->backing_dev_info = mapping->backing_dev_info;

	ret = generic_write_checks(filp, &pos, &count, S_ISBLK(inode->i_mode));
	if (ret || count == 0)
		goto out_backing;

	pi = pmfs_get_inode(sb, inode->i_ino);

	offset = pos & (sb->s_blocksize - 1);
	num_blocks = ((count + offset - 1) >> sb->s_blocksize_bits) + 1;
	/* offset in the actual block size block */
	offset = pos & (pmfs_inode_blk_size(pi) - 1);
	start_blk = pos >> sb->s_blocksize_bits;

	if ((((count + offset - 1) >> pmfs_inode_blk_shift(pi)) == 0) &&
		(block = pmfs_find_data_block(inode, start_blk))) {
		ret = pmfs_file_write_fast(sb, inode, pi, buf, count, pos,
			ppos, block);
		goto out_backing;
	}
	max_logentries = num_blocks / MAX_PTRS_PER_LENTRY + 2;
	if (max_logentries > MAX_METABLOCK_LENTRIES)
		max_logentries = MAX_METABLOCK_LENTRIES;

	trans = pmfs_new_transaction(sb, MAX_INODE_LENTRIES + max_logentries);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		goto out_backing;
	}
	pmfs_add_logentry(sb, trans, pi, MAX_DATA_PER_LENTRY, LE_DATA);

	ret = file_remove_suid(filp);
	if (ret) {
		pmfs_abort_transaction(sb, trans);
		goto out_backing;
	}
	inode->i_ctime = inode->i_mtime = CURRENT_TIME_SEC;
	pmfs_update_time(inode, pi);

	/* We avoid zeroing the alloc'd range, which is going to be overwritten
	 * by this system call anyway */
	if (offset != 0) {
		if (pmfs_find_data_block(inode, start_blk) == 0)
		    new_sblk = true;
	}
	if (((count + offset - 1) >> pmfs_inode_blk_shift(pi)) != 0 &&
			((pos + count) & (pmfs_inode_blk_size(pi) - 1)) != 0) {
		if (pmfs_find_data_block(inode, start_blk + num_blocks - 1)
			== 0)
		    new_eblk = true;
	}

	/* don't zero-out the allocated blocks */
	pmfs_alloc_blocks(trans, inode, start_blk, num_blocks, false);

	/* now zero out the edge blocks which will be partially written */
	pmfs_clear_edge_blk(sb, pi, new_sblk, start_blk);
	pmfs_clear_edge_blk(sb, pi, new_eblk, start_blk + num_blocks - 1);

	written = __pmfs_xip_file_write(mapping, buf, count, pos, ppos);
	if (written < 0 || written != count)
		pmfs_dbg_verbose("write incomplete/failed: written %ld len %ld"
			" pos %llx start_blk %lx num_blocks %lx\n",
			written, count, pos, start_blk, num_blocks);

	pmfs_commit_transaction(sb, trans);
	ret = written;
out_backing:
	current->backing_dev_info = NULL;
out:
	mutex_unlock(&inode->i_mutex);
	sb_end_write(inode->i_sb);
	return ret;
}

/* OOM err return with xip file fault handlers doesn't mean anything.
 * It would just cause the OS to go an unnecessary killing spree !
 */
static int __pmfs_xip_file_fault(struct vm_area_struct *vma,
				  struct vm_fault *vmf)
{
	struct address_space *mapping = vma->vm_file->f_mapping;
	struct inode *inode = mapping->host;
	pgoff_t size;
	void *xip_mem;
	unsigned long xip_pfn;
	int err;

	size = (i_size_read(inode) + PAGE_CACHE_SIZE - 1) >> PAGE_CACHE_SHIFT;
	if (vmf->pgoff >= size) {
		pmfs_dbg("[%s:%d] pgoff >= size(SIGBUS). vm_start(0x%lx),"
			" vm_end(0x%lx), pgoff(0x%lx), VA(%lx)\n",
			__func__, __LINE__, vma->vm_start, vma->vm_end,
			vmf->pgoff, (unsigned long)vmf->virtual_address);
		return VM_FAULT_SIGBUS;
	}

	err = pmfs_get_xip_mem(mapping, vmf->pgoff, 1, &xip_mem, &xip_pfn);
	if (unlikely(err)) {
		pmfs_dbg("[%s:%d] get_xip_mem failed(OOM). vm_start(0x%lx),"
			" vm_end(0x%lx), pgoff(0x%lx), VA(%lx)\n",
			__func__, __LINE__, vma->vm_start, vma->vm_end,
			vmf->pgoff, (unsigned long)vmf->virtual_address);
		return VM_FAULT_SIGBUS;
	}

	pmfs_dbg_mmapv("[%s:%d] vm_start(0x%lx), vm_end(0x%lx), pgoff(0x%lx), "
			"BlockSz(0x%lx), VA(0x%lx)->PA(0x%lx)\n", __func__,
			__LINE__, vma->vm_start, vma->vm_end, vmf->pgoff,
			PAGE_SIZE, (unsigned long)vmf->virtual_address,
			(unsigned long)xip_pfn << PAGE_SHIFT);

	err = vm_insert_mixed(vma, (unsigned long)vmf->virtual_address, xip_pfn);

	if (err == -ENOMEM)
		return VM_FAULT_SIGBUS;
	/*
	 * err == -EBUSY is fine, we've raced against another thread
	 * that faulted-in the same page
	 */
	if (err != -EBUSY)
		BUG_ON(err);
	return VM_FAULT_NOPAGE;
}

extern int
pmfs_xip_file_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	int ret = 0;

	rcu_read_lock();
	ret = __pmfs_xip_file_fault(vma, vmf);
	rcu_read_unlock();
	return ret;
}

static int pmfs_find_and_alloc_blocks(struct inode *inode, sector_t iblock,
				       sector_t *data_block, int create)
{
	int err = -EIO;
	u64 block;
	pmfs_transaction_t *trans;
	struct pmfs_inode *pi;

	block = pmfs_find_data_block(inode, iblock);

	if (!block) {
		struct super_block *sb = inode->i_sb;
		if (!create) {
			err = -ENODATA;
			goto err;
		}

		pi = pmfs_get_inode(sb, inode->i_ino);
		trans = pmfs_current_transaction();
		if (trans) {
			err = pmfs_alloc_blocks(trans, inode, iblock, 1, true);
			if (err) {
				pmfs_dbg_verbose("[%s:%d] Alloc failed!\n",
					__func__, __LINE__);
				goto err;
			}
		} else {
			/* 1 lentry for inode, 1 lentry for inode's b-tree */
			trans = pmfs_new_transaction(sb, MAX_INODE_LENTRIES);
			if (IS_ERR(trans)) {
				err = PTR_ERR(trans);
				goto err;
			}

			rcu_read_unlock();
			mutex_lock(&inode->i_mutex);

			pmfs_add_logentry(sb, trans, pi, MAX_DATA_PER_LENTRY,
				LE_DATA);
			err = pmfs_alloc_blocks(trans, inode, iblock, 1, true);

			pmfs_commit_transaction(sb, trans);

			mutex_unlock(&inode->i_mutex);
			rcu_read_lock();
			if (err) {
				pmfs_dbg_verbose("[%s:%d] Alloc failed!\n",
					__func__, __LINE__);
				goto err;
			}
		}
		block = pmfs_find_data_block(inode, iblock);
		if (!block) {
			pmfs_dbg("[%s:%d] But alloc didn't fail!\n",
				  __func__, __LINE__);
			err = -ENODATA;
			goto err;
		}
	}
	pmfs_dbg_mmapvv("iblock 0x%lx allocated_block 0x%llx\n", iblock,
			 block);

	*data_block = block;
	err = 0;

err:
	return err;
}

static inline int __pmfs_get_block(struct inode *inode, pgoff_t pgoff,
				    int create, sector_t *result)
{
	int rc = 0;

	rc = pmfs_find_and_alloc_blocks(inode, (sector_t)pgoff, result,
					 create);
	return rc;
}

int pmfs_get_xip_mem(struct address_space *mapping, pgoff_t pgoff, int create,
		      void **kmem, unsigned long *pfn)
{
	int rc;
	sector_t block = 0;
	struct inode *inode = mapping->host;

	rc = __pmfs_get_block(inode, pgoff, create, &block);
	if (rc) {
		pmfs_dbg1("[%s:%d] rc(%d), sb->physaddr(0x%llx), block(0x%llx),"
			" pgoff(0x%lx), flag(0x%x), PFN(0x%lx)\n", __func__,
			__LINE__, rc, PMFS_SB(inode->i_sb)->phys_addr,
			block, pgoff, create, *pfn);
		return rc;
	}

	*kmem = pmfs_get_block(inode->i_sb, block);
	*pfn = pmfs_get_pfn(inode->i_sb, block);

	pmfs_dbg_mmapvv("[%s:%d] sb->physaddr(0x%llx), block(0x%lx),"
		" pgoff(0x%lx), flag(0x%x), PFN(0x%lx)\n", __func__, __LINE__,
		PMFS_SB(inode->i_sb)->phys_addr, block, pgoff, create, *pfn);
	return 0;
}

unsigned long pmfs_data_block_size(struct vm_area_struct *vma,
				    unsigned long addr, unsigned long pgoff)
{
	struct file *file = vma->vm_file;
	struct inode *inode = file->f_mapping->host;
	struct pmfs_inode *pi;
	unsigned long map_virt;

	if (addr < vma->vm_start || addr >= vma->vm_end)
		return -EFAULT;

	pi = pmfs_get_inode(inode->i_sb, inode->i_ino);

	map_virt = addr & PUD_MASK;

	if (!cpu_has_gbpages || pi->i_blk_type != PMFS_BLOCK_TYPE_1G ||
	    (vma->vm_start & ~PUD_MASK) ||
	    map_virt < vma->vm_start ||
	    (map_virt + PUD_SIZE) > vma->vm_end)
		goto use_2M_mappings;

	pmfs_dbg_mmapv("[%s:%d] Using 1G Mappings : "
			"vma_start(0x%lx), vma_end(0x%lx), file_pgoff(0x%lx), "
			"VA(0x%lx), MAP_VA(%lx)\n", __func__, __LINE__,
			vma->vm_start, vma->vm_end, pgoff, addr, map_virt);
	return PUD_SIZE;

use_2M_mappings:
	map_virt = addr & PMD_MASK;

	if (!cpu_has_pse || pi->i_blk_type != PMFS_BLOCK_TYPE_2M ||
	    (vma->vm_start & ~PMD_MASK) ||
	    map_virt < vma->vm_start ||
	    (map_virt + PMD_SIZE) > vma->vm_end)
		goto use_4K_mappings;

	pmfs_dbg_mmapv("[%s:%d] Using 2M Mappings : "
			"vma_start(0x%lx), vma_end(0x%lx), file_pgoff(0x%lx), "
			"VA(0x%lx), MAP_VA(%lx)\n", __func__, __LINE__,
			vma->vm_start, vma->vm_end, pgoff, addr, map_virt);

	return PMD_SIZE;

use_4K_mappings:
	pmfs_dbg_mmapvv("[%s:%d] 4K Mappings : "
			 "vma_start(0x%lx), vma_end(0x%lx), file_pgoff(0x%lx), "
			 "VA(0x%lx)\n", __func__, __LINE__,
			 vma->vm_start, vma->vm_end, pgoff, addr);

	return PAGE_SIZE;
}

static inline pte_t *pmfs_xip_hugetlb_pte_offset(struct mm_struct *mm,
						  unsigned long	addr,
						  unsigned long *sz)
{
	return pte_offset_pagesz(mm, addr, sz);
}

static inline pte_t *pmfs_pte_alloc(struct mm_struct *mm,
				     unsigned long addr, unsigned long sz)
{
	return pte_alloc_pagesz(mm, addr, sz);
}

static pte_t pmfs_make_huge_pte(struct vm_area_struct *vma,
				 unsigned long pfn, unsigned long sz,
				 int writable)
{
	pte_t entry;

	if (writable)
		entry = pte_mkwrite(pte_mkdirty(pfn_pte(pfn, vma->vm_page_prot)));
	else
		entry = pte_wrprotect(pfn_pte(pfn, vma->vm_page_prot));

	entry = pte_mkspecial(pte_mkyoung(entry));

	if (sz != PAGE_SIZE) {
		BUG_ON(sz != PMD_SIZE && sz != PUD_SIZE);
		entry = pte_mkhuge(entry);
	}

	return entry;
}

static int __pmfs_xip_file_hpage_fault(struct vm_area_struct *vma,
					struct vm_fault *vmf)
{
	int ret;
	pte_t *ptep, new_pte;
	unsigned long size, block_sz;
	struct mm_struct *mm = vma->vm_mm;
	struct inode *inode = vma->vm_file->f_mapping->host;
	unsigned long address = (unsigned long)vmf->virtual_address;

	static DEFINE_MUTEX(pmfs_instantiation_mutex);

	size = (i_size_read(inode) + PAGE_CACHE_SIZE - 1) >> PAGE_CACHE_SHIFT;

	if (vmf->pgoff >= size) {
		pmfs_dbg("[%s:%d] pgoff >= size(SIGBUS). vm_start(0x%lx),"
			" vm_end(0x%lx), pgoff(0x%lx), VA(%lx)\n",
			__func__, __LINE__, vma->vm_start, vma->vm_end,
			vmf->pgoff, (unsigned long)vmf->virtual_address);
		return VM_FAULT_SIGBUS;
	}

	block_sz = pmfs_data_block_size(vma, address, vmf->pgoff);
	address &= ~(block_sz - 1);
	BUG_ON(block_sz == PAGE_SIZE);
	pmfs_dbg_mmapvv("[%s:%d] BlockSz : %lx",
			 __func__, __LINE__, block_sz);

	ptep = pmfs_pte_alloc(mm, address, block_sz);
	if (!ptep) {
		pmfs_dbg("[%s:%d] pmfs_pte_alloc failed(OOM). vm_start(0x%lx),"
			" vm_end(0x%lx), pgoff(0x%lx), VA(%lx)\n",
			__func__, __LINE__, vma->vm_start, vma->vm_end,
			vmf->pgoff, (unsigned long)vmf->virtual_address);
		return VM_FAULT_SIGBUS;
	}

	/* Serialize hugepage allocation and instantiation, so that we don't
	 * get spurious allocation failures if two CPUs race to instantiate
	 * the same page in the page cache.
	 */
	mutex_lock(&pmfs_instantiation_mutex);
	if (pte_none(*ptep)) {
		void *xip_mem;
		unsigned long xip_pfn;
		if (pmfs_get_xip_mem(vma->vm_file->f_mapping, vmf->pgoff, 1,
				      &xip_mem, &xip_pfn) != 0) {
			pmfs_dbg("[%s:%d] get_xip_mem failed(OOM). vm_start(0x"
				"%lx), vm_end(0x%lx), pgoff(0x%lx), VA(%lx)\n",
				__func__, __LINE__, vma->vm_start,
				vma->vm_end, vmf->pgoff,
				(unsigned long)vmf->virtual_address);
			ret = VM_FAULT_SIGBUS;
			goto out_mutex;
		}

		/* VA has already been aligned. Align xip_pfn to block_sz. */
		xip_pfn <<= PAGE_SHIFT;
		xip_pfn &= ~(block_sz - 1);
		xip_pfn >>= PAGE_SHIFT;
		new_pte = pmfs_make_huge_pte(vma, xip_pfn, block_sz,
					      ((vma->vm_flags & VM_WRITE) &&
					       (vma->vm_flags & VM_SHARED)));
		/* FIXME: Is lock necessary ? */
		spin_lock(&mm->page_table_lock);
		set_pte_at(mm, address, ptep, new_pte);
		spin_unlock(&mm->page_table_lock);

		if (ptep_set_access_flags(vma, address, ptep, new_pte,
					  vmf->flags & FAULT_FLAG_WRITE))
			update_mmu_cache(vma, address, ptep);
	}
	ret = VM_FAULT_NOPAGE;

out_mutex:
	mutex_unlock(&pmfs_instantiation_mutex);
	return ret;
}

int pmfs_xip_file_hpage_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	int ret = 0;

	rcu_read_lock();
	ret = __pmfs_xip_file_hpage_fault(vma, vmf);
	rcu_read_unlock();
	return ret;
}

static const struct vm_operations_struct pmfs_xip_vm_ops = {
	.fault	= pmfs_xip_file_fault,
};

static const struct vm_operations_struct pmfs_xip_hpage_vm_ops = {
	.fault	= pmfs_xip_file_hpage_fault,
};

static inline int pmfs_has_huge_mmap(struct super_block *sb)
{
	struct pmfs_sb_info *sbi = (struct pmfs_sb_info *)sb->s_fs_info;

	return sbi->s_mount_opt & PMFS_MOUNT_HUGEMMAP;
}

int pmfs_xip_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long block_sz;

	BUG_ON(!file->f_mapping->a_ops->get_xip_mem);

	file_accessed(file);

	vma->vm_flags |= VM_MIXEDMAP;

	block_sz = pmfs_data_block_size(vma, vma->vm_start, 0);
	if (pmfs_has_huge_mmap(file->f_mapping->host->i_sb) &&
	    (vma->vm_flags & VM_SHARED) &&
	    (block_sz == PUD_SIZE || block_sz == PMD_SIZE)) {
		/* vma->vm_flags |= (VM_XIP_HUGETLB | VM_SHARED | VM_DONTCOPY); */
		vma->vm_flags |= VM_XIP_HUGETLB;
		vma->vm_ops = &pmfs_xip_hpage_vm_ops;
		pmfs_dbg_mmaphuge("[%s:%d] MMAP HUGEPAGE vm_start(0x%lx),"
			" vm_end(0x%lx), vm_flags(0x%lx), "
			"vm_page_prot(0x%lx)\n", __func__,
			__LINE__, vma->vm_start, vma->vm_end, vma->vm_flags,
			pgprot_val(vma->vm_page_prot));
	} else {
		vma->vm_ops = &pmfs_xip_vm_ops;
		pmfs_dbg_mmap4k("[%s:%d] MMAP 4KPAGE vm_start(0x%lx),"
			" vm_end(0x%lx), vm_flags(0x%lx), "
			"vm_page_prot(0x%lx)\n", __func__,
			__LINE__, vma->vm_start, vma->vm_end,
			vma->vm_flags, pgprot_val(vma->vm_page_prot));
	}

	return 0;
}
