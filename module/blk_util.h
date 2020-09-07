#pragma once

#include "page_array.h"
#include "sector.h"

typedef struct blk_dev_info_s
{
    size_t blk_size;
    sector_t start_sect;
    sector_t count_sect;

    unsigned int io_min;
    unsigned int physical_block_size;
    unsigned short logical_block_size;

}blk_dev_info_t;


int blk_dev_open( dev_t dev_id, struct block_device** p_blk_dev );

void blk_dev_close( struct block_device* blk_dev );


int blk_dev_get_info( dev_t dev_id, blk_dev_info_t* pdev_info );
int _blk_dev_get_info( struct block_device* blk_dev, blk_dev_info_t* pdev_info );

int blk_freeze_bdev( dev_t dev_id, struct block_device* device, struct super_block** psuperblock );
struct super_block* blk_thaw_bdev( dev_t dev_id, struct block_device* device, struct super_block* superblock );

static __inline sector_t blk_dev_get_capacity( struct block_device* blk_dev )
{
    return blk_dev->bd_part->nr_sects;
};

static __inline sector_t blk_dev_get_start_sect( struct block_device* blk_dev )
{
    return blk_dev->bd_part->start_sect;
};


#define bio_vec_page(bv)    bv.bv_page
#define bio_vec_offset(bv)  bv.bv_offset
#define bio_vec_len(bv)     bv.bv_len
#define bio_vec_buffer(bv)  (page_address( bv.bv_page ) + bv.bv_offset)
#define bio_vec_sectors(bv) (bv.bv_len>>SECTOR_SHIFT)

#define bio_bi_sector(bio)  bio->bi_iter.bi_sector
#define bio_bi_size(bio)    bio->bi_iter.bi_size

static inline
sector_t blk_bio_io_vec_sectors( struct bio* bio )
{
    sector_t sect_cnt = 0;
    struct bio_vec bvec;
    struct bvec_iter iter;

    bio_for_each_segment( bvec, bio, iter ){
        sect_cnt += ( bio_vec_len( bvec ) >> SECTOR_SHIFT );
    }
    return sect_cnt;
}

