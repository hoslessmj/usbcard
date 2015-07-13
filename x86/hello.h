#ifndef HELLO_H
#define HELLO_H

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/signal.h>
#include <linux/string.h>

#include <linux/cdev.h>
#include <linux/notifier.h>
#include <linux/security.h>
#include <linux/user_namespace.h>
#include <asm/byteorder.h>
#include <linux/moduleparam.h>
#include <linux/poll.h>
#include <linux/usb/hcd.h>
#include <linux/kmod.h>
//#include <asm-generic/uaccess.h>


/* Define these values to match your devices */
#define USB_SKEL_VENDOR_ID	0x0403
#define USB_SKEL_PRODUCT_ID	0x6014

#define USB_SG_SIZE 16384
struct usbdevfs_ctrltransfer {
    __u8 bRequestType;
    __u8 bRequest;
    __u16 wValue;
    __u16 wIndex;
    __u16 wLength;
    __u32 timeout;  /* in milliseconds */
    void *data;
};

struct usbdevfs_bulktransfer {
    unsigned int ep;
    unsigned int len;
    unsigned int timeout; /* in milliseconds */
    void *data;
};
static struct usb_driver hosless_driver;

extern  struct bus_type usb_bus_type;



struct usbdevfs_setinterface {
    unsigned int interface;
    unsigned int altsetting;
};

struct usbdevfs_disconnectsignal {
    unsigned int signr;
    void *context;
};

#define USBDEVFS_MAXDRIVERNAME 255
#define USBDEVFS_URB_SHORT_NOT_OK	0x01
#define USBDEVFS_URB_ISO_ASAP		0x02
#define USBDEVFS_URB_BULK_CONTINUATION	0x04
#define USBDEVFS_URB_NO_FSBR		0x20
#define USBDEVFS_URB_ZERO_PACKET	0x40
#define USBDEVFS_URB_NO_INTERRUPT	0x80

#define USBDEVFS_URB_TYPE_ISO		   0
#define USBDEVFS_URB_TYPE_INTERRUPT	   1
#define USBDEVFS_URB_TYPE_CONTROL	   2
#define USBDEVFS_URB_TYPE_BULK		   3

/* Device capability flags */
#define USBDEVFS_CAP_ZERO_PACKET		0x01
#define USBDEVFS_CAP_BULK_CONTINUATION		0x02
#define USBDEVFS_CAP_NO_PACKET_SIZE_LIM		0x04
#define USBDEVFS_CAP_BULK_SCATTER_GATHER	0x08

/* USBDEVFS_DISCONNECT_CLAIM flags & struct */

/* disconnect-and-claim if the driver matches the driver field */
#define USBDEVFS_DISCONNECT_CLAIM_IF_DRIVER	0x01
/* disconnect-and-claim except when the driver matches the driver field */
#define USBDEVFS_DISCONNECT_CLAIM_EXCEPT_DRIVER	0x02

#define AS_CONTINUATION	1
#define AS_UNLINK	2






struct usbdevfs_getdriver {
    unsigned int interface;
    char driver[USBDEVFS_MAXDRIVERNAME + 1];
};

struct usbdevfs_connectinfo {
    unsigned int devnum;
    unsigned char slow;
};

enum snoop_when {
    SUBMIT, COMPLETE
};

struct usbdevfs_iso_packet_desc {
    unsigned int length;
    unsigned int actual_length;
    unsigned int status;
};


struct usbdevfs_urb {
    unsigned char type;
    unsigned char endpoint;
    int status;
    unsigned int flags;
    void *buffer;
    int buffer_length;
    int actual_length;
    int start_frame;
    union {
        int number_of_packets;	/* Only used for isoc urbs */
        unsigned int stream_id;	/* Only used with bulk streams */
    };
    int error_count;
    unsigned int signr;	/* signal to be sent on completion,
                  or 0 if none should be sent. */
    void *usercontext;
    struct usbdevfs_iso_packet_desc iso_frame_desc[0];
};

/* ioctls for talking directly to drivers */
struct usbdevfs_ioctl {
    int	ifno;		/* interface 0..N ; negative numbers reserved */
    int	ioctl_code;	/* MUST encode size + direction of data so the
                 * macros in <asm/ioctl.h> give correct values */
    void *data;	/* param buffer (in, or out) */
};

/* You can do most things with hubs just through control messages,
 * except find out what device connects to what port. */
struct usbdevfs_hub_portinfo {
    char nports;		/* number of downstream ports in this hub */
    char port [127];	/* e.g. port 3 connects to device 27 */
};



struct usbdevfs_disconnect_claim {
    unsigned int interface;
    unsigned int flags;
    char driver[USBDEVFS_MAXDRIVERNAME + 1];
};

struct usbdevfs_streams {
    unsigned int num_streams; /* Not used by USBDEVFS_FREE_STREAMS */
    unsigned int num_eps;
    unsigned char eps[0];
};

struct usb_dev_state {
    struct list_head list;      /* state list */
    struct usb_device *dev;
    struct file *file;
    spinlock_t lock;            /* protects the async urb lists */
    struct list_head async_pending;
    struct list_head async_completed;
    struct list_head hosless_completed;
    wait_queue_head_t wait;     /* wake up if a request completed */
    unsigned int discsignr;
    struct pid *disc_pid;
    const struct cred *cred;
    void __user *disccontext;
    unsigned long ifclaimed;
    u32 secid;
    u32 disabled_bulk_eps;
};

struct async {
    struct list_head asynclist;
    struct usb_dev_state *ps;
    struct pid *pid;
    const struct cred *cred;
    unsigned int signr;
    unsigned int ifnum;
    void __user *userbuffer;
    void __user *userurb;
    struct urb *urb;
    unsigned int async_index;
    int status;
    u32 secid;
    u8 bulk_addr;
    u8 bulk_status;
};

struct  hosless_driver_data{
    struct  usb_dev_state*  ps;
    struct  usb_skel*       skel;
};

struct hosless_set_buf{
    unsigned    int     index;
    struct usbdevfs_urb*    uurb;
};

struct hosless_get_buf{
    unsigned    int     index;
    struct usbdevfs_urb*       uurb;
    unsigned    char*   kernel_buffer;
    unsigned    int     is_ok;
    unsigned    int     actual_len;
    struct  list_head   list;
};











#define USBDEVFS_CONTROL           _IOWR('U', 0, struct usbdevfs_ctrltransfer)
#define USBDEVFS_CONTROL32           _IOWR('U', 0, struct usbdevfs_ctrltransfer32)
#define USBDEVFS_BULK              _IOWR('U', 2, struct usbdevfs_bulktransfer)
#define USBDEVFS_BULK32              _IOWR('U', 2, struct usbdevfs_bulktransfer32)
#define USBDEVFS_RESETEP           _IOR('U', 3, unsigned int)
#define USBDEVFS_SETINTERFACE      _IOR('U', 4, struct usbdevfs_setinterface)
#define USBDEVFS_SETCONFIGURATION  _IOR('U', 5, unsigned int)
#define USBDEVFS_GETDRIVER         _IOW('U', 8, struct usbdevfs_getdriver)
#define USBDEVFS_SUBMITURB         _IOR('U', 10, struct usbdevfs_urb)
#define USBDEVFS_SUBMITURB32       _IOR('U', 10, struct usbdevfs_urb32)
#define USBDEVFS_DISCARDURB        _IO('U', 11)
#define USBDEVFS_REAPURB           _IOW('U', 12, void *)
#define USBDEVFS_REAPURB32         _IOW('U', 12, __u32)
#define USBDEVFS_REAPURBNDELAY     _IOW('U', 13, void *)
#define USBDEVFS_REAPURBNDELAY32   _IOW('U', 13, __u32)
#define USBDEVFS_DISCSIGNAL        _IOR('U', 14, struct usbdevfs_disconnectsignal)
#define USBDEVFS_DISCSIGNAL32      _IOR('U', 14, struct usbdevfs_disconnectsignal32)
#define USBDEVFS_CLAIMINTERFACE    _IOR('U', 15, unsigned int)
#define USBDEVFS_RELEASEINTERFACE  _IOR('U', 16, unsigned int)
#define USBDEVFS_CONNECTINFO       _IOW('U', 17, struct usbdevfs_connectinfo)
#define USBDEVFS_IOCTL             _IOWR('U', 18, struct usbdevfs_ioctl)
#define USBDEVFS_IOCTL32           _IOWR('U', 18, struct usbdevfs_ioctl32)
#define USBDEVFS_HUB_PORTINFO      _IOR('U', 19, struct usbdevfs_hub_portinfo)
#define USBDEVFS_RESET             _IO('U', 20)
#define USBDEVFS_CLEAR_HALT        _IOR('U', 21, unsigned int)
#define USBDEVFS_DISCONNECT        _IO('U', 22)
#define USBDEVFS_CONNECT           _IO('U', 23)
#define USBDEVFS_CLAIM_PORT        _IOR('U', 24, unsigned int)
#define USBDEVFS_RELEASE_PORT      _IOR('U', 25, unsigned int)
#define USBDEVFS_GET_CAPABILITIES  _IOR('U', 26, __u32)
#define USBDEVFS_DISCONNECT_CLAIM  _IOR('U', 27, struct usbdevfs_disconnect_claim)
#define USBDEVFS_ALLOC_STREAMS     _IOR('U', 28, struct usbdevfs_streams)
#define USBDEVFS_FREE_STREAMS      _IOR('U', 29, struct usbdevfs_streams)


#define USBDEVFS_HOSLESS_GETURB_SIZE      _IOR('U',100,int)
#define USBDEVFS_HOSLESS_STREAM_START   _IOR('U',101,unsigned int)
#define USBDEVFS_HOSLESS_STREAM_STOP     _IOR('U',102,unsigned int)
#define USBDEVFS_HOSLESS_SET_BUFFER        _IOR('U',103,struct hosless_set_buf)
#define USBDEVFS_HOSLESS_GET_URB             _IOR('U',104,void*)
#define USBDEVFS_HOSLESS_GET_RUNNING         _IOR('U',105,int)

#define USBDEVFS_HOSLESS_STREAM_RESTART      _IOR('U',106,int)



#define USBFS_XFER_MAX		(UINT_MAX / 2 - 1000000)


static atomic_t usbfs_memory_usage;	/* Total memory currently allocated */
#define snoop(dev, format, arg...)				\
    do {							\
        if (usbfs_snoop)				\
            dev_info(dev , format , ## arg);	\
    } while (0)


/* Get a minor range for your devices from the usb maintainer */
#define USB_SKEL_MINOR_BASE	192

/* our private defines. if this grows any larger, use your own .h file */
#define MAX_TRANSFER		(PAGE_SIZE - 512)
/* MAX_TRANSFER is chosen so that the VM is not stressed by
   allocations > PAGE_SIZE and the number of packets in a page
   is an integer 512 is the largest possible packet on EHCI */
#define WRITES_IN_FLIGHT	8
/* arbitrarily chosen */



static void cancel_bulk_urbs(struct usb_dev_state *ps, unsigned bulk_addr);

static  int usb_classdev_add(struct usb_device* dev);

static int hosless_processcompl(struct async* as);

static  bool    usbfs_snoop = true;
static  struct usb_device*  g_dev = 0;
static int g_cur = 0;








#endif // HELLO_H
