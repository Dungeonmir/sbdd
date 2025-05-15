#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/bio.h>
#include <linux/bvec.h>
#include <linux/init.h>
#include <linux/wait.h>
#include <linux/stat.h>
#include <linux/slab.h>
#include <linux/numa.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/blkdev.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/moduleparam.h>
#include <linux/spinlock_types.h>

#define SBDD_SECTOR_SHIFT 9
#define SBDD_SECTOR_SIZE (1 << SBDD_SECTOR_SHIFT)
#define SBDD_MIB_SECTORS (1 << (20 - SBDD_SECTOR_SHIFT))
#define SBDD_NAME "sbdd"

static char *drive_path = "/dev/nvme0n1";

struct sbdd
{
    wait_queue_head_t exitwait;
    spinlock_t datalock;
    atomic_t deleting;
    atomic_t refs_cnt;
    sector_t capacity;
    // u8 *data;
    struct gendisk *gd;

    struct bdev_handle *disk_handle;
};

static struct sbdd __sbdd = {0};
static unsigned long __sbdd_capacity_mib = 100;

static void sbdd_clone_endio(struct bio *clone)
{
    struct bio *orig = clone->bi_private;
    if (clone->bi_status)
    {
        bio_io_error(orig);
    }
    else
    {
        bio_endio(orig); // if clone completed succesfully then orig can be endio
    }
    bio_put(clone); // explicit dereference bc i cloned bio
}
static void sbdd_submit_bio(struct bio *bio)
{

    bio = bio_split_to_limits(bio);
    if (!bio)
        return;

    // https://elixir.bootlin.com/linux/v6.8.12/source/include/linux/gfp_types.h#L16
    //  GFP - flags how to allocate memory
    struct bio *clone = bio_alloc_clone(__sbdd.disk_handle->bdev, bio, GFP_KERNEL, &fs_bio_set);
    if (!clone)
    {
        pr_err("cannot clone bio\n");
        bio_io_error(bio);
        return;
    }
    if (atomic_read(&__sbdd.deleting))
    {
        bio_io_error(bio);
        return;
    }

    if (!atomic_inc_not_zero(&__sbdd.refs_cnt))
    {
        bio_io_error(bio);
        return;
    }

    clone->bi_end_io = sbdd_clone_endio; // setting up callback
    clone->bi_private = bio;             // saving original bio for future use in endio
    submit_bio(clone);

    if (atomic_dec_and_test(&__sbdd.refs_cnt))
        wake_up(&__sbdd.exitwait);
}

/*
There are no read or write operations. These operations are performed by
the request() function associated with the request queue of the disk.
*/
static struct block_device_operations const __sbdd_bdev_ops = {
    .owner = THIS_MODULE,
    .submit_bio = sbdd_submit_bio,
};

static int sbdd_create(void)
{
    int ret = 0;

    blk_mode_t drive_mode = BLK_OPEN_WRITE | BLK_OPEN_READ;

    dev_t dev;
    int bdev = lookup_bdev(drive_path, &dev);
    if (bdev)
    {
        pr_err("Invalid drive path %s\n", drive_path);
        return -1;
    }
    pr_info("disk %s dev: %u\n", drive_path, dev);
    __sbdd.disk_handle = bdev_open_by_path(drive_path, drive_mode, NULL, NULL);
    if (IS_ERR(__sbdd.disk_handle))
    {
        pr_err("Failed to open %s\n", drive_path);
        return -1;
    }

    sector_t capacity = get_capacity(__sbdd.disk_handle->bdev->bd_disk);
    pr_info("disk %s has %llu sectors\n", drive_path, capacity);
    pr_info("disk %s has %llu MiB\n", drive_path, capacity / SBDD_MIB_SECTORS);

    pr_info("setting up capacity \n");
    __sbdd.capacity = capacity;

    spin_lock_init(&__sbdd.datalock);
    init_waitqueue_head(&__sbdd.exitwait);

    pr_info("allocating gendisk struct\n");
    __sbdd.gd = blk_alloc_disk(NUMA_NO_NODE);
    if (IS_ERR(__sbdd.gd))
    {
        pr_err("blk_alloc_disk() failed\n");
        ret = PTR_ERR(__sbdd.gd);
        __sbdd.gd = NULL;
        return ret;
    }

    /* Configure queue */
    blk_queue_logical_block_size(__sbdd.gd->queue, SBDD_SECTOR_SIZE);
    blk_queue_physical_block_size(__sbdd.gd->queue, SBDD_SECTOR_SIZE);

    /* Configure gendisk */
    __sbdd.gd->fops = &__sbdd_bdev_ops;
    __sbdd.gd->private_data = &__sbdd;
    scnprintf(__sbdd.gd->disk_name, DISK_NAME_LEN, SBDD_NAME);
    set_capacity(__sbdd.gd, __sbdd.capacity);
    atomic_set(&__sbdd.refs_cnt, 1);

    /*
    Allocating gd does not make it available, add_disk() is required.
    After this call, gd methods can be called at any time. Should not be
    called before the driver is fully initialized and ready to process reqs.
    */
    pr_info("adding disk %s\n", SBDD_NAME);
    ret = add_disk(__sbdd.gd);
    if (ret)
        pr_err("add_disk() failed\n");

    return ret;
}

static void sbdd_delete(void)
{
    atomic_set(&__sbdd.deleting, 1);
    atomic_dec_if_positive(&__sbdd.refs_cnt);
    wait_event(__sbdd.exitwait, !atomic_read(&__sbdd.refs_cnt));

    /* gd will be removed only after the last reference put */
    if (__sbdd.gd)
    {
        pr_info("deleting disk\n");
        del_gendisk(__sbdd.gd);
        put_disk(__sbdd.gd);
    }

    if (__sbdd.disk_handle)
    {
        pr_info("releasing handle \n");
        bdev_release(__sbdd.disk_handle);
    }
}

/*
Note __init is for the kernel to drop this function after
initialization complete making its memory available for other uses.
There is also __initdata note, same but used for variables.
*/
static int __init sbdd_init(void)
{
    pr_info("drive path: %s\n", drive_path);
    int ret = 0;

    pr_info("starting initialization...\n");
    ret = sbdd_create();

    if (ret)
    {
        pr_err("initialization failed\n");
        sbdd_delete();
    }
    else
    {
        pr_info("initialization complete\n");
    }

    return ret;
}

/*
Note __exit is for the compiler to place this code in a special ELF section.
Sometimes such functions are simply discarded (e.g. when module is built
directly into the kernel). There is also __exitdata note.
*/
static void __exit sbdd_exit(void)
{
    pr_info("exiting...\n");
    sbdd_delete();
    pr_info("exiting complete\n");
}

/* Called on module loading. Is mandatory. */
module_init(sbdd_init);

/* Called on module unloading. Unloading module is not allowed without it. */
module_exit(sbdd_exit);

/* Set desired capacity with insmod */
module_param_named(capacity_mib, __sbdd_capacity_mib, ulong, S_IRUGO);
// Set the drive path to copy to
module_param_named(drive_path, drive_path, charp, S_IRUGO);

/* Note for the kernel: a free license module. A warning will be outputted without it. */
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Simple Block Device Driver");
