/*
 * Intel Persistent Memory Block Driver
 * Copyright (c) <2011-2013>, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* 
 * Intel Persistent Memory Block Driver (v0.9)
 *
 * pmbd.h
 *
 * Intel Corporation <linux-pmbd@intel.com>
 * 03/24/2011
 */ 

#ifndef PMBD_H
#define PMBD_H

#define PMBD_MAJOR 			261		/* FIXME: temporarily use this */
#define PMBD_NAME			"pmbd"		/* pmbd module name */
#define PMBD_MAX_NUM_DEVICES 		26		/* max num of devices */
#define PMBD_MAX_NUM_CPUS		32		/* max num of cpus*/

/*
 * type definitions  
 */ 
typedef uint32_t			PMBD_CHECKSUM_T;/* we use CRC32 to calculate checksum */
typedef sector_t			BBN_T;		/* BBN_T */
typedef sector_t			PBN_T;		/* BBN_T */


/*
 * PMBD device buffer control structure 
 * NOTE: 
 * (1) buffer_space is an array of num_blocks of blocks, the size of which is
 * defined as pmbd->pb_size
 * (2) bbi_space is an array of num_blocks of bbi (buffer block info) units,
 * each of which contains the metadata information of each block in the buffer

    buffer space management variables
 * num_dirty - total number of dirty blocks in buffer
 *  pos_dirty - point to the end of the sequence of dirty blocks 
 *  pos_clean - point to the end of the sequence of clean blocks
 * 
 * post_dirty and pos_clean logically segment the buffer into
 * dirty/clean regions as follows. 
 *  
 *   pos_dirty ----v       v--- pos_clean
 *       ----------------------------
 *       |  clean  |*DIRTY*| clean  |
 *       ----------------------------
 *  buffer_lock - protects reads/writes to the aforesaid three
 */
typedef struct pmbd_bbi {				/* pmbd buffer block info (BBI) */
	PBN_T				pbn;		/* physical block number in PM (converted from sector) */
	unsigned			dirty;		/* dirty (1) or clean (0)*/
} PMBD_BBI_T;

typedef struct pmbd_bsort_entry {			/* pmbd buffer block info for sorting */
	BBN_T				bbn;		/* buffer block number (in buffer)*/
	PBN_T				pbn;		/* physical block number (in PMBD)*/
} PMBD_BSORT_ENTRY_T;

typedef struct pmbd_buffer {
	unsigned			buffer_id;
	struct pmbd_device* 		pmbd;		/* the linked pmbd device */

	BBN_T				num_blocks;	/* buffer space size (# of blocks) */
	void* 				buffer_space;	/* buffer space base vaddr address */
	PMBD_BBI_T*			bbi_space;	/* array of buffer block info (BBI)*/

	BBN_T				num_dirty;	/* num of dirty blocks */
	BBN_T				pos_dirty;	/* the first dirty block */
	BBN_T				pos_clean;	/* the first clean block */
	spinlock_t			buffer_lock;	/* lock to protect metadata updates */
	unsigned int			batch_size;	/* the batch size for flushing buffer pages */

	struct task_struct*		syncer;		/* the syncer daemon */

	spinlock_t			flush_lock;	/* lock to protect metadata updates */
	PMBD_BSORT_ENTRY_T*		bbi_sort_buffer;/* a temp array of the bbi for sorting */
} PMBD_BUFFER_T;

/*
 * PM physical block information (each corresponding to a PM block)
 *
 * (1) if the physical block is buffered, bbn contains a valid buffer block
 * number (BBN) between 0 - (buffer->num_blocks-1), otherwise, it contains an
 * invalid value (buffer->num_blocks + 1)
 * (2) any access to the block (read/write/sync) must have this lock first to
 * prevent multiple concurrent accesses to the same PM block
 */
typedef struct pmbd_pbi{
	BBN_T				bbn;
	spinlock_t			lock;	
} PMBD_PBI_T;

typedef struct pmbd_stat{
	/* stat_lock does not protect cycles_*[] counters */
	spinlock_t			stat_lock;		/* protection lock */

	unsigned			last_access_jiffies;	/* the timestamp of the most recent access */
	uint64_t			num_sectors_read;	/* total num of sectors being read */
	uint64_t			num_sectors_write;	/* total num of sectors being written */
	uint64_t			num_requests_read;	/* total num of requests for read */
	uint64_t			num_requests_write;	/* total num of request for write */
	uint64_t			num_write_barrier;	/* total num of write barriers received */
	uint64_t			num_write_fua;		/* total num of write barriers received */
	
	/* cycles counters (enabled/disabled by timestat)*/
	uint64_t			cycles_total[2][PMBD_MAX_NUM_CPUS];	/* total cycles for read in make_request*/
	uint64_t			cycles_prepare[2][PMBD_MAX_NUM_CPUS];	/* total cycles for prepare in make_request*/
	uint64_t			cycles_wb[2][PMBD_MAX_NUM_CPUS];	/* total cycles for write barrier in make_request*/
	uint64_t			cycles_work[2][PMBD_MAX_NUM_CPUS];	/* total cycles for work in make_request*/
	uint64_t			cycles_endio[2][PMBD_MAX_NUM_CPUS];	/* total cycles for endio in make_request*/
	uint64_t			cycles_finish[2][PMBD_MAX_NUM_CPUS];	/* total cycles for finish-up in make_request*/

	uint64_t			cycles_pmap[2][PMBD_MAX_NUM_CPUS];	/* total cycles for private mapping*/
	uint64_t			cycles_punmap[2][PMBD_MAX_NUM_CPUS];	/* total cycles for private unmapping */
	uint64_t			cycles_memcpy[2][PMBD_MAX_NUM_CPUS];	/* total cycles for memcpy */
	uint64_t			cycles_clflush[2][PMBD_MAX_NUM_CPUS];	/* total cycles for clflush_range */
	uint64_t			cycles_clflushall[2][PMBD_MAX_NUM_CPUS];/* total cycles for clflush_all */
	uint64_t			cycles_wrverify[2][PMBD_MAX_NUM_CPUS];	/* total cycles for doing write verification */
	uint64_t			cycles_checksum[2][PMBD_MAX_NUM_CPUS];	/* total cycles for doing checksum */
	uint64_t			cycles_pause[2][PMBD_MAX_NUM_CPUS];	/* total cycles for pause */
	uint64_t			cycles_slowdown[2][PMBD_MAX_NUM_CPUS];	/* total cycles for slowdown*/
	uint64_t			cycles_setpages_ro[2][PMBD_MAX_NUM_CPUS]; /*total cycles for set pages to ro*/
	uint64_t			cycles_setpages_rw[2][PMBD_MAX_NUM_CPUS]; /*total cycles for set pages to rw*/
} PMBD_STAT_T;

/*
 * pmbd_device structure (each corresponding to a pmbd instance)
 */
#define PBN_TO_PMBD_BUFFER_ID(PMBD, PBN)	(((PBN)/(PMBD)->buffer_stride) % (PMBD)->num_buffers)
#define PBN_TO_PMBD_BUFFER(PMBD, PBN)	((PMBD)->buffers[PBN_TO_PMBD_BUFFER_ID((PMBD), (PBN))])

typedef struct pmbd_device {
	int				pmbd_id;		/* dev id */
	char				pmbd_name[DISK_NAME_LEN];/* device name */

	struct request_queue *		pmbd_queue;
	struct gendisk *		pmbd_disk;
	struct list_head		pmbd_list;

	/* PM backstore space */
	void*				mem_space;	/* pointer to the kernel mem space */
	uint64_t 			num_sectors;	/* PMBD device capacity (num of 512-byte sectors)*/
	unsigned 			sector_size;	/* 512 bytes */

	/* configurations */
	unsigned 			pmbd_type;	/* vmalloc() or high_mem */
	unsigned 			rammode;	/* RAM mode (no write protection) or not */
	unsigned			bufmode;	/* use buffer or not */
	unsigned			wpmode;		/* write protection mode: PTE change (0) or CR0/WP bit switch (1)*/

	/* buffer management */
	PMBD_BUFFER_T**			buffers;	/* buffer control structure */
	unsigned 			num_buffers;	/* number of buffers */
	unsigned			buffer_stride;	/* the number of contiguous blocks mapped to the same buffer */



	/* physical block info (metadata) */	
	PMBD_PBI_T*			pbi_space;	/* physical block info space (each) */
	unsigned			pb_size;	/* the unit size of each block (4096 in default) */

	/* checksum */
	PMBD_CHECKSUM_T*		checksum_space;		/* checksum array */
	unsigned 			checksum_unit_size;	/* checksum unit size (bytes) */
	void*				checksum_iomem_buf;	/* one unit buffer for ioremapped PM */

	/* emulating PM with injected latency */
	unsigned			simmode;	/* simulating whole device (0) or PM only (1)*/
	uint64_t			rdlat;		/* read access latency (in nanoseconds)*/
	uint64_t			wrlat;		/* write access latency (in nanoseconds)*/
	uint64_t			rdbw;		/* read bandwidth (MB/sec) */
	uint64_t			wrbw;		/* write bandwidth (MB/sec) */
	unsigned			rdsx;		/* read slowdown (X) */
	unsigned			wrsx;		/* write slowdown (X) */
	uint64_t			rdpause;	/* read pause (cycles per 4KB page) */
	uint64_t			wrpause;	/* write pause (cycles per 4KB page) */

	spinlock_t			batch_lock;		/* lock protecting batch_* fields */
	uint64_t			batch_start_cycle[2]; 	/* start time of the batch (cycles)*/
	uint64_t			batch_end_cycle[2];	/* end time of the batch (cycles) */
	uint64_t			batch_sectors[2];	/* the total num of sectors in the batch */ 

	PMBD_STAT_T*			pmbd_stat;	/* statistics data */
	struct proc_dir_entry* 		proc_devstat;	/* the proc output */

	spinlock_t			wr_barrier_lock;/* for write barrier and other control */
	atomic_t			num_flying_wr;	/* the counter of writes on the fly */

	spinlock_t			tmp_lock;
	uint64_t			tmp_data;
	unsigned long			tmp_num;
} PMBD_DEVICE_T;

/*
 * support definitions
 */
#define TRUE				1
#define FALSE				0

#define __CURRENT_PID__			(current->pid)
#define CONFIG_PMBD_DEBUG		1
//#define PRINTK_DEBUG_HDR		"DEBUG %s(%d)%u - "
//#define PRINTK_DEBUG_PAR		__FUNCTION__, __LINE__, __CURRENT_PID__
//#define PRINTK_DEBUG_1		if(CONFIG_PMBD_DEBUG >= 1) printk
//#define PRINTK_DEBUG_2		if(CONFIG_PMBD_DEBUG >= 2) printk
//#define PRINTK_DEBUG_3		if(CONFIG_PMBD_DEBUG >= 3) printk

#define MAX_OF(A, B)			(((A) > (B))? (A) : (B))
#define MIN_OF(A, B)			(((A) < (B))? (A) : (B))

#define SECTOR_SHIFT			9
#define PAGE_SHIFT			12
#define SECTOR_SIZE			(1UL << SECTOR_SHIFT)
//#define PAGE_SIZE			(1UL << PAGE_SHIFT)
#define SECTOR_MASK			(~(SECTOR_SIZE-1))
#define PAGE_MASK			(~(PAGE_SIZE-1))
#define PMBD_SECTOR_SIZE			SECTOR_SIZE
#define PMBD_PAGE_SIZE			PAGE_SIZE
#define KB_SHIFT			10
#define MB_SHIFT			20
#define GB_SHIFT			30
#define MB_TO_BYTES(N)			((N) << MB_SHIFT)
#define GB_TO_BYTES(N)			((N) << GB_SHIFT)
#define BYTES_TO_MB(N)			((N) >> MB_SHIFT)
#define BYTES_TO_GB(N)			((N) >> GB_SHIFT)
#define MB_TO_SECTORS(N)		((N) << (MB_SHIFT - SECTOR_SHIFT))
#define GB_TO_SECTORS(N)		((N) << (GB_SHIFT - SECTOR_SHIFT))
#define SECTORS_TO_MB(N)		((N) >> (MB_SHIFT - SECTOR_SHIFT))
#define SECTORS_TO_GB(N)		((N) >> (GB_SHIFT - SECTOR_SHIFT))
#define SECTOR_TO_PAGE(N)		((N) >> (PAGE_SHIFT - SECTOR_SHIFT))
#define SECTOR_TO_BYTE(N)		((N) << SECTOR_SHIFT)
#define BYTE_TO_SECTOR(N)		((N) >> SECTOR_SHIFT)
#define PAGE_TO_SECTOR(N)		((N) << (PAGE_SHIFT - SECTOR_SHIFT))
#define BYTE_TO_PAGE(N)			((N) >> (PAGE_SHIFT))

#define IS_SPACE(C) 			(isspace(C) || (C) == '\0')
#define IS_DIGIT(C) 			(isdigit(C) && (C) != '\0')
#define IS_ALPHA(C)			(isalpha(C) && (C) != '\0')

#define DISABLE_SAVE_IRQ(FLAGS)		{local_irq_save((FLAGS)); local_irq_disable();}
#define ENABLE_RESTORE_IRQ(FLAGS)	{local_irq_restore((FLAGS)); local_irq_enable();}
#define CUR_CPU_ID()			smp_processor_id()

/*
 * PMBD related config
 */ 

#define PMBD_CONFIG_VMALLOC  		0 /* vmalloc() based PMBD (default) */
#define PMBD_CONFIG_HIGHMEM  		1 /* ioremap() based PMBD */


/* global config */
#define PMBD_IS_MERGEABLE()		(g_pmbd_mergeable == TRUE)
#define PMBD_USE_VMALLOC()		(g_pmbd_type == PMBD_CONFIG_VMALLOC)
#define PMBD_USE_HIGHMEM()		(g_pmbd_type == PMBD_CONFIG_HIGHMEM)
#define PMBD_USE_CLFLUSH()		(g_pmbd_cpu_cache_clflush == TRUE)
#define PMBD_CPU_CACHE_FLAG()		((g_pmbd_cpu_cache_flag == _PAGE_CACHE_WB)? "WB" : \
					((g_pmbd_cpu_cache_flag == _PAGE_CACHE_WC)? "WC" : \
					((g_pmbd_cpu_cache_flag == _PAGE_CACHE_UC)? "UC" : \
					((g_pmbd_cpu_cache_flag == _PAGE_CACHE_UC_MINUS)? "UC-Minus" : "UNKNOWN"))))

#define PMBD_CPU_CACHE_USE_WB()		(g_pmbd_cpu_cache_flag == _PAGE_CACHE_WB)	/* write back */
#define PMBD_CPU_CACHE_USE_WC()		(g_pmbd_cpu_cache_flag == _PAGE_CACHE_WC)	/* write combining */
#define PMBD_CPU_CACHE_USE_UC()		(g_pmbd_cpu_cache_flag == _PAGE_CACHE_UC)	/* uncachable */
#define PMBD_CPU_CACHE_USE_UM()		(g_pmbd_cpu_cache_flag == _PAGE_CACHE_UC_MINUS)	/* uncachable minus */

#define PMBD_USE_WRITE_PROTECTION()	(g_pmbd_wr_protect == TRUE)
#define PMBD_USE_WRITE_VERIFICATION()	(g_pmbd_wr_verify == TRUE)
#define PMBD_USE_CHECKSUM()		(g_pmbd_checksum == TRUE)
#define PMBD_USE_LOCK()			(g_pmbd_lock == TRUE)
#define PMBD_USE_SUBPAGE_UPDATE()	(g_pmbd_subpage_update == TRUE)

#define PMBD_USE_PMAP()			(g_pmbd_pmap == TRUE && g_pmbd_type == PMBD_CONFIG_HIGHMEM)
#define PMBD_USE_NTS()			(g_pmbd_nts == TRUE)
#define PMBD_USE_NTL()			(g_pmbd_ntl == TRUE)
#define PMBD_USE_WB()			(g_pmbd_wb == TRUE)
#define PMBD_USE_FUA()			(g_pmbd_fua == TRUE)
#define PMBD_USE_TIMESTAT()		(g_pmbd_timestat == TRUE)

#define TIMESTAMP(TS)			rdtscll((TS))
#define TIMESTAT_POINT(TS)		{(TS) = 0; if (PMBD_USE_TIMESTAT()) rdtscll((TS));}

/* instanced based config */
#define PMBD_DEV_USE_VMALLOC(PMBD)	((PMBD)->pmbd_type == PMBD_CONFIG_VMALLOC)
#define PMBD_DEV_USE_HIGHMEM(PMBD)	((PMBD)->pmbd_type == PMBD_CONFIG_HIGHMEM)
#define PMBD_DEV_USE_BUFFER(PMBD)		((PMBD)->bufmode)
#define PMBD_DEV_USE_WPMODE_PTE(PMBD)	((PMBD)->wpmode == 0)
#define PMBD_DEV_USE_WPMODE_CR0(PMBD)	((PMBD)->wpmode == 1)

#define PMBD_DEV_USE_EMULATION(PMBD)	((PMBD)->rdlat || (PMBD)->wrlat || (PMBD)->rdbw || (PMBD)->wrbw)
#define PMBD_DEV_SIM_PMBD(PMBD)		(PMBD_DEV_USE_EMULATION((PMBD)) && (PMBD)->simmode == 1)
#define PMBD_DEV_SIM_DEV(PMBD)		(PMBD_DEV_USE_EMULATION((PMBD)) && (PMBD)->simmode == 0)
#define PMBD_DEV_USE_SLOWDOWN(PMBD)	((PMBD)->rdsx > 1 || (PMBD)->wrsx > 1)

/* support functions */
#define PMBD_MEM_TOTAL_SECTORS(PMBD)	((PMBD)->num_sectors)
#define PMBD_MEM_TOTAL_BYTES(PMBD)	((PMBD)->num_sectors * (PMBD)->sector_size)
#define PMBD_MEM_TOTAL_PAGES(PMBD)	(((PMBD)->num_sectors) >> (PAGE_SHIFT - SECTOR_SHIFT))
#define PMBD_MEM_SPACE_FIRST_BYTE(PMBD)	((PMBD)->mem_space)
#define PMBD_MEM_SPACE_LAST_BYTE(PMBD)	((PMBD)->mem_space + PMBD_MEM_TOTAL_BYTES(PMBD) - 1)
#define PMBD_CHECKSUM_TOTAL_NUM(PMBD) 	(PMBD_MEM_TOTAL_BYTES(PMBD) / (PMBD)->checksum_unit_size)
#define PMBD_LOCK_TOTAL_NUM(PMBD) 	(PMBD_MEM_TOTAL_BYTES(PMBD) / (PMBD)->lock_unit_size)
#define VADDR_IN_PMBD_SPACE(PMBD, ADDR)	((ADDR) >= PMBD_MEM_SPACE_FIRST_BYTE(PMBD) \
						&& (ADDR) <= PMBD_MEM_SPACE_LAST_BYTE(PMBD))

#define BYTE_TO_PBN(PMBD, BYTES)		((BYTES) / (PMBD)->pb_size)
#define PBN_TO_BYTE(PMBD, PBN)		((PBN) * (PMBD)->pb_size)
#define SECTOR_TO_PBN(PMBD, SECT)	(BYTE_TO_PBN((PMBD), SECTOR_TO_BYTE(SECT)))
#define PBN_TO_SECTOR(PMBD, PBN)		(BYTE_TO_SECTOR(PBN_TO_BYTE((PMBD), (PBN))))


#define PMBD_CACHELINE_SIZE			(64)	/* FIXME: configure this machine by machine? (check x86_clflush_size)*/

/* buffer related functions */
#define CALLER_ALLOCATOR			(0)
#define CALLER_SYNCER				(1)
#define CALLER_DESTROYER			(2)

#define PMBD_BLOCK_VADDR(PMBD, PBN)		((PMBD)->mem_space + ((PMBD)->pb_size * (PBN)))
#define PMBD_BLOCK_PBI(PMBD, PBN)			((PMBD)->pbi_space + (PBN))
#define PMBD_TOTAL_PB_NUM(PMBD) 			(PMBD_MEM_TOTAL_BYTES(PMBD) / (PMBD)->pb_size)
#define PMBD_BLOCK_IS_BUFFERED(PMBD, PBN)		(PMBD_BLOCK_PBI((PMBD),(PBN))->bbn < PBN_TO_PMBD_BUFFER((PMBD), (PBN))->num_blocks)
#define PMBD_SET_BLOCK_BUFFERED(PMBD, PBN, BBN)	(PMBD_BLOCK_PBI((PMBD),(PBN))->bbn = (BBN))
#define PMBD_SET_BLOCK_UNBUFFERED(PMBD, PBN)	(PMBD_BLOCK_PBI((PMBD),(PBN))->bbn = PMBD_TOTAL_PB_NUM((PMBD)) + 3)
//#define PMBD_SET_BLOCK_UNBUFFERED(PMBD, PBN)	(PMBD_BLOCK_PBI((PMBD),(PBN))->bbn = PBN_TO_PMBD_BUFFER((PMBD), (PBN))->num_blocks + 1)

#define PMBD_BUFFER_MIN_BUFSIZE			(4) 	/* buffer size (in MBs) */
#define PMBD_BUFFER_BLOCK(BUF, BBN)		((BUF)->buffer_space + (BUF)->pmbd->pb_size*(BBN))
#define PMBD_BUFFER_BBI(BUF, BBN)		((BUF)->bbi_space + (BBN))
#define PMBD_BUFFER_BBI_INDEX(BUF, ADDR)		((ADDR)-(BUF)->bbi_space)
#define PMBD_BUFFER_SET_BBI_CLEAN(BUF, BBN)	((PMBD_BUFFER_BBI((BUF), (BBN)))->dirty = FALSE)
#define PMBD_BUFFER_SET_BBI_DIRTY(BUF, BBN)	((PMBD_BUFFER_BBI((BUF), (BBN)))->dirty = TRUE)
#define PMBD_BUFFER_BBI_IS_CLEAN(BUF, BBN)	((PMBD_BUFFER_BBI((BUF), (BBN)))->dirty == FALSE)
#define PMBD_BUFFER_BBI_IS_DIRTY(BUF, BBN)	((PMBD_BUFFER_BBI((BUF), (BBN)))->dirty == TRUE)
#define PMBD_BUFFER_SET_BBI_BUFFERED(BUF,BBN,PBN)((PMBD_BUFFER_BBI((BUF), (BBN)))->pbn = (PBN))
#define PMBD_BUFFER_SET_BBI_UNBUFFERED(BUF, BBN)	((PMBD_BUFFER_BBI((BUF), (BBN)))->pbn = PMBD_TOTAL_PB_NUM((BUF)->pmbd) + 2)

#define PMBD_BUFFER_FLUSH_HW			(0.7)	/* high watermark */
#define PMBD_BUFFER_FLUSH_LW			(0.1)	/* low watermark */
#define PMBD_BUFFER_IS_FULL(BUF)			((BUF)->num_dirty >= (BUF)->num_blocks)
#define PMBD_BUFFER_IS_EMPTY(BUF)		((BUF)->num_dirty == 0)
#define PMBD_BUFFER_ABOVE_HW(BUF)		((BUF)->num_dirty >= (((BUF)->num_blocks * PMBD_BUFFER_FLUSH_HW)))
#define PMBD_BUFFER_BELOW_HW(BUF)		((BUF)->num_dirty < (((BUF)->num_blocks * PMBD_BUFFER_FLUSH_HW)))
#define PMBD_BUFFER_ABOVE_LW(BUF)		((BUF)->num_dirty >= (((BUF)->num_blocks * PMBD_BUFFER_FLUSH_LW)))
#define PMBD_BUFFER_BELOW_LW(BUF)		((BUF)->num_dirty < (((BUF)->num_blocks * PMBD_BUFFER_FLUSH_LW)))
#define PMBD_BUFFER_BATCH_SIZE_DEFAULT		(1024)	/* the batch size for each flush */

#define PMBD_BUFFER_NEXT_POS(BUF, POS)		(((POS)==((BUF)->num_blocks - 1))? 0 : ((POS)+1))
#define PMBD_BUFFER_PRIO_POS(BUF, POS)		(((POS)== 0)? ((BUF)->num_blocks - 1) : ((POS)-1))
#define PMBD_BUFFER_NEXT_N_POS(BUF,POS,N)	(((POS)+(N))%((BUF)->num_blocks))
#define PMBD_BUFFER_PRIO_N_POS(BUF,POS,N)	((BUF)->num_blocks - (((N)+(BUF)->num_blocks-(POS))%(BUF)->num_blocks))

/* high memory */
#define PMBD_HIGHMEM_AVAILABLE_SPACE 		(g_highmem_virt_addr + g_highmem_size - g_highmem_curr_addr)

/* emulation */
#define MAX_SYNC_SLOWDOWN			(10000000)	/* use async_slowdown, if larger than 10ms */
#define OVERHEAD_NANOSEC			(100)
#define PMBD_USLEEP(n) 				{set_current_state(TASK_INTERRUPTIBLE); \
		        				schedule_timeout((n)*HZ/1000000);}

/* statistics */
#define PMBD_BATCH_MAX_SECTORS   		(4096)		/* maximum data amount requested in a batch */
#define PMBD_BATCH_MIN_SECTORS   		(256)		/* maximum data amount requested in a batch */
#define PMBD_BATCH_MAX_INTERVAL 		(1000000)	/* maximum interval between two requests in a batch*/
#define PMBD_BATCH_MAX_DURATION  		(10000000)	/* maximum duration of a batch (ns)*/

/* write protection*/
#define VADDR_TO_PAGE(ADDR)			((ADDR) >> PAGE_SHIFT)
#define PAGE_TO_VADDR(PAGE)			((PAGE) << PAGE_SHIFT)

/* checksum */
#define VADDR_TO_CHECKSUM_IDX(PMBD, ADDR)	(((ADDR) - (PMBD)->mem_space) / (PMBD)->checksum_unit_size)
#define CHECKSUM_IDX_TO_VADDR(PMBD, IDX) 	((PMBD)->mem_space + (IDX) * (PMBD)->checksum_unit_size)
#define CHECKSUM_IDX_TO_CKADDR(PMBD, IDX)	((PMBD)->checksum_space + (IDX))

/* idle period timer */
#define PMBD_BUFFER_FLUSH_IDLE_TIMEOUT		(2000)		/* 1 millisecond */
#define PMBD_DEV_UPDATE_ACCESS_TIME(PMBD)		{spin_lock(&(PMBD)->pmbd_stat->stat_lock); \
						(PMBD)->pmbd_stat->last_access_jiffies = jiffies; \
						spin_unlock(&(PMBD)->pmbd_stat->stat_lock);}
#define PMBD_DEV_GET_ACCESS_TIME(PMBD, T)		{spin_lock(&(PMBD)->pmbd_stat->stat_lock); \
						(T) = (PMBD)->pmbd_stat->last_access_jiffies; \
						spin_unlock(&(PMBD)->pmbd_stat->stat_lock);}
#define PMBD_DEV_IS_IDLE(PMBD, IDLE)		((IDLE) > PMBD_BUFFER_FLUSH_IDLE_TIMEOUT)

/* Help info */
#define USAGE_INFO \
"\n\n\
============================================\n\
Intel Persistent Memory Block Driver (v0.9)\n\
============================================\n\n\
usage: $ modprobe pmbd mode=\"pmbd<#>;hmo<#>;hms<#>;[Option1];[Option2];[Option3];..\"\n\
\n\
GENERAL OPTIONS: \n\
\t pmbd<#,#..> \t set PM block device size (GBs) \n\
\t HM|VM \t\t use high memory (HM default) or vmalloc (VM) \n\
\t hmo<#> \t high memory starting offset (GB) \n\
\t hms<#> \t high memory size (GBs) \n\
\t pmap<Y|N> \t use private mapping (Y) or not (N default) - (note: must enable HM and wrprotN) \n\
\t nts<Y|N> \t use non-temporal store (MOVNTQ) and sfence to do memcpy (Y), or regular memcpy (N default)\n\
\t wb<Y|N> \t use write barrier (Y) or not (N default)\n\
\t fua<Y|N> \t use WRITE_FUA (Y default) or not (N) \n\
\t ntl<Y|N> \t use non-temporal load (MOVNTDQA) to do memcpy (Y), or regular memcpy (N default) - this option enforces memory type of write combining\n\
\n\
SIMULATION: \n\
\t simmode<#,#..>  use the specified numbers to the whole device (0 default) or PM only (1)\n\
\t rdlat<#,#..> \t set read access latency (ns) \n\
\t wrlat<#,#..> \t set write access latency (ns)\n\
\t rdbw<#,#..> \t set read bandwidth (MB/sec)  (if set 0, no emulation) \n\
\t wrbw<#,#..> \t set write bandwidth (MB/sec) (if set 0, no emulation) \n\
\t rdsx<#,#..> \t set the relative slowdown (x) for read \n\
\t wrsx<#,#..> \t set the relative slowdown (x) for write \n\
\t rdpause<#,.> \t set a pause (cycles per 4KB) for each read\n\
\t wrpause<#,.> \t set a pause (cycles per 4KB) for each write\n\
\t adj<#> \t set an adjustment to the system overhead (nanoseconds) \n\
\n\
WRITE PROTECTION: \n\
\t wrprot<Y|N> \t use write protection for PM pages? (Y or N)\n\
\t wpmode<#,#,..>  write protection mode: use the PTE change (0 default) or switch CR0/WP bit (1) \n\
\t clflush<Y|N> \t use clflush to flush CPU cache for each write to PM space? (Y or N) \n\
\t wrverify<Y|N> \t use write verification for PM pages? (Y or N) \n\
\t checksum<Y|N> \t use checksum to protect PM pages? (Y or N)\n\
\t bufsize<#,#,..> the buffer size (MBs) (0 - no buffer, at least 4MB)\n\
\t bufnum<#> \t the number of buffers for a PMBD device (16 buffers, at least 1 if using buffer, 0 -no buffer) \n\
\t bufstride<#> \t the number of contiguous blocks(4KB) mapped into one buffer (bucket size for round-robin mapping) (1024 in default)\n\
\t batch<#,#> \t the batch size (num of pages) for flushing PMBD device buffer (1 means no batching) \n\
\n\
MISC: \n\
\t mgb<Y|N> \t mergeable? (Y or N) \n\
\t lock<Y|N> \t lock the on-access page to serialize accesses? (Y or N) \n\
\t cache<WB|WC|UC> use which CPU cache policy? Write back (WB), Write Combined (WB), or Uncachable (UC)\n\
\t subupdate<Y|N>  only update the changed cachelines of a page? (Y or N) (check PMBD_CACHELINE_SIZE) \n\
\t timestat<Y|N>   enable the detailed timing statistics (/proc/pmbd/pmbdstat)? (Y or N) (This will cause significant performance slowdown) \n\
\n\
NOTE: \n\
\t (1) Option rdlat/wrlat only specifies the minimum access times. Real access times can be higher.\n\
\t (2) If rdsx/wrsx is specified, the rdlat/wrlat/rdbw/wrbw would be ignored. \n\
\t (3) Option simmode1 applies the simulated specification to the PM space, rather than the whole device, which may have buffer.\n\
\n\
WARNING: \n\
\t (1) When using simmode1 to simulate slow-speed PM space, soft lockup warning may appear. Use \"nosoftlockup\" boot option to disable it.\n\
\t (2) Enabling timestat may cause performance degradation.\n\
\t (3) FUA is supported in PMBD, but if buffer is used (for PT-based protection), enabling FUA lowers performance due to double writes.\n\
\t (4) No support for changing CPU cache related PTE attributes for VM-based PMBD (RCU stalls).\n\
\n\
PROC ENTRIES: \n\
\t /proc/pmbd/pmbdcfg     config info about the PMBD devices\n\
\t /proc/pmbd/pmbdstat    statistics of the PMBD devices (if timestat is enabled)\n\
\n\
EXAMPLE: \n\
\t Assuming a 16GB PM space with physical memory addresses from 8GB to 24GB:\n\
\t (1) Basic (Ramdisk): \n\
\t     $ sudo modprobe pmbd mode=\"pmbd16;hmo8;hms16;\"\n\n\
\t (2) Protected (with private mapping): \n\
\t     $ sudo modprobe pmbd mode=\"pmbd16;hmo8;hms16;pmapY;\"\n\n\
\t (3) Protected and synced (with private mapping, non-temp store): \n\
\t     $ sudo modprobe pmbd mode=\"pmbd16;hmo8;hms16;pmapY;ntsY;\"\n\n\
\t (4) *** RECOMMENDED CONFIG *** \n\
\t     Protected, synced, and ordered (with private mapping, non-temp store, write barrier): \n\
\t     $ sudo modprobe pmbd mode=\"pmbd16;hmo8;hms16;pmapY;ntsY;wbY;\"\n\
\n"

/* functions */
static inline void pmbd_set_pages_ro(PMBD_DEVICE_T* pmbd, void* addr, uint64_t bytes, unsigned on_access);
static inline void pmbd_set_pages_rw(PMBD_DEVICE_T* pmbd, void* addr, uint64_t bytes, unsigned on_access);
static inline void pmbd_clflush_range(PMBD_DEVICE_T* pmbd, void* dst, size_t bytes);
static inline int pmbd_verify_wr_pages(PMBD_DEVICE_T* pmbd, void* dst, void* src, size_t bytes);
static int pmbd_checksum_on_write(PMBD_DEVICE_T* pmbd, void* vaddr, size_t bytes);
static int pmbd_checksum_on_read(PMBD_DEVICE_T* pmbd, void* vaddr, size_t bytes);

static inline int put_ulong(unsigned long arg, unsigned long val)
{
	return put_user(val, (unsigned long __user *)arg);
}
static inline int put_u64(unsigned long arg, u64 val)
{
	return put_user(val, (u64 __user *)arg);
}

static inline void mfence(void)
{
	asm volatile("mfence": : :);
}

static inline void sfence(void)
{
	asm volatile("sfence": : :);
}

#endif
/* THEN END */
