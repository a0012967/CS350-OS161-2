/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <opt-A3.h>

/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground. You should replace all of this
 * code while doing the VM assignment. In fact, starting in that
 * assignment, this file is not included in your kernel!
 */

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12

/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

bool coreMade=false;//this is necessary because kernel needs to be allocated before coremap and is done using stealmem

struct coremap {
    paddr_t adr;
    bool inUse;
    bool contiguous;
};

struct coremap *coremap;
int totalFrames;

void
vm_bootstrap(void)
{
	#if OPT_A3
	paddr_t lo;
	paddr_t hi;
	ram_getsize(&lo, &hi);//sets lo and hi to the correct spots in memory starting at first free spot
	
	    //kprintf("hi at %u\n", hi);
	    //kprintf("lo at %u\n", lo);
	//set up the coremap
	coremap = (struct coremap *)PADDR_TO_KVADDR(lo);
	//going to be greedy here
	//assume number of frames WITHOUT coremap first
	int frames = (hi-lo)/PAGE_SIZE;
	//now use that number to find necessary space for coremap
	lo += frames*(sizeof(struct coremap));
	//almost forgot to page align it!! a simple loop to realign. Easiest way to see whats happening
	while(lo %PAGE_SIZE !=0) {
	    lo+=1;
	}
	//update the number of frames for our new size
	frames = (hi-lo)/PAGE_SIZE;
	totalFrames=frames;//global variable used when dealing with coremap stuff later
	//now we can setup our coremap
	paddr_t curLo =lo;
	for (int i=0; i<frames; i++) {
	    coremap[i].adr =curLo;
	    coremap[i].inUse = false;
	    coremap[i].contiguous = false;
	    curLo += PAGE_SIZE;
	}
	coreMade=true;
	//kprintf("done coremap\n");
	#endif
}



static
paddr_t
getppages(unsigned long npages)
{
	paddr_t addr;

	spinlock_acquire(&stealmem_lock);
    #if OPT_A3
    //pretty straightforward. We just need to consider if its kernel initializing or coremap is already set
    //and this is some other page we are dealing with
    if(coreMade) {
        //this is where the coremap magic happens
        int pages = (int)npages;//so I dont have to constantly cast stuff
        //kprintf("allocating %d pages\n",pages);
        //everything needs to be contiguous so make sure you accomadate for that
        bool foundArea=false;
        int startingInt;
        for(int i=0; i<totalFrames; i++) {
            if(foundArea==true) {
                break;
            }
            if(coremap[i].inUse==false) {
                int curCount=1;
                if(pages>1) {//check for contiguous block
                    for(int j=i+1;j<i+pages;j++) {
                        if(coremap[j].inUse==false){
                            curCount++;
                            if(curCount==pages){
                                foundArea=true;
                                startingInt=i;
                            }
                        } else {
                            i+=curCount;
                            break;
                        }
                    }
                } else {
                    startingInt=i;
                    foundArea=true;
                }
            }
        }
        
        if(foundArea==true) {
            for(int i=0; i<pages; i++) {
                coremap[startingInt+i].inUse=true;
                if(i==pages-1) {
                    //kprintf("final page allocated at %d\n", i+1);
                    coremap[startingInt+i].contiguous=false;//final page to be allocated
                } else {
                    coremap[startingInt+i].contiguous=true;
                }
            }
            addr=coremap[startingInt].adr;
        } else {
	        spinlock_release(&stealmem_lock);
	        kprintf("outa memory allocating frames\n");
            return ENOMEM;//no space in coremap, outa memory
        }
    } else {
        addr=ram_stealmem(npages);
    }
    #else
	addr = ram_stealmem(npages);
	#endif
	spinlock_release(&stealmem_lock);
	return addr;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t 
alloc_kpages(int npages)
{
    //kprintf("allocating kpages\n");
	paddr_t pa;
	pa = getppages(npages);
	if (pa==0) {
		return 0;
	}
	if(pa==ENOMEM) {//TODO test this
	    return ENOMEM;
	}
    //kprintf("done allocating kpages\n");
	return PADDR_TO_KVADDR(pa);
}

void 
free_kpages(vaddr_t addr)
{
    #if OPT_A3
    //kprintf("freeing kpages\n");
    spinlock_acquire(&stealmem_lock);//better grab this, dont want to try freeing stuff at same time
    if(coreMade==true){//not sure if free_kpages is run before coremap, but just incase keep this check here
        //convert addr since we want a paddr
        //check that the address is legit!
        if(addr==0) {
            spinlock_release(&stealmem_lock);
            //not sure what error would fit, but we definitely cant be accessing addr 0 as it has important stuff
            //return EINVAL;//goto error for random stuff (invalid argument)
            kprintf("freeing error");
            return;
        }
        
        //now lets search for it in coremap
        bool foundIt=false;
        for(int i=0; i<totalFrames; i++) {//probably mathematical ways to find it in O(1) but this 100% works
        //kprintf("core address %u\n", coremap[i].adr);
            if(coremap[i].adr==addr) {
                //kprintf("found starting address in coremap\n");
                foundIt=true;
            }
            if(foundIt==true){
                //kprintf("found it at %d\n",i);
                coremap[i].inUse=false;
                if(coremap[i].contiguous==false) {
                    //kprintf("not contiguous\n");
                    break;//no more to de-allocate
                }
            }
        }
    }
    //kprintf("done freeing kpages\n");
    spinlock_release(&stealmem_lock);
    #else
	/* nothing - leak the memory. */

	(void)addr;
	#endif
}

void
vm_tlbshootdown_all(void)
{
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;
	bool insideText=false;

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
	    #if OPT_A3
	        //can handle having readonly now!
	        return EINVAL; //a general purpose error since we dont have anything more specific
	    #else
		/* We always create pages read-write, so we can't get this */
		panic("dumbvm: got VM_FAULT_READONLY\n");
		#endif
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = curproc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
	KASSERT(as->as_vbase1 != 0);
	KASSERT(as->as_pbase1 != 0);
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
	KASSERT(as->as_pbase2 != 0);
	KASSERT(as->as_npages2 != 0);
	KASSERT(as->as_stackpbase != 0);
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
	KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
	KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);

	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

	if (faultaddress >= vbase1 && faultaddress < vtop1) {
		paddr = (faultaddress - vbase1) + as->as_pbase1;
		//according to notes this should be the text section
		insideText=true;
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
		paddr = (faultaddress - vbase2) + as->as_pbase2;
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
		paddr = (faultaddress - stackbase) + as->as_stackpbase;
	}
	else {
		return EFAULT;
	}

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
	    #if OPT_A3
	        if(insideText==true && as->as_loaded==true) {//check if as is fully loaded and we are in the text section
	            //kprintf("dirt bit on now\n");
	            elo &= ~TLBLO_DIRTY; //turn off as described in notes
	        }
	    #endif
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}

	
	
	#if OPT_A3
	    //kprintf("randomizing!\n");
	    //use the above for loop of writing an address for inspiration
	    ehi = faultaddress;
	    elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
	    if(insideText==true && as->as_loaded==true) {//check if as is fully loaded and we are in the text section
	            //kprintf("dirt bit on now\n");
	       elo &= ~TLBLO_DIRTY; //turn off as described in notes
	    }
	    //from tlb.h
	    tlb_random(ehi, elo);
	    splx(spl);
	    return 0;
	#else    
	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
	#endif
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
	as->as_stackpbase = 0;
	#if OPT_A3
	as->as_loaded=false;
	#endif

	return as;
}

void
as_destroy(struct addrspace *as)
{
    #if OPT_A3
        //number of pages is stored
    //for(unsigned i=0; i<as->as_npages1; i++) {
        //now free 
    //}
    //kprintf("1 pages is %d \n", as->as_npages1);
    free_kpages(as->as_pbase1);
    //kprintf("2 pages is %d \n", as->as_npages2);
    free_kpages(as->as_pbase2);
    free_kpages(as->as_stackpbase);
    #endif
	kfree(as);
}

void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = curproc_getas();
#ifdef UW
        /* Kernel threads don't have an address spaces to activate */
#endif
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void
as_deactivate(void)
{
	/* nothing */
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
    #if OPT_A3
    if(readable) {
        //kprintf("setting readable\n");
        as->as_readable=1;
    } else {
        as->as_readable=0;
    }
    if(writeable) {
        //kprintf("setting writeable\n");
        as->as_writeable=1;
    } else {
        as->as_writeable=0;
    }
    if(executable) {
        //kprintf("setting executable\n");
        as->as_executable=1;
    } else {
        as->as_executable=0;
    }
    #else
	(void)readable;
	(void)writeable;
	(void)executable;
    #endif

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

static
void
as_zero_region(paddr_t paddr, unsigned npages)
{
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}

int
as_prepare_load(struct addrspace *as)
{
	KASSERT(as->as_pbase1 == 0);
	KASSERT(as->as_pbase2 == 0);
	KASSERT(as->as_stackpbase == 0);

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
	
	as_zero_region(as->as_pbase1, as->as_npages1);
	as_zero_region(as->as_pbase2, as->as_npages2);
	as_zero_region(as->as_stackpbase, DUMBVM_STACKPAGES);

	return 0;
}

int
as_complete_load(struct addrspace *as)
{
    #if OPT_A3
        as->as_loaded=true;
    #else
	(void)as;
	#endif
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	KASSERT(as->as_stackpbase != 0);

	*stackptr = USERSTACK;
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

	/* (Mis)use as_prepare_load to allocate some physical memory. */
	if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}

	KASSERT(new->as_pbase1 != 0);
	KASSERT(new->as_pbase2 != 0);
	KASSERT(new->as_stackpbase != 0);

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
