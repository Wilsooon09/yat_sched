#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/highmem.h>
#include <asm/page.h>
#include <linux/miscdevice.h>
#include <linux/module.h>

#include <yat/yat.h>

/* device for allocating pages not cached by the CPU */

#define UNCACHE_NAME        "yat/uncache"

void yat_uncache_vm_open(struct vm_area_struct *vma)
{
}

void yat_uncache_vm_close(struct vm_area_struct *vma)
{
}

vm_fault_t yat_uncache_vm_fault(struct vm_fault* vmf)
{
	/* modeled after SG DMA video4linux, but without DMA. */
	/* (see drivers/media/video/videobuf-dma-sg.c) */
	struct page *page;

	page = alloc_page(GFP_USER);
	if (!page)
		return VM_FAULT_OOM;

	clear_user_highpage(page, (unsigned long)vmf->address);
	vmf->page = page;

	return 0;
}

static struct vm_operations_struct yat_uncache_vm_ops = {
	.open = yat_uncache_vm_open,
	.close = yat_uncache_vm_close,
	.fault = yat_uncache_vm_fault,
};

static int yat_uncache_mmap(struct file* filp, struct vm_area_struct* vma)
{
	/* first make sure mapper knows what he's doing */

	/* you can only map the "first" page */
	if (vma->vm_pgoff != 0)
		return -EINVAL;

	/* you can't share it with anyone */
	if (vma->vm_flags & (VM_MAYSHARE | VM_SHARED))
		return -EINVAL;

	/* cannot be expanded, and is not a "normal" page. */
	vma->vm_flags |= VM_DONTEXPAND;

	/* noncached pages are not explicitly locked in memory (for now). */
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	vma->vm_ops = &yat_uncache_vm_ops;

	return 0;
}

static struct file_operations yat_uncache_fops = {
	.owner = THIS_MODULE,
	.mmap  = yat_uncache_mmap,
};

static struct miscdevice yat_uncache_dev = {
	.name  = UNCACHE_NAME,
	.minor = MISC_DYNAMIC_MINOR,
	.fops  = &yat_uncache_fops,
	/* pages are not locked, so there is no reason why
	   anyone cannot allocate an uncache pages */
	.mode  = (S_IRUGO | S_IWUGO),
};

static int __init init_yat_uncache_dev(void)
{
	int err;

	printk("Initializing YAT^RT uncache device.\n");
	err = misc_register(&yat_uncache_dev);
	if (err)
		printk("Could not allocate %s device (%d).\n", UNCACHE_NAME, err);
	return err;
}

static void __exit exit_yat_uncache_dev(void)
{
	misc_deregister(&yat_uncache_dev);
}

module_init(init_yat_uncache_dev);
module_exit(exit_yat_uncache_dev);
