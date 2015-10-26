#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/time.h>

#define NEW_CHRDEV
#define DRIVER_NAME "pktmonitor"
#define MONITOR_LIST_NAME "monitor_list"
#define RESULT_LIST_NAME "result_list"
#define DATA_BUF_SIZE 1024

#if 0
static int c1 = 0;
static int c2 = 0;
static int c3 = 0;
#define LOG(msg, ...) printk("XXXXX " msg, __VA_ARGS__)
#else
#define LOG(msg, ...)
#endif

static unsigned int pktmonitor_major = 60;
#ifdef NEW_CHRDEV
static unsigned int pktmonitor_devs = 1;
static struct cdev pktmonitor_cdev;
#endif

typedef struct monitor_data
{
    struct monitor_data *prev;
    struct monitor_data *next;
    void *pkt;
    unsigned int pktid;
    long time_tx_start;
    long time_tx_add_msg_ring;
    long time_tx_ready;
    long time_rx_start;
    long time_rx_ready;
    int tx_queue_len;
    bool using;
} monitor_data_t;
typedef struct data_list
{
    monitor_data_t *head;
    monitor_data_t *tail;
    int count;
} data_list_t;

static data_list_t monitor_list;
static data_list_t result_list;
static monitor_data_t *monitor_data_buf = NULL;
static int tx_queue_len = 0;
static int next_data_idx = 0;
static int alloc_count = 0;
static char msg_buf[256];
static struct timeval time;

static wait_queue_head_t wait_q;
static spinlock_t lock;

static monitor_data_t *alloc_monitor_data(void)
{
    monitor_data_t *data = NULL;

    if (!monitor_data_buf) {
        goto done;
    }
    if (alloc_count >= DATA_BUF_SIZE) {
        LOG("%s: buff full alloc_count = %d monitor_list.count = %d monitor_list.head = %p result_list.count = %d result_list.head = %p\n",
                __FUNCTION__, alloc_count, monitor_list.count, monitor_list.head, result_list.count, result_list.head);
        goto done;
    }
    do {
        data = &monitor_data_buf[next_data_idx++];
        if (next_data_idx >= DATA_BUF_SIZE)
            next_data_idx = 0;
    } while (data->using);
    data->using = true;
    alloc_count++;
    LOG("%s: next_data_idx = %d alloc_count = %d\n", __FUNCTION__, next_data_idx, alloc_count);

done:
    return data;
}

static void free_monitor_data(monitor_data_t* data)
{
    memset(data, 0, sizeof(monitor_data_t));
    alloc_count--;
    LOG("%s: alloc_count = %d\n", __FUNCTION__, alloc_count);
}

static void add_to_list(monitor_data_t *data, data_list_t *list)
{
    if (data) {
        if (!list->head) {
            list->head = data;
            list->tail = data;
        } else {
            list->tail->next = data;
            data->prev = list->tail;
            list->tail = data;
        }
        list->count++;
    }

    LOG("%s: %s count = %d head = %p\n", __FUNCTION__, list == &monitor_list ? MONITOR_LIST_NAME : RESULT_LIST_NAME,
            list->count, list->head);
}

static void remove_from_list(monitor_data_t *data, data_list_t *list)
{
    monitor_data_t *cur;

    cur = list->head;
    while (cur) {
        if (cur == data) {
            if (cur->next)
                cur->next->prev = cur->prev;
            if (cur->prev)
                cur->prev->next = cur->next;
            if (cur == list->head)
                list->head = cur->next;
            if (cur == list->tail)
                list->tail = cur->prev;
            cur->next = NULL;
            cur->prev = NULL;
            list->count--;
            break;
        }
        cur = cur->next;
    }

    LOG("%s: %s count = %d head = %p\n", __FUNCTION__, list == &monitor_list ? MONITOR_LIST_NAME : RESULT_LIST_NAME,
            list->count, list->head);
}

static monitor_data_t *find_monitoring_data(void *pkt, unsigned int pktid)
{
    monitor_data_t *cur;
    monitor_data_t *data = NULL;

    cur = monitor_list.head;
    while (cur) {
        if (cur->pkt == pkt || cur->pktid == pktid) {
            data = cur;
            LOG("%s: find data\n", __FUNCTION__);
            goto done;
        }
        cur = cur->next;
    }
    LOG("%s: cannot find data\n", __FUNCTION__);

done:
    return data;
}

void pktmonitor_tx_start(void *pkt)
{
    monitor_data_t* data;
    unsigned long flags;

    spin_lock_irqsave(&lock, flags);

    //if (!monitor_data_buf)
        goto done;

    LOG("%s: %d\n", __FUNCTION__, ++c1);

    data = alloc_monitor_data();
    if (!data)
        goto done;

    data->pkt = pkt;
    data->time_tx_start = jiffies;

    add_to_list(data, &monitor_list);

done:
    spin_unlock_irqrestore(&lock, flags);
}

void pktmonitor_tx_add_msg_ring(void *pkt, unsigned int pktid)
{
    monitor_data_t* data;
    unsigned long flags;

    LOG("%s +++\n", __FUNCTION__);

    spin_lock_irqsave(&lock, flags);

    if (!monitor_data_buf)
        goto done;

    LOG("%s: %d\n", __FUNCTION__, ++c2);

    /*data = find_monitoring_data(pkt, 0);
    if (!data)
        return;
    data->pktid = pktid;
    data->time_tx_add_msg_ring = jiffies;*/

    data = alloc_monitor_data();
    if (!data)
        goto done;

    do_gettimeofday(&time);
    data->pkt = pkt;
    data->pktid = pktid;
    data->time_tx_add_msg_ring = time.tv_usec;

    add_to_list(data, &monitor_list);
    data->tx_queue_len = ++tx_queue_len;

done:
    spin_unlock_irqrestore(&lock, flags);

    LOG("%s ---\n", __FUNCTION__);
}

void pktmonitor_tx_ready(void *pkt, unsigned int pktid)
{
    monitor_data_t* data;
    unsigned long flags;

    LOG("%s +++\n", __FUNCTION__);

    spin_lock_irqsave(&lock, flags);

    if (!monitor_data_buf)
        goto done;

    LOG("%s: %d\n", __FUNCTION__, ++c3);

    data = find_monitoring_data(pkt, pktid);
    if (!data)
        goto done;
    do_gettimeofday(&time);
    data->time_tx_ready = time.tv_usec;

    remove_from_list(data, &monitor_list);
    add_to_list(data, &result_list);
    tx_queue_len--;

    spin_unlock_irqrestore(&lock, flags);

    wake_up_interruptible(&wait_q);

    LOG("%s ---\n", __FUNCTION__);

    return;

done:
    spin_unlock_irqrestore(&lock, flags);

    LOG("%s ---\n", __FUNCTION__);
}

void pktmonitor_rx_start(void *pkt)
{
    monitor_data_t* data;
    unsigned long flags;

    LOG("%s +++\n", __FUNCTION__);

    spin_lock_irqsave(&lock, flags);

    if (!monitor_data_buf)
        goto done;

    data = alloc_monitor_data();
    if (!data)
        goto done;
    do_gettimeofday(&time);
    data->pkt = pkt;
    data->time_rx_start = time.tv_usec;

    add_to_list(data, &monitor_list);

done:
    spin_unlock_irqrestore(&lock, flags);

    LOG("%s ---\n", __FUNCTION__);
}

void pktmonitor_rx_ready(void *pkt)
{
    monitor_data_t* data;
    unsigned long flags;

    LOG("%s +++\n", __FUNCTION__);

    spin_lock_irqsave(&lock, flags);

    if (!monitor_data_buf)
        goto done;

    data = find_monitoring_data(pkt, -1);
    if (!data)
        goto done;
    do_gettimeofday(&time);
    data->time_rx_ready = time.tv_usec;

    remove_from_list(data, &monitor_list);
    add_to_list(data, &result_list);

    spin_unlock_irqrestore(&lock, flags);

    wake_up_interruptible(&wait_q);

    LOG("%s ---\n", __FUNCTION__);

    return;

done:
    spin_unlock_irqrestore(&lock, flags);

    LOG("%s ---\n", __FUNCTION__);
}

static int pktmonitor_open(struct inode *inode, struct file *file)
{
    int ret = 0;
    unsigned long flags;

    LOG("%s\n", __FUNCTION__);

    spin_lock_irqsave(&lock, flags);

    if (monitor_data_buf) {
        ret = EBUSY;
        goto done;
    }

    monitor_data_buf = kmalloc(sizeof(monitor_data_t) * (DATA_BUF_SIZE + 1), GFP_KERNEL);
    if (!monitor_data_buf) {
        LOG("%s: failed to malloc data buffer!\n", __FUNCTION__);
        ret = ENOMEM;
        goto done;
    }
    memset(monitor_data_buf, 0, sizeof(monitor_data_t) * (DATA_BUF_SIZE + 1));
    LOG("%s: success to malloc data buffer!\n", __FUNCTION__);

done:
    spin_unlock_irqrestore(&lock, flags);

    return ret;
}

static int pktmonitor_close(struct inode *inode, struct file *file)
{
    unsigned long flags;

    LOG("%s\n", __FUNCTION__);

    spin_lock_irqsave(&lock, flags);

    if (monitor_data_buf) {
        kfree(monitor_data_buf);
        monitor_data_buf = NULL;
        next_data_idx = 0;
        alloc_count = 0;
        tx_queue_len = 0;
        memset(&monitor_list, 0, sizeof(data_list_t));
        memset(&result_list, 0, sizeof(data_list_t));
        LOG("%s: memory free! monitor_data_buf = %p, next_data_idx = %d\n",
                __FUNCTION__, monitor_data_buf, next_data_idx);
    }

    spin_unlock_irqrestore(&lock, flags);

    return 0;
}

static ssize_t pktmonitor_read(struct file *file, char __user *buf, size_t count, loff_t *f_pos)
{
    int retval = 0;
    int msg_len = 0;
    monitor_data_t *data;
    unsigned long flags;

    LOG("%s\n", __FUNCTION__);

    if (file->f_flags & O_NONBLOCK) {
        return -EAGAIN;
    }
    if (wait_event_interruptible(wait_q, result_list.head)) {
        LOG("%s, leave by signal!\n", __FUNCTION__);
        return 0;
    }

    spin_lock_irqsave(&lock, flags);

    data = result_list.head;
    if (data) {
    remove_from_list(data, &result_list);

    spin_unlock_irqrestore(&lock, flags);

    if (data->time_tx_ready > 0) {
        sprintf(msg_buf, "t,%ld,%d\n", data->time_tx_ready - data->time_tx_add_msg_ring, data->tx_queue_len);
    } else {
        sprintf(msg_buf, "r,%ld\n", data->time_rx_ready - data->time_rx_start);
    }
    msg_len = strlen(msg_buf);
    retval = count > msg_len ? msg_len : count;
    if (copy_to_user(buf, msg_buf, retval)) {
        retval = -EFAULT;
    }
    LOG("msg = %s, retval = %d\n", msg_buf, retval);

    spin_lock_irqsave(&lock, flags);

    free_monitor_data(data);
    } else {
        sprintf(msg_buf, "data == NULL, count = %d ", result_list.count);
        retval = strlen(msg_buf);
        if (copy_to_user(buf, msg_buf, retval)) {
            retval = -EFAULT;
        }
    }
    spin_unlock_irqrestore(&lock, flags);

    return retval;
}

struct file_operations pktmonitor_fops = {
        .owner = THIS_MODULE,
        .open = pktmonitor_open,
        .release = pktmonitor_close,
        .read = pktmonitor_read,
};

static int pktmonitor_init(void)
{
#ifdef NEW_CHRDEV
    dev_t dev = MKDEV(pktmonitor_major, 0);
    int alloc_ret = 0;
#endif
    int cdev_err = 0;

    LOG("%s\n", __FUNCTION__);

#ifdef NEW_CHRDEV
    alloc_ret = alloc_chrdev_region(&dev, 0, pktmonitor_devs, DRIVER_NAME);
    if (alloc_ret)
        goto error;

    pktmonitor_major = MAJOR(dev);
    cdev_init(&pktmonitor_cdev, &pktmonitor_fops);
    pktmonitor_cdev.owner = THIS_MODULE;

    cdev_err = cdev_add(&pktmonitor_cdev, MKDEV(pktmonitor_major, 0), pktmonitor_devs);
    if (cdev_err)
        goto error;
#else
    cdev_err = register_chrdev(pktmonitor_major, DRIVER_NAME, &pktmonitor_fops);
    if (cdev_err)
        goto error;
#endif

    LOG("%s installed\n", DRIVER_NAME);

    memset(&monitor_list, 0, sizeof(data_list_t));
    memset(&result_list, 0, sizeof(data_list_t));

    init_waitqueue_head(&wait_q);
    spin_lock_init(&lock);

    return 0;

error:
#ifdef NEW_CHRDEV
    if (cdev_err == 0)
        cdev_del(&pktmonitor_cdev);
    if (alloc_ret == 0)
        unregister_chrdev_region(dev, pktmonitor_devs);
#endif
    return -1;
}

static void pktmonitor_exit(void)
{
    dev_t dev = MKDEV(pktmonitor_major, 0);

    LOG("%s\n", __FUNCTION__);

#ifdef NEW_CHRDEV
    cdev_del(&pktmonitor_cdev);
    unregister_chrdev_region(dev, pktmonitor_devs);
#else
    unregister_chrdev(pktmonitor_major, DRIVER_NAME);
#endif
}

module_init(pktmonitor_init);
module_exit(pktmonitor_exit);
