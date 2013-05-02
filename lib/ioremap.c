/*
 * Re-map IO memory to kernel address space so that we can access it.
 * This is needed for high PCI addresses that aren't mapped in the
 * 640k-1MB IO memory area on PC's
 *
 * (C) Copyright 1995 1996 Linus Torvalds
 */
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/io.h>
#include <linux/export.h>
#include <asm/cacheflush.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>

static int ioremap_pte_range(pmd_t *pmd, unsigned long addr,
		unsigned long end, phys_addr_t phys_addr, pgprot_t prot)
{
	pte_t *pte;
	u64 pfn;

	pfn = phys_addr >> PAGE_SHIFT;
	pte = pte_alloc_kernel(pmd, addr);
	if (!pte)
		return -ENOMEM;
	do {
		BUG_ON(!pte_none(*pte));
		set_pte_at(&init_mm, addr, pte, pfn_pte(pfn, prot));
		pfn++;
	} while (pte++, addr += PAGE_SIZE, addr != end);
	return 0;
}

static inline int ioremap_pmd_range(pud_t *pud, unsigned long addr,
		unsigned long end, phys_addr_t phys_addr, pgprot_t prot, int hpages)
{
	pmd_t *pmd_page, *pmd;
	unsigned long next;

	phys_addr -= addr;
	pmd_page = pmd_alloc(&init_mm, pud, addr);
	if (!pmd_page)
		return -ENOMEM;

	if (hpages)
	{
		printk (KERN_INFO "PMD_MAPPING (START) [%s,%d]"
			" VA START(0x%lx), VA END(0x%lx), "
			"PA(0x%lx), SIZE(0x%lx)\n", __FUNCTION__, __LINE__,
			addr, end, (unsigned long)(phys_addr+addr), (end-addr));

	}

	pmd = pmd_page;
	do {
		next = pmd_addr_end(addr, end);
		if (hpages && cpu_has_pse && ((next-addr)>=PMD_SIZE))
		{
			u64 pfn = ((u64)(phys_addr + addr)) >> PAGE_SHIFT;
			prot = __pgprot((unsigned long)prot.pgprot | _PAGE_PSE);

			if ((s64)pfn < 0)
			{
				printk (KERN_INFO "MAPPING ERROR [%s, %d] : phys_addr(0x%lx)"
						"addr(0x%lx), next(0x%lx), end(0x%lx),"
						"pfn(0x%lx)\n", __FUNCTION__, __LINE__, 
						(unsigned long)phys_addr,
						(unsigned long)addr, (unsigned long)next, 
						(unsigned long)end, (unsigned long)pfn);
				return -ENOMEM;
			}

			spin_lock(&init_mm.page_table_lock);
			set_pte((pte_t *)pmd, pfn_pte(pfn, prot));
			spin_unlock(&init_mm.page_table_lock);
		}
		else
		{
			if (ioremap_pte_range(pmd, addr, next, phys_addr + addr, prot))
				return -ENOMEM;
		}
	} while (pmd++, addr = next, addr != end);
	return 0;
}

static inline int ioremap_pud_range(pgd_t *pgd, unsigned long addr,
		unsigned long end, phys_addr_t phys_addr, pgprot_t prot, int hpages)
{
	pud_t *pud_page, *pud;
	unsigned long next;

	phys_addr -= addr;
	pud_page = pud_alloc(&init_mm, pgd, addr);
	if (!pud_page)
		return -ENOMEM;

	if (hpages)
	{
		printk (KERN_INFO "PUD_MAPPING (START) [%s,%d]"
			" VA START(0x%lx), VA END(0x%lx), "
			"PA(0x%lx), SIZE(0x%lx)\n", __FUNCTION__, __LINE__,
			addr, end, (unsigned long)(phys_addr+addr), (end-addr));
	}

	pud = pud_page;
	do {
		next = pud_addr_end(addr, end);
		if (hpages && cpu_has_gbpages && ((next-addr)>=PUD_SIZE))
		{
			u64 pfn = ((u64)(phys_addr + addr)) >> PAGE_SHIFT;
			prot = __pgprot((unsigned long)prot.pgprot | _PAGE_PSE);
			if ((s64)pfn < 0)
			{
				printk (KERN_INFO "MAPPING ERROR [%s, %d] : phys_addr(0x%lx)"
						"addr(0x%lx), next(0x%lx), end(0x%lx),"
						"pfn(0x%lx)\n", __FUNCTION__, __LINE__, 
						(unsigned long)phys_addr,
						(unsigned long)addr, (unsigned long)next, 
						(unsigned long)end, (unsigned long)pfn);
				return -ENOMEM;
			}

			spin_lock(&init_mm.page_table_lock);
			set_pte((pte_t *)pud, pfn_pte(pfn, prot));
			spin_unlock(&init_mm.page_table_lock);
		}
		else
		{
			if (ioremap_pmd_range(pud, addr, next, phys_addr + addr,
															prot, hpages))
				return -ENOMEM;
		}
	} while (pud++, addr = next, addr != end);
	return 0;
}

int ioremap_page_range(unsigned long addr,
		       unsigned long end, phys_addr_t phys_addr, pgprot_t prot)
{
	pgd_t *pgd;
	unsigned long start;
	unsigned long next;
	int err;

	BUG_ON(addr >= end);

	start = addr;
	phys_addr -= addr;
	pgd = pgd_offset_k(addr);
	do {
		next = pgd_addr_end(addr, end);
		err = ioremap_pud_range(pgd, addr, next, phys_addr+addr, prot, 0);
		if (err)
			break;
	} while (pgd++, addr = next, addr != end);

	flush_cache_vmap(start, end);

	return err;
}
EXPORT_SYMBOL_GPL(ioremap_page_range);

int ioremap_hpage_range(unsigned long addr,
		       unsigned long end, phys_addr_t phys_addr, pgprot_t prot)
{
	pgd_t *pgd;
	unsigned long start;
	unsigned long next;
	int err;

	BUG_ON(addr >= end);

	printk (KERN_INFO "[%s,%d] hpages ON; startVA(0x%lx), endVA(0x%lx), "
			"startPA(0x%lx), startPFN(0x%lx)\n", __FUNCTION__, __LINE__,
			addr, end, (unsigned long)phys_addr,
			(unsigned long)phys_addr >> PAGE_SHIFT);

	start = addr;
	phys_addr -= addr;
	pgd = pgd_offset_k(addr);

	do {
		next = pgd_addr_end(addr, end);
		err = ioremap_pud_range(pgd, addr, next, phys_addr+addr, prot, 1);
		if (err)
			break;
	} while (pgd++, addr = next, addr != end);

	flush_cache_vmap(start, end);

	return err;
}
EXPORT_SYMBOL_GPL(ioremap_hpage_range);
