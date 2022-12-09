/**********************************************************************
 * Copyright (c) 2020-2022
 *  Sang-Hoon Kim <sanghoonkim@ajou.ac.kr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTIABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 **********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "types.h"
#include "list_head.h"
#include "vm.h"

/**
 * Ready queue of the system
 */
extern struct list_head processes;

/**
 * Currently running process
 */
extern struct process *current;

/**
 * Page Table Base Register that MMU will walk through for address translation
 */
extern struct pagetable *ptbr;

/**
 * TLB of the system.
 */
extern struct tlb_entry tlb[1UL << (PTES_PER_PAGE_SHIFT * 2)];


/**
 * The number of mappings for each page frame. Can be used to determine how
 * many processes are using the page frames.
 */
extern unsigned int mapcounts[];



/**
 * lookup_tlb(@vpn, @pfn)
 *
 * DESCRIPTION
 *   Translate @vpn of the current process through TLB. DO NOT make your own
 *   data structure for TLB, but use the defined @tlb data structure
 *   to translate. If the requested VPN exists in the TLB, return true
 *   with @pfn is set to its PFN. Otherwise, return false.
 *   The framework calls this function when needed, so do not call
 *   this function manually.
 *
 * RETURN
 *   Return true if the translation is cached in the TLB.
 *   Return false otherwise
 */
bool lookup_tlb(unsigned int vpn, unsigned int *pfn)
{	
	for(int i=0;i<NR_TLB_ENTRIES;i++){
		if(tlb[i].valid==false){
			continue;
		}else{
			if(tlb[i].vpn==vpn){
				*pfn=tlb[i].pfn;
				return true;
			}
		}
	}

	return false;
}


/**
 * insert_tlb(@vpn, @pfn)
 *
 * DESCRIPTION
 *   Insert the mapping from @vpn to @pfn into the TLB. The framework will call
 *   this function when required, so no need to call this function manually.
 *
 */
void insert_tlb(unsigned int vpn, unsigned int pfn)
{
	static int index=0;
	struct tlb_entry t = {true, vpn, pfn};
	tlb[index]=t;
	index++;

	return;
}


/**
 * alloc_page(@vpn, @rw)re
 *
 * DESCRIPTION
 *   Allocate a page frame that is not allocated to any process, and map it
 *   to @vpn. When the system has multiple free pages, this function should
 *   allocate the page frame with the **smallest pfn**.
 *   You may construct the page table of the @current process. When the page
 *   is allocated with RW_WRITE flag, the page may be later accessed for writes.
 *   However, the pages populated with RW_READ only should not be accessed with
 *   RW_WRITE accesses.
 *
 * RETURN
 *   Return allocated page frame number.
 *   Return -1 if all page frames are allocated.
 */
unsigned int alloc_page(unsigned int vpn, unsigned int rw)
{
	unsigned int pfn;
	for(int i=0;i<NR_PAGEFRAMES;i++){
		if(mapcounts[i]==0){
			pfn=i;
			break;
		}
	}
	int pd_index = vpn / NR_PTES_PER_PAGE; // look at this!!
	int pte_index = vpn % NR_PTES_PER_PAGE;

	if(pd_index >= NR_PTES_PER_PAGE){
		return -1;
	}

	bool isWrite;
	if(rw==RW_READ){
		isWrite=false;
	}else{
		isWrite=true;
	}
	bool wasWrite=isWrite;
	

	struct pte  p = {true, isWrite, pfn, wasWrite};
	mapcounts[pfn]++;
	

	if(!ptbr->outer_ptes[pd_index]){
		ptbr->outer_ptes[pd_index]=(struct pte_directory*)malloc(sizeof(struct pte)*NR_PTES_PER_PAGE);
	}
	struct pte_directory* pd = ptbr->outer_ptes[pd_index];
	pd->ptes[pte_index] = p;
	
	
	return pfn;
}


/**
 * free_page(@vpn)
 *
 * DESCRIPTION
 *   Deallocate the page from the current processor. Make sure that the fields
 *   for the corresponding PTE (valid, writable, pfn) is set @false or 0.
 *   Also, consider carefully for the case when a page is shared by two processes,
 *   and one process is to free the page.
 */
void free_page(unsigned int vpn)
{
	//tlb valid false
	for(int i=0;i<NR_TLB_ENTRIES;i++){
		if(tlb[i].valid==true){
			if(tlb[i].vpn==vpn){
				tlb[i].valid=false;
				break;
			}
		}
	}

	int pd_index = vpn / NR_PTES_PER_PAGE; 
	int pte_index = vpn % NR_PTES_PER_PAGE;

	struct pte_directory* pd = ptbr->outer_ptes[pd_index];
	mapcounts[pd->ptes[pte_index].pfn]--;
	struct pte p = {false,false,0,0};
	pd->ptes[pte_index].valid = false;
	pd->ptes[pte_index].writable=false;
	pd->ptes[pte_index].pfn=0;

	

	return;
}


/**
 * handle_page_fault()
 *
 * DESCRIPTION
 *   Handle the page fault for accessing @vpn for @rw. This function is called
 *   by the framework when the __translate() for @vpn fails. This implies;
 *   0. page directory is invalid
 *   1. pte is invalid
 *   2. pte is not writable but @rw is for write
 *   This function should identify the situation, and do the copy-on-write if
 *   necessary.
 *
 * RETURN
 *   @true on successful fault handling
 *   @false otherwise
 */
bool handle_page_fault(unsigned int vpn, unsigned int rw)
{	
	int pd_index = vpn / NR_PTES_PER_PAGE; 
	int pte_index = vpn % NR_PTES_PER_PAGE;
	struct pte p = ptbr->outer_ptes[pd_index]->ptes[pte_index];
	unsigned int pfn = p.pfn;
	bool wasWrite = p.private;

	if(rw==RW_WRITE){
		if(mapcounts[pfn]>1 && wasWrite){
			free_page(vpn);
			alloc_page(vpn, rw);
			return true;
		}else if(mapcounts[pfn]==1 && wasWrite){
			free_page(vpn);
			alloc_page(vpn, rw);
			return true;
		}
	}
	return false;
}


/**
 * switch_process()
 *
 * DESCRIPTION
 *   If there is a process with @pid in @processes, switch to the process.
 *   The @current process at the moment should be put into the @processes
 *   list, and @current should be replaced to the requested process.
 *   Make sure that the next process is unlinked from the @processes, and
 *   @ptbr is set properly.
 *
 *   If there is no process with @pid in the @processes list, fork a process
 *   from the @current. This implies the forked child process should have
 *   the identical page table entry 'values' to its parent's (i.e., @current)
 *   page table. 
 *   To implement the copy-on-write feature, you should manipulate the writable
 *   bit in PTE and mapcounts for shared pages. You may use pte->private for 
 *   storing some useful information :-)
 */
void switch_process(unsigned int pid)
{
	
	//find pid in processes
	struct process *pos;
	
	bool isfind=false;
	list_for_each_entry(pos, &processes, list){
		if(pos->pid==pid){
			isfind=true;
			break;
		}
	}

	//change
	if(isfind){
		list_del_init(&pos->list);
		list_add_tail(&current->list,&processes);
		current = pos;
		ptbr = &current->pagetable;

	}else{//fork
		list_add_tail(&current->list,&processes);
		struct pagetable* prev;
		struct process* fork=(struct process*)malloc(sizeof(struct process));

		prev=&current->pagetable;
		fork->pid=pid;
		
		INIT_LIST_HEAD(&fork->list);
		ptbr=&fork->pagetable;
		//copy parent ptbr to child
		struct pte_directory* pd;
		for(int i=0;i<NR_PTES_PER_PAGE;i++){
			if(prev->outer_ptes[i]){
				if(!ptbr->outer_ptes[i]){
					ptbr->outer_ptes[i]=(struct pte_directory*)malloc(sizeof(struct pte)*NR_PTES_PER_PAGE);
				}
				pd=prev->outer_ptes[i];
				for(int j=0;j<NR_PTES_PER_PAGE;j++){
					if(pd->ptes[j].valid==true){
						struct pte p;
						pd->ptes[j].writable=false;
						struct pte copy=pd->ptes[j];
						
						//Deep copy
						p.valid=copy.valid;
						p.writable=copy.writable;
						p.pfn=copy.pfn;
						p.private=copy.private;

						ptbr->outer_ptes[i]->ptes[j]=p;
						mapcounts[p.pfn]++;
					}
				}
			}
		}
		current=fork;
		
	}
	for(int i=0;i<NR_TLB_ENTRIES;i++){
		if(tlb[i].valid==true){
			tlb[i].valid=false;
		}
	}
	return;
}

