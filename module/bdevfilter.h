/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_BDEVFILTER_H
#define __LINUX_BDEVFILTER_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/blk_types.h>

struct bdev_filter;
struct bdev_filter_operations {
	bool (*submit_bio_cb)(struct bio *bio, struct bdev_filter *flt);
	/*
	bool (*read_page_cb)(struct block_device *bdev,
			     sector_t sector, struct page *page,
			     struct bdev_filter *flt);
	bool (*write_page_cb)(struct block_device *bdev,
			      sector_t sector, struct page *page,
			      struct bdev_filter *flt);
	*/
	void (*detach_cb)(struct kref *kref);
};

/**
 * struct bdev_filter - Description of the block device filter.
 * @kref:
 *
 * @fops:
 *
 */
struct bdev_filter {
	struct kref kref;
	const struct bdev_filter_operations *fops;
};

static inline void bdev_filter_init(struct bdev_filter *flt,
		const struct bdev_filter_operations *fops)
{
	kref_init(&flt->kref);
	flt->fops = fops;
};

int bdev_filter_attach(struct block_device *bdev, const char *name,
		       struct bdev_filter *flt);
int bdev_filter_detach(struct block_device *bdev, const char *name);
struct bdev_filter *bdev_filter_get_by_bdev(struct block_device *bdev);
static inline void bdev_filter_get(struct bdev_filter *flt)
{
	kref_get(&flt->kref);
};
static inline void bdev_filter_put(struct bdev_filter *flt)
{
	if (likely(flt))
		kref_put(&flt->kref, flt->fops->detach_cb);
};

/* Only for livepatch version */
int lp_bdev_filter_detach(const dev_t dev_id, const char *name);

#if defined(HAVE_QC_SUBMIT_BIO_NOACCT)
extern blk_qc_t (*submit_bio_noacct_notrace)(struct bio *);
#elif defined(HAVE_VOID_SUBMIT_BIO_NOACCT)
extern void (*submit_bio_noacct_notrace)(struct bio *);
#endif

#endif /* __LINUX_BDEVFILTER_H */
