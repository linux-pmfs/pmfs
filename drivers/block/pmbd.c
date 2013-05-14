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
 * Parts derived with changes from drivers/block/brd.c, lib/crc32.c, and
 * arch/x86/lib/mmx_32.c
 *
 * Intel Corporation <linux-pmbd@intel.com>
 * 03/24/2011
 *
 * Authors
 * 2013 - Released the open-source version 0.9 (fchen)
 * 2012 - Ported to Linux 3.2.1 (fchen)
 * 2011 - Feng Chen (Intel) implemented version 1 of PMBD for Linux 2.6.34.
 */


/*
 *******************************************************************************
 * Persistent Memory Block Device Driver
 *
 * USAGE:
 *  % sudo modprobe pmbd mode="pmbd<#>;hmo<#>;hms<#>;[OPTION1];[OPTION2];..>"
 *
 * GENERAL OPTIONS:
 *  - pmbd<#,..>:    a sequence of integer numbers setting PMBD device sizes (in
 *                   units of GBs). For example, mode="pmbd4,1" means creating a
 *                   4GB and a 1GB PMBD device (/dev/pma and /dev/pmb).
 *
 *  - HM|VM:         choose two types of PMBD devices
 *                   - VM:  vmalloc() based 
 *                   - HM:  HIGH_MEM based (default)
 *                   - In /boot/grub/grub.conf, add "mem=<n>G memmap=<m>G$<n>G" 
 *                    to reserve the high m GBs for PM, starting from offset n 
 *                    GBs in physical memory
 *
 *  - hmo<#>:        if HM is set, setting the starting physical mem address 
 *                   (in units of GBs).
 *
 *  - hms<#>:        if HM is set, setting the remapping memory size (in GBs)
 *
 *  - pmap<Y|N>      set private mapping (Y) or not (N default). using 
 *                   pmap_atomic_pfn() to dynamically map/unmap the 
 *                   to-be-accessed PM page for protection purpose. 
 *                   This option must work with HM enabled. In the Linux boot 
 *                   option, "mem" option must be removed.
 *
 *  - nts<Y|N>       set non-temporal store/sfence (Y) or not (N default). 
 *
 *  - wb<Y|N>:       use write barrier (Y) or not (N default)
 *
 *  - fua<Y|N>       use WRITE_FUA (Y default) or not (N)
 *  			 FUA with PT-based protection (with buffer) incurs
 *  			 double-write overhead
 *
 * SIMULATION OPTIONS:
 *
 *  - simmode<#,#..> set the simulation mode for each PMBD device
 *                   - 0 for simulating the whole device 
 *                   - 1 for simulating the PM space only
 *                   Note that simulating the PM space may cause some system 
 *                   warning of soft lockup. To disable it, add nonsoftlockup 
 *                   in the boot options.
 *
 *  - rdlat<#,#..>:  a sequence of integer numbers setting emulated read 
 *                   latencies (in units of nanoseconds) for reading each 
 *                   sector. Each number is corresponding to a device. Default
 *                   value is 0. 
 *
 *  - wrlat<#,#..>:  set emulated write access latencies (see rdlat)
 *
 *  - rdbw<#,#..>:   a sequence of integer numbers setting emulated read 
 *                   bandwidth (in units of MB/sec) for reading each sector. 
 *                   Each number corresponds to a device. Default value is 0;
 *
 *  - wrbw<#,#..>:   set emulated write bandwidth (see rdbw)
 *
 *  - rdsx<#,#..>:   set the slowdown ratio (x) for reads as compared to DRAM
 *
 *  - wrsx<#,#..>:   set the slowdown ratio (x) for writes as compared to DRAM
 *
 *  - rdpause<#,#..>: set the injected delay (cycles per page) for read (not
 *                   for emulation, just inject latencies 
 *                   for each read per page)
 *
 *  - wrpause<#,#..>: set the injected delay (cycles per page) for write
 *  		     (not for emulation, just inject latencies for
 *  		      each read per page).
 *
 *  - adj<#>:        offset the overhead with estimated system overhead. Default
 *  		     is 4us, however, this could vary system by system.
 *
 * WRITE PROTECTION:
 *
 *  - wrprot<Y|N>:   provide write protection on PM space by setting page
 *                   read-only (default: N).
 *                   This option is incompatible with pmap.
 *
 *  - wpmode<#,#,..> write protection mode: use the PTE change (0 default) or
 *                   switch CR0/WP bit (1)
 *
 *  - wrverify<Y|N>: read out the data for verification after writing into PM
 *                   space
 *
 *  - clflush<Y|N>:  flush CPU cache or not (default: N) 
 *
 *  - checksum<Y|N>: use checksum to provide further protection from data
 *                   corruption (default: N)
 *
 *  - lock<Y|N>:     lock the on-access PM page to serialize accesses
 *  			 (default: Y)
 *
 *  - bufsize<#,#,#.#...>  -- the buffer size in MBs (for speeding up write
 *                   protection) 0 means no buffer, minimum size is 16 MBs
 *
 *  - bufnum<#>      the number of buffers for a pmbd device (16 buffers, at
 *                   least 1 if using buffering, 0 will disable buffer mode)
 *
 *  - bufstride<#>   the number of contiguous blocks(4KB) mapped into one
 *                   buffer (the bucket size for round-robin mapping)
 *                   (1024 in default)
 *
 *  - batch<#,#>     the batch size (num of pages) for flushing PMBD buffer (1
 *                   means no batching)
 *
 * MISC OPTIONS:
 *
 *  - subupdate<Y|N> only update changed cachelines of a page (check
 *                   PMBD_CACHELINE_SIZE, default: N)
 *
 *  - mgb<Y|N>:      setting mergeable or not (default: Y)
 *
 *  - cache<WB|WC|UM|UC>:
 *  		     WB -- write back (both read/write cache used)
 *  		     WC -- write combined (write through but cachable)
 *  		     UM -- uncachable but write back 
 *  		     UC -- write through and uncachable
 *			 No support for changing CPU cache flags
 *			 with vmalloc() based PMBD
 *
 *  - timestat<Y|N> enable the detailed timing statistics (/proc/pmbd/pmbdstat) or
 *                  not (default: N). This will cause significant performance loss. 
 *
 * EXAMPLE:
 *  mode="pmbd2,1;rdlat100,2000;wrlat500,4000;rdbw100,100;wrbw100,100;HM;hmo4;hms3;
 *  mgbY;flushY;cacheWB;wrprotY;wrverifyY;checksumY;lockY;rammode0,1;bufsize16,0;
 *  subupdateY;"
 *
 *  Explanation: Create two PMBD devices, /dev/pma (2GB) and /dev/pmb (1GB).
 *  Insert 100ns and 500ns for reading and writing a sector to /dev/pma,
 *  respectively.  Insert 2000ns and 4000ns for reading and writing a sector
 *  to /dev/pmb.  Make the read/write bandwidth for both devices 100MB/sec.
 *  No system overhead adjustment is applied.  We use 3GB high memory for the
 *  PMBD devices, starting from 4GB physical memory address. Make it
 *  mergeable, use writeback and flush CPU cache for the PM space, use write
 *  protection for PM space by setting PM space read-only, verify each
 *  write by reading out written data, use checksum to protect PM space, use
 *  spinlock to protect from corruption caused by concurrent accesses, the
 *  first device is applied without write protection, the second device is
 *  applied with write protection, and use sub-page updates.
 *
 * NOTE:
 *  - We can create no more than 26 devices, 4 partitions each. 
 *
 * FIXME: 
 *  (1) We use an unoccupied major device num (261) temporarily
 *******************************************************************************
 */

#include <linux/init.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/major.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/time.h>
#include <asm/timer.h>
#include <linux/cpufreq.h>
#include <linux/crc32.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/kthread.h>
#include <linux/sort.h>
#include <linux/timex.h>
#include <linux/proc_fs.h>
#include <asm/tlbflush.h>
#include <asm/i387.h>
#include <asm/asm.h>
#include <linux/pmbd.h>
#include <linux/delay.h>

/* device configs  */
static int max_part = 4;	/* maximum num of partitions */
static int part_shift = 0;	/* partition shift */
static LIST_HEAD(pmbd_devices);	/* device list */
static DEFINE_MUTEX(pmbd_devices_mutex); /* device mutex */

/* /proc file system entry */
static struct proc_dir_entry* proc_pmbd = NULL;
static struct proc_dir_entry* proc_pmbdstat = NULL;
static struct proc_dir_entry* proc_pmbdcfg = NULL;

/* pmbd device default configuration */
static unsigned g_pmbd_type 		= PMBD_CONFIG_HIGHMEM;	/* vmalloc(PMBD_CONFIG_VMALLOC) or reserve highmem (PMBD_CONFIG_HIGHMEM default) */
static unsigned g_pmbd_pmap		= FALSE;		/* use pmap_atomic() to map/unmap space on demand  */
static unsigned g_pmbd_nts		= FALSE;		/* use non-temporal store (movntq) */
static unsigned g_pmbd_wb		= FALSE;		/* use write barrier */
static unsigned g_pmbd_fua		= TRUE;	/* use fua support */
static unsigned g_pmbd_mergeable 	= TRUE;			/* mergeable or not  */
static unsigned g_pmbd_cpu_cache_clflush= FALSE;	 	/* flush CPU cache or not*/
static unsigned g_pmbd_wr_protect	= FALSE;		/* flip PTE R/W bits for write protection */
static unsigned g_pmbd_wr_verify	= FALSE;		/* read out written data for verification */
static unsigned g_pmbd_checksum		= FALSE;		/* do checksum on PM data */
static unsigned g_pmbd_lock		= TRUE;			/* do spinlock on accessing a PM page */
static unsigned g_pmbd_subpage_update	= FALSE;		/* do subpage update (only write changed content) */
static unsigned g_pmbd_timestat		= FALSE;		/* do a detailed timestamp breakdown statistics */
static unsigned g_pmbd_ntl		= FALSE;		/* use non-temporal load (movntdqa)*/
static unsigned long g_pmbd_cpu_cache_flag = _PAGE_CACHE_WB;	/* CPU cache flag (default - write back) */

/* high memory configs */
static unsigned long 	g_highmem_size = 0; 			/* size of the reserved physical mem space (bytes) */
static phys_addr_t 	g_highmem_phys_addr = 0;		/* beginning of the reserved phy mem space (bytes)*/
static void* 		g_highmem_virt_addr = NULL;		/* beginning of the reserve HIGH_MEM space */
static void* 		g_highmem_curr_addr = NULL;		/* beginning of the available HIGH_MEM space for alloc*/ 

/* module parameters */
static unsigned g_pmbd_nr = 0;					/* num of PMBD devices */
static unsigned long long g_pmbd_size[PMBD_MAX_NUM_DEVICES];	/* PMBD device sizes in units of GBs */
static unsigned long long g_pmbd_rdlat[PMBD_MAX_NUM_DEVICES]; 	/* access latency for read (nanosecs) */
static unsigned long long g_pmbd_wrlat[PMBD_MAX_NUM_DEVICES]; 	/* access latency for write nanosecs) */
static unsigned long long g_pmbd_rdbw[PMBD_MAX_NUM_DEVICES]; 	/* bandwidth for read (MB/sec) */
static unsigned long long g_pmbd_wrbw[PMBD_MAX_NUM_DEVICES]; 	/* bandwidth for write (MB/sec)*/
static unsigned long long g_pmbd_rdsx[PMBD_MAX_NUM_DEVICES]; 	/* read slowdown (x) */
static unsigned long long g_pmbd_wrsx[PMBD_MAX_NUM_DEVICES]; 	/* write slowdown (x)*/
static unsigned long long g_pmbd_rdpause[PMBD_MAX_NUM_DEVICES];	/* read pause (cycles per page) */
static unsigned long long g_pmbd_wrpause[PMBD_MAX_NUM_DEVICES];	/* write pause (cycles per page)*/
static unsigned long long g_pmbd_simmode[PMBD_MAX_NUM_DEVICES];	/* simulating PM space (1) or the whole device (0 default) */
static unsigned long long g_pmbd_adjust_ns = 0;			/* nanosec of adjustment to offset system overhead */
static unsigned long long g_pmbd_rammode[PMBD_MAX_NUM_DEVICES];	/* do write optimization or not */
static unsigned long long g_pmbd_bufsize[PMBD_MAX_NUM_DEVICES];	/* the buffer size (in MBs) */
static unsigned long long g_pmbd_buffer_batch_size[PMBD_MAX_NUM_DEVICES]; /* the batch size (num of pages) for flushing PMBD buffer */
static unsigned long long g_pmbd_wpmode[PMBD_MAX_NUM_DEVICES];	/* write protection mode: PTE change (0 default) and CR0 Switch (1)*/

static unsigned long long g_pmbd_num_buffers = 0;		/* number of individual buffers */
static unsigned long long g_pmbd_buffer_stride = 1024;		/* number of contiguous PBNs belonging to the same buffer */

/* definition of functions */
static inline uint64_t cycle_to_ns(uint64_t cycle);
static inline void sync_slowdown_cycles(uint64_t cycles);
static uint64_t emul_start(PMBD_DEVICE_T* pmbd, int num_sectors, int rw);
static uint64_t emul_end(PMBD_DEVICE_T* pmbd, int num_sectors, int rw, uint64_t start);

/*
 * *************************************************************************
 * parse module parameters functions
 * *************************************************************************
 */
static char *mode = ""; 
module_param(mode, charp, 444);
MODULE_PARM_DESC(mode, USAGE_INFO);

/* print pmbd configuration info */
static void pmbd_print_conf(void)
{
	int i;
#ifndef CONFIG_X86
	printk(KERN_INFO "pmbd: running on a non-x86 platform, check ioremap()...\n");
#endif
	printk(KERN_INFO "pmbd: cacheline_size=%d\n", PMBD_CACHELINE_SIZE);
	printk(KERN_INFO "pmbd: PMBD_SECTOR_SIZE=%lu, PMBD_PAGE_SIZE=%lu\n", PMBD_SECTOR_SIZE, PMBD_PAGE_SIZE);
	printk(KERN_INFO "pmbd: g_pmbd_type = %s\n", PMBD_USE_VMALLOC()? "VMALLOC" : "HIGH_MEM");
	printk(KERN_INFO "pmbd: g_pmbd_mergeable = %s\n", PMBD_IS_MERGEABLE()? "YES" : "NO");
	printk(KERN_INFO "pmbd: g_pmbd_cpu_cache_clflush = %s\n", PMBD_USE_CLFLUSH()? "YES" : "NO");
	printk(KERN_INFO "pmbd: g_pmbd_cpu_cache_flag = %s\n", PMBD_CPU_CACHE_FLAG());
	printk(KERN_INFO "pmbd: g_pmbd_wr_protect = %s\n", PMBD_USE_WRITE_PROTECTION()? "YES" : "NO");
	printk(KERN_INFO "pmbd: g_pmbd_wr_verify = %s\n", PMBD_USE_WRITE_VERIFICATION()? "YES" : "NO");
	printk(KERN_INFO "pmbd: g_pmbd_checksum = %s\n", PMBD_USE_CHECKSUM()? "YES" : "NO");
	printk(KERN_INFO "pmbd: g_pmbd_lock = %s\n", PMBD_USE_LOCK()? "YES" : "NO");
	printk(KERN_INFO "pmbd: g_pmbd_subpage_update = %s\n", PMBD_USE_SUBPAGE_UPDATE()? "YES" : "NO");
	printk(KERN_INFO "pmbd: g_pmbd_adjust_ns = %llu ns\n", g_pmbd_adjust_ns);
	printk(KERN_INFO "pmbd: g_pmbd_num_buffers = %llu\n", g_pmbd_num_buffers);
	printk(KERN_INFO "pmbd: g_pmbd_buffer_stride = %llu blocks\n", g_pmbd_buffer_stride);
	printk(KERN_INFO "pmbd: g_pmbd_timestat = %u \n", g_pmbd_timestat);
	printk(KERN_INFO "pmbd: HIGHMEM offset [%llu] size [%lu] Private Mapping (%s) (%s) (%s) Write Barrier(%s) FUA(%s)\n", 
			g_highmem_phys_addr, g_highmem_size, (PMBD_USE_PMAP()? "Enabled" : "Disabled"), 
			(PMBD_USE_NTS()? "Non-Temporal Store":"Temporal Store"),	
			(PMBD_USE_NTL()? "Non-Temporal Load":"Temporal Load"),	
			(PMBD_USE_WB()? "Enabled": "Disabled"),
			(PMBD_USE_FUA()? "Enabled":"Disabled"));

	/* for each pmbd device */
	for (i = 0; i < g_pmbd_nr; i ++) {
		printk(KERN_INFO "pmbd: /dev/pm%c (%d)[%llu GB] read[%llu ns %llu MB/sec (%llux) (pause %llu cyc/pg)] write[%llu ns %llu MB/sec (%llux) (pause %llu cyc/pg)] [%s] [Buf: %llu MBs, batch %llu pages] [%s] [%s]\n", 
			'a'+i, i, g_pmbd_size[i], g_pmbd_rdlat[i], g_pmbd_rdbw[i], g_pmbd_rdsx[i], g_pmbd_rdpause[i], g_pmbd_wrlat[i], g_pmbd_wrbw[i], g_pmbd_wrsx[i], g_pmbd_wrpause[i],\
			(g_pmbd_rammode[i] ? "RAM" : "PMBD"), g_pmbd_bufsize[i], g_pmbd_buffer_batch_size[i], \
			(g_pmbd_simmode[i] ? "Simulating PM only" : "Simulating the whole device"), \
			(PMBD_USE_PMAP() ? "PMAP" : (g_pmbd_wpmode[i] ? "WP-CR0/WP" : "WP-PTE")));

		if (g_pmbd_simmode[i] > 0){
			printk(KERN_INFO "pmbd: ********************************* WARNING **************************************\n");
			printk(KERN_INFO "pmbd: Using simmode%llu to simulate a slowed-down PM space may cause system soft lockup.\n", g_pmbd_simmode[i]);
			printk(KERN_INFO "pmbd: To disable the warning message, please add \"nosoftlockup\" in the boot option. \n");
			printk(KERN_INFO "pmbd: ********************************************************************************\n");
		}
	}

	printk(KERN_INFO "pmbd: ****************************** WARNING ***********************************\n");
	printk(KERN_INFO "pmbd: 1. Checksum mismatch can be detected but not handled \n");
	printk(KERN_INFO "pmbd: 2. PMAP is incompatible with \"wrprotY\"\n");
	printk(KERN_INFO "pmbd: **************************************************************************\n");

	return;
}

/*
 * Parse a string with config for multiple devices (e.g. mode="pmbd4,1,3;")
 * @mode: input option string
 * @tag:  the tag being looked for (e.g. pmbd)
 * @data: output in an array
 */
static int _pmbd_parse_multi(char* mode, char* tag, unsigned long long data[])
{
	int nr = 0;
	if (strlen(mode)) {
	       	char* head = mode;
        	char* tail = mode;
		char* end  = mode + strlen(mode);
       		char tmp[128];
	
        	if ((head = strstr(mode, tag))) {
	        	head = head + strlen(tag);
		        tail = head;
			while(head < end){
	                	int len = 0;

				/* locate the position of the first non-number char */
				for(tail = head; IS_DIGIT(*tail) && tail < end; tail++) {};

				/* pick up the numbers */
	                	len = tail - head;
				if(len > 0) {
					nr ++;
					if (nr > PMBD_MAX_NUM_DEVICES) {
						printk(KERN_ERR "pmbd: %s(%d) - too many (%d) device config for %s\n", 
							__FUNCTION__, __LINE__, nr, tag);
						return -1;
					}
		        	        strncpy(tmp, head, len); tmp[len] = '\0';
        		        	data[nr - 1] = simple_strtoull(tmp, NULL, 0);
				} 

				/* check the next sequence of numbers */
				for(; !IS_DIGIT(*tail) && tail < end; tail++) {
					/* if we meet the first alpha char or space, clause ends */
					if(IS_ALPHA(*tail) || IS_SPACE(*tail))
						goto done;
				};

				/* move head to the next sequence of numbers */
				head = tail;
			}
		}
	}
done:
	return nr;
}

/*
 * Parse a string with config for all devices (e.g. mode="adj1000")
 * @mode: input option string
 * @tag:  the tag being looked for (e.g. pmbd)
 * @data: output 
 */
static int _pmbd_parse_single(char* mode, char* tag, unsigned long long* data)
{
	if (strlen(mode)) {
	       	char* head = mode;
        	char* tail = mode;
       		char tmp[128];

		if (strstr(mode, tag)) {
			head = strstr(mode, tag) + strlen(tag);
			for(tail=head; IS_DIGIT(*tail); tail++) {};
			if(tail == head) {
				return -1;
			} else {
				int len = tail - head;
				strncpy(tmp, head, len); tmp[len] = '\0';
				*data = simple_strtoull(tmp, NULL, 0);
			}
		} 
	}
	return 0;
}

static void load_default_conf(void)
{
	int i = 0;
	for (i = 0; i < PMBD_MAX_NUM_DEVICES; i ++) 
		g_pmbd_buffer_batch_size[i] = PMBD_BUFFER_BATCH_SIZE_DEFAULT;
}

/* parse the module parameters (mode) */
static void pmbd_parse_conf(void)
{
	int i = 0;
	static unsigned enforce_cache_wc = FALSE;

	load_default_conf();

	if (strlen(mode)) {
		unsigned long long data = 0;

		/* check pmbd size/usable */
		if (strstr(mode, "pmbd")) {
			if( (g_pmbd_nr = _pmbd_parse_multi(mode, "pmbd", g_pmbd_size)) <= 0)
				goto fail;
		} else {
			printk(KERN_ERR "pmbd: no pmbd size set\n");
			goto fail;
		}
		
		/* rdlat/wrlat (emulated read/write latency) in nanosec */
		if (strstr(mode, "rdlat"))
			if (_pmbd_parse_multi(mode, "rdlat", g_pmbd_rdlat) < 0)
				goto fail;
		if (strstr(mode, "wrlat")) 
			if (_pmbd_parse_multi(mode, "wrlat", g_pmbd_wrlat) < 0)
				goto fail;

		/* rdbw/wrbw (emulated read/write bandwidth) in MB/sec*/
		if (strstr(mode, "rdbw"))
			if (_pmbd_parse_multi(mode, "rdbw", g_pmbd_rdbw) < 0)
				goto fail;
		if (strstr(mode, "wrbw")) 
			if (_pmbd_parse_multi(mode, "wrbw", g_pmbd_wrbw) < 0)
				goto fail;

		/* rdsx/wrsx (emulated read/write slowdown X) */
		if (strstr(mode, "rdsx"))
			if (_pmbd_parse_multi(mode, "rdsx", g_pmbd_rdsx) < 0)
				goto fail;
		if (strstr(mode, "wrsx")) 
			if (_pmbd_parse_multi(mode, "wrsx", g_pmbd_wrsx) < 0)
				goto fail;

		/* rdsx/wrsx (emulated read/write slowdown X) */
		if (strstr(mode, "rdpause"))
			if (_pmbd_parse_multi(mode, "rdpause", g_pmbd_rdpause) < 0)
				goto fail;
		if (strstr(mode, "wrpause")) 
			if (_pmbd_parse_multi(mode, "wrpause", g_pmbd_wrpause) < 0)
				goto fail;

		/* do write optimization */
		if (strstr(mode, "rammode")){
			printk(KERN_ERR "pmbd: rammode removed\n");
			goto fail;
			if (_pmbd_parse_multi(mode, "rammode", g_pmbd_rammode) < 0)
				goto fail;
		}

		if (strstr(mode, "bufsize")){
			if (_pmbd_parse_multi(mode, "bufsize", g_pmbd_bufsize) < 0)
				goto fail;
			for (i = 0; i < PMBD_MAX_NUM_DEVICES; i ++) {
				if (g_pmbd_bufsize[i] > 0 && g_pmbd_bufsize[i] < PMBD_BUFFER_MIN_BUFSIZE){
					printk(KERN_ERR "pmbd: bufsize cannot be smaller than %d MBs. Setting 0 to disable PMBD buffer.\n", PMBD_BUFFER_MIN_BUFSIZE);
					goto fail;
				}
			}
		}

		/* numbuf and bufstride*/
		if (strstr(mode, "bufnum")) { 
			if(_pmbd_parse_single(mode, "bufnum", &data) < 0) {
				printk(KERN_ERR "pmbd: incorrect bufnum (must be at least 1)\n");
				goto fail;
			} else {
				g_pmbd_num_buffers = data;
			}
		}
		if (strstr(mode, "bufstride")) { 
			if(_pmbd_parse_single(mode, "bufstride", &data) < 0) {
				printk(KERN_ERR "pmbd: incorrect bufstride (must be at least 1)\n");
				goto fail;
			} else {
				g_pmbd_buffer_stride = data;
			}
		}

		/* check the nanoseconds of overhead to compensate */
		if (strstr(mode, "adj")) { 
			if(_pmbd_parse_single(mode, "adj", &data) < 0) {
				printk(KERN_ERR "pmbd: incorrect adj\n");
				goto fail;
			} else {
				g_pmbd_adjust_ns = data;
			}
		}

		/* check PMBD device type */
		if ((strstr(mode, "VM"))) {
			g_pmbd_type = PMBD_CONFIG_VMALLOC;
		} else if ((strstr(mode, "HM"))) {
			g_pmbd_type = PMBD_CONFIG_HIGHMEM;
		}

		/* use pmap*/
		if ((strstr(mode, "pmapY"))) {
			g_pmbd_pmap = TRUE;
		} else if ((strstr(mode, "pmapN"))) {
			g_pmbd_pmap = FALSE;
		} 
		if ((strstr(mode, "PMAP"))){
			printk("WARNING: !!! pmbd: PMAP is not supported any more (use pmapY) !!!\n");
			goto fail;
		}

		/* use nts*/
		if ((strstr(mode, "ntsY"))) {
			g_pmbd_nts = TRUE;
		} else if ((strstr(mode, "ntsN"))) {
			g_pmbd_nts = FALSE;
		}
		if ((strstr(mode, "NTS"))){
			printk("WARNING: !!! pmbd: NTS is not supported any more (use ntsY) !!!\n");
			goto fail;
		}

		/* use ntl*/
		if ((strstr(mode, "ntlY"))) {
			g_pmbd_ntl = TRUE;
			enforce_cache_wc = TRUE;
		} else if ((strstr(mode, "ntlN"))) {
			g_pmbd_ntl = FALSE;
		}

		/* timestat */
		if ((strstr(mode, "timestatY"))) {
			g_pmbd_timestat = TRUE;
		} else if ((strstr(mode, "timestatN"))) {
			g_pmbd_timestat = FALSE;
		}


		/* write barrier */
		if ((strstr(mode, "wbY"))) {
			g_pmbd_wb = TRUE;
		} else if ((strstr(mode, "wbN"))) {
			g_pmbd_wb = FALSE;
		}

		/* write barrier */
		if ((strstr(mode, "fuaY"))) {
			g_pmbd_fua = TRUE;
		} else if ((strstr(mode, "fuaN"))) {
			g_pmbd_fua = FALSE;
		}


		/* check if HIGH_MEM PMBD is configured */
		if (PMBD_USE_HIGHMEM()) { 
			if (strstr(mode, "hmo") && strstr(mode, "hms")) {
				/* parse reserved HIGH_MEM offset */
				if(_pmbd_parse_single(mode, "hmo", &data) < 0){
					printk(KERN_ERR "pmbd: incorrect hmo\n");
					g_highmem_phys_addr = 0;
					goto fail;
				} else {
					g_highmem_phys_addr = data * 1024 * 1024 * 1024;
				}

				/* parse reserved HIGH_MEM size */
				if(_pmbd_parse_single(mode, "hms", &data) < 0 || data == 0){
					printk(KERN_ERR "pmbd: incorrect hms\n");
					g_highmem_size = 0;
					goto fail;
				} else {
					g_highmem_size = data * 1024 * 1024 * 1024;
				} 
			} else {
				printk(KERN_ERR "pmbd: hmo or hms not set ***\n");
				goto fail;
			}


		} 


		/* check if mergeable */
		if((strstr(mode,"mgbY")))
			g_pmbd_mergeable = TRUE;
		else if((strstr(mode,"mgbN")))
			g_pmbd_mergeable = FALSE;

		/* CPU cache flushing  */
		if((strstr(mode,"clflushY")))
			g_pmbd_cpu_cache_clflush = TRUE;
		else if((strstr(mode,"clflushN")))
			g_pmbd_cpu_cache_clflush = FALSE;

		/* CPU cache setting */
		if((strstr(mode,"cacheWB")))		/* cache write back */
			g_pmbd_cpu_cache_flag = _PAGE_CACHE_WB;
		else if((strstr(mode,"cacheWC")))	/* cache write combined (through) */
			g_pmbd_cpu_cache_flag = _PAGE_CACHE_WC;
		else if((strstr(mode,"cacheUM")))	/* cache cachable but write back */
			g_pmbd_cpu_cache_flag = _PAGE_CACHE_UC_MINUS;
		else if((strstr(mode,"cacheUC")))	/* cache uncablable */
			g_pmbd_cpu_cache_flag = _PAGE_CACHE_UC;


		/* write protectable  */
		if((strstr(mode,"wrprotY")))
			g_pmbd_wr_protect = TRUE;
		else if((strstr(mode,"wrprotN")))
			g_pmbd_wr_protect = FALSE;

		/* write protectable  */
		if((strstr(mode,"wrverifyY")))
			g_pmbd_wr_verify = TRUE;
		else if((strstr(mode,"wrverifyN")))
			g_pmbd_wr_verify = FALSE;

		/* checksum  */
		if((strstr(mode,"checksumY")))
			g_pmbd_checksum = TRUE;
		else if((strstr(mode,"checksumN")))
			g_pmbd_checksum = FALSE;

		/* checksum  */
		if((strstr(mode,"lockY")))
			g_pmbd_lock = TRUE;
		else if((strstr(mode,"lockN")))
			g_pmbd_lock = FALSE;

		/* write protectable  */
		if((strstr(mode,"subupdateY")))
			g_pmbd_subpage_update = TRUE;
		else if((strstr(mode,"subupdateN")))
			g_pmbd_subpage_update = FALSE;


		/* batch */
		if (strstr(mode, "batch")){
			if (_pmbd_parse_multi(mode, "batch", g_pmbd_buffer_batch_size) < 0)
				goto fail;
			/* check if any batch size is set too small */
			for (i = 0; i < PMBD_MAX_NUM_DEVICES; i ++) {
				if (g_pmbd_buffer_batch_size[i] < 1){
					printk(KERN_ERR "pmbd: buffer batch size cannot be smaller than 1 page (default: 1024 pages)\n");
					goto fail;
				}
			}
		}

		/* simmode */
		if (strstr(mode, "simmode")){
			if (_pmbd_parse_multi(mode, "simmode", g_pmbd_simmode) < 0)
				goto fail;
		}

		/* wpmode */
		if (strstr(mode, "wpmode")){
			if (_pmbd_parse_multi(mode, "wpmode", g_pmbd_wpmode) < 0)
				goto fail;
		}

	} else {
		goto fail;
	}

	/* apply some enforced configuration */
	if (enforce_cache_wc)	/* if ntl is used, we must use WC */
		g_pmbd_cpu_cache_flag = _PAGE_CACHE_WC;

	/* Done, print input options */
	pmbd_print_conf();
	return;

fail:
	printk(KERN_ERR "pmbd: wrong mode config! Check modinfo\n\n");
	g_pmbd_nr = 0;
	return;
}

/*
 * *****************************************************************
 * simple emulation API functions
 * pmbd_rdwr_pause - pause read/write for a specified cycles/page
 * pmbd_rdwr_slowdown - slowdown read/write proportionally to DRAM 
 * *****************************************************************/

/* handle rdpause and wrpause options*/
static void pmbd_rdwr_pause(PMBD_DEVICE_T* pmbd, size_t bytes, unsigned rw)
{
	uint64_t cycles = 0;
	uint64_t time_p1, time_p2;

	/* sanity check */
	if (pmbd->rdpause == 0 && pmbd->wrpause == 0)
		return;

	/* start */
	TIMESTAT_POINT(time_p1);

	/* calculate the cycles to pause */
	if (rw == READ && pmbd->rdpause){
		cycles = MAX_OF((BYTE_TO_PAGE(bytes) * pmbd->rdpause), pmbd->rdpause);
	} else if (rw == WRITE && pmbd->wrpause){
		cycles = MAX_OF((BYTE_TO_PAGE(bytes) * pmbd->wrpause), pmbd->wrpause);
	}

	/* slow down now */
	if (cycles)
		sync_slowdown_cycles(cycles);
	
	TIMESTAT_POINT(time_p2);

	if(PMBD_USE_TIMESTAT()){
		int cid = CUR_CPU_ID();
		PMBD_STAT_T* pmbd_stat = pmbd->pmbd_stat;
		pmbd_stat->cycles_pause[rw][cid] += time_p2 - time_p1;
	}

	return;
}


/* handle rdsx and wrsx options */
static void pmbd_rdwr_slowdown(PMBD_DEVICE_T* pmbd, int rw, uint64_t start, uint64_t end)
{
	uint64_t cycles = 0;
	uint64_t time_p1, time_p2;

	/* sanity check */
	if ( !((rw == READ && pmbd->rdsx > 1) || (rw == WRITE && pmbd->wrsx > 1)))
		return;

	if (end < start){
		printk(KERN_WARNING "pmbd: %s(%d) end (%llu) is earlier than start (%llu)\n", \
			__FUNCTION__, __LINE__, (unsigned long long) start, (unsigned long long)end);
		return;
	}

	/* start */
	TIMESTAT_POINT(time_p1);

	/*FIXME: should we allow to do async slowdown? */
	cycles = (end-start)*((rw == READ) ? (pmbd->rdsx - 1) : (pmbd->wrsx -1));

	/*FIXME: should we minus a slack here (80-100cycles)? */
	if (cycles)
		sync_slowdown_cycles(cycles);

	TIMESTAT_POINT(time_p2);

	/* updating statistics */
	if(PMBD_USE_TIMESTAT()){
		int cid = CUR_CPU_ID();
		PMBD_STAT_T* pmbd_stat = pmbd->pmbd_stat;
		pmbd_stat->cycles_slowdown[rw][cid] += time_p2 - time_p1;
	}

	return;
}


/* 
 * set page's cache flags
 * @vaddr: start virtual address
 * @num_pages: the range size
 */
static void set_pages_cache_flags(unsigned long vaddr, int num_pages)
{
	switch (g_pmbd_cpu_cache_flag) {
		case _PAGE_CACHE_WB:
			printk(KERN_INFO "pmbd: set PM pages cache flags (WB)\n");
			set_memory_wb(vaddr, num_pages);
			break;
		case _PAGE_CACHE_WC:
			printk(KERN_INFO "pmbd: set PM pages cache flags (WC)\n");
			set_memory_wc(vaddr, num_pages);
			break;
		case _PAGE_CACHE_UC:
			printk(KERN_INFO "pmbd: set PM pages cache flags (UC)\n");
			set_memory_uc(vaddr, num_pages);
			break;
		case _PAGE_CACHE_UC_MINUS:
			printk(KERN_INFO "pmbd: set PM pages cache flags (UM)\n");
			set_memory_uc(vaddr, num_pages);
			break;
		default:
			set_memory_wb(vaddr, num_pages);
			printk(KERN_WARNING "pmbd: PM page attribute is not set - use WB\n");
			break;
	}
	return;
}


/* 
 * *************************************************************************
 * PMAP - Private mapping interface APIs
 * *************************************************************************
 *
 * The private mapping is for providing write protection -- only when we need
 * to access the PM page, we map it into the kernel virtual memory space, once
 * we finish using it, we unmap it, so the spatial and temporal window left for
 * bug attack is really small.
 *
 * Notes: pmap works similar to kmap_atomic*. It does the following:
 * (1) pmap_create(): allocate 128 pages with vmalloc, these 128 pte mapping is
 * saved to a backup place, and then be cleared to prevent accidental accesses.
 * Each page is assigned correspondingly to the CPU ID where the calling thread
 * is running on. So we support at most 128 CPU IDs. 
 * (2) pmap_atomic_pfn(): map the specified pfn into the entry, whose index is
 * the ID of the CPU on which the current thread is running. The pfn is loaded
 * into the corresponding pte entry and the corresponding TLB entry is flushed
 * (3) punmap_atomic(): the specified pte entry is cleared, and the TLB entry
 * is flushed
 * (4) pmap_destroy(): the saved pte mapping of the 128 pages are restored, and
 * vfree() is called to release the 128 pages allocated through vmalloc().
 *
 */

#define PMAP_NR_PAGES 	(128)
static unsigned int 	pmap_nr_pages = 0;			/* the total number of available pages for private mapping */
static void* 		pmap_va_start = NULL;			/* the first PMAP virtual address */
static pte_t*  		pmap_ptep[PMAP_NR_PAGES];		/* the array of PTE entries */
static unsigned long	pmap_pfn[PMAP_NR_PAGES];		/* the array of page frame numbers for restoring */
static pgprot_t 	pmap_prot[PMAP_NR_PAGES];		/* the array of page protection fields */
#define PMAP_VA(IDX)	(pmap_va_start + (IDX) * PAGE_SIZE)
#define PMAP_IDX(VA)	(((unsigned long)(VA) - (unsigned long)pmap_va_start) >> PAGE_SHIFT)

static inline void pmap_flush_tlb_single(unsigned long addr)
{
	asm volatile("invlpg (%0)" ::"r" (addr) : "memory");
}

static inline void* update_pmap_pfn(unsigned long pfn, unsigned int idx)
{
	void* va 	= PMAP_VA(idx);
	pte_t* ptep 	= pmap_ptep[idx];
	pte_t old_pte 	= *ptep;
	pte_t new_pte 	= pfn_pte(pfn, pmap_prot[idx]);

	if (pte_val(old_pte) == pte_val(new_pte))
		return va;

	/* update the pte entry */
	set_pte_atomic(ptep, new_pte);
//	set_pte(ptep, new_pte);

	/* flush one single tlb */
	__flush_tlb_one((unsigned long) va);
//	pmap_flush_tlb_single((unsigned long) va);

	/* return the old one for bkup */
	return va;
}

static inline void clear_pmap_pfn(unsigned idx)
{
	if (idx < pmap_nr_pages){

		void* va = PMAP_VA(idx);
		pte_t* ptep = pmap_ptep[idx];

		/* clear the mapping */
		pte_clear(NULL, (unsigned long) va, ptep);
		__flush_tlb_one((unsigned long) va);

	} else {
		panic("%s(%d) illegal pmap idx\n", __FUNCTION__, __LINE__);
	}
}

static int pmap_atomic_init(void)
{
	unsigned int i;

	/* checking */
	if (pmap_va_start)
		panic("%s(%d) something is wrong\n", __FUNCTION__, __LINE__);

	/* allocate an array of dummy pages as pmap virtual addresses */
	pmap_va_start = vmalloc(PAGE_SIZE * PMAP_NR_PAGES);
	if (!pmap_va_start){
		printk(KERN_ERR "pmbd:%s(%d) pmap_va_start cannot be initialized\n", __FUNCTION__, __LINE__);
		return -ENOMEM;
	}
	pmap_nr_pages = PMAP_NR_PAGES;

	/* set pages' cache flags, this flag would be saved into pmap_prot
	 * and will be applied together with the dynamically mapped page too (01/12/2012)*/
	set_pages_cache_flags((unsigned long)pmap_va_start, pmap_nr_pages);

	/* save the dummy pages' ptep, pfn, and prot info */	
	printk(KERN_INFO "pmbd: saving dummy pmap entries\n");
	for (i = 0; i < pmap_nr_pages; i ++){
		pte_t old_pte;
		unsigned int level;
		void* va = PMAP_VA(i);

		/* get the ptep */
		pte_t* ptep = lookup_address((unsigned long)(va), &level);

		/* sanity check */
		if (!ptep)
			panic("%s(%d) mapping not found\n", __FUNCTION__, __LINE__);

		old_pte = *ptep;
		if (!pte_val(old_pte))
			panic("%s(%d) invalid pte value\n", __FUNCTION__, __LINE__);

		if (level != PG_LEVEL_4K)
			panic("%s(%d) not PG_LEVEL_4K \n", __FUNCTION__, __LINE__);

		/* save dummy entries */
		pmap_ptep[i] = ptep;
		pmap_pfn[i] = pte_pfn(old_pte);
		pmap_prot[i] = pte_pgprot(old_pte);

/*		printk(KERN_INFO "%s(%d): saving dummy pmap entries: %u va=%p pfn=%lx\n", \
					__FUNCTION__, __LINE__, i, va, pmap_pfn[i]);
*/
	}

	/* clear the pte to make it illegal to access */
	for (i = 0; i < pmap_nr_pages; i ++)
		clear_pmap_pfn(i);

	return 0;
}

static void pmap_atomic_done(void)
{
	int i;
	
	/* restore the dummy pages' pte */
	printk(KERN_INFO "pmbd: restoring dummy pmap entries\n");
	for (i = 0; i < pmap_nr_pages; i ++){
/*		void* va = PMAP_VA(i);
		printk(KERN_INFO "%s(%d): restoring dummy pmap entries: %d va=%p pfn=%lx\n", \
					__FUNCTION__, __LINE__, i, va, pmap_pfn[i]);
*/
		/* restore the old pfn */
		update_pmap_pfn(pmap_pfn[i], i);
		pmap_ptep[i]= NULL;
		pmap_pfn[i] = 0;
	}

	/* free the dummy pages*/
	if (pmap_va_start)
		vfree(pmap_va_start);
	else
		panic("%s(%d): freeing dummy pages failed\n", __FUNCTION__, __LINE__);

	pmap_va_start = NULL;
	pmap_nr_pages = 0;
	return;
}

static void* pmap_atomic_pfn(unsigned long pfn, PMBD_DEVICE_T* pmbd, unsigned rw)
{
	void* va = NULL;
	unsigned int idx = CUR_CPU_ID();
	uint64_t time_p1 = 0;
	uint64_t time_p2 = 0;

	TIMESTAMP(time_p1);

	/* disable page fault temporarily */
	pagefault_disable();

	/* change the mapping to the specified pfn*/
	va = update_pmap_pfn(pfn, idx);

	TIMESTAMP(time_p2);

	/* update time statistics */
	if(PMBD_USE_TIMESTAT()){
		int cid = CUR_CPU_ID();
		PMBD_STAT_T* pmbd_stat = pmbd->pmbd_stat;
		pmbd_stat->cycles_pmap[rw][cid] += time_p2 - time_p1;
	}

	return va;
}

static void punmap_atomic(void* va, PMBD_DEVICE_T* pmbd, unsigned rw)
{
	unsigned int idx = PMAP_IDX(va);
	uint64_t time_p1 = 0;
	uint64_t time_p2 = 0;

	TIMESTAMP(time_p1);

	/* clear the mapping */
	clear_pmap_pfn(idx);

	/* re-enable the page fault */
	pagefault_enable();

	TIMESTAMP(time_p2);

	/* update time statistics */
	if(PMBD_USE_TIMESTAT()){
		int cid = CUR_CPU_ID();
		PMBD_STAT_T* pmbd_stat = pmbd->pmbd_stat;
		pmbd_stat->cycles_punmap[rw][cid] += time_p2 - time_p1;
	}

	return;
}

/* create the dummy pmap space */
static int pmap_create(void)
{
	pmap_atomic_init();
	return 0;
}

/* destroy the dummy pmap space */
static void pmap_destroy(void)
{
	pmap_atomic_done();
	return;
}

/*
 * *************************************************************************
 * Non-temporal memcpy 
 * *************************************************************************
 * Non-temporal memcpy does the following:
 * (1) use movntq to copy into PM space
 * (2) use sfence to flush the data to memory controller
 * 
 * Compared to regular temporal memcpy, it provides several benefits here:
 * (1) writes to PM bypass the CPU cache, which avoids polluting CPU cache
 * (2) reads from PM still benefit from the CPU cache
 * (3) sfence used for each write guarantees data will be flushed out of buffer
 */

static void nts_memcpy_64bytes_v2(void* to, void* from, size_t size)
{
	int i;
	unsigned bs = 64;	/* write unit size 8 bytes */

	if (size < bs)
		panic("%s(%d) size (%zu) is smaller than %u\n", __FUNCTION__, __LINE__, size, bs);

	if (((unsigned long) from & 64UL) || ((unsigned long)to & 64UL))
		panic("%s(%d) not aligned\n", __FUNCTION__, __LINE__);

	/* start */
	kernel_fpu_begin();
	
	/* do the non-temporal mov */	
	for (i = 0; i < size; i += bs){
		__asm__ __volatile__ (
		"movdqa (%0), %%xmm0\n"
		"movdqa 16(%0), %%xmm1\n"
		"movdqa 32(%0), %%xmm2\n"
		"movdqa 48(%0), %%xmm3\n"
		"movntdq %%xmm0, (%1)\n"
		"movntdq %%xmm1, 16(%1)\n"
		"movntdq %%xmm2, 32(%1)\n"
		"movntdq %%xmm3, 48(%1)\n"
		:
		: "r" (from), "r" (to)
		: "memory");

		to += bs;
		from += bs;
	}

	/* do sfence to push data out */
	__asm__ __volatile__ (
		" sfence\n" : :
	);

	/* end */
	kernel_fpu_end();

	/*NOTE: we assume it would be multiple units of 64 bytes*/
	if (i != size)
		panic("%s:%s:%d size (%zu) is in multiple units of 64 bytes\n", __FILE__, __FUNCTION__, __LINE__, size);

	return;
}

/* non-temporal store */
static void nts_memcpy(void* to, void* from, size_t size)
{
	if (size < 64){
		panic("no support for nt load smaller than 64 bytes yet\n");
	} else {
		nts_memcpy_64bytes_v2(to, from, size);
	}
}


static void ntl_memcpy_64bytes(void* to, void* from, size_t size)
{
	int i;
	unsigned bs = 64;	/* write unit size 16 bytes */

	if (size < bs)
		panic("%s(%d) size (%zu) is smaller than %u\n", __FUNCTION__, __LINE__, size, bs);

	if (((unsigned long) from & 64UL) || ((unsigned long)to & 64UL))
		panic("%s(%d) not aligned\n", __FUNCTION__, __LINE__);

	/* start */
	kernel_fpu_begin();
	
	/* do the non-temporal mov */	
	for (i = 0; i < size; i += bs){
		__asm__ __volatile__ (
		"movntdqa (%0), %%xmm0\n"
		"movntdqa 16(%0), %%xmm1\n"
		"movntdqa 32(%0), %%xmm2\n"
		"movntdqa 48(%0), %%xmm3\n"
		"movdqa %%xmm0, (%1)\n"
		"movdqa %%xmm1, 16(%1)\n"
		"movdqa %%xmm2, 32(%1)\n"
		"movdqa %%xmm3, 48(%1)\n"
		:
		: "r" (from), "r" (to)
		: "memory");

		to += bs;
		from += bs;
	}

	/* end */
	kernel_fpu_end();

	/*NOTE: we assume it would be multiple units of 64 bytes (at least 512 bytes)*/
	if (i != size)
		panic("%s:%s:%d size (%zu) is in multiple units of 64 bytes\n", __FILE__, __FUNCTION__, __LINE__, size);

	return;
}

/* non-temporal load */
static void ntl_memcpy(void* to, void* from, size_t size)
{
	if (size < 64){
		panic("no support for nt load smaller than 128 bytes yet\n");
	} else {
		ntl_memcpy_64bytes(to, from, size);
	}
}


/*
 * *************************************************************************
 * COPY TO/FROM PM
 * *************************************************************************
 * 
 * NOTE: copying into PM needs particular care, we use different solution here:
 * (1) pmap: we only map/unmap PM pages when we need to access, which provides
 *     us the most protection, for both reads and writes
 * (2) non-pmap: we always map every page into the kernel space, however, we
 *     put different protection for writes only. In both cases, PM pages are
 *     initialized as read-only 
 *     - PTE manipulation: before each write, the page writable bit is enabled, and
 *       disabled right after the write operation is done.
 *     - CR0/WP switch: before each write, the WP bit in the CR0 register turned
 *       off, and turned back on right after the write operation is done. Once
 *       CR0/WP bit is turned off, the CPU would not check the writable bit in the
 *       TLB in local CPU. So it is a tricky way to hack and walk around this
 *       problem. 
 *
 */

#define PMBD_PMAP_DUMMY_BASE_VA	(4096)
#define PMBD_PMAP_VA_TO_PA(VA)	(g_highmem_phys_addr + (VA) - PMBD_PMAP_DUMMY_BASE_VA)
/*
 * copying from/to a contiguous PM space using pmap
 * @ram_va: the RAM virtual address
 * @pmbd_dummy_va: the dummy PM virtual address (for converting to phys addr)
 * @rw: 0 - read, 1 - write
 */

#define MEMCPY_TO_PMBD(dst, src, bytes) { if (PMBD_USE_NTS()) \
						nts_memcpy((dst), (src), (bytes)); \
					else \
						memcpy((dst), (src), (bytes));}

#define MEMCPY_FROM_PMBD(dst, src, bytes) { if (PMBD_USE_NTL()) \
						ntl_memcpy((dst), (src), (bytes)); \
					else \
						memcpy((dst), (src), (bytes));}

static inline int _memcpy_pmbd_pmap(PMBD_DEVICE_T* pmbd, void* ram_va, void* pmbd_dummy_va, size_t bytes, unsigned rw, unsigned do_fua)
{
	unsigned long flags = 0;
	uint64_t pa = (uint64_t) PMBD_PMAP_VA_TO_PA(pmbd_dummy_va);

	/* disable interrupt (PMAP entry is shared) */	
	DISABLE_SAVE_IRQ(flags);
	
	/* do the real work */
	while(bytes){
		uint64_t time_p1 = 0;
		uint64_t time_p2 = 0;

		unsigned long pfn = (pa >> PAGE_SHIFT);		/* page frame number */
		unsigned off = pa & (~PAGE_MASK);		/* offset in one page */
		unsigned size = MIN_OF((PAGE_SIZE - off), bytes);/* the size to copy */

		/* map it */
		void * map = pmap_atomic_pfn(pfn, pmbd, rw);
		void * pmbd_va = map + off;

		/* do memcopy */
		TIMESTAMP(time_p1);
		if (rw == READ) { 
			MEMCPY_FROM_PMBD(ram_va, pmbd_va, size);
		} else { 
			if (PMBD_USE_SUBPAGE_UPDATE()) {
				/* if we do subpage write, write a cacheline each time */
				/* FIXME: we probably need to check the alignment here */
				size = MIN_OF(size, PMBD_CACHELINE_SIZE);
				if (memcmp(pmbd_va, ram_va, size)){
					MEMCPY_TO_PMBD(pmbd_va, ram_va, size);
				}
			} else {
				MEMCPY_TO_PMBD(pmbd_va, ram_va, size);
			}
		}
		TIMESTAMP(time_p2);

		/* emulating slowdown*/
		if(PMBD_DEV_USE_SLOWDOWN(pmbd))
			pmbd_rdwr_slowdown((pmbd), rw, time_p1, time_p2);

		/* for write check if we need to do clflush or do FUA*/
		if (rw == WRITE){ 
			if (PMBD_USE_CLFLUSH() || (do_fua && PMBD_CPU_CACHE_USE_WB() && !PMBD_USE_NTS()))
				pmbd_clflush_range(pmbd, pmbd_va, (size));
		}

		/* if write combine is used, we need to do sfence (like in ntstore) */
		if (PMBD_CPU_CACHE_USE_WC() || PMBD_CPU_CACHE_USE_UM()) 
			sfence();

		/* update time statistics */
		if(PMBD_USE_TIMESTAT()){
			int cid = CUR_CPU_ID();
			PMBD_STAT_T* pmbd_stat = pmbd->pmbd_stat;
			pmbd_stat->cycles_memcpy[rw][cid] += time_p2 - time_p1;
		}

		/* unmap it */
		punmap_atomic(map, pmbd, rw);

		/* prepare the next iteration */
		ram_va  += size;
		bytes 	-= size;
		pa 	+= size;
	}
	
	/* re-enable interrupt */	
	ENABLE_RESTORE_IRQ(flags);

	return 0;
}

static inline int memcpy_from_pmbd_pmap(PMBD_DEVICE_T* pmbd, void* dst, void* src, size_t bytes)
{
	return _memcpy_pmbd_pmap(pmbd, dst, src, bytes, READ, FALSE);
}

static inline int memcpy_to_pmbd_pmap(PMBD_DEVICE_T* pmbd, void* dst, void* src, size_t bytes, unsigned do_fua)
{
	return _memcpy_pmbd_pmap(pmbd, src, dst, bytes, WRITE, do_fua);
}


/*
 * memcpy from/to PM without using pmap
 */

#define DISABLE_CR0_WP(CR0,FLAGS)	{\
						if (PMBD_USE_WRITE_PROTECTION()){\
							DISABLE_SAVE_IRQ((FLAGS));\
							(CR0) = read_cr0();\
							write_cr0((CR0) & ~X86_CR0_WP);\
						}\
					}
#define ENABLE_CR0_WP(CR0,FLAGS)	{\
						if (PMBD_USE_WRITE_PROTECTION()){\
							write_cr0((CR0));\
							ENABLE_RESTORE_IRQ((FLAGS));\
						}\
					}

static inline int memcpy_from_pmbd_nopmap(PMBD_DEVICE_T* pmbd, void* dst, void* src, size_t bytes)
{
	uint64_t time_p1 = 0;
	uint64_t time_p2 = 0;

	/* start memcpy */
	TIMESTAMP(time_p1);
#if 0
	if (PMBD_DEV_USE_VMALLOC((pmbd))) 
		memcpy((dst), (src), (bytes)); 
	else if (PMBD_DEV_USE_HIGHMEM((pmbd))) 
		memcpy_fromio((dst), (src), (bytes));
#endif
	MEMCPY_FROM_PMBD(dst, src, bytes);

	TIMESTAMP(time_p2);

	/* emulating slowdown*/
	if(PMBD_DEV_USE_SLOWDOWN(pmbd))
		pmbd_rdwr_slowdown((pmbd), READ, time_p1, time_p2);

	/* update time statistics */
	if(PMBD_USE_TIMESTAT()){
		int cid = CUR_CPU_ID();
		PMBD_STAT_T* pmbd_stat = pmbd->pmbd_stat;
		pmbd_stat->cycles_memcpy[READ][cid] += time_p2 - time_p1;
	}

	return 0;
}

static int memcpy_to_pmbd_nopmap(PMBD_DEVICE_T* pmbd, void* dst, void* src, size_t bytes, unsigned do_fua)
{

	unsigned long cr0 = 0;
	unsigned long flags = 0;
	size_t left = bytes;


	/* get a bkup copy of the CR0 (to allow writable)*/
	if (PMBD_DEV_USE_WPMODE_CR0(pmbd))
		DISABLE_CR0_WP(cr0, flags);

	/* do the real work */
	while(left){
		size_t size = left; // the size to copy 
		uint64_t time_p1 = 0;
		uint64_t time_p2 = 0;

		TIMESTAMP(time_p1);
		/* do memcopy */
		if (PMBD_USE_SUBPAGE_UPDATE()) {
			/* if we do subpage write, write a cacheline each time */
			size = MIN_OF(size, PMBD_CACHELINE_SIZE);

			if (memcmp(dst, src, size)){
				MEMCPY_TO_PMBD(dst, src, size);
			}
		} else {
			MEMCPY_TO_PMBD(dst, src, size);
		}
		TIMESTAMP(time_p2);

		/* emulating slowdown*/
		if(PMBD_DEV_USE_SLOWDOWN(pmbd))
			pmbd_rdwr_slowdown((pmbd), WRITE, time_p1, time_p2);

		/* if write, check if we need to do clflush or we do FUA */
		if (PMBD_USE_CLFLUSH() || (do_fua && PMBD_CPU_CACHE_USE_WB() && !PMBD_USE_NTS()))
			pmbd_clflush_range(pmbd, dst, (size));

		/* if write combine is used, we need to do sfence (like in ntstore) */
		if (PMBD_CPU_CACHE_USE_WC() || PMBD_CPU_CACHE_USE_UM())
			sfence();

		/* update time statistics */
		if(PMBD_USE_TIMESTAT()){
			int cid = CUR_CPU_ID();
			PMBD_STAT_T* pmbd_stat = pmbd->pmbd_stat;
			pmbd_stat->cycles_memcpy[WRITE][cid] += time_p2 - time_p1;
		}

		/* prepare the next iteration */
		dst  	+= size;
		src 	+= size;
		left 	-= size;
	}
	
	/* restore the CR0 */
	if (PMBD_DEV_USE_WPMODE_CR0(pmbd))
		ENABLE_CR0_WP(cr0, flags);

	return 0;
}

static int memcpy_to_pmbd(PMBD_DEVICE_T* pmbd, void* dst, void* src, size_t bytes, unsigned do_fua)
{
	uint64_t start = 0; 
	uint64_t end = 0; 

	/* start simulation timing */
	if (PMBD_DEV_SIM_PMBD((pmbd)))
		start = emul_start((pmbd), BYTE_TO_SECTOR((bytes)), WRITE);

	/* do memcpy now */
	if (PMBD_USE_PMAP()){
		memcpy_to_pmbd_pmap(pmbd, dst, src, bytes, do_fua);
	} else {
		memcpy_to_pmbd_nopmap(pmbd, dst, src, bytes, do_fua);
	}

	/* stop simulation timing */
	if (PMBD_DEV_SIM_PMBD((pmbd))) 
		end = emul_end((pmbd), BYTE_TO_SECTOR((bytes)), WRITE, start); 

	/* pause write for a while*/
	pmbd_rdwr_pause(pmbd, bytes, WRITE);

	return 0;
}



static int memcpy_from_pmbd(PMBD_DEVICE_T* pmbd, void* dst, void* src, size_t bytes)
{
	uint64_t start = 0; 
	uint64_t end = 0; 

	/* start simulation timing */
	if (PMBD_DEV_SIM_PMBD((pmbd)))
		start = emul_start((pmbd), BYTE_TO_SECTOR((bytes)), READ);

	/* do memcpy here */
	if (PMBD_USE_PMAP()){
		memcpy_from_pmbd_pmap(pmbd, dst, src, bytes);
	}else{
		memcpy_from_pmbd_nopmap(pmbd, dst, src, bytes);
	}

	/* stop simulation timing */
	if (PMBD_DEV_SIM_PMBD((pmbd))) 
		end = emul_end((pmbd), BYTE_TO_SECTOR((bytes)), READ, start); 

	/* pause read for a while */
	pmbd_rdwr_pause(pmbd, bytes, READ);

	return 0;
}



/*
 * *************************************************************************
 * PMBD device buffer management
 * *************************************************************************
 *
 * Since write protection involves high performance overhead (due to TLB
 * shootdown and other system locking, linked list scan overhead related with
 * set_memory_* functions), we cannot change page table attributes for each
 * incoming write to PM space. In order to battle this issue, we added a
 * buffer to temporarily hold the incoming writes into a DRAM buffer, and
 * launch a syncer daemon to periodically flush dirty pages from the buffer to
 * the PM storage.  This brings two benefits: first, more contiguous pages can
 * be clustered together, and we only need to do one page attribute change for
 * a cluster; second, high overhead is hidden in the background, since the
 * writes become asynchronous now. 
 * 
 */


/* support functions to sort the bbi entries */
static int compare_bbi_sort_entries(const void* m, const void* n)
{
	PMBD_BSORT_ENTRY_T* a = (PMBD_BSORT_ENTRY_T*) m;
	PMBD_BSORT_ENTRY_T* b = (PMBD_BSORT_ENTRY_T*) n;
	if (a->pbn < b->pbn)
		return -1;
	else if (a->pbn == b->pbn)
		return 0;
	else
		return 1;

}

static void swap_bbi_sort_entries(void* m, void* n, int size)
{
	PMBD_BSORT_ENTRY_T* a = (PMBD_BSORT_ENTRY_T*) m;
	PMBD_BSORT_ENTRY_T* b = (PMBD_BSORT_ENTRY_T*) n;
	PMBD_BSORT_ENTRY_T tmp;
	tmp = *a;
	*a = *b;
	*b = tmp;
	return;
}


/*
 * get the aligned in-block offsets for a given request
 * @pmbd: the pmbd device
 * @sector: the starting offset (in sectors) of the incoming request
 * @bytes: the size of the incoming request
 * 
 * return: the in-block offset of the starting sector in the request
 * 
 * Since the block size (4096 bytes) is larger than the sector size (512 bytes),
 * if the incoming request is not completely aligned in units of blocks, then
 * we need to pull the whole block from PM space into the buffer, and apply
 * changes to partial blocks. This function is needed to calculate the offset
 * for the beginning and ending sectors. 
 *
 * For example: assuming sector size is 1024, buffer block size is 4096, sector
 * is 5, size is 1024, then the returned start offset is 1 (the second sector
 * in the buffer block), and the returned end offset is 2 (the third sector in
 * the buffer block)
 *
 * offset_s -----v     v--- offset_e
 *      ----------------------------------
 *      |        |*****|        |        |
 *      ----------------------------------
 *
 */

static sector_t pmbd_buffer_aligned_request_start(PMBD_DEVICE_T* pmbd, sector_t sector, size_t bytes)
{
	sector_t sector_s  	= sector;
	PBN_T pbn_s 		= SECTOR_TO_PBN(pmbd, sector_s);
	sector_t block_s 	= PBN_TO_SECTOR(pmbd, pbn_s);	/* the block's starting offset (in sector) */
	sector_t offset_s 	= 0;
	if (sector_s >= block_s) /* if not aligned */
		offset_s = sector_s - block_s;
	return offset_s;
}

static sector_t pmbd_buffer_aligned_request_end(PMBD_DEVICE_T* pmbd, sector_t sector, size_t bytes)
{
	sector_t sector_e  	= sector + BYTE_TO_SECTOR(bytes) - 1;
	PBN_T pbn_e 		= SECTOR_TO_PBN(pmbd, sector_e);
	sector_t block_e 	= PBN_TO_SECTOR(pmbd, pbn_e);	/* the block's starting offset (in sector) */
	sector_t offset_e 	= PBN_TO_SECTOR(pmbd, 1) - 1;
	
	if (sector_e >= block_e) /* if not aligned */
		offset_e = (sector_e - block_e);
	return offset_e;
}


/*
 * check and see if a physical block (pbn) is buffered
 * @pmbd: 	pmbd device
 * @pbn: 	buffer block number
 * 
 * NOTE: The caller must hold the pbi->lock
 */ 
static PMBD_BBI_T* _pmbd_buffer_lookup(PMBD_BUFFER_T* buffer, PBN_T pbn)
{
	PMBD_BBI_T* bbi = NULL;
	PMBD_DEVICE_T* pmbd = buffer->pmbd;
	PMBD_PBI_T* pbi = PMBD_BLOCK_PBI(pmbd, pbn);

	if (PMBD_BLOCK_IS_BUFFERED(pmbd, pbn)) {
		bbi = PMBD_BUFFER_BBI(buffer, pbi->bbn);
	}
	return bbi;
}

/*
 * Alloc/flush buffer functions
 */

/* 
 * flushing a range of contiguous physical blocks from buffer to PM space
 * @pmbd: pmbd device
 * @pbn_s: the first physical block number to flush (start)
 * @pbn_e: the last physical block number to flush (end)
 *
 * This function only flushes blocks from buffer to PM and unlink(free) the
 * corresponding buffer blocks and physical PM blocks, and it does not update
 * the buffer control info (num_dirty, pos_dirty).  This is because after
 * sorting, the processing order of buffer blocks (BBNs) may be different from
 * the spatial order of the buffer blocks, which makes it impossible to move
 * pos_dirty forward exactly one after one. In other words, pos_dirty only
 * points to the end of the dirty range, and we may flush a dirty block in the
 * middle of the range, rather than from the end first. 
 *
 * NOTE: The caller must hold the flush_lock; only one thread is allowed to do
 * this sync; we also assume all the physical blocks in the specified range are
 * buffered.
 *
 */

static unsigned long _pmbd_buffer_flush_range(PMBD_BUFFER_T* buffer, PBN_T pbn_s, PBN_T pbn_e)
{
	PBN_T pbn = 0;
	unsigned long num_cleaned = 0;
	PMBD_DEVICE_T* pmbd = buffer->pmbd;
	void* dst = PMBD_BLOCK_VADDR(pmbd, pbn_s);
	size_t bytes = PBN_TO_BYTE(pmbd, (pbn_e - pbn_s + 1));
	
	/* NOTE: we are protected by the flush_lock here, no-one else here */

	/* set the pages readwriteable */
	/* if we use CR0/WP to temporarily switch the writable permission, 
 	 * we don't have to change the PTE attributes directly */
	if (PMBD_DEV_USE_WPMODE_PTE(pmbd))
		pmbd_set_pages_rw(pmbd, dst, bytes, TRUE);
	

	/* for each physical block, flush it from buffer to the PM space */
	for (pbn = pbn_s; pbn <= pbn_e; pbn ++){
		BBN_T bbn 	= 0;
		PMBD_PBI_T* pbi = PMBD_BLOCK_PBI(pmbd, pbn);
		void* to 	= PMBD_BLOCK_VADDR(pmbd, pbn);
		size_t size 	= pmbd->pb_size;
		void* from	= NULL;		/* wait to get it in locked region */
		PMBD_BBI_T* bbi	= NULL;		/* wait to get it in locked region */

		/* 
		 * NOTE: This would not cause a deadlock, because the block
		 * here are already buffered, and these blocks would not call
		 * pmbd_buffer_alloc_block() 
		 */
		spin_lock(&pbi->lock);		/* lock the block */

		/* get related buffer block info */
		if (PMBD_BLOCK_IS_BUFFERED(pmbd, pbn)) {
			bbn	= pbi->bbn;
			bbi	= PMBD_BUFFER_BBI(buffer, pbi->bbn);
			from  	= PMBD_BUFFER_BLOCK(buffer, pbi->bbn);
		} else {
			panic("pmbd: %s(%d) something wrong here \n", __FUNCTION__, __LINE__);
		}
		
		/* sync data from buffer into PM first */
		if (PMBD_BUFFER_BBI_IS_DIRTY(buffer, bbn)) {
			/* flush to PM */	
			memcpy_to_pmbd(pmbd, to, from, size, FALSE);

			/* mark it as clean */
			PMBD_BUFFER_SET_BBI_CLEAN(buffer, bbn);
		}
	}

	/* set the pages back to read-only */
	if (PMBD_DEV_USE_WPMODE_PTE(pmbd))
		pmbd_set_pages_ro(pmbd, dst, bytes, TRUE);
	

	/* finish the remaining work */	
	for (pbn = pbn_s; pbn <= pbn_e; pbn ++){
		PMBD_PBI_T* pbi = PMBD_BLOCK_PBI(pmbd, pbn);
		void* to 	= PMBD_BLOCK_VADDR(pmbd, pbn);
		size_t size 	= pmbd->pb_size;
		BBN_T bbn	= pbi->bbn;
		void* from  	= PMBD_BUFFER_BLOCK(buffer, pbi->bbn);

		/* verify that the write operation succeeded */
		if(PMBD_USE_WRITE_VERIFICATION())
			pmbd_verify_wr_pages(pmbd, to, from, size);

		/* reset the bbi and pbi link info */
		PMBD_BUFFER_SET_BBI_UNBUFFERED(buffer, bbn);
		PMBD_SET_BLOCK_UNBUFFERED(pmbd, pbn);

		/* unlock the block */
		spin_unlock(&pbi->lock);

		num_cleaned ++;
	}

	/* generate checksum */
	if (PMBD_USE_CHECKSUM())
		pmbd_checksum_on_write(pmbd, dst, bytes);
	
	return num_cleaned;
}


/*
 * core function of flushing the pmbd buffer
 * @pmbd: pmbd device
 *
 * NOTE: this function performs the flushing in the following steps
 * (1) get the flush lock (to allow only one to do flushing)
 * (2) get the buffer_lock to protect the buffer control info (num_dirty,
 * pos_dirty, pos_clean)
 * (3) check if someone else has already done the flushing work while waiting
 * for the lock 
 * (4) copy the buffer block info from pos_dirty to pos_clean to a temporary
 * array
 * (5) release the buffer_lock (to allow alloc to proceed, as long as free
 * blocks exist)
 *
 * (6) sort the temporary array of buffer blocks in the order of their PBNs.
 * This is because we need to organize sequences of contiguous physical blocks,
 * so that we can use only one set_memory_* function for a sequence of memory
 * pages, rather than once for each page. So the larger the sequence is, the
 * more efficient it would be.
 * (7) scan the sorted list, and form sequences of contiguous physical blocks,
 * and call __pmbd_buffer_flush_range() to synchronize the sequences one by one
 *
 * (8) get the flush_lock again
 * (9) update the pos_dirty and num_dirty to reflect the recent changes
 * (10) release the flush_lock
 *
 * NOTE: The caller must not hold flush_lock and buffer_lock, but can hold
 * pbi->lock. 
 *
 */
static unsigned long pmbd_buffer_flush(PMBD_BUFFER_T* buffer, unsigned long num_to_clean)
{
	BBN_T i = 0;
	BBN_T bbn_s = 0;
	BBN_T bbn_e = 0; 
	PBN_T first_pbn = 0;
	PBN_T last_pbn = 0;
	unsigned long num_cleaned = 0;
	unsigned long num_scanned = 0; 
	PMBD_DEVICE_T* pmbd = buffer->pmbd;
	PMBD_BSORT_ENTRY_T* bbi_sort_buffer = buffer->bbi_sort_buffer;

	/* lock the flush_lock to ensure no-one else can do flush in parallel */
	spin_lock(&buffer->flush_lock);

	/* now we lock the buffer to protect buffer control info */
	spin_lock(&buffer->buffer_lock);

	/* check if num_to_clean is too large */
	if (num_to_clean > buffer->num_dirty)
		num_to_clean = buffer->num_dirty;

	/* check if the buffer is empty (someone else may have done the flushing job) */
	if (PMBD_BUFFER_IS_EMPTY(buffer) || num_to_clean == 0) {
		spin_unlock(&buffer->buffer_lock);
		goto done;
	}

	/* set up the range of BBNs we need to check */
	bbn_s = buffer->pos_dirty; 				/* the first bbn */
	bbn_e = PMBD_BUFFER_PRIO_POS(buffer, buffer->pos_clean);/* the last bbn */

	/* scan the buffer range and put it into the sort buffer */ 
	/* 
         * NOTE: bbn_s could be equal to PMBD_BUFFER_NEXT_POS(buffer, bbn_e), if
         * the buffer is filled with dirty blocks, so we need to check num_scanned
         * here. 
         * */
	for (i = bbn_s; 
	    (i != PMBD_BUFFER_NEXT_POS(buffer, bbn_e)) || (num_scanned == 0); 
	     i = PMBD_BUFFER_NEXT_POS(buffer, i)) {
		/* 
		 * FIXME: it may be possible that some blocks in the dirty
		 * block range are "clean", because after the block is
		 * allocated, and before it is being written, the block is
		 * marked as CLEAN, but it is allocated already. However, it is
		 * safe to attempt to flush it, because the pbi->lock would
		 * protect us. 
		 *
		 * UPDATES: we changed the allocator code to mark it dirty as
		 * soon as the block is allocated. So the aforesaid situation
		 * would not happen anymore. 
		 */
		if(PMBD_BUFFER_BBI_IS_CLEAN(buffer, i)){ 
			/* found clean blocks */
			panic("ERR: %s(%d)%u: found clean block in the range of dirty blocks (bbn_s=%lu bbn_e=%lu, i=%lu, num_scanned=%lu num_to_clean=%lu num_dirty=%lu pos_dirty=%lu pos_clean=%lu)\n", 
					__FUNCTION__, __LINE__, __CURRENT_PID__,bbn_s, bbn_e, i, num_scanned, num_to_clean, buffer->num_dirty, buffer->pos_dirty, buffer->pos_clean);
			continue;
		} else {
			PMBD_BBI_T* bbi = PMBD_BUFFER_BBI(buffer, i);
			PMBD_BSORT_ENTRY_T* se = bbi_sort_buffer + num_scanned;

			/* add it to the buffer for sorting */
			se->pbn = bbi->pbn;
			se->bbn = i;
			num_scanned ++;

			/* only clean num_to_clean blocks */
			if (num_scanned >= num_to_clean)
				break;
		}
	}
	/* unlock the buffer to let allocator continue */
	spin_unlock(&buffer->buffer_lock);

	/* if no valid dirty block to be cleaned*/
	if (num_scanned == 0)
		goto done;

	/* 
	 * sort the buffer to get sequences of contiguous blocks 
	 */
	if (PMBD_DEV_USE_WPMODE_PTE(pmbd))
		sort(bbi_sort_buffer, num_scanned, sizeof(PMBD_BSORT_ENTRY_T), compare_bbi_sort_entries, swap_bbi_sort_entries);

	/* scan the sorted list to organize and flush the sequences of contiguous PBNs */
	for (i = 0; i < num_scanned; i ++) {
		PMBD_BSORT_ENTRY_T* se = bbi_sort_buffer + i;
		PMBD_BBI_T* bbi = PMBD_BUFFER_BBI(buffer, se->bbn);
		if (i == 0) {
			/* the first one */ 
			first_pbn = bbi->pbn;
			last_pbn = bbi->pbn;
			continue;
		} else {
			if (bbi->pbn == (last_pbn + 1) ) {
				/* if blocks are contiguous */
				last_pbn = bbi->pbn;
				continue;
			} else {
				/* if blocks are not contiguous */
				num_cleaned += _pmbd_buffer_flush_range(buffer, first_pbn, last_pbn);

				/* start a new sequence */
				first_pbn = bbi->pbn;
				last_pbn = bbi->pbn;
				continue;
			}
		}
	}

	/* finish the last sequence of contiguous PBNs */
	num_cleaned += _pmbd_buffer_flush_range(buffer, first_pbn, last_pbn);

	/* update the buffer control info */
	spin_lock(&buffer->buffer_lock);
	buffer->pos_dirty = PMBD_BUFFER_NEXT_N_POS(buffer, bbn_s, num_cleaned);	/* move pos_dirty forward */
	buffer->num_dirty -= num_cleaned;	/* decrement the counter*/
	spin_unlock(&buffer->buffer_lock);

done:
	spin_unlock(&buffer->flush_lock);
	return num_cleaned;
}

/*
 * entry function of flushing buffer
 * This function is called by both allocator and syncer
 * @pmbd: pmbd device
 * @num_to_clean: how many blocks to clean 
 * @i_am_syncer: indicate which caller is (TRUE for syncer and FALSE for allocator)
 */
static unsigned long pmbd_buffer_check_and_flush(PMBD_BUFFER_T* buffer, unsigned long num_to_clean, unsigned caller)
{
	unsigned long num_cleaned = 0;

	/* 
	 * Since there may exist more than one thread (e.g. alloc/flush or
	 * alloc/alloc) trying to flush the buffer, we need to first check if
	 * someone else has already done the job while waiting for the lock. If
	 * true, we don't have to proceed and flush it again. This improves the
	 * responsiveness of applications 
	 */
	if (caller == CALLER_DESTROYER){
		/* if destroyer calls this function, just flush everything */
		goto do_it;

	} else if (caller == CALLER_SYNCER) {
		/* if syncer calls this function and the buffer is empty, do nothing */
		spin_lock(&buffer->buffer_lock);
		if (PMBD_BUFFER_IS_EMPTY(buffer)){
			spin_unlock(&buffer->buffer_lock);
			goto done;
		}
		spin_unlock(&buffer->buffer_lock);

	} else if (caller == CALLER_ALLOCATOR){
	
		/* if reader/writer calls this function, some blocks are freed, then 
		 * we just do nothing */
		spin_lock(&buffer->buffer_lock);
		if (!PMBD_BUFFER_IS_FULL(buffer)){
			spin_unlock(&buffer->buffer_lock);
			goto done;
		}
		spin_unlock(&buffer->buffer_lock);

	} else {
		panic("ERR: %s(%d) unknown caller id\n", __FUNCTION__, __LINE__);
	}

	/* otherwise, we do flushing */
do_it:
	num_cleaned = pmbd_buffer_flush(buffer, num_to_clean);

done:
	return num_cleaned;
}

/* 
 * Core function of allocating a buffer block
 * 
 * We first grab the buffer_lock, and check to see if the buffer is full. If
 * not, we allocate a buffer block, move the pos_clean, and update num_dirty,
 * then release the buffer_lock. Since we already hold the pbi->lock, it is
 * safe to release the lock and let other threads proceed (before we really
 * write data into the buffer block), because no one else can read/write or
 * access the same buffer block concurrently. If the buffer is full, we release
 * the buffer_lock to allow others to proceed (because we may be blocked at
 * flush_lock later), and then we call the function to synchronously flush the
 * buffer. Note that someone else may be there already, so we may be blocked
 * there, and if we find someone has already flushed the buffer, we need to
 * grab the buffer_lock and check if there is available buffer block again.  
 *
 * NOTE: The caller must hold the pbi->lock.
 *
 */
static PMBD_BBI_T* pmbd_buffer_alloc_block(PMBD_BUFFER_T* buffer, PBN_T pbn)
{
	BBN_T pos		= 0;
	PMBD_BBI_T* bbi		= NULL;
	PMBD_DEVICE_T* pmbd 	= buffer->pmbd;
	PMBD_PBI_T* pbi 	= PMBD_BLOCK_PBI(pmbd, pbn);

	/* lock the buffer control info (we will check and update it) */
	spin_lock(&buffer->buffer_lock);

check_again:
	/* check if the buffer is completely full, if yes, flush it to PM */
	if (PMBD_BUFFER_IS_FULL(buffer)) {
		/* release the buffer_lock (someone may be doing flushing)*/
		spin_unlock(&buffer->buffer_lock);

		/* If the buffer is full, we must flush it synchronously.
		 * 
		 * NOTE: this on-demand flushing can improve performance a lot, since
		 * the allocator has not to wait for waking up syncer to do this, which
		 * is much faster. Another merit is that it makes the application run
		 * more smoothly (it is abrupt if completely relying on syncer). Also
		 * note that we only flush a batch (e.g. 1024) of blocks, rather than
		 * all the buffer blocks, this is because we only need a few blocks to
		 * satisfy the application's own need, and this reduces the time that 
		 * the application spends on allocation. */
		pmbd_buffer_check_and_flush(buffer, buffer->batch_size, CALLER_ALLOCATOR);

		/* grab the lock and check the availability of free buffer blocks 
		 * again, because someone may use up all the free buffer blocks, right
		 * after the buffer is flushed but before we can get one */
		spin_lock(&buffer->buffer_lock);
		goto check_again;
	} 

	/* if buffer is not full, only reserve one spot first.
	 * 
	 * NOTE that we do not have to do link and memcpy in the locked region,
	 * because pbi->lock guarantees that no-one else can use it now. This
	 * moves the high-cost operations out of the critical section */
	pos = buffer->pos_clean;
	buffer->pos_clean = PMBD_BUFFER_NEXT_POS(buffer, buffer->pos_clean); 
	buffer->num_dirty ++;

	/* NOTE: we mark it "dirty" here, but actually the data has not been
	 * really written into the PMBD buffer block yet. This is safe, because
	 * we are protected by the pbi->lock  */
	PMBD_BUFFER_SET_BBI_DIRTY(buffer, pos); 

	/* now link them up (no-one else can see it) */
	bbi = PMBD_BUFFER_BBI(buffer, pos);

	bbi->pbn = pbn;
	pbi->bbn = pos;

	/* unlock the buffer_lock and let others proceed */
	spin_unlock(&buffer->buffer_lock);

	return bbi;
}


/*
 * syncer daemon worker function
 */

static inline uint64_t pmbd_device_is_idle(PMBD_DEVICE_T* pmbd)
{
	unsigned last_jiffies, now_jiffies;
	uint64_t interval = 0;

	now_jiffies = jiffies;
	PMBD_DEV_GET_ACCESS_TIME(pmbd, last_jiffies);
	interval = jiffies_to_usecs(now_jiffies - last_jiffies);
	
	if (PMBD_DEV_IS_IDLE(pmbd, interval)) {
		return interval;
	} else {
		return 0;
	}
}

static int pmbd_syncer_worker(void* data)
{
	PMBD_BUFFER_T* buffer = (PMBD_BUFFER_T*) data;

	set_user_nice(current, 0);

	do {
		unsigned do_flush  = 0;
//		unsigned long loop = 0;
		uint64_t idle_usec = 0;
		spin_lock(&buffer->buffer_lock);

		/* we start flushing, if 
		 * (1) the num of dirty blocks hits the high watermark, or
		 * (2) the device has been idle for a while */
		if (PMBD_BUFFER_ABOVE_HW(buffer)) {
			//printk("High watermark is hit\n";
			do_flush = 1;
		}
//		if (pmbd_device_is_idle(buffer->pmbd) && !PMBD_BUFFER_IS_EMPTY(buffer)) {
		if ((idle_usec = pmbd_device_is_idle(buffer->pmbd)) && PMBD_BUFFER_ABOVE_LW(buffer)) {
			//printk("Device is idle for %llu uSeconds\n", idle_usec);
			do_flush = 1;
		}
		if (do_flush){
			unsigned long num_dirty = 0;
			unsigned long num_cleaned = 0;
repeat:
			num_dirty = buffer->num_dirty;
			spin_unlock(&buffer->buffer_lock);

			/* start flushing 
			 * 
			 * NOTE: we only allocate a batch (e.g. 1024) of blocks each time. The
			 * purpose is to let the applications wait for free blocks, so that they can
			 * get a few free blocks and proceed, rather than waiting for the whole
			 * buffer gets flushed. Otherwise, the bandwidth would be lower and the
			 * applications cannot run smoothly. 
			 */
			num_cleaned = pmbd_buffer_check_and_flush(buffer, buffer->batch_size, CALLER_SYNCER);
			//printk("Syncer(%u) activated (%lu) - Before (%lu) Cleaned (%lu) After (%lu)\n", 
			//		buffer->buffer_id, loop++, num_dirty, num_cleaned, buffer->num_dirty);
			
			/* continue to flush until we hit the low watermark */
			spin_lock(&buffer->buffer_lock);
			if (PMBD_BUFFER_ABOVE_LW(buffer)) {
//			if (buffer->num_dirty > 0) {
				goto repeat;
			}
		}
		spin_unlock(&buffer->buffer_lock);

		/* go to sleep */
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(1);
		set_current_state(TASK_RUNNING);

	} while(!kthread_should_stop());
	return 0;
}

static struct task_struct* pmbd_buffer_syncer_init(PMBD_BUFFER_T* buffer)
{
	struct task_struct* tsk = NULL;
	tsk = kthread_run(pmbd_syncer_worker, (void*) buffer, "nsyncer");
	if (!tsk) {
		printk(KERN_ERR "pmbd: initializing buffer syncer failed\n");
		return NULL;
	}

	buffer->syncer = tsk;
	printk("pmbd: buffer syncer launched\n");
	return tsk;
}

static int pmbd_buffer_syncer_stop(PMBD_BUFFER_T* buffer)
{
	if (buffer->syncer){
		kthread_stop(buffer->syncer);
		buffer->syncer = NULL;
		printk(KERN_INFO "pmbd: buffer syncer stopped\n");
	}
	return 0;
}

/*
 * read and write to PMBD with buffer 
 */ 
static void copy_to_pmbd_buffered(PMBD_DEVICE_T* pmbd, void *src, sector_t sector, size_t bytes)
{
	PBN_T pbn   = 0;
	void*  from = src;

	/*
	 * get the start and end in-block offset
	 * 
	 * NOTE: Since the buffer block (4096 bytes) can be larger than the
	 * sector(512 bytes), if incoming request is not completely aligned to
	 * buffer blocks, we need to read the full block from PM into the
	 * buffer block and apply writes to partial of the buffer block. Here,
	 * offset_s and offset_e are the start and end in-block offsets (in
	 * units of sectors) for the first and the last sector in the request,
	 * they may or may not appear in the same buffer block, depending on the
	 * request size.
	 */
	PBN_T pbn_s = SECTOR_TO_PBN(pmbd, sector);
	PBN_T pbn_e = BYTE_TO_PBN(pmbd, (SECTOR_TO_BYTE(sector) + bytes - 1));
	sector_t offset_s = pmbd_buffer_aligned_request_start(pmbd, sector, bytes);
	sector_t offset_e = pmbd_buffer_aligned_request_end(pmbd, sector, bytes);

	/* for each physical block */
	for (pbn = pbn_s; pbn <= pbn_e; pbn ++){
		void* to 	= NULL;
		PMBD_BBI_T* bbi = NULL;
		PMBD_PBI_T* pbi = PMBD_BLOCK_PBI(pmbd, pbn);
		sector_t sect_s	= (pbn == pbn_s) ? offset_s : 0; /* sub-block access */
		sector_t sect_e	= (pbn == pbn_e) ? offset_e : (PBN_TO_SECTOR(pmbd, 1) - 1);/* sub-block access */
		size_t size 	= SECTOR_TO_BYTE(sect_e - sect_s + 1);	/* get the real size */
		PMBD_BUFFER_T* buffer = PBN_TO_PMBD_BUFFER(pmbd, pbn);

		/* lock the physical block first */
		spin_lock(&pbi->lock);

		/* check if the physical block is buffered */
		bbi = _pmbd_buffer_lookup(buffer, pbn);

		if (bbi){
			/* if the block is already buffered */
			to = PMBD_BUFFER_BLOCK(buffer, pbi->bbn) + SECTOR_TO_BYTE(sect_s);
		} else {
			/* if not buffered, allocate one free buffer block */
			bbi = pmbd_buffer_alloc_block(buffer, pbn);

			/* if not aligned to a full block, we have to copy the whole 
			 * block from the PM space to the buffer block first */
			if (size < pmbd->pb_size){
				memcpy_from_pmbd(pmbd, PMBD_BUFFER_BLOCK(buffer, pbi->bbn), PMBD_BLOCK_VADDR(pmbd, pbn), pmbd->pb_size);
			}
			to = PMBD_BUFFER_BLOCK(buffer, pbi->bbn) + SECTOR_TO_BYTE(sect_s);
		}
		
		/* writing it into buffer */
		memcpy(to, from, size);
		PMBD_BUFFER_SET_BBI_DIRTY(buffer, pbi->bbn);

		/* unlock the block */
		spin_unlock(&pbi->lock);

		from += size;
	}

	return;
}

static void copy_from_pmbd_buffered(PMBD_DEVICE_T* pmbd, void *dst, sector_t sector, size_t bytes)
{
	PBN_T pbn = 0;
	void*  to = dst;

	/* get the start and end in-block offset */
	PBN_T pbn_s 	= SECTOR_TO_PBN(pmbd, sector);
	PBN_T pbn_e 	= BYTE_TO_PBN(pmbd, SECTOR_TO_BYTE(sector) + bytes - 1);
	sector_t offset_s = pmbd_buffer_aligned_request_start(pmbd, sector, bytes);
	sector_t offset_e = pmbd_buffer_aligned_request_end(pmbd, sector, bytes);

	for (pbn = pbn_s; pbn <= pbn_e; pbn ++){
		/* Scan the incoming request and check each block, for each block, we
		 * check if it is in the buffer. If true, we read it from the buffer,
		 * otherwise, we read from the PM space. */

		void* from 	= NULL;
		PMBD_BBI_T* bbi = NULL;
		PMBD_PBI_T* pbi = PMBD_BLOCK_PBI(pmbd, pbn);
		sector_t sect_s	= (pbn == pbn_s) ? offset_s : 0;				
		sector_t sect_e	= (pbn == pbn_e) ? offset_e : (PBN_TO_SECTOR(pmbd, 1) - 1);/* sub-block access */
		size_t size 	= SECTOR_TO_BYTE(sect_e - sect_s + 1);	/* get the real size */
		PMBD_BUFFER_T* buffer = PBN_TO_PMBD_BUFFER(pmbd, pbn);

		/* lock the physical block first */
		spin_lock(&pbi->lock);

		/* check if the block is in the buffer */
		bbi = _pmbd_buffer_lookup(buffer, pbn);

		/* start reading data */
		if (bbi) { 
			/* if buffered, read it from the buffer */
			from = PMBD_BUFFER_BLOCK(buffer, pbi->bbn) + SECTOR_TO_BYTE(sect_s);

			/* read it out */
			memcpy(to, from, size);

		} else {
			/* if not buffered, read it from PM space */
			from = PMBD_BLOCK_VADDR(pmbd, pbn) + SECTOR_TO_BYTE(sect_s);

			/* verify the checksum first */
			if (PMBD_USE_CHECKSUM())
				pmbd_checksum_on_read(pmbd, from, size);

			/* read it out*/
			memcpy_from_pmbd(pmbd, to, from, size);
		}

		/* unlock the block */
		spin_unlock(&pbi->lock);

		to += size;
	}

	return;
}

/*
 * buffer related space alloc/free functions
 */
static int pmbd_pbi_space_alloc(PMBD_DEVICE_T* pmbd)
{
	int err = 0;

	/* allocate checksum space */
	pmbd->pbi_space = vmalloc(PMBD_TOTAL_PB_NUM(pmbd) * sizeof(PMBD_PBI_T));
	if (pmbd->pbi_space) {
		PBN_T i;
		for (i = 0; i < PMBD_TOTAL_PB_NUM(pmbd); i ++) {
			PMBD_PBI_T* pbi = PMBD_BLOCK_PBI(pmbd, i);
			PMBD_SET_BLOCK_UNBUFFERED(pmbd, i);
			spin_lock_init(&pbi->lock);
		}
		printk(KERN_INFO "pmbd(%d): pbi space is initialized\n", pmbd->pmbd_id);
	} else {
		err = -ENOMEM;
	}

	return err;
}

static int pmbd_pbi_space_free(PMBD_DEVICE_T* pmbd)
{
	if (pmbd->pbi_space){
		vfree(pmbd->pbi_space);
		pmbd->pbi_space = NULL;
		printk(KERN_INFO "pmbd(%d): pbi space is freed\n", pmbd->pmbd_id);
	}
	return 0;
}

static PMBD_BUFFER_T* pmbd_buffer_create(PMBD_DEVICE_T* pmbd)
{
	int i;
	PMBD_BUFFER_T* buffer = kzalloc (sizeof(PMBD_BUFFER_T), GFP_KERNEL);
	if (!buffer){
		goto fail;
	}

	/* link to the pmbd device */	
	buffer->pmbd = pmbd;

	/* set size */
	if (g_pmbd_bufsize[pmbd->pmbd_id] > PMBD_BUFFER_MIN_BUFSIZE) {
		buffer->num_blocks = MB_TO_BYTES(g_pmbd_bufsize[pmbd->pmbd_id]) / pmbd->pb_size;
	} else {
		if (PMBD_DEV_USE_BUFFER(pmbd)) {
			printk(KERN_INFO "pmbd(%d): WARNING - too small buffer size (%llu MBs). Buffer set to %d MBs\n", 
				pmbd->pmbd_id, g_pmbd_bufsize[pmbd->pmbd_id], PMBD_BUFFER_MIN_BUFSIZE);
		}
		buffer->num_blocks = MB_TO_BYTES(PMBD_BUFFER_MIN_BUFSIZE) / pmbd->pb_size;
	}
	
	/* buffer space */
	buffer->buffer_space = vmalloc(buffer->num_blocks * pmbd->pb_size);
	if (!buffer->buffer_space)
		goto fail;

	/* BBI array */
	buffer->bbi_space = vmalloc(buffer->num_blocks * sizeof(PMBD_BBI_T));
	if (!buffer->bbi_space)
		goto fail;
	memset(buffer->bbi_space, 0, buffer->num_blocks * sizeof(PMBD_BBI_T));

	/* temporary array of bbi for sorting */
	buffer->bbi_sort_buffer = vmalloc(buffer->num_blocks * sizeof(PMBD_BSORT_ENTRY_T));
	if (!buffer->bbi_sort_buffer)
		goto fail;

	/* initialize the locks*/
	spin_lock_init(&buffer->buffer_lock);
	spin_lock_init(&buffer->flush_lock);

	/* initialize the BBI array */
	for (i = 0; i < buffer->num_blocks; i ++){
		PMBD_BUFFER_SET_BBI_CLEAN(buffer, i);
		PMBD_BUFFER_SET_BBI_UNBUFFERED(buffer, i);
	}
	
	/* initialize the buffer control info */
	buffer->num_dirty = 0;
	buffer->pos_dirty = 0;
	buffer->pos_clean = 0;
	buffer->batch_size = g_pmbd_buffer_batch_size[pmbd->pmbd_id];

	/* launch the syncer daemon */
	pmbd_buffer_syncer_init(buffer);
	if (!buffer->syncer) 
		goto fail;

	printk(KERN_INFO "pmbd: pmbd device buffer (%u) allocated (%lu blocks - block size %u bytes)\n", 
			buffer->buffer_id, buffer->num_blocks, pmbd->pb_size);
	return buffer;

fail:
	if (buffer && buffer->bbi_sort_buffer)
		vfree(buffer->bbi_sort_buffer);
	if (buffer && buffer->bbi_space)
		vfree(buffer->bbi_space);
	if (buffer && buffer->buffer_space)
		vfree(buffer->buffer_space);
	if (buffer)
		kfree(buffer);
	printk(KERN_ERR "%s(%d) vzalloc failed\n", __FUNCTION__, __LINE__);
	return NULL;
}

static int pmbd_buffer_destroy(PMBD_BUFFER_T* buffer)
{
	unsigned id = buffer->buffer_id;

	/* stop syncer first */
	pmbd_buffer_syncer_stop(buffer);
	
	/* flush the buffer to the PM space */
	pmbd_buffer_check_and_flush(buffer, buffer->num_blocks, CALLER_DESTROYER);
	
	/* FIXME: wait for the on-going operations to finish first? */
	if (buffer && buffer->bbi_sort_buffer)
		vfree(buffer->bbi_sort_buffer);
	if (buffer && buffer->bbi_space)
		vfree(buffer->bbi_space);
	if (buffer && buffer->buffer_space)
		vfree(buffer->buffer_space);
	if (buffer)
		kfree(buffer);
	printk(KERN_INFO "pmbd: pmbd device buffer (%u) space freed\n", id);
	return 0;
}

static int pmbd_buffers_create(PMBD_DEVICE_T* pmbd)
{
	int i;
	for (i = 0; i < pmbd->num_buffers; i ++){
		pmbd->buffers[i] = pmbd_buffer_create(pmbd);
		if (pmbd->buffers[i] == NULL)
			return -ENOMEM;
		(pmbd->buffers[i])->buffer_id = i;
	}
	return 0;
}

static int pmbd_buffers_destroy(PMBD_DEVICE_T* pmbd)
{
	int i;
	for (i = 0; i < pmbd->num_buffers; i ++){
		if(pmbd->buffers[i]){
			pmbd_buffer_destroy(pmbd->buffers[i]);
			pmbd->buffers[i] = NULL;
		}
	}
	return 0;
}

static int pmbd_buffer_space_alloc(PMBD_DEVICE_T* pmbd)
{
	int err = 0;
	
	if (pmbd->num_buffers <= 0)
		return 0;

	/* allocate buffers array */
	pmbd->buffers = kzalloc (sizeof(PMBD_BUFFER_T*) * pmbd->num_buffers, GFP_KERNEL);
	if (pmbd->buffers == NULL){
		err = -ENOMEM;
		goto fail;
	}

	/* allocate each buffer */
	err = pmbd_buffers_create(pmbd);
	printk(KERN_INFO "pmbd: pmbd buffer space allocated.\n");
fail:
	return err;
}

static int pmbd_buffer_space_free(PMBD_DEVICE_T* pmbd)
{
	if (pmbd->num_buffers <=0)
		return 0;

	pmbd_buffers_destroy(pmbd);
	kfree(pmbd->buffers);
	pmbd->buffers = NULL;
	printk(KERN_INFO "pmbd: pmbd buffer space freed.\n");

	return 0;
}


/*
 * *************************************************************************
 * High memory based PMBD functions
 * *************************************************************************
 *
 * NOTE:
 * (1) memcpy_fromio() and memcpy_intoio() are used for reading/writing PM,
 *     but it is unnecessary on x86 architectures.
 * (2) Currently we only allocate the reserved space to multiple PMBDs once.  
 *     No dynamic allocate/deallocate of the space is needed so far. 
 */


static void* pmbd_highmem_map(void)
{
	/* 
	 * NOTE: we can also use ioremap_* functions to directly set memory
	 * page attributes when do remapping, but to make it consistent with
	 * using vmalloc(), we do ioremap_cache() and call set_memory_* later. 
	 */

	if (PMBD_USE_PMAP()){
		/* NOTE: If we use pmap(), we don't need to map the reserved
		 * physical memory into the kernel space. Instead we use
		 * pmap_atomic() to make and unmap the to-be-accessed pages on
		 * demand. Since such mapping is private to the processor,
		 * there is no need to change PTE, and TLB shootdown either. 
		 *
		 * Also note that We use PMBD_PMAP_DUMMY_BASE_VA to make the rest
		 * of code happy with a valid virtual address. The real
		 * physical address is calculated as follows:
		 * g_highmem_phys_addr + (vaddr) - PMBD_PMAP_DUMMY_BASE_VA 
		 *
		 * (updated 10/25/2011) 
		 */

		g_highmem_virt_addr = (void*) PMBD_PMAP_DUMMY_BASE_VA;
		g_highmem_curr_addr = g_highmem_virt_addr;
		printk(KERN_INFO "pmbd: PMAP enabled - setting g_highmem_virt_addr to a dummy address (%d)\n", PMBD_PMAP_DUMMY_BASE_VA);
		return g_highmem_virt_addr;

	} else if ((g_highmem_virt_addr = ioremap_prot(g_highmem_phys_addr, g_highmem_size, g_pmbd_cpu_cache_flag))) {

		g_highmem_curr_addr = g_highmem_virt_addr;
		printk(KERN_INFO "pmbd: high memory space remapped (offset: %llu MB, size=%lu MB, cache flag=%s)\n",
			BYTES_TO_MB(g_highmem_phys_addr), BYTES_TO_MB(g_highmem_size), PMBD_CPU_CACHE_FLAG());
		return g_highmem_virt_addr;

	} else {

		printk(KERN_ERR "pmbd: %s(%d) - failed remapping high memory space (offset: %llu MB size=%lu MB)\n",
			__FUNCTION__, __LINE__, BYTES_TO_MB(g_highmem_phys_addr), BYTES_TO_MB(g_highmem_size));
		return NULL;
	}
}

static void pmbd_highmem_unmap(void)
{
	/* de-remap the high memory from kernel address space */
	/* NOTE: if we use pmap(), the g_highmem_virt_addr is fake */
	if (!PMBD_USE_PMAP()){ 
		if(g_highmem_virt_addr){
			iounmap(g_highmem_virt_addr);
			g_highmem_virt_addr = NULL;
			printk(KERN_INFO "pmbd: unmapping high mem space (offset: %llu MB, size=%lu MB)is unmapped\n",
				BYTES_TO_MB(g_highmem_phys_addr), BYTES_TO_MB(g_highmem_size));
		}
	}
	return;
}

static void* hmalloc(uint64_t bytes)
{
	void* rtn = NULL;
	
	/* check if there is still available reserve high memory space */
	if (bytes <= PMBD_HIGHMEM_AVAILABLE_SPACE) {
		rtn = g_highmem_curr_addr;
		g_highmem_curr_addr += bytes;
	} else {
		printk(KERN_ERR "pmbd: %s(%d) - no available space (< %llu bytes) in reserved high memory\n", 
			__FUNCTION__, __LINE__, bytes);
	}
	return rtn;
}

static int hfree(void* addr)
{	
	/* FIXME: no support for dynamic alloc/dealloc in HIGH_MEM space */
	return 0;
}


/*
 * *************************************************************************
 * Device Emulation
 * *************************************************************************
 *
 * Our emulation is based on a simple model - access time and transfer time.
 *
 *     emulated time = access time + (request size / bandwidth)
 *     inserted delay = emulated time - observed time
 *
 * (1) Access time is applied to each request. We check each request's real
 * access time and pad it with an extra delay to meet the designated latency.
 * This is a best-effort solution, which means we just guarantee that no
 * request can be completed with a response time less than the specified
 * latency, but the real access latencies could be higher. In addition, if the
 * total number of threads is larger than the number of available processors,
 * the simulated latencies could be higher, due to CPU saturation. 
 *
 * (2) Transfer time is calculated based on batches
 *     - A batch is a sequence of consecutive requests with a short interval in
 *     between; requests in a batch can be overlapped with each other (parallel
 *     jobs); there is a limit for the total amount of data and the duration of
 *     a batch 
 *     - For each batch, we calculate its target emulated transfer time as
 *     "emul_trans_time = num_sectors/emul_bandwidth" and calculate a delay as
 *     "delay = emul_trans_time - real_trans_time"
 *     - The calculated delay is applied to each batch at the end
 *     - A lock is used to slow down all threads, because bandwidth is a
 *     system-wide specification. In this way, we serialize the threads
 *     accessing the device, which simulates that the device is busy on a task.
 *
 * (3) Two types of delays implemented 
 *     - Sync delay:  if delay is less than 10ms, we keep polling the TSC
 *     counter, which is basically "busy waiting", like spin-lock. This allows
 *     to reach precision of one hundred of cycles
 *     - Async delay: if delay is more than 10ms, we call msleep() to sleep for
 *     a while, which relinquish CPU control, which results in a low precision.
 *     The left-over delay is done with sync delay in nanosecs.  Async delay
 *     cannot be used while holding a lock.
 *
 */ 


static inline uint64_t DIV64_ROUND(uint64_t dividend, uint64_t divisor)
{
	if (divisor > 0) {
		uint32_t quot1 = dividend / divisor;
		uint32_t mod = dividend % divisor;
		uint32_t mult = mod * 2;
		uint32_t quot2 = mult / divisor;
		uint64_t result = quot1 + quot2;
		return result;
	} else { // FIXME: how to handle this?
		printk(KERN_WARNING "pmbd: WARNING - %s(%d) divisor is zero\n", __FUNCTION__, __LINE__);
		return 0;
	}
}

static inline unsigned int get_cpu_freq(void)
{
#if 0
	unsigned int khz = cpufreq_quick_get(0);  /* FIXME: use cpufreq_get() ??? */
	if (!khz) 
		khz = cpu_khz;
	printk("khz=%u, cpu_khz=%u\n", khz, cpu_khz);
#endif
	return cpu_khz;
}

static inline uint64_t _cycle_to_ns(uint64_t cycle, unsigned int khz)
{
	return cycle * 1000000 / khz;
}

static inline uint64_t cycle_to_ns(uint64_t cycle)
{
	unsigned int khz = get_cpu_freq();
	return _cycle_to_ns(cycle, khz);
}

/* 
 * emulate the latency for a given request size/type on a device  
 * @num_sectors: num of sectors to read/write
 * @rw: read or write
 * @pmbd: the pmbd device
 */
static uint64_t cal_trans_time(unsigned int num_sectors, unsigned rw, PMBD_DEVICE_T* pmbd)
{
	uint64_t ns = 0;
	uint64_t bw = (rw == READ) ? pmbd->rdbw : pmbd->wrbw;   /* bandwidth */
	if (bw) {
		uint64_t tmp = num_sectors * PMBD_SECTOR_SIZE;
		uint64_t tt = 1000000000UL >> MB_SHIFT;
		ns += DIV64_ROUND((tmp * tt), bw);
	}
	return ns;
}

static uint64_t cal_access_time(unsigned int num_sectors, unsigned rw, PMBD_DEVICE_T* pmbd)
{
	uint64_t ns = (rw == READ) ? pmbd->rdlat : pmbd->wrlat; /* access time */
	return ns;
}

static inline void sync_slowdown(uint64_t ns)
{
	uint64_t start, now;
	unsigned int khz = get_cpu_freq();
	if (ns) {
		/* 
		 * We keep reading TSC counter to check if the delay has
		 * been passed and this prevents CPU from being scaled down,
		 * which provides a stable estimation of the elapsed time.
		 */
		TIMESTAMP(start);
		while(1) {
			TIMESTAMP(now);
			if (_cycle_to_ns((now-start), khz) > ns)
				break;
		}
	}
	return;
}

static inline void sync_slowdown_cycles(uint64_t cycles)
{

	uint64_t start, now;
	if (cycles){
		/* 
		 * We keep reading TSC counter to check if the delay has
		 * been passed and this prevents CPU from being scaled down,
		 * which provides a stable estimation of the elapsed time.
		 */
		TIMESTAMP(start);
		while(1) {
			TIMESTAMP(now);
			if ((now - start) >= cycles)
				break;
		}
	}
	return;
}

static inline void async_slowdown(uint64_t ns)
{
	uint64_t ms = ns / 1000000;
	uint64_t left = ns - (ms * 1000000);
	/* do ms delay with sleep */
	msleep(ms);		

	/* make up the sub-ms delay */
	sync_slowdown(left);	
}

#if 0
static inline void slowdown_us(unsigned long long us)
{
	set_current_state(TASK_INTERRUPTIBLE);
	schedule_timeout(us * HZ / 1000000);
}
#endif

static void pmbd_slowdown(uint64_t ns, unsigned in_lock)
{
	/* 
	 * NOTE: if the delay is less than 10ms, we use sync_slowdown to keep
	 * polling the CPU cycle counter and busy waiting for the delay elapse;
	 * otherwise, we use msleep() to relinquish the CPU control. 
	 */
	if (ns > MAX_SYNC_SLOWDOWN && !in_lock)
		async_slowdown(ns);
	else if (ns > 0) 
		sync_slowdown(ns);

	return;
}

/*
 * Emulating the transfer time for a batch of requests for specific bandwidth
 * 
 * We group a bunch of consecutive requests as a "batch". In one batch, the
 * interval between two consecutive requests should be small, and the total
 * amount of accessed data should be a good size (not too small, not too
 * large), the duration is reasonable (not too long). For each batch, we
 * estimate the emulated transfer time and compare it with the real transfer
 * time (the start and end time of the batch), if the real transfer time is
 * less than the emulated time, we apply an extra delay to the end of batch for
 * making up the difference. In this way we can make the bandwidth emulation
 * closer to real situation. Note that, since requests from multiple threads
 * could be processed in parallel, so we must slowdown ALL the threads
 * accessing the PMBD device, thus, we use batch_lock to coordinate all threads. 
 *
 * @num_sectors: the num of sectors of the request
 * @rw: read or write
 * @pmbd: the involved pmbd device
 *
 */

static void pmbd_emul_transfer_time(int num_sectors, int rw, PMBD_DEVICE_T* pmbd)
{
	uint64_t interval_ns 	= 0;
	uint64_t duration_ns 	= 0; 
	unsigned new_batch 	= FALSE;
	unsigned end_batch 	= FALSE;
	uint64_t now_cycle 	= 0;

	spin_lock(&pmbd->batch_lock);

	/* get a timestamp for now */
	TIMESTAMP(now_cycle);

	/* if this is the first timestamp */
	if (pmbd->batch_start_cycle[rw] == 0) {
		pmbd->batch_start_cycle[rw] = now_cycle;
		pmbd->batch_end_cycle[rw] = now_cycle;
		goto done;
	}

	/* calculate the interval from the last request */
	if (now_cycle >= pmbd->batch_end_cycle[rw]){
		interval_ns = cycle_to_ns(now_cycle - pmbd->batch_end_cycle[rw]); 
	} else {
		panic(KERN_ERR "%s(%d): timestamp in the past found.\n", __FUNCTION__, __LINE__);
	}

	/* check the interval length (cannot be too distant) */
	if (interval_ns >= PMBD_BATCH_MAX_INTERVAL) {
		/* interval is too big, break it to two batches */
		new_batch = TRUE;
		end_batch = TRUE;
	} else {
		/* still in the same batch, good */
		pmbd->batch_sectors[rw] += num_sectors;
		pmbd->batch_end_cycle[rw] = now_cycle;
	}

	/* check current batch duration (cannot be too long) */
	duration_ns = cycle_to_ns(pmbd->batch_end_cycle[rw] - pmbd->batch_start_cycle[rw]);
	if (duration_ns >= PMBD_BATCH_MAX_DURATION) 
		end_batch = TRUE;

	/* check current batch data amount (cannot be too large) */
	if (pmbd->batch_sectors[rw] >= PMBD_BATCH_MAX_SECTORS)
		end_batch = TRUE;

	/* if the batch ends, check and apply slow-down */
	if (end_batch) {
		/* batch size must be large enough, if not, just skip it */
		if (pmbd->batch_sectors[rw] > PMBD_BATCH_MIN_SECTORS) {
			uint64_t real_ns = cycle_to_ns(pmbd->batch_end_cycle[rw] - pmbd->batch_start_cycle[rw]);
			uint64_t emul_ns = cal_trans_time(pmbd->batch_sectors[rw], rw, pmbd);

			if (emul_ns > real_ns)
				pmbd_slowdown((emul_ns - real_ns), TRUE);
		}

		pmbd->batch_sectors[rw] = 0;
		pmbd->batch_start_cycle[rw] = now_cycle;
		pmbd->batch_end_cycle[rw] = now_cycle;
	}

	/* if a new batch begins, add the first request */
	if (new_batch) {
		pmbd->batch_sectors[rw] = num_sectors;
		pmbd->batch_start_cycle[rw] = now_cycle;
		pmbd->batch_end_cycle[rw] = now_cycle;
	}

done:
	spin_unlock(&pmbd->batch_lock);
	return;
}

/*
 * Emulating access time for a request
 *
 * Different from emulating bandwidths, we emulate access time for each
 * individual access. Right after we simulate the transfer time, we examine
 * the real access time (including transfer time), if the real time is smaller
 * than the specified access time, we slow down the request by applying a delay
 * to make up the difference.  Note that we do not use any lock to coordinate
 * multiple threads for a system-wide "slowdown", but apply this delay on each
 * request individually and separately. 
 *
 * Also note that since we basically use "busy-waiting", when the total number
 * of threads exceeds or be close to the total number of processors, the
 * simulated access time observed at application level could be longer than the
 * specified access time due to high CPU usage. But for each request, after
 * directly examining the duration of being in the make_request() function, the
 * simulated access time is still very precise. 
 *
 */ 
static void pmbd_emul_access_time(uint64_t start, uint64_t end, int num_sectors, int rw, PMBD_DEVICE_T* pmbd)
{
	/* 
	 * Access time can be overlapped with each other, so there is no need
	 * to use a lock to serialize it.
	 * FIXME: should we apply this on each batch or each request?
	 */
	uint64_t real_ns = cycle_to_ns(end - start);
	uint64_t emul_ns = cal_access_time(num_sectors, rw, pmbd);

	if (emul_ns > real_ns)
		pmbd_slowdown((emul_ns - real_ns), FALSE);
		
	return;
}

/* 
 * set the starting hook for PM emulation 
 *
 * @pmbd: pmbd device
 * @num_sectors: sectors being accessed
 * @rw: READ/WRITE
 * return value: the start cycle
 */
static uint64_t emul_start(PMBD_DEVICE_T* pmbd, int num_sectors, int rw)
{
	uint64_t start = 0;
	if (PMBD_DEV_USE_EMULATION(pmbd) && num_sectors > 0) {
		/* start timer here */
		TIMESTAMP(start);	
	}
	return start;
}

/* 
 * set the stopping hook for PM emulation 
 *
 * @pmbd: pmbd device
 * @num_sectors: sectors being accessed
 * @rw: READ/WRITE
 * @start: the starting cycle
 * return value: the end cycle
 */
static uint64_t emul_end(PMBD_DEVICE_T* pmbd, int num_sectors, int rw, uint64_t start)
{
	uint64_t end = 0;
	uint64_t end2 = 0;
	/*
	 * NOTE: emulation can be done in two ways - (1) directly specify the
	 * read/write latencies and bandwidths (2) only specify a relative
	 * slowdown ratio (X), compared to DRAM.
	 *
	 * Also note that if rdsx/wrsx is set, we will ignore
	 * rdlat/wrlat/rdbw/wrbw. 
	 */
	if (PMBD_DEV_USE_EMULATION(pmbd) && num_sectors > 0) {
		/* 
		 * NOTE: we first attempt to meet the target bandwidth and then
		 * latency. This means the actual bandwidth should be close
		 * to the emulated bandwidth, and then we guarantee that the
		 * latency would not be SMALLER than the target latency. 
		 */

		/* emulate the bandwidth first */	
		if (pmbd->rdbw > 0 && pmbd->wrbw > 0) {
			/* emulate transfer time (bandwidth) */
			pmbd_emul_transfer_time(num_sectors, rw, pmbd);
		}

		/* emulate the latency now */
		TIMESTAMP(end);
		if (pmbd->rdlat > 0 || pmbd->wrlat > 0) {
			/* emulate access time (latency) */
			pmbd_emul_access_time(start, end, num_sectors, rw, pmbd);
		}
	}
	/* get the ending timestamp */
	TIMESTAMP(end2);

	return end2;
}

/*
 * *************************************************************************
 * PM space protection functions 
 * - clflush
 * - write protection
 * - write verification
 * - checksum
 * *************************************************************************
 */

/* 
 * flush designated cache lines in CPU cache 
 */

static inline void pmbd_clflush_all(PMBD_DEVICE_T* pmbd)
{
	uint64_t time_p1 = 0;
	uint64_t time_p2 = 0;

	TIMESTAMP(time_p1);
	if (cpu_has_clflush){
#ifdef CONFIG_X86
		wbinvd_on_all_cpus();
#else
		printk(KERN_WARNING "pmbd: WARNING - %s(%d) flush_cache_all() not implemented\n", __FUNCTION__, __LINE__);
#endif
	}
	TIMESTAMP(time_p2);

	/* emulating slowdown */
	if(PMBD_DEV_USE_SLOWDOWN(pmbd))
		pmbd_rdwr_slowdown((pmbd), WRITE, time_p1, time_p2);

	/* update time statistics */
	if(PMBD_USE_TIMESTAT()){
		int cid = CUR_CPU_ID();
		PMBD_STAT_T* pmbd_stat = pmbd->pmbd_stat;
		pmbd_stat->cycles_clflushall[WRITE][cid] += time_p2 - time_p1;
	}
	return;
}

static inline void pmbd_clflush_range(PMBD_DEVICE_T* pmbd, void* dst, size_t bytes)
{
	uint64_t time_p1 = 0;
	uint64_t time_p2 = 0;

	TIMESTAMP(time_p1);
	if (cpu_has_clflush){
		clflush_cache_range(dst, bytes);
	}
	TIMESTAMP(time_p2);

	/* emulating slowdown */
	if(PMBD_DEV_USE_SLOWDOWN(pmbd))
		pmbd_rdwr_slowdown((pmbd), WRITE, time_p1, time_p2);

	/* update time statistics */
	if(PMBD_USE_TIMESTAT()){
		int cid = CUR_CPU_ID();
		PMBD_STAT_T* pmbd_stat = pmbd->pmbd_stat;
		pmbd_stat->cycles_clflush[WRITE][cid] += time_p2 - time_p1;
	}
	return;
}


/* 
 * Write-protection 
 *
 * Being used as storage, PMBD needs to provide certain protection on accidental
 * change caused by wild pointers. So we initialize all the PM pages as
 * read-only; before we perform write operations into PM space, we set the
 * pages writable, after done, we set it back to read-only. This would
 * introduce extra overhead. However, this is a realistic solution to tackle
 * wild pointer problem.
 *
 */

/*
 * set PM pages to read-only
 * @addr -  the starting virtual address (PM space)
 * @bytes - the range in bytes
 * @on_access - this change command from request or during creating/destroying
 */

static inline void pmbd_set_pages_ro(PMBD_DEVICE_T* pmbd, void* addr, uint64_t bytes, unsigned on_access)
{
	if (PMBD_USE_WRITE_PROTECTION()) {
		/* FIXME: type conversion happens here */
		/* FIXME: add range and bytes check here?? - not so necessary */
		uint64_t time_p1 = 0;
		uint64_t time_p2 = 0;
		unsigned long offset = (unsigned long) addr;
		unsigned long vaddr = PAGE_TO_VADDR(VADDR_TO_PAGE(offset));
		int num_pages = VADDR_TO_PAGE(offset + bytes - 1) - VADDR_TO_PAGE(offset) + 1;

		if(!(VADDR_IN_PMBD_SPACE(pmbd, addr) && VADDR_IN_PMBD_SPACE(pmbd, addr + bytes-1)))
			printk(KERN_WARNING "pmbd: WARNING - %s(%d): PM space range exceeded (%lu : %d pages)\n", 
					__FUNCTION__, __LINE__, vaddr, num_pages);

		TIMESTAMP(time_p1);
		set_memory_ro(vaddr, num_pages);
		TIMESTAMP(time_p2);

		/* update time statistics */
//		if(PMBD_USE_TIMESTAT() && on_access){
		if(PMBD_USE_TIMESTAT()){
			int cid = CUR_CPU_ID();
			PMBD_STAT_T* pmbd_stat = pmbd->pmbd_stat;
			pmbd_stat->cycles_setpages_ro[WRITE][cid] += time_p2 - time_p1;
		}
	}
	return;
}

static inline void pmbd_set_pages_rw(PMBD_DEVICE_T* pmbd, void* addr, uint64_t bytes, unsigned on_access)
{
	if (PMBD_USE_WRITE_PROTECTION()) {
		uint64_t time_p1 = 0;
		uint64_t time_p2 = 0;
		unsigned long offset = (unsigned long) addr;
		unsigned long vaddr = PAGE_TO_VADDR(VADDR_TO_PAGE(offset));
		int num_pages = VADDR_TO_PAGE(offset + bytes - 1) - VADDR_TO_PAGE(offset) + 1;

		if(!(VADDR_IN_PMBD_SPACE(pmbd, addr) && VADDR_IN_PMBD_SPACE(pmbd, addr + bytes-1)))
			printk(KERN_WARNING "pmbd: WARNING - %s(%d): PM space range exceeded (%lu : %d pages)\n", __FUNCTION__, __LINE__, vaddr, num_pages);

		TIMESTAMP(time_p1);
		set_memory_rw(vaddr, num_pages);
		TIMESTAMP(time_p2);

		/* update time statistics */
//		if(PMBD_USE_TIMESTAT() && on_access){
		if(PMBD_USE_TIMESTAT()){
			int cid = CUR_CPU_ID();
			PMBD_STAT_T* pmbd_stat = pmbd->pmbd_stat;
			pmbd_stat->cycles_setpages_rw[WRITE][cid] += time_p2 - time_p1;
		}
	}
	return;
}


/*
 * Write verification (EXPERIMENTAL)
 *
 * Note: Even we do write protection by setting PM space read-only, there is
 * still a short vulnerable window when we write pages into PM space - between
 * the time when the pages are set RW and the time when the pages are set back
 * to RO. So we need to verify that no data has been changed during this window
 * by reading out the written data and comparing with the source data. 
 *
 */


static inline int pmbd_verify_wr_pages_pmap(PMBD_DEVICE_T* pmbd, void* pmbd_dummy_va, void* ram_va, size_t bytes)
{

	unsigned long flags = 0;

	/*NOTE: we assume src is starting from 0 */
	uint64_t pa = (uint64_t) PMBD_PMAP_VA_TO_PA(pmbd_dummy_va);

	/* disable interrupt (FIXME: do we need to do this?)*/	
	DISABLE_SAVE_IRQ(flags);

	/* do the real work */
	while(bytes){
		uint64_t pfn = (pa >> PAGE_SHIFT);	// page frame number
		unsigned off = pa & (~PAGE_MASK);	// offset in one page
		unsigned size = MIN_OF((PAGE_SIZE - off), bytes); // the size to copy 

		/* map it */
		void * map = pmap_atomic_pfn(pfn, pmbd, WRITE);
		void * pmbd_va = map + off;

		/* do memcopy */
		if (memcmp(pmbd_va, ram_va, size)){
			punmap_atomic(map, pmbd, WRITE);
			goto bad;
		}

		/* unmap it */
		punmap_atomic(map, pmbd, WRITE);

		/* prepare the next iteration */
		ram_va  += size;
		bytes 	-= size;
		pa 	+= size;
	}
	
	/* re-enable interrupt */
	ENABLE_RESTORE_IRQ(flags);
	return 0;

bad:
	ENABLE_RESTORE_IRQ(flags);
	return -1;
}


static inline int pmbd_verify_wr_pages_nopmap(PMBD_DEVICE_T* pmbd, void* pmbd_va, void* ram_va, size_t bytes)
{
	if (memcmp(pmbd_va, ram_va, bytes)) 
		return -1;
	else
		return 0;
}

static inline int pmbd_verify_wr_pages(PMBD_DEVICE_T* pmbd, void* pmbd_va, void* ram_va, size_t bytes)
{
	int rtn = 0;
	uint64_t time_p1, time_p2;

	TIMESTAT_POINT(time_p1);

	/* check it */
	if (PMBD_USE_PMAP())
		rtn = pmbd_verify_wr_pages_pmap(pmbd, pmbd_va, ram_va, bytes);
	else
		rtn = pmbd_verify_wr_pages_nopmap(pmbd, pmbd_va, ram_va, bytes);

	/* found mismatch */
	if (rtn < 0){
		panic("pmbd: *** writing into PM failed (error found) ***\n");
		return -1;
	}

	TIMESTAT_POINT(time_p2);

	/* timestamp */
	if(PMBD_USE_TIMESTAT()){
		int cid = CUR_CPU_ID();
		PMBD_STAT_T* pmbd_stat = pmbd->pmbd_stat;
		pmbd_stat->cycles_wrverify[WRITE][cid] += time_p2 - time_p1;
	}

	return 0;
}

/*
 * Checksum (EXPERIMENTAL)
 *
 * Note: With write-protection and write verification, we can largely reduce
 * the risk of PM data corruption caused by wild in-kernel pointers, however,
 * it is still possible that some data gets corrupted (e.g. PM pages are
 * maliciously changed to writable). Thus, we need to provide another layer of
 * protection by checksuming the PM pages. When writing a page, we compute a
 * checksum and write it into memory; When reading a page, we compute its
 * checksum and compare it with the stored checksum. If a mismatch is found,
 * it indicates that either PM data or the checksum has been corrupted. 
 *
 * FIXME:
 * (1) checksum should be stored in PM space, currently we just store it in RAM.
 * (2) probably we should use the CPU cache to speed up and avoid reading the same 
 *     chunk of data again. 
 * (3) currently we always allocate checksum space, whether we enable or disable it
 * in the module config options; may need to make it more efficient in the future. 
 *
 */ 


static int pmbd_checksum_space_alloc(PMBD_DEVICE_T* pmbd)
{
	int err = 0;

	/* allocate checksum space */
	pmbd->checksum_space= vmalloc(PMBD_CHECKSUM_TOTAL_NUM(pmbd) * sizeof(PMBD_CHECKSUM_T));
	if (pmbd->checksum_space){
		memset(pmbd->checksum_space, 0, (PMBD_CHECKSUM_TOTAL_NUM(pmbd) * sizeof(PMBD_CHECKSUM_T)));
		printk(KERN_INFO "pmbd(%d): checksum space is allocated\n", pmbd->pmbd_id);
	} else {
		err = -ENOMEM;
	}

	/* allocate checksum buffer space */
	pmbd->checksum_iomem_buf = vmalloc(pmbd->checksum_unit_size);
	if (pmbd->checksum_iomem_buf){
		memset(pmbd->checksum_iomem_buf, 0, pmbd->checksum_unit_size);
		printk(KERN_INFO "pmbd(%d): checksum iomem buffer space is allocated\n", pmbd->pmbd_id);
	} else {
		err = -ENOMEM;
	}

	return err;
}

static int pmbd_checksum_space_free(PMBD_DEVICE_T* pmbd)
{
	if (pmbd->checksum_space) {
		vfree(pmbd->checksum_space);
		pmbd->checksum_space = NULL;
		printk(KERN_INFO "pmbd(%d): checksum space is freed\n", pmbd->pmbd_id);
	}
	if (pmbd->checksum_iomem_buf) {
		vfree(pmbd->checksum_iomem_buf);
		pmbd->checksum_iomem_buf = NULL;
		printk(KERN_INFO "pmbd(%d): checksum iomem buffer space is freed\n", pmbd->pmbd_id);
	}
	return 0;
}


/*
 * Derived from linux/lib/crc32.c GPL v2
 */
static unsigned int crc32_my(unsigned char const *p, unsigned int len)
{
        int i;
        unsigned int crc = 0;
        while (len--) {
                crc ^= *p++;
                for (i = 0; i < 8; i++)
                        crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
        }
       return crc;
}

static inline PMBD_CHECKSUM_T pmbd_checksum_func(void* data, size_t size)
{
	return crc32_my(data, size);
}

/*
 * calculate the checksum for a chunksum unit
 * @pmbd: the pmbd device
 * @data: the virtual address of the target data (must be aligned to the
 * checksum unit boundaries)
 */ 


static inline PMBD_CHECKSUM_T pmbd_cal_checksum(PMBD_DEVICE_T* pmbd, void* data)
{
	void* vaddr = data;
	size_t size = pmbd->checksum_unit_size;
	PMBD_CHECKSUM_T chk = 0;

#if 0
#ifndef CONFIG_X86
	/* 
	 * Note: If we are directly using vmalloc(), we don't have to copy it
 	 * to the checksum buffer; however, if we are using High Memory, we should not
	 * directly dereference the ioremapped data (on non-x86 platform), so we have to
	 * first copy it to a temporary buffer, this extra copy would significantly
	 * slows down operations. We do this here is just to remove this extra copy on
	 * x86 platform.  (see kernel/Documents/IO-mapping.txt)
	 *
	 */
	if (PMBD_DEV_USE_HIGHMEM(pmbd) && VADDR_IN_PMBD_SPACE(pmbd, data)) {
		memcpy_fromio(pmbd->checksum_iomem_buf, data, pmbd->checksum_unit_size);
		vaddr = pmbd->checksum_iomem_buf;
	} 
#endif
#endif

	if (pmbd->checksum_unit_size != PAGE_SIZE){
		panic("ERR: %s(%d) checksum unit size (%u) must be %lu\n", __FUNCTION__, __LINE__, pmbd->checksum_unit_size, PAGE_SIZE);
		return 0;
	}

	/* FIXME: do we really need to copy the data out first (if not pmap)*/
	memcpy_from_pmbd(pmbd, pmbd->checksum_iomem_buf, data, pmbd->checksum_unit_size);

	/* calculate the checksum */
	vaddr = pmbd->checksum_iomem_buf;
	chk = pmbd_checksum_func(vaddr, size);

	return chk;
}

static int pmbd_checksum_on_write(PMBD_DEVICE_T* pmbd, void* vaddr, size_t bytes)
{
	unsigned long i;
	unsigned long ck_id_s = VADDR_TO_CHECKSUM_IDX(pmbd, vaddr);
	unsigned long ck_id_e = VADDR_TO_CHECKSUM_IDX(pmbd, (vaddr + bytes - 1));

	uint64_t time_p1, time_p2;

	TIMESTAT_POINT(time_p1);

	for (i = ck_id_s; i <= ck_id_e; i ++){
		void* data = CHECKSUM_IDX_TO_VADDR(pmbd, i);
		void* chk = CHECKSUM_IDX_TO_CKADDR(pmbd, i); 

		PMBD_CHECKSUM_T checksum = pmbd_cal_checksum(pmbd, data);
		memcpy(chk, &checksum, sizeof(PMBD_CHECKSUM_T));
	}

	TIMESTAT_POINT(time_p2);

	/* timestamp */
	if(PMBD_USE_TIMESTAT()){
		int cid = CUR_CPU_ID();
		PMBD_STAT_T* pmbd_stat = pmbd->pmbd_stat;
		pmbd_stat->cycles_checksum[WRITE][cid] += time_p2 - time_p1;
	}
	return 0;
}

static int pmbd_checksum_on_read(PMBD_DEVICE_T* pmbd, void* vaddr, size_t bytes)
{
	unsigned long i;
	unsigned long ck_id_s = VADDR_TO_CHECKSUM_IDX(pmbd, vaddr);
	unsigned long ck_id_e = VADDR_TO_CHECKSUM_IDX(pmbd, (vaddr + bytes - 1));

	uint64_t time_p1, time_p2;
	TIMESTAT_POINT(time_p1);

	for (i = ck_id_s; i <= ck_id_e; i ++){
		void* data = CHECKSUM_IDX_TO_VADDR(pmbd, i);
		void* chk = CHECKSUM_IDX_TO_CKADDR(pmbd, i); 

		PMBD_CHECKSUM_T checksum = pmbd_cal_checksum(pmbd, data);
		if (memcmp(chk, &checksum, sizeof(PMBD_CHECKSUM_T))){
			printk(KERN_WARNING "pmbd(%d): checksum mismatch found!", pmbd->pmbd_id);
		}
	}

	TIMESTAT_POINT(time_p2);

	/* timestamp */
	if(PMBD_USE_TIMESTAT()){
		int cid = CUR_CPU_ID();
		PMBD_STAT_T* pmbd_stat = pmbd->pmbd_stat;
		pmbd_stat->cycles_checksum[READ][cid] += time_p2 - time_p1;
	}

	return 0;
}

#if 0
/* WARN: Calculating checksum for a big PM space is slow and could lockup system*/
static int pmbd_checksum_space_init(PMBD_DEVICE_T* pmbd)
{
	unsigned long i;
	PMBD_CHECKSUM_T checksum = pmbd_cal_checksum(pmbd, pmbd->mem_space);
	unsigned long ck_s = VADDR_TO_CHECKSUM_IDX(pmbd, PMBD_MEM_SPACE_FIRST_BYTE(pmbd));
	unsigned long ck_e = VADDR_TO_CHECKSUM_IDX(pmbd, PMBD_MEM_SPACE_LAT_BYTE(pmbd));

	for (i = ck_s; i <= ck_e; i ++){
		void* dst = CHECKSUM_IDX_TO_CKADDR(pmbd, i); 
		memcpy(dst, &checksum, sizeof(PMBD_CHECKSUM_T));
	}
	return 0;
}
#endif

/*
 * locks
 *
 * Note:  We should prevent multiple threads from concurrently accessing the same
 * chunk of data. For example, if two writes access the same page, the PM page
 * could be corrupted with a merged content of two. So we allocate one spinlock
 * for each 4KB PM page. When read/writing PM data, we lock the related pages
 * and unlock them after done.  
 *
 */

static int pmbd_lock_on_access(PMBD_DEVICE_T* pmbd, sector_t sector, size_t bytes)
{
	if (PMBD_USE_LOCK()) {
		PBN_T pbn = 0;
		PBN_T pbn_s = SECTOR_TO_PBN(pmbd, sector);
		PBN_T pbn_e = BYTE_TO_PBN(pmbd, (SECTOR_TO_BYTE(sector) + bytes - 1));

		for (pbn = pbn_s; pbn <= pbn_e; pbn ++) {
			PMBD_PBI_T* pbi 	= PMBD_BLOCK_PBI(pmbd, pbn);
			spin_lock(&pbi->lock);
		}
	}
	return 0;
}

static int pmbd_unlock_on_access(PMBD_DEVICE_T* pmbd, sector_t sector, size_t bytes)
{
	if (PMBD_USE_LOCK()){
		PBN_T pbn = 0;
		PBN_T pbn_s = SECTOR_TO_PBN(pmbd, sector);
		PBN_T pbn_e = BYTE_TO_PBN(pmbd, (SECTOR_TO_BYTE(sector) + bytes - 1));

		for (pbn = pbn_s; pbn <= pbn_e; pbn ++) {
			PMBD_PBI_T* pbi 	= PMBD_BLOCK_PBI(pmbd, pbn);
			spin_unlock(&pbi->lock);
		}
	}
	return 0;
}

/*
 **************************************************************************
 * Unbuffered Read/write functions
 **************************************************************************
 */
static void copy_to_pmbd_unbuffered(PMBD_DEVICE_T* pmbd, void *src, sector_t sector, size_t bytes, unsigned do_fua)
{
	void *dst;

	dst = pmbd->mem_space + sector * pmbd->sector_size;

	/* lock the pages */
	pmbd_lock_on_access(pmbd, sector, bytes);

	/* set the pages writable */
	/* if we use CR0/WP to temporarily switch the writable permission, 
 	 * we don't have to change the PTE attributes directly */
	if (PMBD_DEV_USE_WPMODE_PTE(pmbd))
		pmbd_set_pages_rw(pmbd, dst, bytes, TRUE);

	/* do memcpy */
	memcpy_to_pmbd(pmbd, dst, src, bytes, do_fua);

	/* finish up */
	/* set the pages read-only */
	if (PMBD_DEV_USE_WPMODE_PTE(pmbd)) 
		pmbd_set_pages_ro(pmbd, dst, bytes, TRUE);

	/* verify that the write operation succeeded */
	if(PMBD_USE_WRITE_VERIFICATION())
		pmbd_verify_wr_pages(pmbd, dst, src, bytes);

	/* generate check sum */
	if (PMBD_USE_CHECKSUM())
		pmbd_checksum_on_write(pmbd, dst, bytes);

	/* unlock the pages */
	pmbd_unlock_on_access(pmbd, sector, bytes);

	return;
}


static void copy_from_pmbd_unbuffered(PMBD_DEVICE_T* pmbd, void *dst, sector_t sector, size_t bytes)
{
	void *src = pmbd->mem_space + sector * pmbd->sector_size;

	/* lock the pages */
	pmbd_lock_on_access(pmbd, sector, bytes);

	/* check checksum first */
	if (PMBD_USE_CHECKSUM())
		pmbd_checksum_on_read(pmbd, src, bytes);

	/* read it out*/
	memcpy_from_pmbd(pmbd, dst, src, bytes);

	/* unlock the pages */
	pmbd_unlock_on_access(pmbd, sector, bytes);

	return;
}


/*
 * *************************************************************************
 * Read/write functions 
 * *************************************************************************
 */ 

static void copy_to_pmbd(PMBD_DEVICE_T* pmbd, void *dst, sector_t sector, size_t bytes, unsigned do_fua)
{
	if (PMBD_DEV_USE_BUFFER(pmbd)){
		copy_to_pmbd_buffered(pmbd, dst, sector, bytes);
		if (do_fua){
			/* NOTE: 
			 * When we use a FUA, if the buffer is enabled, we
			 * still write into the buffer first, but then we
			 * directly write into the PM space without using the
			 * buffer again.  This is suboptimal (we need to write
			 * the data twice), however, it is better than changing
			 * the buffering code. 
			 */
			copy_to_pmbd_unbuffered(pmbd, dst, sector, bytes, do_fua);
		}
	}else
		copy_to_pmbd_unbuffered(pmbd, dst, sector, bytes, do_fua);
	return;
}

static void copy_from_pmbd(PMBD_DEVICE_T* pmbd, void *dst, sector_t sector, size_t bytes)
{
	if (PMBD_DEV_USE_BUFFER(pmbd))
		copy_from_pmbd_buffered(pmbd, dst, sector, bytes);
	else
		copy_from_pmbd_unbuffered(pmbd, dst, sector, bytes);
	return;
}

static int pmbd_seg_read_write(PMBD_DEVICE_T* pmbd, struct page *page, unsigned int len, 
					unsigned int off, int rw, sector_t sector, unsigned do_fua)
{
	void *mem;
	int err = 0;

	mem = kmap_atomic(page);

	if (rw == READ) {
		copy_from_pmbd(pmbd, mem + off, sector, len);
		flush_dcache_page(page);
	} else {
		flush_dcache_page(page);
		copy_to_pmbd(pmbd, mem + off, sector, len, do_fua);
	}

	kunmap_atomic(mem);

	return err;
}

static int pmbd_do_bvec(PMBD_DEVICE_T* pmbd, struct page *page,
			unsigned int len, unsigned int off, int rw, sector_t sector, unsigned do_fua)
{
	return pmbd_seg_read_write(pmbd, page, len, off, rw, sector, do_fua);
}

/*
 * Handling write barrier 
 * @pmbd: the pmbd device
 *
 * When the application sends fsync(), a bio labeled with WRITE_BARRIER would be 
 * received by pmbd_make_request(), and we need to stop accepting new incoming 
 * writes (by locking pmbd->wr_barrier_lock), and wait for the on-the-fly writes
 * to complete (by checking pmbd->num_flying_wr), then if we use buffer, we flush
 * the whole entire DRAM buffer with clflush enabled. If we do not use the buffer,
 * we flush the CPU cache to let all the data securely be written into PM. 
 *
 */


static void __x86_mfence_all(void *arg)
{
	unsigned long cache = (unsigned long)arg;
	if (cache && boot_cpu_data.x86 >= 4)
		mfence();
}

static void x86_mfence_all(unsigned long cache)
{
	BUG_ON(irqs_disabled());
	on_each_cpu(__x86_mfence_all, (void*) cache, 1);
}

static inline void pmbd_mfence_all(PMBD_DEVICE_T* pmbd)
{
	x86_mfence_all(1);
}


static void __x86_sfence_all(void *arg)
{
	unsigned long cache = (unsigned long)arg;
	if (cache && boot_cpu_data.x86 >= 4)
		sfence();
}

static void x86_sfence_all(unsigned long cache)
{
	BUG_ON(irqs_disabled());
	on_each_cpu(__x86_sfence_all, (void*) cache, 1);
	
}

static inline void pmbd_sfence_all(PMBD_DEVICE_T* pmbd)
{
	x86_sfence_all(1);
}

static int pmbd_write_barrier(PMBD_DEVICE_T* pmbd)
{
	unsigned i;

	/* blocking incoming writes */
	spin_lock(&pmbd->wr_barrier_lock);

	/* wait for all on-the-fly writes to finish first */
	while (atomic_read(&pmbd->num_flying_wr) != 0)
		;

	if (PMBD_DEV_USE_BUFFER(pmbd)){
		/* if buffer is used, flush the entire buffer */
		for (i = 0; i < pmbd->num_buffers; i ++){
			PMBD_BUFFER_T* buffer = pmbd->buffers[i];
			pmbd_buffer_check_and_flush(buffer, buffer->num_blocks, CALLER_DESTROYER);
		}
	} 

	/* 
	 * considering the following:
	 * UC (write-through): 		strong ordering, we do nothing
	 * UC-Minus:			strong ordering (may be overridden by WC), we use sfence, do nothing
	 * WC (write-combining):	sfence should be used after each write, so we do nothing
	 * WB (write-back):		non-temporal store : sfence is used, do nothing
	 * 				clflush/mfence: mfence is used in clflush_cache_range(), do nothing
	 * 				nothing: wbinvd needed to drop the entire cache
	 */
	if (PMBD_CPU_CACHE_USE_WB()){
		if (PMBD_USE_NTS()){
			/* sfence is used after each movntq, so it is safe, we
 			* do nothing, just stop accepting any incoming requests */
		} else if (PMBD_USE_CLFLUSH()) {
			/* if use clflush/mfence to sync I/O, we do nothing*/
//			pmbd_mfence_all(pmbd);
		} else {
			/* if no sync operations, we have to drop the entire cache */
			pmbd_clflush_all(pmbd);
		}
	} else if (PMBD_CPU_CACHE_USE_WC() || PMBD_CPU_CACHE_USE_UM()) {
		/* if using WC, sfence should used already, so do nothing */
	
	} else if (PMBD_CPU_CACHE_USE_UC()) {
		/* strong ordering is used, no need to do anything else*/
	} else {
		panic("%s(%d): something is wrong\n", __FUNCTION__, __LINE__);
	}

	/* unblock incoming writes */
	spin_unlock(&pmbd->wr_barrier_lock);
	return 0;
}


//	#define BIO_WR_BARRIER(BIO)	(((BIO)->bi_rw & REQ_FLUSH) == REQ_FLUSH)
//	#define BIO_WR_BARRIER(BIO)	((BIO)->bi_rw & (REQ_FLUSH | REQ_FLUSH_SEQ))
	#define BIO_WR_BARRIER(BIO)	(((BIO)->bi_rw & WRITE_FLUSH) == WRITE_FLUSH)
	#define BIO_WR_FUA(BIO)		(((BIO)->bi_rw & WRITE_FUA) == WRITE_FUA)
	#define BIO_WR_SYNC(BIO)	(((BIO)->bi_rw & WRITE_SYNC) == WRITE_SYNC)

static void pmbd_make_request(struct request_queue *q, struct bio *bio)
{
	int i 	= 0;
	int err = -EIO;
	uint64_t start = 0;
	uint64_t end   = 0;
	struct bio_vec *bvec;
	int rw 	= bio_rw(bio);
	sector_t sector = bio->bi_sector;
	int num_sectors = bio_sectors(bio);
	struct block_device *bdev = bio->bi_bdev;
	PMBD_DEVICE_T *pmbd = bdev->bd_disk->private_data;
	PMBD_STAT_T* pmbd_stat = pmbd->pmbd_stat;
	unsigned bio_is_write_fua = FALSE;
	unsigned bio_is_write_barrier = FALSE;
	unsigned do_fua = FALSE;
	uint64_t time_p1, time_p2, time_p3, time_p4, time_p5, time_p6;
	time_p1 = time_p2 = time_p3 = time_p4 = time_p5 = time_p6 = 0;


	TIMESTAT_POINT(time_p1);
//	printk("ACCESS: %u %d %X %d\n", sector, num_sectors, bio->bi_rw, rw);
	
	/* update rw */
	if (rw == READA)
		rw = READ;
	if (rw != READ && rw != WRITE)
		panic("pmbd: %s(%d) found request not read or write either\n", __FUNCTION__, __LINE__);

	/* handle write barrier (we don't do for BIO_WR_SYNC(bio) anymore*/
	if (BIO_WR_BARRIER(bio)){
		/* 
		 * Note: Linux kernel 2.6.37 and later use file systems and FUA 
		 * to ensure data reliability, rather than write barriers. 
		 * See http://monolight.cc/2011/06/barriers-caches-filesystems
		 */
		bio_is_write_barrier = TRUE;
//		printk(KERN_INFO "pmbd: received barrier request %u %d %lx %d\n", (unsigned int) sector, num_sectors, bio->bi_rw, rw);

		if (PMBD_USE_WB())
			pmbd_write_barrier(pmbd);
	}

	if (BIO_WR_FUA(bio)){
		bio_is_write_fua = TRUE;
//		printk(KERN_INFO "pmbd: received FUA request %u %d %lx %d\n", (unsigned int) sector, num_sectors, bio->bi_rw, rw);

		if (PMBD_USE_FUA())
			do_fua = TRUE;
	}

	TIMESTAT_POINT(time_p2);

	/* blocking write until write barrier is done */
	if (rw == WRITE){
		spin_lock(&pmbd->wr_barrier_lock);
		spin_unlock(&pmbd->wr_barrier_lock);
	}

	/* increment on-the-fly writes counter */
	atomic_inc(&pmbd->num_flying_wr);

	/* starting emulation */
	if (PMBD_DEV_SIM_DEV(pmbd))
		start = emul_start(pmbd, num_sectors, rw);

	/* check if out of range */
	if (sector + (bio->bi_size >> SECTOR_SHIFT) > get_capacity(bdev->bd_disk)){
		printk(KERN_WARNING "pmbd: request exceeds the PMBD capacity\n");
		TIMESTAT_POINT(time_p3);
		goto out;
	}

//	printk("DEBUG: ACCESS %lu %d %d\n", sector, num_sectors, rw);

	/*
	 * NOTE: some applications (e.g. fdisk) call fsync() to request
	 * flushing dirty data from the buffer cache. In default, fsync() is
	 * linked to blkdev_fsync() in the def_blk_fops structure, and
	 * blkdev_fsync() will call blkdev_issue_flush(), which generates an
	 * empty bio carrying a write barrier down to the block device through
	 * generic_make_request(), which calls pmbd_make_request() in turn. If
	 * we don't set err=0 here, this error message would pass upwards back
	 * to the application. For example, fdisk will fail and reports error
	 * when trying to write the partition table before it exits. Thus we
	 * must reset the error code here if the bio is empty. Also note that
	 * we directly check the bio size, rather than using bio_wr_barrier(),
	 * to handle other cases.
	 *
 	 */
	if (num_sectors == 0) { 
		err = 0;
		TIMESTAT_POINT(time_p3);
		goto out;
	} 

	/* update the access time*/
	PMBD_DEV_UPDATE_ACCESS_TIME(pmbd);

	TIMESTAT_POINT(time_p3);

	/* 
	 * Do read/write now. We first perform the operation, then check how
	 * long it actually takes to finish the operation, then we calculate an
	 * emulated time for a given slow-down model, if the actual access time
	 * is less than the emulated time, we just make up the difference to
	 * emulate a slower device. 
	 */
	bio_for_each_segment(bvec, bio, i) {
		unsigned int len = bvec->bv_len;
		err = pmbd_do_bvec(pmbd, bvec->bv_page, len, 
					bvec->bv_offset, rw, sector, do_fua);
		if (err)
			break;
		sector += len >> SECTOR_SHIFT;
	}

out:
	TIMESTAT_POINT(time_p4);

	bio_endio(bio, err);

	TIMESTAT_POINT(time_p5);

	/* ending emulation (simmode0)*/
	if (PMBD_DEV_SIM_DEV(pmbd))
		end = emul_end(pmbd, num_sectors, rw, start);

	/* decrement on-the-fly writes counter */
	atomic_dec(&pmbd->num_flying_wr);

	TIMESTAT_POINT(time_p6);

	/* update statistics data */
	spin_lock(&pmbd_stat->stat_lock);
	if (rw == READ) {
		pmbd_stat->num_requests_read ++;
		pmbd_stat->num_sectors_read += num_sectors;
	} else {
		pmbd_stat->num_requests_write ++;
		pmbd_stat->num_sectors_write += num_sectors;
	}
	if (bio_is_write_barrier)
		pmbd_stat->num_write_barrier ++;
	if (bio_is_write_fua)
		pmbd_stat->num_write_fua ++;
	spin_unlock(&pmbd_stat->stat_lock);

	/* cycles */
	if (PMBD_USE_TIMESTAT()){
		int cid = CUR_CPU_ID();
		pmbd_stat->cycles_total[rw][cid] 	+= time_p6 - time_p1;
		pmbd_stat->cycles_wb[rw][cid] 		+= time_p2 - time_p1;		/* write barrier */
		pmbd_stat->cycles_prepare[rw][cid] 	+= time_p3 - time_p2;	
		pmbd_stat->cycles_work[rw][cid] 		+= time_p4 - time_p3;
		pmbd_stat->cycles_endio[rw][cid] 	+= time_p5 - time_p4;
		pmbd_stat->cycles_finish[rw][cid] 	+= time_p6 - time_p5;
	}
}


/*
 **************************************************************************
 * Allocating memory space for PMBD device 
 **************************************************************************
 */ 

/*
 * Set the page attributes for the PMBD backstore memory space
 *  - WB: cache enabled, write back (default)
 *  - WC: cache disabled, write through, speculative writes combined
 *  - UC: cache disabled, write through, no write combined
 *  - UC-Minus: the same as UC 
 *
 * REF: 
 * - http://www.kernel.org/doc/ols/2008/ols2008v2-pages-135-144.pdf
 * - http://www.mjmwired.net/kernel/Documentation/x86/pat.txt
 */ 

static int pmbd_set_pages_cache_flags(PMBD_DEVICE_T* pmbd)
{
	if (pmbd->mem_space && pmbd->num_sectors) {
		/* NOTE: we convert it here with no problem on 64-bit system */
		unsigned long vaddr = (unsigned long) pmbd->mem_space;
		int num_pages = PMBD_MEM_TOTAL_PAGES(pmbd);

		printk(KERN_INFO "pmbd: setting %s PTE flags (%lx:%d)\n", pmbd->pmbd_name, vaddr, num_pages);
		set_pages_cache_flags(vaddr, num_pages);
		printk(KERN_INFO "pmbd: setting %s PTE flags done.\n", pmbd->pmbd_name);
	}
	return 0;
}

static int pmbd_reset_pages_cache_flags(PMBD_DEVICE_T* pmbd)
{
	if (pmbd->mem_space){
		unsigned long vaddr = (unsigned long) pmbd->mem_space;
		int num_pages = PMBD_MEM_TOTAL_PAGES(pmbd);
		set_memory_wb(vaddr, num_pages);
		printk(KERN_INFO "pmbd: %s pages cache flags are reset to WB\n", pmbd->pmbd_name);
	}
	return 0;
}


/*
 * Allocate/free memory backstore space for PMBD devices
 */
static int pmbd_mem_space_alloc (PMBD_DEVICE_T* pmbd)
{
	int err = 0;

	/* allocate PM memory space */
	if (PMBD_DEV_USE_VMALLOC(pmbd)){
		pmbd->mem_space = vmalloc (PMBD_MEM_TOTAL_BYTES(pmbd));
	} else if (PMBD_DEV_USE_HIGHMEM(pmbd)){
		pmbd->mem_space = hmalloc (PMBD_MEM_TOTAL_BYTES(pmbd));
	}

	if (pmbd->mem_space) {
#if 0
		/* FIXME: No need to do this. It's slow, system could be locked up */
		memset(pmbd->mem_space, 0, pmbd->sectors * pmbd->sector_size);
#endif
		printk(KERN_INFO "pmbd: /dev/%s is created [%lu : %llu MBs]\n", 
				pmbd->pmbd_name, (unsigned long) pmbd->mem_space, SECTORS_TO_MB(pmbd->num_sectors));
	} else {
		printk(KERN_ERR "pmbd: %s(%d): PMBD space allocation failed\n", __FUNCTION__, __LINE__);
		err = -ENOMEM;
	} 
	return err;
}

static int pmbd_mem_space_free(PMBD_DEVICE_T* pmbd)
{
	/* free it up */
	if (pmbd->mem_space) {
		if (PMBD_DEV_USE_VMALLOC(pmbd))
			vfree(pmbd->mem_space);
		else if (PMBD_DEV_USE_HIGHMEM(pmbd)) {
			hfree(pmbd->mem_space);
		}
		pmbd->mem_space = NULL;
	}
	return 0;
}

/* pmbd->pmbd_stat */
static int pmbd_stat_alloc(PMBD_DEVICE_T* pmbd)
{
	int err = 0;
	pmbd->pmbd_stat = (PMBD_STAT_T*)kzalloc(sizeof(PMBD_STAT_T), GFP_KERNEL);
	if (pmbd->pmbd_stat){
		spin_lock_init(&pmbd->pmbd_stat->stat_lock);
	} else {
		printk(KERN_ERR "pmbd: %s(%d): PMBD space allocation failed\n", __FUNCTION__, __LINE__);
		err = -ENOMEM;
	}
	return 0;
}

static int pmbd_stat_free(PMBD_DEVICE_T* pmbd)
{
	if(pmbd->pmbd_stat) {
		kfree(pmbd->pmbd_stat);
		pmbd->pmbd_stat = NULL;
	}
	return 0;
}

/* /proc/pmbd/<dev> */
static int pmbd_proc_pmbdstat_read(char* buffer, char** start, off_t offset, int count, int* eof, void* data)
{
	int rtn;
	if (offset > 0) {
		*eof = 1;
		rtn  = 0;
	} else {
		//char local_buffer[1024];
		char* local_buffer = kzalloc(8192, GFP_KERNEL);
		PMBD_DEVICE_T* pmbd, *next;
		char rdwr_name[2][16] = {"read\0", "write\0"};
		local_buffer[0] = '\0';

		list_for_each_entry_safe(pmbd, next, &pmbd_devices, pmbd_list) {
			unsigned i, j;
			BBN_T num_dirty = 0;
			BBN_T num_blocks = 0; 
			PMBD_STAT_T* pmbd_stat = pmbd->pmbd_stat;

			/* FIXME: should we lock the buffer? (NOT NECESSARY)*/
			for (i = 0; i < pmbd->num_buffers; i ++){
				num_blocks += pmbd->buffers[i]->num_blocks;
				num_dirty += pmbd->buffers[i]->num_dirty;
			}

			/* print stuff now */
			spin_lock(&pmbd->pmbd_stat->stat_lock);

			sprintf(local_buffer+strlen(local_buffer), "num_dirty_blocks[%s] %u\n", pmbd->pmbd_name, (unsigned int) num_dirty);
			sprintf(local_buffer+strlen(local_buffer), "num_clean_blocks[%s] %u\n", pmbd->pmbd_name, (unsigned int) (num_blocks - num_dirty));
			sprintf(local_buffer+strlen(local_buffer), "num_sectors_read[%s] %llu\n",  pmbd->pmbd_name, pmbd_stat->num_sectors_read);
			sprintf(local_buffer+strlen(local_buffer), "num_sectors_write[%s] %llu\n", pmbd->pmbd_name, pmbd_stat->num_sectors_write);
			sprintf(local_buffer+strlen(local_buffer), "num_requests_read[%s] %llu\n", pmbd->pmbd_name, pmbd_stat->num_requests_read);
			sprintf(local_buffer+strlen(local_buffer), "num_requests_write[%s] %llu\n",pmbd->pmbd_name, pmbd_stat->num_requests_write);
			sprintf(local_buffer+strlen(local_buffer), "num_write_barrier[%s] %llu\n", pmbd->pmbd_name, pmbd_stat->num_write_barrier);
			sprintf(local_buffer+strlen(local_buffer), "num_write_fua[%s] %llu\n", pmbd->pmbd_name, pmbd_stat->num_write_fua);

			spin_unlock(&pmbd->pmbd_stat->stat_lock);

//			sprintf(local_buffer+strlen(local_buffer), "\n");
				
			for (j = 0; j <= 1; j ++){
				int k=0;

				unsigned long long cycles_total = 0;
				unsigned long long cycles_prepare = 0;
				unsigned long long cycles_wb = 0;
				unsigned long long cycles_work = 0;
				unsigned long long cycles_endio = 0;
				unsigned long long cycles_finish = 0;

				unsigned long long cycles_pmap = 0;
				unsigned long long cycles_punmap = 0;
				unsigned long long cycles_memcpy = 0;
				unsigned long long cycles_clflush = 0;
				unsigned long long cycles_clflushall = 0;
				unsigned long long cycles_wrverify = 0;
				unsigned long long cycles_checksum = 0;
				unsigned long long cycles_pause = 0;
				unsigned long long cycles_slowdown = 0;
				unsigned long long cycles_setpages_ro = 0;
				unsigned long long cycles_setpages_rw = 0;

				for (k = 0; k < PMBD_MAX_NUM_CPUS; k ++){
					cycles_total 	+= pmbd_stat->cycles_total[j][k];
					cycles_prepare 	+= pmbd_stat->cycles_prepare[j][k];
					cycles_wb	+= pmbd_stat->cycles_wb[j][k];
					cycles_work	+= pmbd_stat->cycles_work[j][k];
					cycles_endio	+= pmbd_stat->cycles_endio[j][k];
					cycles_finish	+= pmbd_stat->cycles_finish[j][k];

					cycles_pmap	+= pmbd_stat->cycles_pmap[j][k];
					cycles_punmap	+= pmbd_stat->cycles_punmap[j][k];
					cycles_memcpy	+= pmbd_stat->cycles_memcpy[j][k];
					cycles_clflush	+= pmbd_stat->cycles_clflush[j][k];
					cycles_clflushall+=pmbd_stat->cycles_clflushall[j][k];
					cycles_wrverify	+= pmbd_stat->cycles_wrverify[j][k];
					cycles_checksum += pmbd_stat->cycles_checksum[j][k];
					cycles_pause	+= pmbd_stat->cycles_pause[j][k];
					cycles_slowdown	+= pmbd_stat->cycles_slowdown[j][k];
					cycles_setpages_ro+= pmbd_stat->cycles_setpages_ro[j][k];
					cycles_setpages_rw+= pmbd_stat->cycles_setpages_rw[j][k];
				}

				sprintf(local_buffer+strlen(local_buffer), "cycles_total_%s[%s] %llu\n", rdwr_name[j], pmbd->pmbd_name, cycles_total);
				sprintf(local_buffer+strlen(local_buffer), "cycles_prepare_%s[%s] %llu\n", rdwr_name[j], pmbd->pmbd_name, cycles_prepare);
				sprintf(local_buffer+strlen(local_buffer), "cycles_wb_%s[%s] %llu\n", rdwr_name[j], pmbd->pmbd_name, cycles_wb);
				sprintf(local_buffer+strlen(local_buffer), "cycles_work_%s[%s] %llu\n", rdwr_name[j], pmbd->pmbd_name, cycles_work);
				sprintf(local_buffer+strlen(local_buffer), "cycles_endio_%s[%s] %llu\n", rdwr_name[j], pmbd->pmbd_name, cycles_endio);
				sprintf(local_buffer+strlen(local_buffer), "cycles_finish_%s[%s] %llu\n", rdwr_name[j], pmbd->pmbd_name, cycles_finish);
				sprintf(local_buffer+strlen(local_buffer), "cycles_pmap_%s[%s] %llu\n", rdwr_name[j], pmbd->pmbd_name, cycles_pmap);
				sprintf(local_buffer+strlen(local_buffer), "cycles_punmap_%s[%s] %llu\n", rdwr_name[j], pmbd->pmbd_name, cycles_punmap);
				sprintf(local_buffer+strlen(local_buffer), "cycles_memcpy_%s[%s] %llu\n", rdwr_name[j], pmbd->pmbd_name, cycles_memcpy);
				sprintf(local_buffer+strlen(local_buffer), "cycles_clflush_%s[%s] %llu\n", rdwr_name[j], pmbd->pmbd_name, cycles_clflush);
				sprintf(local_buffer+strlen(local_buffer), "cycles_clflushall_%s[%s] %llu\n", rdwr_name[j], pmbd->pmbd_name, cycles_clflushall);
				sprintf(local_buffer+strlen(local_buffer), "cycles_wrverify_%s[%s] %llu\n", rdwr_name[j], pmbd->pmbd_name, cycles_wrverify);
				sprintf(local_buffer+strlen(local_buffer), "cycles_checksum_%s[%s] %llu\n", rdwr_name[j], pmbd->pmbd_name, cycles_checksum);
				sprintf(local_buffer+strlen(local_buffer), "cycles_pause_%s[%s] %llu\n", rdwr_name[j], pmbd->pmbd_name, cycles_pause);
				sprintf(local_buffer+strlen(local_buffer), "cycles_slowdown_%s[%s] %llu\n", rdwr_name[j], pmbd->pmbd_name, cycles_slowdown);
				sprintf(local_buffer+strlen(local_buffer), "cycles_setpages_ro_%s[%s] %llu\n", rdwr_name[j], pmbd->pmbd_name, cycles_setpages_ro);
				sprintf(local_buffer+strlen(local_buffer), "cycles_setpages_rw_%s[%s] %llu\n", rdwr_name[j], pmbd->pmbd_name, cycles_setpages_rw);
			}

#if 0
			/* print something temporary for debugging purpose */
			if (0) {
				spin_lock(&pmbd->tmp_lock);
				printk("%llu %lu\n", pmbd->tmp_data, pmbd->tmp_num);
				spin_unlock(&pmbd->tmp_lock);
			}
#endif
		}

		memcpy(buffer, local_buffer, strlen(local_buffer));
		rtn = strlen(local_buffer);
		kfree(local_buffer);
	}
	return rtn;
}

/* /proc/pmbdcfg */
static int pmbd_proc_pmbdcfg_read(char* buffer, char** start, off_t offset, int count, int* eof, void* data)
{
	int rtn;
	if (offset > 0) {
		*eof = 1;
		rtn  = 0;
	} else {
		char* local_buffer = kzalloc(8192, GFP_KERNEL);
		PMBD_DEVICE_T* pmbd, *next;
		local_buffer[0] = '\0';

		/* global configurations */
		sprintf(local_buffer+strlen(local_buffer), "MODULE OPTIONS: %s\n", mode);
		sprintf(local_buffer+strlen(local_buffer), "\n");

		sprintf(local_buffer+strlen(local_buffer), "max_part %d\n", max_part);
		sprintf(local_buffer+strlen(local_buffer), "part_shift %d\n", part_shift);

		sprintf(local_buffer+strlen(local_buffer), "g_pmbd_type %u\n", g_pmbd_type);
		sprintf(local_buffer+strlen(local_buffer), "g_pmbd_mergeable %u\n", g_pmbd_mergeable);
		sprintf(local_buffer+strlen(local_buffer), "g_pmbd_cpu_cache_clflush %u\n", g_pmbd_cpu_cache_clflush);
		sprintf(local_buffer+strlen(local_buffer), "g_pmbd_cpu_cache_flag %lu\n", g_pmbd_cpu_cache_flag);
		sprintf(local_buffer+strlen(local_buffer), "g_pmbd_wr_protect %u\n", g_pmbd_wr_protect);
		sprintf(local_buffer+strlen(local_buffer), "g_pmbd_wr_verify %u\n", g_pmbd_wr_verify);
		sprintf(local_buffer+strlen(local_buffer), "g_pmbd_checksum %u\n", g_pmbd_checksum);
		sprintf(local_buffer+strlen(local_buffer), "g_pmbd_lock %u\n", g_pmbd_lock);
		sprintf(local_buffer+strlen(local_buffer), "g_pmbd_subpage_update %u\n", g_pmbd_subpage_update);
		sprintf(local_buffer+strlen(local_buffer), "g_pmbd_pmap %u\n", g_pmbd_pmap);
		sprintf(local_buffer+strlen(local_buffer), "g_pmbd_nts %u\n", g_pmbd_nts);
		sprintf(local_buffer+strlen(local_buffer), "g_pmbd_ntl %u\n", g_pmbd_ntl);
		sprintf(local_buffer+strlen(local_buffer), "g_pmbd_wb %u\n", g_pmbd_wb);
		sprintf(local_buffer+strlen(local_buffer), "g_pmbd_fua %u\n", g_pmbd_fua);
		sprintf(local_buffer+strlen(local_buffer), "g_pmbd_timestat %u\n", g_pmbd_timestat);
		sprintf(local_buffer+strlen(local_buffer), "g_highmem_size %lu\n", g_highmem_size);
		sprintf(local_buffer+strlen(local_buffer), "g_highmem_phys_addr %llu\n", (unsigned long long) g_highmem_phys_addr);
		sprintf(local_buffer+strlen(local_buffer), "g_highmem_virt_addr %llu\n", (unsigned long long) g_highmem_virt_addr);
		sprintf(local_buffer+strlen(local_buffer), "g_pmbd_nr %u\n", g_pmbd_nr);
		sprintf(local_buffer+strlen(local_buffer), "g_pmbd_adjust_ns %llu\n", g_pmbd_adjust_ns);
		sprintf(local_buffer+strlen(local_buffer), "g_pmbd_num_buffers %llu\n", g_pmbd_num_buffers);
		sprintf(local_buffer+strlen(local_buffer), "g_pmbd_buffer_stride %llu\n", g_pmbd_buffer_stride);
		sprintf(local_buffer+strlen(local_buffer), "\n");

		/* device specific configurations */
		list_for_each_entry_safe(pmbd, next, &pmbd_devices, pmbd_list) {
			int i = 0;

			sprintf(local_buffer+strlen(local_buffer), "pmbd_id[%s] %d\n", pmbd->pmbd_name, pmbd->pmbd_id);
			sprintf(local_buffer+strlen(local_buffer), "num_sectors[%s] %llu\n", pmbd->pmbd_name, (unsigned long long) pmbd->num_sectors);
			sprintf(local_buffer+strlen(local_buffer), "sector_size[%s] %u\n", pmbd->pmbd_name, pmbd->sector_size);
			sprintf(local_buffer+strlen(local_buffer), "pmbd_type[%s] %u\n", pmbd->pmbd_name, pmbd->pmbd_type);
			sprintf(local_buffer+strlen(local_buffer), "rammode[%s] %u\n", pmbd->pmbd_name, pmbd->rammode);
			sprintf(local_buffer+strlen(local_buffer), "bufmode[%s] %u\n", pmbd->pmbd_name, pmbd->bufmode);
			sprintf(local_buffer+strlen(local_buffer), "wpmode[%s] %u\n", pmbd->pmbd_name, pmbd->wpmode);
			sprintf(local_buffer+strlen(local_buffer), "num_buffers[%s] %u\n", pmbd->pmbd_name, pmbd->num_buffers);
			sprintf(local_buffer+strlen(local_buffer), "buffer_stride[%s] %u\n", pmbd->pmbd_name, pmbd->buffer_stride);
			sprintf(local_buffer+strlen(local_buffer), "pb_size[%s] %u\n", pmbd->pmbd_name, pmbd->pb_size);
			sprintf(local_buffer+strlen(local_buffer), "checksum_unit_size[%s] %u\n", pmbd->pmbd_name, pmbd->checksum_unit_size);
			sprintf(local_buffer+strlen(local_buffer), "simmode[%s] %u\n", pmbd->pmbd_name, pmbd->simmode);
			sprintf(local_buffer+strlen(local_buffer), "rdlat[%s] %llu\n", pmbd->pmbd_name, (unsigned long long) pmbd->rdlat);
			sprintf(local_buffer+strlen(local_buffer), "wrlat[%s] %llu\n", pmbd->pmbd_name, (unsigned long long) pmbd->wrlat);
			sprintf(local_buffer+strlen(local_buffer), "rdbw[%s] %llu\n", pmbd->pmbd_name, (unsigned long long) pmbd->rdbw);
			sprintf(local_buffer+strlen(local_buffer), "wrbw[%s] %llu\n", pmbd->pmbd_name, (unsigned long long) pmbd->wrbw);
			sprintf(local_buffer+strlen(local_buffer), "rdsx[%s] %u\n", pmbd->pmbd_name, pmbd->rdsx);
			sprintf(local_buffer+strlen(local_buffer), "wrsx[%s] %u\n", pmbd->pmbd_name, pmbd->wrsx);
			sprintf(local_buffer+strlen(local_buffer), "rdpause[%s] %llu\n", pmbd->pmbd_name, (unsigned long long) pmbd->rdpause);
			sprintf(local_buffer+strlen(local_buffer), "wrpause[%s] %llu\n", pmbd->pmbd_name, (unsigned long long) pmbd->wrpause);

			for (i = 0; i < pmbd->num_buffers; i ++){
				PMBD_BUFFER_T* buffer = pmbd->buffers[i];
					sprintf(local_buffer+strlen(local_buffer), "buffer%d[%s]buffer_id %u\n", i, pmbd->pmbd_name, buffer->buffer_id);
					sprintf(local_buffer+strlen(local_buffer), "buffer%d[%s]num_blocks %lu\n", i, pmbd->pmbd_name, (unsigned long) buffer->num_blocks);
					sprintf(local_buffer+strlen(local_buffer), "buffer%d[%s]batch_size %lu\n", i, pmbd->pmbd_name, (unsigned long) buffer->batch_size);
			}

		}

		memcpy(buffer, local_buffer, strlen(local_buffer));
		rtn = strlen(local_buffer);
		kfree(local_buffer);
	}
	return rtn;
}



static int pmbd_proc_devstat_read(char* buffer, char** start, off_t offset, int count, int* eof, void* data)
{
	int rtn;
	char local_buffer[1024];
	if (offset > 0) {
		*eof = 1;
		rtn  = 0;
	} else {
		sprintf(local_buffer, "N/A\n");
		memcpy(buffer, local_buffer, strlen(local_buffer));
		rtn = strlen(local_buffer);
	}
	return rtn;
}

static int pmbd_proc_devstat_create(PMBD_DEVICE_T* pmbd)
{
	/* create a /proc/pmbd/<dev> entry */
	pmbd->proc_devstat = create_proc_entry(pmbd->pmbd_name, S_IRUGO, proc_pmbd);
	if (pmbd->proc_devstat == NULL) {
		remove_proc_entry(pmbd->pmbd_name, proc_pmbd);
		printk(KERN_ERR "pmbd: cannot create /proc/pmbd/%s\n", pmbd->pmbd_name);
		return -ENOMEM;
	}
	pmbd->proc_devstat->read_proc = pmbd_proc_devstat_read;
	printk(KERN_INFO "pmbd: /proc/pmbd/%s created\n", pmbd->pmbd_name);

	return 0;
}

static int pmbd_proc_devstat_destroy(PMBD_DEVICE_T* pmbd)
{
	remove_proc_entry(pmbd->pmbd_name, proc_pmbd);
	printk(KERN_INFO "pmbd: /proc/pmbd/%s removed\n", pmbd->pmbd_name);
	return 0;
}

static int pmbd_create (PMBD_DEVICE_T* pmbd, uint64_t sectors)
{
	int err = 0;

	pmbd->num_sectors = sectors; 
	pmbd->sector_size = PMBD_SECTOR_SIZE;	 	/* FIXME: now we use 512, do we need to change it? */
	pmbd->pmbd_type = g_pmbd_type;
	pmbd->checksum_unit_size = PAGE_SIZE;
	pmbd->pb_size = PAGE_SIZE;

	spin_lock_init(&pmbd->batch_lock);
	spin_lock_init(&pmbd->wr_barrier_lock);

	spin_lock_init(&pmbd->tmp_lock);
	pmbd->tmp_data = 0;
	pmbd->tmp_num = 0;

	/* allocate statistics info */
	if ((err = pmbd_stat_alloc(pmbd)) < 0)
		goto error;

	/* allocate memory space */
	if ((err = pmbd_mem_space_alloc(pmbd)) < 0)
		goto error;

	/* allocate buffer space */
	if ((err = pmbd_buffer_space_alloc(pmbd)) < 0)
		goto error;

	/* allocate checksum space */
	if ((err = pmbd_checksum_space_alloc(pmbd)) < 0)
		goto error;
	
	/* allocate block info space */
	if ((err = pmbd_pbi_space_alloc(pmbd)) < 0)
		goto error;

	/* create a /proc/pmbd/<dev> entry*/
	if ((err = pmbd_proc_devstat_create(pmbd)) < 0)
		goto error;

#if 0
	/* FIXME: No need to do it. It's slow and could lock up the system*/
	pmbd_checksum_space_init(pmbd);
#endif

	/* set up the page attributes related with CPU cache 
	 * if using vmalloc(), we need to set up the page cache flags (WB,WC,UC,UM);
	 * if using high memory, we set up the page cache flag with ioremap_prot();
	 * WARN: In Linux 3.2.1, this function is slow and could cause system hangs. 
 	 */
	
	if (PMBD_USE_VMALLOC()){
		pmbd_set_pages_cache_flags(pmbd);
	}

	/* initialize PM pages read-only */
	if (!PMBD_USE_PMAP() && PMBD_USE_WRITE_PROTECTION())	
		pmbd_set_pages_ro(pmbd, pmbd->mem_space, PMBD_MEM_TOTAL_BYTES(pmbd), FALSE);

	printk(KERN_INFO "pmbd: %s created\n", pmbd->pmbd_name);
error:
	return err;
}

static int pmbd_destroy (PMBD_DEVICE_T* pmbd)
{
	/* flush everything down */
	// FIXME: this implies flushing CPU cache
	pmbd_write_barrier(pmbd);
	
	/* free /proc entry */
	pmbd_proc_devstat_destroy(pmbd);

	/* free buffer space */
	pmbd_buffer_space_free(pmbd);

	/* set PM pages writable */
	if (!PMBD_USE_PMAP() && PMBD_USE_WRITE_PROTECTION())
		pmbd_set_pages_rw(pmbd, pmbd->mem_space, PMBD_MEM_TOTAL_BYTES(pmbd), FALSE);

	/* reset memory attributes to WB */
	if (PMBD_USE_VMALLOC())
		pmbd_reset_pages_cache_flags(pmbd);
		
	/* free block info space */
	pmbd_pbi_space_free(pmbd);

	/* free checksum space */
	pmbd_checksum_space_free(pmbd);

	/* free memory backstore space */
	pmbd_mem_space_free(pmbd);

	/* free statistics data */
	pmbd_stat_free(pmbd);
	
	printk(KERN_INFO "pmbd: /dev/%s is destroyed (%llu MB)\n", pmbd->pmbd_name, SECTORS_TO_MB(pmbd->num_sectors));

	pmbd->num_sectors = 0;
	pmbd->sector_size = 0;
	pmbd->checksum_unit_size = 0;
	return 0;
}

static int pmbd_free_pages(PMBD_DEVICE_T* pmbd)
{
	return pmbd_destroy(pmbd);
}

/*
 **************************************************************************
 * /proc file system entries
 **************************************************************************
 */

static int pmbd_proc_create(void)
{
	proc_pmbd= proc_mkdir("pmbd", 0);
	if(proc_pmbd == NULL){
		printk(KERN_ERR "pmbd: %s(%d): cannot create /proc/pmbd\n", __FUNCTION__, __LINE__);
		return -ENOMEM;
	}

	proc_pmbdstat = create_proc_entry("pmbdstat", S_IRUGO, proc_pmbd);
	if (proc_pmbdstat == NULL){
		remove_proc_entry("pmbdstat", proc_pmbd);
		printk(KERN_ERR "pmbd: cannot create /proc/pmbd/pmbdstat\n");
		return -ENOMEM;
	}
	proc_pmbdstat->read_proc = pmbd_proc_pmbdstat_read;
	printk(KERN_INFO "pmbd: /proc/pmbd/pmbdstat created\n");

	proc_pmbdcfg = create_proc_entry("pmbdcfg", S_IRUGO, proc_pmbd);
	if (proc_pmbdcfg == NULL){
		remove_proc_entry("pmbdcfg", proc_pmbd);
		printk(KERN_ERR "pmbd: cannot create /proc/pmbd/pmbdcfg\n");
		return -ENOMEM;
	}
	proc_pmbdcfg->read_proc = pmbd_proc_pmbdcfg_read;
	printk(KERN_INFO "pmbd: /proc/pmbd/pmbdcfg created\n");

	return 0;
}

static int pmbd_proc_destroy(void)
{
	remove_proc_entry("pmbdcfg", proc_pmbd);
	printk(KERN_INFO "pmbd: /proc/pmbd/pmbdcfg is removed\n");

	remove_proc_entry("pmbdstat", proc_pmbd);
	printk(KERN_INFO "pmbd: /proc/pmbd/pmbdstat is removed\n");

	remove_proc_entry("pmbd", 0);
	printk(KERN_INFO "pmbd: /proc/pmbd is removed\n");
	return 0;
}

/*
 **************************************************************************
 * device driver interface hook functions
 **************************************************************************
 */

static int pmbd_mergeable_bvec(struct request_queue *q, 
                              struct bvec_merge_data *bvm,
                              struct bio_vec *biovec) {
	static int flag = 0;
    
	if(PMBD_IS_MERGEABLE()) {
		/* always merge */
		if (!flag) {
		    printk(KERN_INFO "pmbd: bio merging enabled\n");
		    flag = 1;
		}
		return biovec->bv_len;
	} else {
		/* never merge */
		if (!flag) {
			printk(KERN_INFO "pmbd: bio merging disabled\n");
			flag = 1;
		}
		if (!bvm->bi_size) {
        		return biovec->bv_len;
		} else {
			return 0;
		}
	}
}

int pmbd_fsync(struct file* file, struct dentry* dentry, int datasync)
{
	printk(KERN_WARNING "pmbd: pmbd_fsync not implemented\n");

	return 0;
}

int pmbd_open(struct block_device* bdev, fmode_t mode)
{
	printk(KERN_DEBUG "pmbd: pmbd (/dev/%s) opened\n", bdev->bd_disk->disk_name);
	return 0;
}

int pmbd_release (struct gendisk* disk, fmode_t mode)
{
	printk(KERN_DEBUG "pmbd: pmbd (/dev/%s) released\n", disk->disk_name);
	return 0;
}

static const struct block_device_operations pmbd_fops = {
	.owner =		THIS_MODULE,
//	.open =			pmbd_open,
//	.release = 		pmbd_release,
};

/*
 * NOTE: partial of the following code is derived from linux/block/brd.c
 */


static PMBD_DEVICE_T *pmbd_alloc(int i)
{
	PMBD_DEVICE_T *pmbd;
	struct gendisk *disk;

	/* no more than 26 devices */
	if (i >= PMBD_MAX_NUM_DEVICES)
		return NULL;

	/* alloc and set up pmbd object */
	pmbd = kzalloc(sizeof(*pmbd), GFP_KERNEL);
	if (!pmbd) 
		goto out;
	pmbd->pmbd_id = i;
	pmbd->pmbd_queue = blk_alloc_queue(GFP_KERNEL);
	sprintf(pmbd->pmbd_name, "pm%c", ('a' + i));
	pmbd->rdlat = g_pmbd_rdlat[i];
	pmbd->wrlat = g_pmbd_wrlat[i];
	pmbd->rdbw  = g_pmbd_rdbw[i];
	pmbd->wrbw  = g_pmbd_wrbw[i];
	pmbd->rdsx  = g_pmbd_rdsx[i];
	pmbd->wrsx  = g_pmbd_wrsx[i];
	pmbd->rdpause  = g_pmbd_rdpause[i];
	pmbd->wrpause  = g_pmbd_wrpause[i];
	pmbd->simmode  = g_pmbd_simmode[i];
	pmbd->rammode  = g_pmbd_rammode[i];
	pmbd->wpmode   = g_pmbd_wpmode[i];
	pmbd->num_buffers  = g_pmbd_num_buffers;
	pmbd->buffer_stride  = g_pmbd_buffer_stride;
	pmbd->bufmode  = (g_pmbd_bufsize[i] > 0 && g_pmbd_num_buffers > 0) ? TRUE : FALSE;

	if (!pmbd->pmbd_queue)
		goto out_free_dev;

	/* hook functions */
	blk_queue_make_request(pmbd->pmbd_queue, pmbd_make_request);

	/* set flush capability, otherwise, WRITE_FLUSH and WRITE_FUA will be filtered in
 	   generic_make_request() */
	if (PMBD_USE_FUA())
		blk_queue_flush(pmbd->pmbd_queue, REQ_FLUSH | REQ_FUA);
	else if (PMBD_USE_WB())
		blk_queue_flush(pmbd->pmbd_queue, REQ_FLUSH);

	blk_queue_max_hw_sectors(pmbd->pmbd_queue, 1024);
	blk_queue_bounce_limit(pmbd->pmbd_queue, BLK_BOUNCE_ANY);
    	blk_queue_merge_bvec(pmbd->pmbd_queue, pmbd_mergeable_bvec);

	disk = pmbd->pmbd_disk = alloc_disk(1 << part_shift);
	if (!disk)
		goto out_free_queue;

	disk->major		= PMBD_MAJOR;
	disk->first_minor	= i << part_shift;
	disk->fops		= &pmbd_fops;
	disk->private_data	= pmbd;
	disk->queue		= pmbd->pmbd_queue;
	strcpy(disk->disk_name, pmbd->pmbd_name);
	set_capacity(disk, GB_TO_SECTORS(g_pmbd_size[i])); /* num of sectors */

	/* allocate PM space */
	if (pmbd_create(pmbd, GB_TO_SECTORS(g_pmbd_size[i])) < 0)
		goto out_free_queue;

	/* done */
	return pmbd;

out_free_queue:
	blk_cleanup_queue(pmbd->pmbd_queue);
out_free_dev:
	kfree(pmbd);
out:
	return NULL;
}

static void pmbd_free(PMBD_DEVICE_T *pmbd)
{
	put_disk(pmbd->pmbd_disk);
	blk_cleanup_queue(pmbd->pmbd_queue);
	pmbd_free_pages(pmbd);
	kfree(pmbd);
}

static void pmbd_del_one(PMBD_DEVICE_T *pmbd)
{
	list_del(&pmbd->pmbd_list);
	del_gendisk(pmbd->pmbd_disk);
	pmbd_free(pmbd);
}

static int __init pmbd_init(void)
{
	int i, nr;
	unsigned long range;
	PMBD_DEVICE_T *pmbd, *next;

	/* parse input options */
	pmbd_parse_conf();

	/* initialize pmap start*/
	pmap_create();

	/* ioremap high memory space */
	if (PMBD_USE_HIGHMEM()) {
		if (pmbd_highmem_map() == NULL) 
			return -ENOMEM;
	}

	part_shift = 0;
	if (max_part > 0)
		part_shift = fls(max_part);

	if (g_pmbd_nr > 1UL << (MINORBITS - part_shift))
		return -EINVAL;

	if (g_pmbd_nr) {
		nr = g_pmbd_nr;
		range = g_pmbd_nr;
	} else {
		printk(KERN_ERR "pmbd: %s(%d) - g_pmbd_nr=%d\n", __FUNCTION__, __LINE__, g_pmbd_nr);
		return -EINVAL;
	} 

	pmbd_proc_create();

	if (register_blkdev(PMBD_MAJOR, PMBD_NAME))
		return -EIO;
	else
		printk(KERN_INFO "pmbd: registered device at major %d\n", PMBD_MAJOR);

	for (i = 0; i < nr; i++) {
		pmbd = pmbd_alloc(i);
		if (!pmbd)
			goto out_free;
		list_add_tail(&pmbd->pmbd_list, &pmbd_devices);
	}

	/* point of no return */
	list_for_each_entry(pmbd, &pmbd_devices, pmbd_list)
		add_disk(pmbd->pmbd_disk);

	printk(KERN_INFO "pmbd: module loaded\n");
	return 0;

out_free:
	list_for_each_entry_safe(pmbd, next, &pmbd_devices, pmbd_list) {
		list_del(&pmbd->pmbd_list);
		pmbd_free(pmbd);
	}
	unregister_blkdev(PMBD_MAJOR, PMBD_NAME);

	return -ENOMEM;
}


static void __exit pmbd_exit(void)
{
	unsigned long range;
	PMBD_DEVICE_T *pmbd, *next;

	range = g_pmbd_nr ? g_pmbd_nr :  1UL << (MINORBITS - part_shift);

	/* deactivate each pmbd instance*/
	list_for_each_entry_safe(pmbd, next, &pmbd_devices, pmbd_list)
		pmbd_del_one(pmbd);

	/* deioremap high memory space */
	if (PMBD_USE_HIGHMEM()) {
		pmbd_highmem_unmap(); 
	}

	/* destroy pmap entries */
	pmap_destroy();

	unregister_blkdev(PMBD_MAJOR, PMBD_NAME);

	pmbd_proc_destroy();

	printk(KERN_INFO "pmbd: module unloaded\n");
	return;
}

/* module setup */
MODULE_AUTHOR("Intel Corporation <linux-pmbd@intel.com>");
MODULE_ALIAS("pmbd");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("0.9");
MODULE_ALIAS_BLOCKDEV_MAJOR(PMBD_MAJOR);
module_init(pmbd_init);
module_exit(pmbd_exit);

/* THE END */


