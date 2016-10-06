/*
   raid0.c : Multiple Devices driver for Linux
             Copyright (C) 1994-96 Marc ZYNGIER
	     <zyngier@ufr-info-p7.ibp.fr> or
	     <maz@gloups.fdn.fr>
             Copyright (C) 1999, 2000 Ingo Molnar, Red Hat


   RAID-0 management functions.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.
   
   You should have received a copy of the GNU General Public License
   (for example /usr/src/linux/COPYING); if not, write to the Free
   Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.  
*/

#include <linux/blkdev.h>
#include <linux/seq_file.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include "md.h"
#include "hybrid_storage.h"

#define DIRECT_MODE (0)

#if DIRECT_MODE
#define SSD (0)
#else
#define ONLY_SSD (0)
#endif

#define SSD_META_GRP 			(60)
#define SSD_META_IN_MB     		(128 * SSD_META_GRP)
#define SSD_META_IN_SECTORS 	(SSD_META_IN_MB * 1024 * 2)
#define SSD_DATA_GRP 			(16)
#define SSD_DATA_IN_MB 			(128 * SSD_DATA_GRP)
#define SSD_DATA_IN_SECTORS 	(SSD_DATA_IN_MB * 1024 * 2)
#define DATA_START_IN_SECTORS 	(SSD_META_IN_SECTORS + SSD_DATA_IN_SECTORS)


#define BLK_BITMAP_START_IN_BLOCK 	(1025)
#define INO_BITMAP_START_IN_BLOCK	(4750)
#define INO_TABLE_START_IN_BLOCK	(8475)
#define JOURNAL_START_IN_BLOCK		(59 * 128 * 256)
static int hstorage_mergeable_bvec(struct request_queue *q,
				struct bvec_merge_data *bvm,
				struct bio_vec *biovec)
{
	struct mddev *mddev = q->queuedata;
	sector_t sector = bvm->bi_sector + get_start_sect(bvm->bi_bdev);
	int max;
	unsigned int chunk_sectors = mddev->chunk_sectors;
	unsigned int bio_sectors = bvm->bi_size >> 9;


	if (is_power_of_2(chunk_sectors))
		max =  (chunk_sectors - ((sector & (chunk_sectors-1))
						+ bio_sectors)) << 9;
	else
		max =  (chunk_sectors - (sector_div(sector, chunk_sectors)
						+ bio_sectors)) << 9;
	if (max < 0)
		max = 0; /* bio_add cannot handle a negative return */
	if (max <= biovec->bv_len && bio_sectors == 0)
		return biovec->bv_len;
	if (max < biovec->bv_len)
		/* too small already, no need to check further */
		return max;
	return biovec->bv_len;

}

static sector_t hstorage_size(struct mddev *mddev, sector_t sectors, int raid_disks)
{
	sector_t array_sectors = 0;
	struct md_rdev *rdev;

	printk("md/hybrid_storage size.\n");
	WARN_ONCE(sectors || raid_disks,
		  "%s does not support generic reshape\n", __func__);

	rdev_for_each(rdev, mddev)
	{
#if DIRECT_MODE

#if SSD
		if ((blk_queue_nonrot(bdev_get_queue(rdev->bdev))))
		{
			array_sectors = rdev->sectors;
		}
#else
		if (!blk_queue_nonrot(bdev_get_queue(rdev->bdev)))
		{
			array_sectors = rdev->sectors;
		}
#endif

#else
#if ONLY_SSD
		if ((array_sectors == 0) || array_sectors > rdev->sectors)
		{
			array_sectors = rdev->sectors;
		}
#else
		if ((!blk_queue_nonrot(bdev_get_queue(rdev->bdev))) && (array_sectors < rdev->sectors))
		{
			array_sectors = rdev->sectors;
		}
#endif
#endif
	}
	if (array_sectors == 0)
	{
		array_sectors = ((sector_t)0xA00000);
	}
	return array_sectors;
}

static int hstorage_stop(struct mddev *mddev);

long long meta_reserve[2] = {0, },meta_i_table[2] = {0, }, meta_i_bitmap[2] = {0, }, meta_b_bitmap[2] = {0, }, meta_sb[2] = {0, }, meta_gdt[2] = {0, }, meta_journal[2] = {0, }, file_data[2] = {0, };

static int hstorage_run(struct mddev *mddev)
{
	struct hstorage_conf *conf;
	struct md_rdev * rdev;
	sector_t md_size;
	int ret;

	if (mddev->chunk_sectors == 0) {
		printk(KERN_ERR "md/raid0:%s: chunk size must be set.\n",
		       mdname(mddev));
		return -EINVAL;
	}
	if (md_check_no_bitmap(mddev))
		return -EINVAL;
	blk_queue_max_hw_sectors(mddev->queue, mddev->chunk_sectors);
	blk_queue_max_write_same_sectors(mddev->queue, mddev->chunk_sectors);
	blk_queue_max_discard_sectors(mddev->queue, mddev->chunk_sectors);

	/* if private is not null, we are here after takeover */
	if (mddev->private == NULL)
	{
		mddev->private = kzalloc(sizeof(struct hstorage_conf), GFP_KERNEL);
	}
	conf = mddev->private;

	/* calculate array device size */
	md_size = hstorage_size(mddev, 0, 0);
	md_set_array_sectors(mddev, md_size - 262144);

	conf->data_dev = NULL;
	conf->meta_dev = NULL;

	rdev_for_each(rdev, mddev)
	{
#if DIRECT_MODE
		if (md_size == rdev->sectors)
		{
			conf->data_dev = rdev;
		}
#else
		if ((!conf->data_dev)&& (md_size == rdev->sectors))
		{
			conf->data_dev = rdev;
		}
		else
		{
			conf->meta_dev = rdev;
		}
#endif
	}
	printk(KERN_INFO "md/raid-hstorage:%s: md_size is %llu sectors.\n",
	       mdname(mddev),
	       (unsigned long long)mddev->array_sectors);
	/* calculate the max read-ahead size.
	 * For read-ahead of large files to be effective, we need to
	 * readahead at least twice a whole stripe. i.e. number of devices
	 * multiplied by chunk size times 2.
	 * If an individual device has an ra_pages greater than the
	 * chunk size, then we will not drive that device as hard as it
	 * wants.  We consider this a configuration error: a larger
	 * chunksize should be used in that case.
	 */
/*	{
		int stripe = mddev->raid_disks *
			(mddev->chunk_sectors << 9) / PAGE_SIZE;
		if (mddev->queue->backing_dev_info.ra_pages < 2* stripe)
			mddev->queue->backing_dev_info.ra_pages = 2* stripe;
	}
*/
	blk_queue_merge_bvec(mddev->queue, hstorage_mergeable_bvec);
//	dump_zones(mddev);

	ret = md_integrity_register(mddev);
	if (ret)
		hstorage_stop(mddev);

	return ret;
}

static int hstorage_stop(struct mddev *mddev)
{
	struct hstorage_conf *conf = mddev->private;

	blk_sync_queue(mddev->queue); /* the unplug fn references 'conf'*/

	kfree(conf);
	mddev->private = NULL;
	return 0;
}
static void hstorage_meta_update(struct bio * bio)
{
	int offset = 0;
	
	if (bio->bi_rw & 1)
	{
		offset = 1;
	}	
	
	if (bio->bi_sector >= SSD_META_IN_SECTORS)
	{
		meta_reserve[offset] += bio->bi_size;
	//	printk("reserve start: %d, size%d\n",bi_sector, bi_size);
	}
	else if (bio->bi_sector >= (JOURNAL_START_IN_BLOCK * 8))
		meta_journal[offset] += bio->bi_size;
	else if (bio->bi_sector >= (INO_TABLE_START_IN_BLOCK * 8))
		meta_i_table[offset] += bio->bi_size;
	else if (bio->bi_sector >= (INO_BITMAP_START_IN_BLOCK * 8))
		meta_i_bitmap[offset] += bio->bi_size;
	else if (bio->bi_sector >= (BLK_BITMAP_START_IN_BLOCK * 8))
		meta_b_bitmap[offset] += bio->bi_size;
	else if (bio->bi_sector >= 1 * 8)
		meta_gdt[offset] += bio->bi_size;
	else 
		meta_sb[offset] += bio->bi_size;
}
static void hstorage_make_request(struct mddev *mddev, struct bio *bio)
{
	struct hstorage_conf * conf = mddev->private;

	if (unlikely(bio->bi_rw & REQ_FLUSH)) {
		md_flush_request(mddev, bio);
		return;
	}

#if DIRECT_MODE
	bio->bi_bdev = conf->data_dev->bdev;
	if (bio->bi_rw & 1)
	{
		file_data[1] += bio->bi_size;
	}
	else
	{
		file_data[0] += bio->bi_size;			
	}
#else
	if (bio->bi_sector >= DATA_START_IN_SECTORS)
	{ 
		bio->bi_bdev = conf->data_dev->bdev;
		bio->bi_sector = bio->bi_sector - DATA_START_IN_SECTORS + 262144;
		if (bio->bi_rw & 1)
		{
			file_data[1] += bio->bi_size;
		}
		else
		{
			file_data[0] += bio->bi_size;			
		}	
	}
	else
	{
		if (unlikely((bio->bi_sector + (bio->bi_size >> 9) ) > DATA_START_IN_SECTORS))
		{
			struct bio_pair *bp;

			bp = bio_split(bio, (DATA_START_IN_SECTORS) - bio->bi_sector);

			bp->bio1.bi_bdev = conf->meta_dev->bdev;
			bp->bio1.bi_sector += 262144;
			hstorage_meta_update(&bp->bio1);
			//meta_data += bp->bio1.bi_size; 
			
			bp->bio2.bi_bdev = conf->data_dev->bdev;
			bp->bio2.bi_sector = bp->bio2.bi_sector - DATA_START_IN_SECTORS + 262144;
			//file_data += bp->bio2.bi_size;
			if (bio->bi_rw & 1)
			{
				file_data[1] += bp->bio2.bi_size;
			}
			else
			{
				file_data[0] += bp->bio2.bi_size;			
			}
			generic_make_request(&(bp->bio1));
			generic_make_request(&(bp->bio2));

			bio_pair_release(bp);

			return;
		}
		else
		{
		/*	if (bio->bi_rw & 1)
				printk("write\n");
			else
				printk("read\n");*/
			hstorage_meta_update(bio);
			//meta_data += bio->bi_size;
			bio->bi_bdev = conf->meta_dev->bdev;
			bio->bi_sector += 262144;
		}
	}
#endif
	if (unlikely((bio->bi_rw & REQ_DISCARD) &&
		     !blk_queue_discard(bdev_get_queue(bio->bi_bdev)))) {
		/* Just ignore it */
		bio_endio(bio, 0);
		return;
	}

	generic_make_request(bio);
	return;
}

static void hstorage_status(struct seq_file *seq, struct mddev *mddev)
{
	seq_printf(seq, " %dk chunks", mddev->chunk_sectors / 2);
	return;
}


static void hstorage_quiesce(struct mddev *mddev, int state)
{
//	printk("quiesce: %d\n",state);
}

static struct md_personality raid0_personality=
{
	.name		= "hstorage",
	.level		= 20,
	.owner		= THIS_MODULE,
	.make_request	= hstorage_make_request,
	.run		= hstorage_run,
	.stop		= hstorage_stop,
	.status		= hstorage_status,
	.size		= hstorage_size,
	.quiesce	= hstorage_quiesce,
};

static int hello_proc_show(struct seq_file *m, void *v)
{

	seq_printf(m, "Hybrid ext4\nMetadata\nSB_w - %lld\nSB_r - %lld\nGDT_w - %lld\nGDT_r - %lld\nBLK_BITMAP_w - %lld\nBLK_BITMAP_r - %lld\nINO_BITMAP_w - %lld\nINO_BITMAP_r - %lld\nINO_TABLE_w - %lld\nINO_TABLE_r - %lld\nJOURNAL_w - %lld\nJOURNAL_r - %lld\nRESERVE_w - %lld\nRESERVE_r - %lld\nFiledata_w - %lld\nFiledata_r - %lld\n", meta_sb[1], meta_sb[0], meta_gdt[1], meta_gdt[0], meta_b_bitmap[1], meta_b_bitmap[0], meta_i_bitmap[1], meta_i_bitmap[0], meta_i_table[1], meta_i_table[0], meta_journal[1], meta_journal[0], meta_reserve[1], meta_reserve[0], file_data[1], file_data[0]);
	return 0;
}

static int hstorage_proc_open (struct inode * inode, struct file * file) 
{
	return single_open (file, hello_proc_show, NULL);
}

static const struct file_operations hstorage_proc_fops = 
{
	.owner = THIS_MODULE,
	.open = hstorage_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init raid0_init (void)
{
	proc_create("hstorage-raid", 0, NULL, &hstorage_proc_fops);
	return register_md_personality (&raid0_personality);
}

static void raid0_exit (void)
{
	remove_proc_entry("hstorage-raid",NULL);
	unregister_md_personality (&raid0_personality);
}

module_init(raid0_init);
module_exit(raid0_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RAID0 (striping) personality for MD");
MODULE_ALIAS("md-personality-2"); /* RAID0 */
MODULE_ALIAS("md-raid0");
MODULE_ALIAS("md-level-0");
