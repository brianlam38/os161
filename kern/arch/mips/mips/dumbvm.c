#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <array.h>
#include <thread.h>
#include <curthread.h>
#include <addrspace.h>
#include <vm.h>
#include <vfs.h>
#include <uio.h>
#include <vnode.h>
#include <kern/unistd.h>
#include <machine/spl.h>
#include <machine/tlb.h>

/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground. You should replace all of this
 * code while doing the VM assignment. In fact, starting in that
 * assignment, this file is not included in your kernel!
 */

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12

int vm_initialized = 0;
struct vnode *randdev;
struct array *buddylist;

struct buddy_entry
{
	u_int32_t paddr;
	int pages;
	int inuse;
};

void tlb_printstats(void)
{
	int i;
	u_int32_t elo, ehi;

	kprintf("+---TLB---------------------+\n");
	kprintf("| idx | ehi      | elo      |\n");
	for (i=0; i<NUM_TLB; i++)
	{
		TLB_Read(&ehi, &elo, i);
		kprintf("| %03d | %08x | %08x |\n", i, ehi, elo);
	}
	kprintf("+---------------------------+\n");
}

void buddylist_printstats(void)
{
	int i;
	struct buddy_entry *be;

	kprintf("+-----BUDDYLIST--------------------+\n");
	kprintf("| idx |    paddr   | pages | inuse |\n");
	for (i=0; i<array_getnum(buddylist); i++)
	{
		be = (struct buddy_entry *) array_getguy(buddylist, i);	
		kprintf("| %03d | 0x%08x |    %02d |     %01d |\n", i, be->paddr, be->pages, be->inuse);
	}
	kprintf("+----------------------------------+\n");
}

void
vm_bootstrap(void)
{
	u_int32_t lo, hi;
	struct buddy_entry *be;
	int npages;

	buddylist = array_create();

	be = (struct buddy_entry *) kmalloc(sizeof(struct buddy_entry));
	if (be==NULL)
	{
		panic("vm_bootstrap unable to allocate memory\n");
	}

	ram_getsize(&lo, &hi);
	kprintf("memory after bootstraps:\n");
	kprintf("first: 0x%08x, last 0x%08x\n", lo, hi);

	/* calculate the first buddylist entry */
	npages=(hi-lo)/PAGE_SIZE;

	vfs_open("random:", O_RDONLY, &randdev);

	be->paddr = lo;
	be->pages = npages;
	be->inuse = 0;
	array_add(buddylist, be);
	vm_initialized = 1;
	kprintf("initialized vm with one buddy @ 0x%08x with %u pages\n", lo, npages);

}

/* finds the best fit for the request pageload
 * return an index into the buddylist */
static
int
find_buddy(int npages)
{
	int i;
	int chosen;
	struct buddy_entry *curbe, *chosbe;

	chosen = -1;
	for (i=0; i<array_getnum(buddylist); i++)
	{
		curbe = (struct buddy_entry *) array_getguy(buddylist, i);
		if (curbe->inuse == 0 && curbe->pages >= npages)
		{
			if (chosen==-1 || curbe->pages < chosbe->pages)
			{
				chosen = i;
				chosbe = curbe;
			}
		}

	}

	return chosen;
}


static
paddr_t
calculate_buddy(int npages)
{
	struct buddy_entry *be;
	struct buddy_entry *b2;
	int buddyi;
	int nextsize, oldsize;
	paddr_t nextpaddr;

	// finds the first buddy able to fit the number of requested pages 
	buddyi = find_buddy(npages);
	be = (struct buddy_entry *) array_getguy(buddylist, buddyi);

	nextpaddr = be->paddr;
	oldsize = be->pages;
	nextsize = oldsize / 2;
	while (nextsize >= npages)
	{
		// create two new buddy entries 
		kfree(be);
		be = (struct buddy_entry *) kmalloc(sizeof(struct buddy_entry));
		if (be==NULL)
			panic("vm: could not calculate next buddy\n");
		b2 = (struct buddy_entry *) kmalloc(sizeof(struct buddy_entry));
		if (b2==NULL)
			panic("vm: could not calculate next buddy\n");

		be->paddr = nextpaddr;
		be->pages = nextsize;
		be->inuse = 0;

		b2->paddr = nextpaddr + (nextsize * PAGE_SIZE);
		b2->pages = oldsize - nextsize;
		b2->inuse = 0;

		array_setguy(buddylist, buddyi, be);
		array_add(buddylist, b2);

		// divide the page 
		oldsize = nextsize;
		nextsize /= 2;
	}

	be->inuse = 1;
	return be->paddr;
}

static
void
freeppage(paddr_t addr)
{
	int i;
	struct buddy_entry *be;

	for (i=0; i<array_getnum(buddylist); i++)
	{
		be = (struct buddy_entry *) array_getguy(buddylist, i);	
		if (be->paddr == addr)
		{
			be->inuse = 0;
			return;
		}
	}
}

static
paddr_t
getpframes(unsigned long npages)
{
	int spl;
	paddr_t addr;

	spl = splhigh();

	addr = calculate_buddy(npages);

	splx(spl);
	return addr;
}

static
paddr_t
getppages(unsigned long npages)
{
	int spl;
	paddr_t addr;

	spl = splhigh();

	if (vm_initialized)
		addr = calculate_buddy(npages);
	else
		addr = ram_stealmem(npages);
	
	splx(spl);
	return addr;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t 
alloc_kpages(int npages)
{
	paddr_t pa;
	pa = getppages(npages);
	if (pa==0) {
		return 0;
	}
	return PADDR_TO_KVADDR(pa);
}

void 
free_kpages(vaddr_t addr)
{
	freeppage(KVADDR_TO_PADDR(addr));
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	u_int32_t ehi, elo;
	struct addrspace *as;
	int spl;

	spl = splhigh();

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
		/* We always create pages read-write, so we can't get this */
		panic("dumbvm: got VM_FAULT_READONLY\n");
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		splx(spl);
		return EINVAL;
	}

	as = curthread->t_vmspace;
	if (as == NULL) {
		/*
		 * No address space set up. This is probably a kernel
		 * fault early in boot. Return EFAULT so as to panic
		 * instead of getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
	assert(as->as_vbase1 != 0);
	assert(as->as_pbase1 != 0);
	assert(as->as_npages1 != 0);
	assert(as->as_vbase2 != 0);
	assert(as->as_pbase2 != 0);
	assert(as->as_npages2 != 0);
	assert(as->as_stackpbase != 0);
	assert((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	assert((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
	assert((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
	assert((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
	assert((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);

	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = as->as_stackvbase - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = as->as_stackvbase;
	//stackbase = USERTOP - (DUMBVM_STACKPAGES * PAGE_SIZE);
	//stacktop = USERTOP;

	if (faultaddress >= vbase1 && faultaddress < vtop1) {
		paddr = (faultaddress - vbase1) + as->as_pbase1;
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
		paddr = (faultaddress - vbase2) + as->as_pbase2;
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
		paddr = (faultaddress - stackbase) + as->as_stackpbase;
	}
	else {
		splx(spl);
		buddylist_printstats();
		return EFAULT;
	}

	/* make sure it's page-aligned */
	assert((paddr & PAGE_FRAME)==paddr);

	for (i=0; i<NUM_TLB; i++) {
		TLB_Read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		TLB_Write(ehi, elo, i);
		splx(spl);
		return 0;
	}


	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
}

struct addrspace *
as_create(void)
{
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as==NULL) {
		return NULL;
	}

	as->as_vbase1 = 0;
	as->as_pbase1 = 0;
	as->as_npages1 = 0;
	as->as_vbase2 = 0;
	as->as_pbase2 = 0;
	as->as_npages2 = 0;
	as->as_stackvbase = 0;
	as->as_stackpbase = 0;

	return as;
}

void
as_destroy(struct addrspace *as)
{
	freeppage(as->as_pbase1);
	freeppage(as->as_pbase2);
	freeppage(as->as_stackpbase);
	kfree(as);
}

/* this is such a stupid function */
/* my guess is that the only reason this exists is to
 * avoid conflicts within the TLB. A process table
 * will need to be written before this can be scrapped.
 * A process table would allow us to write unique TLB
 * entries while allowing programs to use identical
 * virtual addresses */
void
as_activate(struct addrspace *as)
{
	int i, spl;

	(void)as;

	spl = splhigh();

	// only uncomment this when a breakpoint is set on as_activate!!
	// tlb_printstats();

	for (i=0; i<NUM_TLB; i++) {
		TLB_Write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	size_t npages; 

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

	/* We don't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	(void)executable;

	if (as->as_vbase1 == 0) {
		as->as_vbase1 = vaddr;
		as->as_npages1 = npages;
		return 0;
	}

	if (as->as_vbase2 == 0) {
		as->as_vbase2 = vaddr;
		as->as_npages2 = npages;
		return 0;
	}

	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("dumbvm: Warning: too many regions\n");
	return EUNIMP;
}

/* gets physical pages for each region */
int
as_prepare_load(struct addrspace *as)
{
	assert(as->as_pbase1 == 0);
	assert(as->as_pbase2 == 0);
	assert(as->as_stackpbase == 0);

	as->as_pbase1 = getppages(as->as_npages1);
	if (as->as_pbase1 == 0) {
		return ENOMEM;
	}

	as->as_pbase2 = getppages(as->as_npages2);
	if (as->as_pbase2 == 0) {
		return ENOMEM;
	}

	as->as_stackpbase = getppages(DUMBVM_STACKPAGES);
	if (as->as_stackpbase == 0) {
		return ENOMEM;
	}

	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	(void)as;
	return 0;
}

/* prototype for ASLR stack */
int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	struct vnode *v;
	struct uio ku;
	unsigned int rand;
	int newstack;
	int result;

	assert(as->as_stackpbase != 0);

	mk_kuio(&ku, &rand, 4, 0, UIO_READ);
	VOP_READ(randdev, &ku);

	// code starts at        	   0x00400000
	//
	// code will be imagined to end at 0x00500000
	// 12 pages for stack 		   0x005c0000
	// kernel code starts at 	   0x80000000

	rand %= 0x7fa40000;
	newstack = 0x005c0000 + rand;
	newstack &= PAGE_FRAME;

	as->as_stackvbase = newstack;
	*stackptr = newstack;
	return 0;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *new;

	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}

	new->as_vbase1 = old->as_vbase1;
	new->as_npages1 = old->as_npages1;
	new->as_vbase2 = old->as_vbase2;
	new->as_npages2 = old->as_npages2;
	new->as_stackvbase = old->as_stackvbase;

	if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}

	assert(new->as_pbase1 != 0);
	assert(new->as_pbase2 != 0);
	assert(new->as_stackpbase != 0);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase1),
		(const void *)PADDR_TO_KVADDR(old->as_pbase1),
		old->as_npages1*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase2),
		(const void *)PADDR_TO_KVADDR(old->as_pbase2),
		old->as_npages2*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase),
		(const void *)PADDR_TO_KVADDR(old->as_stackpbase),
		DUMBVM_STACKPAGES*PAGE_SIZE);
	
	*ret = new;
	return 0;
}
