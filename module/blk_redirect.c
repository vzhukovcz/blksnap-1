#include "common.h"
#include "blk_util.h"
#include "blk_redirect.h"
#include "blk_direct.h"

#define SECTION "blk       "
#include "log_format.h"

#define bio_vec_sectors(bv) (bv.bv_len >> SECTOR_SHIFT)

struct bio_set g_BlkRedirectBioset = { 0 };

int blk_redirect_bioset_create( void )
{
	return bioset_init(&g_BlkRedirectBioset, 64, 0, BIOSET_NEED_BVECS | BIOSET_NEED_RESCUER);
}

void blk_redirect_bioset_free( void )
{
	bioset_exit(&g_BlkRedirectBioset);
}

void blk_redirect_bio_endio( struct bio *bb )
{
	blk_redirect_bio_t* rq_redir = (blk_redirect_bio_t*)bb->bi_private;

	if (rq_redir != NULL) {
		int err = SUCCESS;

		if (bb->bi_status != BLK_STS_OK)
			err = -EIO;

		if (err != SUCCESS){
			log_err_d( "Failed to process redirect IO request. errno=", 0 - err );

			if (rq_redir->err == SUCCESS)
				rq_redir->err = err;
		}

		if (atomic64_dec_and_test( &rq_redir->bio_count ))
			blk_redirect_complete( rq_redir, rq_redir->err );
	}
	bio_put( bb );
}


struct bio* _blk_dev_redirect_bio_alloc( int nr_iovecs, void* bi_private )
{
	struct bio* new_bio = bio_alloc_bioset( GFP_NOIO, nr_iovecs, &g_BlkRedirectBioset );
	if (new_bio){
		new_bio->bi_end_io = blk_redirect_bio_endio;
		new_bio->bi_private = bi_private;
	}
	return new_bio;
}

blk_redirect_bio_list_t* _redirect_bio_allocate_list( struct bio* new_bio )
{
	blk_redirect_bio_list_t* next = kzalloc( sizeof( blk_redirect_bio_list_t ), GFP_NOIO );
	if (next){
		next->next = NULL;
		next->this = new_bio;
	}
	return next;
}

int  bio_endio_list_push( blk_redirect_bio_t* rq_redir, struct bio* new_bio )
{
	blk_redirect_bio_list_t* head;

	if (rq_redir->bio_list_head == NULL){
		if (NULL == (rq_redir->bio_list_head = _redirect_bio_allocate_list( new_bio )))
			return -ENOMEM;
		return SUCCESS;
	}

	head = rq_redir->bio_list_head;
	while (head->next != NULL)
		head = head->next;

	if (NULL == (head->next = _redirect_bio_allocate_list( new_bio )))
		return -ENOMEM;
	return SUCCESS;
}

void bio_endio_list_cleanup( blk_redirect_bio_list_t* curr )
{
	while (curr != NULL){
		blk_redirect_bio_list_t* next = curr->next;
		kfree( curr );
		curr = next;
	}
}

int _blk_dev_redirect_part_fast( blk_redirect_bio_t* rq_redir, int direction, struct block_device*  blk_dev, sector_t target_pos, sector_t rq_ofs, sector_t rq_count )
{
	__label__ __fail_out;
	__label__ __reprocess_bv;

	int res = SUCCESS;

	struct bio_vec bvec;
	struct bvec_iter iter;

	struct bio* new_bio = NULL;

	sector_t sect_ofs = 0;
	sector_t processed_sectors = 0;
	int nr_iovecs;
	unsigned int max_sect;
	blk_redirect_bio_list_t* bio_endio_rec;

	{
		struct request_queue *q = bdev_get_queue( blk_dev );
		max_sect = min( (unsigned int)(BIO_MAX_PAGES << (PAGE_SHIFT - SECTOR_SHIFT)), q->limits.max_sectors );
	}

	nr_iovecs = max_sect >> (PAGE_SHIFT - SECTOR_SHIFT);

	bio_for_each_segment( bvec, rq_redir->bio, iter ){
		sector_t bvec_ofs;
		sector_t bvec_sectors;

		if ((sect_ofs + bio_vec_sectors( bvec )) <= rq_ofs){
			sect_ofs += bio_vec_sectors( bvec );
			continue;
		}
		if (sect_ofs >= (rq_ofs + rq_count))
			break;

		bvec_ofs = 0;
		if (sect_ofs < rq_ofs)
			bvec_ofs = rq_ofs - sect_ofs;

		bvec_sectors = bio_vec_sectors( bvec ) - bvec_ofs;
		if (bvec_sectors >( rq_count - processed_sectors ))
			bvec_sectors = rq_count - processed_sectors;

		if (bvec_sectors == 0){
			res = -EIO;
			goto __fail_out;
		}

__reprocess_bv:
		if (new_bio == NULL){
			while (NULL == (new_bio = _blk_dev_redirect_bio_alloc( nr_iovecs, rq_redir ))){
				log_err( "Unable to allocate new bio for redirect IO." );
				res = -ENOMEM;
				goto __fail_out;
			}

			bio_set_dev(new_bio, blk_dev);

			if (direction == READ)
				bio_set_op_attrs( new_bio, REQ_OP_READ, 0 );

			if (direction == WRITE)
				bio_set_op_attrs( new_bio, REQ_OP_WRITE, 0 );

			new_bio->bi_iter.bi_sector = target_pos + processed_sectors;
		}

		if (0 == bio_add_page( new_bio, bvec.bv_page, (unsigned int)from_sectors( bvec_sectors ),
			bvec.bv_offset + (unsigned int)from_sectors( bvec_ofs ) )){
			if (bio_sectors( new_bio ) == 0){
				res = -EIO;
				goto __fail_out;
			}

			res = bio_endio_list_push( rq_redir, new_bio );
			if (res != SUCCESS){
				log_tr( "Failed to add bio into bio_endio_list" );
				goto __fail_out;
			}

			atomic64_inc( &rq_redir->bio_count );
			new_bio = NULL;

			goto __reprocess_bv;
		}
		processed_sectors += bvec_sectors;

		sect_ofs += bio_vec_sectors( bvec );
	}

	if (new_bio != NULL){
		res = bio_endio_list_push( rq_redir, new_bio );
		if (res != SUCCESS){
			log_tr( "Failed to add bio into bio_endio_list" );
			goto __fail_out;
		}

		atomic64_inc( &rq_redir->bio_count );
		new_bio = NULL;
	}

	return SUCCESS;

__fail_out:
	bio_endio_rec = rq_redir->bio_list_head;
	while (bio_endio_rec != NULL){

		if (bio_endio_rec->this != NULL)
			bio_put( bio_endio_rec->this );

		bio_endio_rec = bio_endio_rec->next;
	}

	bio_endio_list_cleanup( bio_endio_rec );

	log_err_format( "Failed to process part of redirect IO request. rq_ofs=%lld, rq_count=%lld", rq_ofs, rq_count );
	return res;
}

int blk_dev_redirect_part( blk_redirect_bio_t* rq_redir, int direction, struct block_device* blk_dev, sector_t target_pos, sector_t rq_ofs, sector_t rq_count )
{
	struct request_queue *q = bdev_get_queue( blk_dev );
	sector_t logical_block_size_mask = (sector_t)((q->limits.logical_block_size >> SECTOR_SHIFT) - 1);

	if (likely( logical_block_size_mask == 0 ))
		return _blk_dev_redirect_part_fast( rq_redir, direction, blk_dev, target_pos, rq_ofs, rq_count );

	if (likely( (0 == (target_pos & logical_block_size_mask)) && (0 == (rq_count & logical_block_size_mask)) ))
		return _blk_dev_redirect_part_fast( rq_redir, direction, blk_dev, target_pos, rq_ofs, rq_count );

	return -EFAULT;
}


void blk_dev_redirect_submit( blk_redirect_bio_t* rq_redir )
{
	blk_redirect_bio_list_t* head;
	blk_redirect_bio_list_t* curr;

	head = curr = rq_redir->bio_list_head;
	rq_redir->bio_list_head = NULL;

	while (curr != NULL) {
		submit_bio( curr->this );

		curr = curr->next;
	}

	bio_endio_list_cleanup( head );
}


int blk_dev_redirect_memcpy_part( blk_redirect_bio_t* rq_redir, int direction, void* buff, sector_t rq_ofs, sector_t rq_count )
{
	struct bio_vec bvec;
	struct bvec_iter iter;

	sector_t sect_ofs = 0;
	sector_t processed_sectors = 0;

	bio_for_each_segment( bvec, rq_redir->bio, iter ){
		sector_t bvec_ofs;
		sector_t bvec_sectors;

		if ((sect_ofs + bio_vec_sectors( bvec )) <= rq_ofs){
			sect_ofs += bio_vec_sectors( bvec );
			continue;
		}
		if (sect_ofs >= (rq_ofs + rq_count)){
			break;
		}

		bvec_ofs = 0;
		if (sect_ofs < rq_ofs){
			bvec_ofs = rq_ofs - sect_ofs;
		}

		bvec_sectors = bio_vec_sectors( bvec ) - bvec_ofs;
		if (bvec_sectors >( rq_count - processed_sectors ))
			bvec_sectors = rq_count - processed_sectors;

		{
			void* mem = kmap_atomic( bvec.bv_page );
			if (direction == READ){
				memcpy(
					mem + bvec.bv_offset + (unsigned int)from_sectors( bvec_ofs ),
					buff + (unsigned int)from_sectors( processed_sectors ),
					(unsigned int)from_sectors( bvec_sectors ) );
			}
			else{
				memcpy(
					buff + (unsigned int)from_sectors( processed_sectors ),
					mem + bvec.bv_offset + (unsigned int)from_sectors( bvec_ofs ),
					(unsigned int)from_sectors( bvec_sectors ) );
			}
			kunmap_atomic( mem );
		}

		processed_sectors += bvec_sectors;

		sect_ofs += bio_vec_sectors( bvec );
	}

	return SUCCESS;
}

int blk_dev_redirect_zeroed_part( blk_redirect_bio_t* rq_redir, sector_t rq_ofs, sector_t rq_count )
{
	struct bio_vec bvec;
	struct bvec_iter iter;

	sector_t sect_ofs = 0;
	sector_t processed_sectors = 0;

	bio_for_each_segment( bvec, rq_redir->bio, iter ){
		sector_t bvec_ofs;
		sector_t bvec_sectors;

		if ((sect_ofs + bio_vec_sectors( bvec )) <= rq_ofs){
			sect_ofs += bio_vec_sectors( bvec );
			continue;
		}
		if (sect_ofs >= (rq_ofs + rq_count)){
			break;
		}

		bvec_ofs = 0;
		if (sect_ofs < rq_ofs){
			bvec_ofs = rq_ofs - sect_ofs;
		}

		bvec_sectors = bio_vec_sectors( bvec ) - bvec_ofs;
		if (bvec_sectors >( rq_count - processed_sectors ))
			bvec_sectors = rq_count - processed_sectors;

		{
			void* mem = kmap_atomic( bvec.bv_page );

			memset( mem + bvec.bv_offset + (unsigned int)from_sectors( bvec_ofs ), 0,
				(unsigned int)from_sectors( bvec_sectors ) );

			kunmap_atomic( mem );
		}

		processed_sectors += bvec_sectors;

		sect_ofs += bio_vec_sectors( bvec );
	}

	return SUCCESS;
}

int blk_dev_redirect_read_zeroed(blk_redirect_bio_t* rq_redir, struct block_device*  blk_dev,
	sector_t rq_pos, sector_t blk_ofs_start, sector_t blk_ofs_count,
	rangevector_t* zero_sectors )
{
	int res = SUCCESS;
	sector_t current_portion;
	sector_t ofs = 0;

	rangevector_el_t* el = NULL;

	sector_t from_sect;
	sector_t to_sect;

	BUG_ON( NULL == zero_sectors );

	down_read( &zero_sectors->lock );
	RANGEVECTOR_FOREACH_EL_BEGIN( zero_sectors, el )
	{
		struct blk_range* first_zero_range;
		struct blk_range* last_zero_range;
		size_t limit;

		limit = (size_t)atomic_read( &el->cnt );
		if (limit <= 0)
			continue;

		first_zero_range = &el->ranges[0];
		last_zero_range = &el->ranges[limit - 1];

		from_sect = (rq_pos + blk_ofs_start + ofs);
		to_sect = (rq_pos + blk_ofs_start + blk_ofs_count);

		if ((last_zero_range->ofs + last_zero_range->cnt) <= from_sect){
			continue;
		}

		if (first_zero_range->ofs >= to_sect){
			break;
		}

		while (from_sect < to_sect){
			struct blk_range* zero_range;
			zero_range = rangevector_el_find_first_hit( el, from_sect, to_sect );
			if (zero_range == NULL)
				break;

			if (zero_range->ofs > rq_pos + blk_ofs_start + ofs){
				sector_t pre_zero_cnt = zero_range->ofs - (rq_pos + blk_ofs_start + ofs);

				res = blk_dev_redirect_part( rq_redir, READ, blk_dev, rq_pos + blk_ofs_start + ofs, blk_ofs_start + ofs, pre_zero_cnt );
				if (res != SUCCESS){
					break;
				}
				ofs += pre_zero_cnt;
			}

			current_portion = min_t( sector_t, zero_range->cnt, blk_ofs_count - ofs );

			res = blk_dev_redirect_zeroed_part( rq_redir, blk_ofs_start + ofs, current_portion );
			if (res != SUCCESS){
				break;
			}
			ofs += current_portion;

			from_sect = (rq_pos + blk_ofs_start + ofs);
		};
	}
	RANGEVECTOR_FOREACH_EL_END( );
	up_read( &zero_sectors->lock );
	if ((blk_ofs_count - ofs) > 0)
		res = blk_dev_redirect_part( rq_redir, READ, blk_dev, rq_pos + blk_ofs_start + ofs, blk_ofs_start + ofs, blk_ofs_count - ofs );
	return res;
}

void blk_redirect_complete( blk_redirect_bio_t* rq_redir, int res )
{
	rq_redir->complete_cb( rq_redir->complete_param, rq_redir->bio, res );
	redirect_bio_queue_free( rq_redir );
}




void redirect_bio_queue_init( redirect_bio_queue_t* queue)
{
	INIT_LIST_HEAD( &queue->list );

	spin_lock_init( &queue->lock );

	atomic_set( &queue->in_queue_cnt, 0);
	atomic_set( &queue->alloc_cnt, 0 );

	atomic_set( &queue->active_state, true );
}

blk_redirect_bio_t* redirect_bio_queue_new( redirect_bio_queue_t* queue )
{
	blk_redirect_bio_t* rq_redir = kzalloc( sizeof(blk_redirect_bio_t), GFP_NOIO );

	if (NULL == rq_redir)
		return NULL;

	atomic_inc( &queue->alloc_cnt );

	INIT_LIST_HEAD( &rq_redir->link );
	rq_redir->queue = queue;

	return rq_redir;
}

void redirect_bio_queue_free( blk_redirect_bio_t* rq_redir )
{
	if (rq_redir) {
		if (rq_redir->queue)
			atomic_dec( &rq_redir->queue->alloc_cnt );

		kfree( rq_redir );
	}
}

int redirect_bio_queue_push_back( redirect_bio_queue_t* queue, blk_redirect_bio_t* rq_redir )
{
	int res = SUCCESS;

	spin_lock( &queue->lock );

	if (atomic_read( &queue->active_state )) {
		INIT_LIST_HEAD( &rq_redir->link );
		list_add_tail( &rq_redir->link, &queue->list );
		atomic_inc( &queue->in_queue_cnt );
	}
	else
		res = -EACCES;

	spin_unlock( &queue->lock );
	return res;
}

blk_redirect_bio_t* redirect_bio_queue_get_first( redirect_bio_queue_t* queue )
{
	blk_redirect_bio_t* rq_redir = NULL;

	spin_lock( &queue->lock );

	if (!list_empty( &queue->list )) {
		rq_redir = list_entry( queue->list.next, blk_redirect_bio_t, link );
		list_del( &rq_redir->link );
		atomic_dec( &queue->in_queue_cnt );
	}

	spin_unlock( &queue->lock );

	return rq_redir;
}

bool redirect_bio_queue_active( redirect_bio_queue_t* queue, bool state )
{
	bool prev_state;

	spin_lock( &queue->lock );

	prev_state = atomic_read( &queue->active_state );
	atomic_set( &queue->active_state, state );

	spin_unlock( &queue->lock );

	return prev_state;
}
