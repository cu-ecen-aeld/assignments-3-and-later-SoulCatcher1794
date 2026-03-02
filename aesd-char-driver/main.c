/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include "aesdchar.h"

int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Alan Cano");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp){
    PDEBUG("open");

    struct aesd_dev *dev; // Device information
    // Takes a pointer (inode->i_cdev) to cdev field inside structure aesd_dev
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev); 
    filp->private_data = dev; // Store pointer in the private_data of the file pointer
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp){
    PDEBUG("release");
    /*No need to deallocate anything here, everything to be handles at cleaup function*/
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos){
    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);

    ssize_t retval = 0;
    size_t entry_offset, entry_remaining_bytes, bytes_to_read;
    struct aesd_buffer_entry *entry; // Define variable to hold information out of circular buffer

    /* Get to aesd_dev using filp->private_data saved in open function */
    struct aesd_dev *dev = filp->private_data;

    /* Lock while reading */
    if (mutex_lock_interruptible(&dev->mutex)){
        /* 
            Kernel will either restart the call or return error to the user.
            Should undo any user-visible changes that might have been made.
        */
        PDEBUG("Mutex lock failed");
        return -ERESTARTSYS; 
    }

    PDEBUG("Data locked with mutex, reading operation");
    
    /* Find entry that matches byte offset position or return NULL if none was found */
    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buffer, *f_pos, &entry_offset);
    /* Verify if there's an existing entry in that position, otherwise return 0 */
    if(entry != NULL){
        /* Figure out how many bytes are left to read of the entry after offset */
        entry_remaining_bytes = (entry->size - entry_offset);
        /* Verifiy if count is less or equal than entry remaining-bytes, if so will read count */
        if(entry_remaining_bytes >= count ){ 
            bytes_to_read = count;
        }else{ // Otherwise, will read only entry remaining-bytes of the entry
            bytes_to_read = entry_remaining_bytes;
        }

        /* Starting at offset position of the entry, will send remaining bytes of the entry to the user */
        if( copy_to_user(buf, (entry->buffptr + entry_offset), bytes_to_read) ){
            /* Will retrun -EFAULT if pointer is invalid or if invalid address is encountered during copy */
            retval = -EFAULT;
            goto exit;
        }
        
        /* Update offset to new position after reading */
        *f_pos += bytes_to_read;
        /* Return how many bytes were read */
        retval = bytes_to_read;
    }
    
    exit:
    /* Unlock data */
    mutex_unlock(&dev->mutex);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos){
    ssize_t retval = -ENOMEM;
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);

    /* Get to aesd_dev using filp->private_data saved in open function */
    struct aesd_dev *dev = filp->private_data;

    /* Lock when writting */
    if(mutex_lock_interruptible(&dev->mutex)){
        return -ERESTARTSYS;
    }

    /* Calculate old and new size of buffptr */
    size_t current_size, new_size;
    current_size = dev->entry.size;
    new_size = current_size + count;

    /* Resize memory space needed for the entry string */
    char *tmp = krealloc(dev->entry.buffptr, new_size, GFP_KERNEL); // Use tmp pointer to avoid losing data if krealloc fails
    if(tmp == NULL){
        /* Out of memory failure if no memory allocation failed */
        retval = -ENOMEM;
        goto exit;
    }
    /* buffptr will now point to the new address of the reallocated memory when succesfull */
    dev->entry.buffptr = tmp;

    /* Copy string from user space and save in working entry at the last character written (current_size) */
    if(copy_from_user( (dev->entry.buffptr + current_size), buf, count)){
        /* If copy fails, free memory of buffptr, point to NULL and set size as 0 */
        kfree(dev->entry.buffptr);
        dev->entry.buffptr = NULL;
        dev->entry.size = 0;
        retval = -EFAULT;
        goto exit;
    }

    /* Update entry size after successful copy */
    dev->entry.size = new_size;

    /* If new_line character was found, save into circular buffer */
    if(memchr(dev->entry.buffptr, '\n', new_size)){
        aesd_circular_buffer_add_entry(&dev->buffer, &dev->entry);
        dev->entry.buffptr = NULL;
        dev->entry.size = 0;
    }

    /* Return bytes saved on the entry (count) */
    retval = count;
    *f_pos += count;

    exit:
    mutex_unlock(&dev->mutex);
    return retval;
}

ssize_t aesd_seek(struct file *filp, loff_t off, int whence){
    struct aesd_dev *dev = filp->private_data;
    loff_t newpos;

    PDEBUG("seek offset %lld whence %d", off, whence);

    if(mutex_lock_interruptible(&dev->mutex)){
        return -ERESTARTSYS;
    }

    switch(whence){
        case 0: /* SEEK_SET */
            newpos = off;
            break;
        case 1: /* SEEK_CUR */
            newpos = filp->f_pos + off;
            break;
        case 2: /* SEEK_END */
            newpos = aesd_circular_buffer_calculate_size(&dev->buffer) + off;
            break;
        default: /* Invalid argument */
            mutex_unlock(&dev->mutex);
            return -EINVAL;
    }

    if (newpos < 0){
        mutex_unlock(&dev->mutex);
        return -EINVAL;
    }

    filp->f_pos = newpos;
    PDEBUG("seek complete, new position %lld", newpos);
    mutex_unlock(&dev->mutex);
    return newpos;
}

static long aesd_adjust_file_offset(struct file *filp, unsigned int write_cmd, unsigned int write_cmd_offset){
    struct aesd_dev *dev = filp->private_data;
    long retval = 0;
    size_t count;
    size_t entry_pos_off;
    loff_t cmd_offset = 0;
    unsigned int index;

    if(mutex_lock_interruptible(&dev->mutex)){
        return -ERESTARTSYS;
    }

    /* Determine total number of entries saved inside circular buffer */
    count = dev->buffer.full ? AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED : dev->buffer.in_offs;
    
    /* If write_cmd exceeds total number of entries, return invalid argument */
    if(write_cmd >= count){
        retval = -EINVAL;
        goto out;
    }
    
    /* Find write command according to offset */
    entry_pos_off = (dev->buffer.out_offs + write_cmd) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    
    /* If write_cmd_offset exceeds entry size, return invalid argument */
    if (write_cmd_offset >= dev->buffer.entry[entry_pos_off].size){
        retval = -EINVAL;
        goto out;
    }

    for(index = 0; index < write_cmd; index++){
        size_t cmd_index = (dev->buffer.out_offs + index) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
        cmd_offset += dev->buffer.entry[cmd_index].size;
    }
    
    filp->f_pos = cmd_offset + write_cmd_offset;

    out:
    mutex_unlock(&dev->mutex);
    return retval;
}

long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg){
    long retval = 0;

    switch(cmd){
        case AESDCHAR_IOCSEEKTO:
            struct aesd_seekto seekto;

            if ( copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto)) != 0 ){
                return -EFAULT;
            }else{
                retval = aesd_adjust_file_offset(filp, seekto.write_cmd, seekto.write_cmd_offset);
            }
            break;

        default:
            return -ENOTTY;
    }

    return retval;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
    .unlocked_ioctl = aesd_ioctl,
    .llseek = aesd_seek,
};

static int aesd_setup_cdev(struct aesd_dev *dev){
    int err, devno = MKDEV(aesd_major, aesd_minor);

    // Initialize cdev structure and allocates memory
    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;

    // Add device to the system
    err = cdev_add(&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}

int aesd_init_module(void){
    dev_t dev = 0; // Variable to hold device numbers (both major and minor parts)
    int result; // Variable used to hold function outputs

    result = alloc_chrdev_region(&dev, aesd_minor, 1, "aesdchar"); // Get a major number dynamically
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }

    memset(&aesd_device,0,sizeof(struct aesd_dev)); // Set aesd_device structure to 0 for clean start

    // Initialize device auxiliary structures
    aesd_circular_buffer_init(&aesd_device.buffer);
    memset(&aesd_device.entry, 0, sizeof(struct aesd_buffer_entry));
    mutex_init(&aesd_device.mutex);

    // Initilize aesd_device
    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        mutex_destroy(&aesd_device.mutex);
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void){
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    /* Remove char device from the system */
    cdev_del(&aesd_device.cdev);

    /* Destroy locking device and free memory of working entry */
    mutex_destroy(&aesd_device.mutex);
    kfree(aesd_device.entry.buffptr);

    /* Deallocate memory inside circular buffer */
    uint8_t index;
    struct aesd_buffer_entry *tmp_entry;
    AESD_CIRCULAR_BUFFER_FOREACH(tmp_entry, &aesd_device.buffer, index){
        kfree(tmp_entry->buffptr);
    }

    // Free device numbers once device is no longer in use
    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
