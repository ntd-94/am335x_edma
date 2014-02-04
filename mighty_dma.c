
/*
 *
 * Using the DMA Pool API (Documentation/DMA-API-HOWTO.txt) to allocate memory for DMA transfers
 *
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/dmapool.h>
#include <linux/dma-mapping.h>	
#include <linux/dmaengine.h>
#include <linux/genalloc.h>	
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <linux/list.h>

#include <linux/platform_device.h>		/* Register/Unregister Platform Drivers */
#include <linux/platform_data/edma.h>   /* edmacc_param type */

#include "virt-dma.h"

#define DEBUG		1	
#define PINGCHAN	20
#define PONGCHAN	64
#define DMA_POOL_SIZE	4096

#define EDMA_CTLRS	2
#define EDMA_CHANS	32

/* Max of 16 segments per channel to conserve PaRAM slots */
#define MAX_NR_SG		16
#define EDMA_MAX_SLOTS		MAX_NR_SG
#define EDMA_DESCRIPTORS	16

/* from d/d/edma.c */
struct edma_desc {
	//struct virt_dma_desc	vdesc;
	//struct list_head		node;
	int				absync;
	int				pset_nr;
	struct edmacc_param		pset[0];
};

struct edma_cc;

struct edma_chan {
	struct virt_dma_chan		vchan;
	//struct list_head		node;
	struct edma_desc		*edesc;
	struct edma_cc			*ecc;
	int				ch_num;
	bool				alloced;
	int				slot[EDMA_MAX_SLOTS];
	//struct dma_slave_config	cfg;
	//struct dmaengine_chan_caps	caps;
};

struct edma_cc {
	int				ctlr;
	//struct dma_device		dma_slave;
	struct edma_chan		slave_chans[EDMA_CHANS];
	int				num_slave_chans;
	int				slot_20;			/* TESTING */
	int 				slot_17;			/* FOR SPI TRIGGER */
	
	struct ebic_dmac		*dmac_a;
	struct ebic_dmac		*dmac_b;

	struct mem_addr			*kmem_addr_a;		/* Memory Address returned from dma_pool_alloc */
	struct mem_addr			*kmem_addr_b;

};


/* DMA memory addresses */
struct mem_addr {

	u32	src_addr;
	u32	dst_addr;
};

/* EBIC DMA Channel Structure */
struct ebic_dmac {
	dma_addr_t		dma_handle;		/* DMA Handle set by dma_pool_alloc */

	struct edmacc_param	*dma_test_params;
	struct edmacc_param	*pingset;
	struct edmacc_param	*pongset;

	spinlock_t		lock;
	int			dma_ch;
	int			dma_link[2];
	int			slot;
};

static struct dma_pool *pool_a;				/* Required for dma_pool_create */
static struct dma_pool *pool_b;

/*
static struct edmacc_param pingset = {
	.opt = 0,
	.src = 0,
	.a_b_cnt = 0,
	.dst = 0,
	.src_dst_bidx = 0,
	.link_bcntrld = 0,
	.src_dst_cidx = 0,
	.ccnt = 0,
};


static struct edmacc_param pongset = {
	.opt = 0,
	.src = 0,
	.a_b_cnt = 0,
	.dst = 0,
	.src_dst_bidx = 0,
	.link_bcntrld = 0,
	.src_dst_cidx = 0,
	.ccnt = 0,
};
*/

static inline struct edma_chan *to_edma_chan(struct dma_chan *c) {
	return container_of(c, struct edma_chan, vchan.chan);
}

static void dma_callback(unsigned link, u16 ch_status, void *data) {

	printk(KERN_INFO "EDMA transfer test: link=%d, status=0x%x\n", link, ch_status);

	if (unlikely(ch_status != DMA_COMPLETE))
		return;
}

/* 
* Allocate CHANNELS
*
*  Here's the skinny: Allocating dma_channel (master), exception handler if RET < 0 then allocation failed
*	then print to kernel debug (dmesg | tail) then I'm setting RET equal to the struct ebic_dmac 
* 	field dma_chan for later use.
*
*/
static int edma_alloc_chan_resources(struct dma_chan *chan) {

	struct edma_chan *echan = to_edma_chan(chan);
	//struct device *dev = chan->device->dev;
	int a_ch_num;
	int ret;

	a_ch_num = edma_alloc_channel(echan->ch_num, dma_callback, chan, EVENTQ_0);

	if (a_ch_num < 0) {
		printk(KERN_INFO "allocating channel for DMA failed\n");
		ret = -EINVAL;
		return ret;
	}

	echan->alloced = true;
	echan->slot[0] = a_ch_num;
#ifdef DEBUG
	printk(KERN_INFO "ret=%d, dma_ch=%d\n", ret, echan->slot[0]);
#endif
	return 0;
}

static int edma_probe(struct platform_device *pdev) {

	struct edma_cc *ecc;

	struct ebic_dmac *dmac_a;
	struct ebic_dmac *dmac_b;

	struct mem_addr *kmem_addr_a;
	struct mem_addr *kmem_addr_b;

#ifdef DEBUG
	printk(KERN_INFO "Entering edma_probe function\n");
	printk(KERN_INFO "Allocating Memory for DMA\n");
#endif

	ecc = devm_kzalloc(&pdev->dev, sizeof(*ecc), GFP_KERNEL);
	if (!ecc) {
		printk(KERN_INFO "ecc memory can not be allocated\n");
		return -ENOMEM;
	}

	ecc->ctlr = pdev->id;
	
	dmac_a = kzalloc(sizeof(struct ebic_dmac), GFP_KERNEL);
	dmac_b = kzalloc(sizeof(struct ebic_dmac), GFP_KERNEL);


	if (!dmac_a) {
		printk(KERN_ERR "%s: failed to alloc mem for dmac_a\n", __func__);
		return -ENOMEM;
	}

	if (!dmac_b) {
		printk(KERN_ERR "%s: failed to alloc mem for dmac_b\n", __func__);
		return -ENOMEM;
	}

	pool_a = dma_pool_create("pool_a memtest", NULL, DMA_POOL_SIZE, 16, 0);
	pool_b = dma_pool_create("pool_b memtest", NULL, DMA_POOL_SIZE, 16, 0);
	

	if (!pool_a) {
		printk(KERN_ERR "%s: failed to create pool_a\n", __func__);
		return -ENOMEM;
	}

	if (!pool_b) {
		printk(KERN_ERR "%s: failed to create pool_b\n", __func__);
		return -ENOMEM;
	}


	kmem_addr_a = dma_pool_alloc(pool_a, GFP_ATOMIC, &dmac_a->dma_handle);
	kmem_addr_b = dma_pool_alloc(pool_b, GFP_ATOMIC, &dmac_b->dma_handle);


	if (!kmem_addr_a) {
		printk(KERN_ERR "%s: failed to allocate memory for kmem_addr_a\n", __func__);
		return -ENOMEM;
	}
	if (!kmem_addr_b) {
		printk(KERN_ERR "%s: failed to allocate memory for kmem_addr_b\n", __func__);
		return -ENOMEM;
	}
s

	//ecc->kmem_addr_a = kmem_addr_a;
	//ecc->kmem_addr_b = kmem_addr_b;

#ifdef DEBUG
	printk(KERN_INFO "dma_handle_a: 0x%08x dma_handle_b: 0x%08x", ecc->dmac_a->dma_handle, ecc->dmac_b->dma_handle);
	printk(KERN_INFO "kmem_addr_a:%p, kmem_addr_b:%p", &ecc->kmem_addr_a, &ecc->kmem_addr_b);
	printk(KERN_INFO "kmem_addr_a->src_addr: %08x, kmem_addr_b->src_addr: %08x", ecc->kmem_addr_a->src_addr, ecc->kmem_addr_b->dst_addr);
	printk(KERN_INFO "kmem_addr_a->dst_addr: %08x, kmem_addr_b->dst_addr: %08x", ecc->kmem_addr_a->dst_addr, ecc->kmem_addr_b->dst_addr);
#endif
	// Run the EDMA Allocate Channel function
	//edma_alloc_chan_resources;

	/* 
	* Allocate SLOTS
	*
	*  Here's the skinny: For testing I just need a single secondary channel to transfer data and test this shit
	*
	*/
	/*
	ret = edma_alloc_slot(EDMA_CTLR(edev->dma_ch), EDMA_SLOT_ANY);
	if (ret < 0) {
		printk(KERN_INFO "allocating slot for DMA failed\n");
		return ret;
	}
	edev->dma_link[0] = ret;
	printk(KERN_INFO "ret=%d, dma_link[0]=%d\n", ret, edev->dma_link[0]);
	
	// Allocate slot before writing, first arg should be the returned INT from edma_alloc_slot (REF: L447 davinci-pcm.c)
	edma_write_slot(ret, &edev->pingset);
	edev->slot = ret;

	// Enable DMA
	ret = edma_start(edev->dma_ch);
	if (ret != 0) {
		printk(KERN_INFO "edma_start failed\n");
		return ret;
	}
	*/

	//ecc->dmac_a = dmac_a;
	//ecc->dmac_b = dmac_b;

	return 0;
}

static int edma_remove(struct platform_device *pdev) {

	struct device *dev = &pdev->dev;
	struct edma_cc *ecc = dev_get_drvdata(dev);						/* return dev->driver_data; */

	printk(KERN_INFO "Freeing all dma_pool memory\n");

	dma_pool_free(pool_a, &ecc->kmem_addr_a, ecc->dmac_a->dma_handle);
	dma_pool_free(pool_b, &ecc->kmem_addr_b, ecc->dmac_b->dma_handle);
	kfree(ecc->dmac_a);
	kfree(ecc->dmac_b);
	dma_pool_destroy(pool_a);
	dma_pool_destroy(pool_b);


	printk(KERN_INFO "dmac_a->dma_chan=%d\n", ecc->dmac_a->dma_ch);
	printk(KERN_INFO "dmac_b->dma_chan%d\n", ecc->dmac_b->dma_ch);
	// Disable EDMA
	edma_stop(ecc->dmac_a->dma_ch);
	/* Moved from edma_remove */
	edma_free_slot(ecc->slot_20); 

	return 0;
}

static struct platform_driver edma_driver = {
	.probe = edma_probe,
	.remove	= edma_remove,
	.driver = {
		.name = "mighty-ebic-dma",
		.owner = THIS_MODULE,
	},

};

static struct platform_device *pdev0;

static const struct platform_device_info edma_dev_info0 = {
	.name = "mighty-ebic-dma",
	.id = 0,
	.dma_mask = DMA_BIT_MASK(32),
};

static int __init dma_init(void) {

	int ret = platform_driver_register(&edma_driver);

	if (ret == 0) {
		pdev0 = platform_device_register_full(&edma_dev_info0);
		if (IS_ERR(pdev0)) {
			platform_driver_unregister(&edma_driver);
			ret = PTR_ERR(pdev0);
			goto out;
		}
	}

out:
	return ret;
}
subsys_initcall(dma_init);

static void __exit dma_exit(void) {

	printk(KERN_INFO "Unregistering Device and Driver\n");

	platform_device_unregister(pdev0);
	platform_driver_unregister(&edma_driver);

}
module_exit(dma_exit);
 
MODULE_AUTHOR("Ephemeron Labs Inc.");
MODULE_DESCRIPTION("Basic EDMA to Network Transfers");
MODULE_LICENSE("GPL");
