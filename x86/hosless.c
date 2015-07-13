#include "hello.h"


static  unsigned    int              g_urbsize = 0;
static struct async*                 g_async[256];
static struct usb_interface*         g_interface;

static  struct hosless_get_buf*      g_data[128];


static  bool                          g_request_data = false;
static  bool                          g_isrunning = false;
static  struct  usb_dev_state*        g_ps = NULL;
static  struct  hosless_driver_data*  g_hd = NULL;


//static void    hbus_dump_devices();


static int copy_urb_data_to_user(u8 __user *userbuffer, struct urb *urb)
{
    unsigned i, len, size;
    unsigned int pt = 0;

    if (urb->number_of_packets > 0)		/* Isochronous */
        len = urb->transfer_buffer_length;
    else					/* Non-Isoc */
        len = urb->actual_length;

    if (urb->num_sgs == 0) {
        if (copy_to_user(userbuffer, urb->transfer_buffer, len)){
            printk("copy_to_user urb->num_sgs == 0 error\n");
            return -EFAULT;
        }
        return 0;
    }
//    if(access_ok(VERIFY_WRITE, userbuffer,0x10000)){
//        printk("userbuf is write ok 0x10000 \n");
//    }else{
//        printk("userbuf is not write 0x10000\n");
//    }
//    if(access_ok(VERIFY_WRITE, userbuffer,USB_SG_SIZE)){
//        printk("userbuf is write ok USB_SG_SIZE\n");
//    }else{
//        printk("userbuf is not write USB_SG_SIZE\n");
//    }
    for (i = 0; i < urb->num_sgs && len; i++) {
        size = (len > USB_SG_SIZE) ? USB_SG_SIZE : len;
        pt = copy_to_user(userbuffer,(char*)sg_virt(&urb->sg[i]),size);
        if(pt != 0){
            return  -EFAULT;
        }
        userbuffer += size;
        len -= size;
    }
    return 0;
}


static  long    hosless_ioctl(struct file* file,unsigned int cmd,unsigned long arg);


static void snoop_urb(struct usb_device *udev,void __user *userurb, int pipe, unsigned length,int timeout_or_status, enum snoop_when when,unsigned char *data, unsigned data_len)
{
    static const char *types[] = {"isoc", "int", "ctrl", "bulk"};
    static const char *dirs[] = {"out", "in"};
    int ep;
    const char *t, *d;

//    if (!usbfs_snoop)
//        return;

    ep = usb_pipeendpoint(pipe);
    t = types[usb_pipetype(pipe)];
    d = dirs[!!usb_pipein(pipe)];

    if (userurb) {		/* Async */
        if (when == SUBMIT)
            dev_info(&udev->dev, "userurb %p, ep%d %s-%s, "
                    "length %u\n",
                    userurb, ep, t, d, length);
        else
            dev_info(&udev->dev, "userurb %p, ep%d %s-%s, "
                    "actual_length %u status %d\n",
                    userurb, ep, t, d, length,
                    timeout_or_status);
    } else {
        if (when == SUBMIT)
            dev_info(&udev->dev, "ep%d %s-%s, length %u, "
                    "timeout %d\n",
                    ep, t, d, length, timeout_or_status);
        else
            dev_info(&udev->dev, "ep%d %s-%s, actual_length %u, "
                    "status %d\n",
                    ep, t, d, length, timeout_or_status);
    }

    if (data && data_len > 0) {
        print_hex_dump(KERN_DEBUG, "data: ", DUMP_PREFIX_NONE, 32, 1,
            data, data_len, 1);
    }
}

static void snoop_urb_data(struct urb *urb, unsigned len);





static int releaseintf(struct usb_dev_state *ps, unsigned int ifnum)
{
    struct usb_device *dev;
    struct usb_interface *intf;
    int err;

    err = -EINVAL;
    if (ifnum >= 8*sizeof(ps->ifclaimed))
        return err;
    dev = ps->dev;
    intf = usb_ifnum_to_if(dev, ifnum);
    if (!intf)
        err = -ENOENT;
    else if (test_and_clear_bit(ifnum, &ps->ifclaimed)) {
        usb_driver_release_interface(&hosless_driver, intf);
        err = 0;
    }
    return err;
}



static void destroy_async(struct usb_dev_state *ps, struct list_head *list)
{
    struct urb *urb;
    struct async *as;
    unsigned long flags;

    spin_lock_irqsave(&ps->lock, flags);
    while (!list_empty(list)) {
        as = list_entry(list->next, struct async, asynclist);
        list_del_init(&as->asynclist);
        urb = as->urb;
        usb_get_urb(urb);

        /* drop the spinlock so the completion handler can run */
        spin_unlock_irqrestore(&ps->lock, flags);
        usb_kill_urb(urb);
        usb_put_urb(urb);
        spin_lock_irqsave(&ps->lock, flags);
    }
    spin_unlock_irqrestore(&ps->lock, flags);
}

static void destroy_all_async(struct usb_dev_state *ps)
{
    destroy_async(ps, &ps->async_pending);
}

static void destroy_async_on_interface(struct usb_dev_state *ps,unsigned int ifnum)
{
    struct list_head *p, *q, hitlist;
    unsigned long flags;

    INIT_LIST_HEAD(&hitlist);
    spin_lock_irqsave(&ps->lock, flags);
    list_for_each_safe(p, q, &ps->async_pending)
        if (ifnum == list_entry(p, struct async, asynclist)->ifnum)
            list_move_tail(p, &hitlist);
    spin_unlock_irqrestore(&ps->lock, flags);
    destroy_async(ps, &hitlist);
}

static struct usb_host_endpoint *ep_to_host_endpoint(struct usb_device *dev,unsigned char ep)
{
    if (ep & USB_ENDPOINT_DIR_MASK)
        return dev->ep_in[ep & USB_ENDPOINT_NUMBER_MASK];
    else
        return dev->ep_out[ep & USB_ENDPOINT_NUMBER_MASK];
}



static struct async *alloc_async(unsigned int numisoframes)
{
    struct async *as;

    as = kzalloc(sizeof(struct async), GFP_KERNEL);
    if (!as){
        printk("alloc as error from alloc_async()\n");
        return NULL;
    }
    as->urb = usb_alloc_urb(numisoframes, GFP_KERNEL);
    if (!as->urb) {
        printk("alloc urb error form alloc_async()\n");
        kfree(as);
        return NULL;
    }

    return as;
}

static void hosless_async_completed(struct urb* urb)
{
    struct  async* as = urb->context;
    int     ret = 0;
    int     x   = 0;
    unsigned len = 0;
    unsigned size = 0;
    unsigned    char*   kernel_buf = 0;
    if(g_request_data == false){
        ret = usb_submit_urb(urb,GFP_ATOMIC);
        if(ret != 0){
            printk("urb = %p urb->hcpriv = %p urb->completed = %p urb->internal = %d error code = %d\n\n\n\n",
                   urb,urb->hcpriv,urb->complete,urb->interval,ret);
        }
        return  ;
    }
    if(urb->status == 0){
        as->status = 0;
        len = urb->actual_length;
        kernel_buf = g_data[as->async_index]->kernel_buffer;
        g_data[as->async_index]->actual_len = urb->actual_length;
        g_data[as->async_index]->is_ok = 1;
        for (x = 0; x < urb->num_sgs && len; ++x) {
            size = (len > USB_SG_SIZE) ? USB_SG_SIZE : len;
            memcpy(kernel_buf,sg_virt(&urb->sg[x]),size);
            kernel_buf += size;
            len -= size;
        }
        list_move_tail(&g_data[as->async_index]->list,&as->ps->hosless_completed);
    }
    ret = usb_submit_urb(urb,GFP_ATOMIC);
    if(ret != 0){
        printk("urb = %p urb->hcpriv = %p urb->completed = %p urb->internal = %d error code = %d\n\n\n\n",
               urb,urb->hcpriv,urb->complete,urb->interval,ret);
    }
}

static void async_completed(struct urb *urb)
{
    struct async *as = urb->context;
    struct usb_dev_state *ps = as->ps;
    struct siginfo sinfo;
    struct pid *pid = NULL;
    u32 secid = 0;
    const struct cred *cred = NULL;
    int signr;
    spin_lock(&ps->lock);
    list_move_tail(&as->asynclist, &ps->async_completed);
    as->status = urb->status;
    signr = as->signr;
    if (signr) {
        sinfo.si_signo = as->signr;
        sinfo.si_errno = as->status;
        sinfo.si_code = SI_ASYNCIO;
        sinfo.si_addr = as->userurb;
        pid = get_pid(as->pid);
        cred = get_cred(as->cred);
        secid = as->secid;
    }
    if (as->status < 0 && as->bulk_addr && as->status != -ECONNRESET &&
            as->status != -ENOENT){
//        printk("urb transfer error state = %d\n",as->status);
        cancel_bulk_urbs(ps, as->bulk_addr);
    }
    spin_unlock(&ps->lock);
//    printk("status = %d : urb transfer completed len = %d : actual_len = %d\n",urb->status,urb->transfer_buffer_length,urb->actual_length);


    wake_up(&ps->wait);
}


static void free_async(struct async *as)
{
    int i;
    put_pid(as->pid);
    if (as->cred)
        put_cred(as->cred);
    for (i = 0; i < as->urb->num_sgs; i++) {
        if (sg_page(&as->urb->sg[i]))
            kfree(sg_virt(&as->urb->sg[i]));
    }
    kfree(as->urb->sg);
    kfree(as->urb->transfer_buffer);
    kfree(as->urb->setup_packet);
    usb_free_urb(as->urb);
    kfree(as);
}
static void async_newpending(struct async *as)
{
    struct usb_dev_state *ps = as->ps;
    unsigned long flags;

    spin_lock_irqsave(&ps->lock, flags);
    list_add_tail(&as->asynclist, &ps->async_pending);
    spin_unlock_irqrestore(&ps->lock, flags);
}

static void async_removepending(struct async *as)
{
    struct usb_dev_state *ps = as->ps;
    unsigned long flags;

    spin_lock_irqsave(&ps->lock, flags);
    list_del_init(&as->asynclist);
    spin_unlock_irqrestore(&ps->lock, flags);
}

static struct async* hosless_async_getcompleted(struct usb_dev_state* ps)
{
    struct async*   as = NULL;
    if(!list_empty(&ps->async_completed)){
        as = list_entry(ps->async_completed.next,struct async,asynclist);
        list_del_init(&as->asynclist);
        --g_urbsize;
    }
    return  as;
}

static struct async *async_getcompleted(struct usb_dev_state *ps)
{
    unsigned long flags;
    struct async *as = NULL;

    spin_lock_irqsave(&ps->lock, flags);
    if (!list_empty(&ps->async_completed)) {
        as = list_entry(ps->async_completed.next, struct async,
                asynclist);
        list_del_init(&as->asynclist);
    }
    spin_unlock_irqrestore(&ps->lock, flags);
    return as;
}

static struct async *async_getpending(struct usb_dev_state *ps,void __user *userurb)
{
    struct async *as;

    list_for_each_entry(as, &ps->async_pending, asynclist)
        if (as->userurb == userurb) {
            list_del_init(&as->asynclist);
            return as;
        }

    return NULL;
}







static void snoop_urb_data(struct urb *urb, unsigned len)
{
    int i, size;

    if (urb->num_sgs == 0) {
        print_hex_dump(KERN_DEBUG, "data: ", DUMP_PREFIX_NONE, 32, 1,
            urb->transfer_buffer, len, 1);
        return;
    }

    for (i = 0; i < urb->num_sgs && len; i++) {
        size = (len > USB_SG_SIZE) ? USB_SG_SIZE : len;
        print_hex_dump(KERN_DEBUG, "data: ", DUMP_PREFIX_NONE, 32, 1,
            sg_virt(&urb->sg[i]), size, 1);
        len -= size;
    }
}

static int husb_driver_claim_interface(struct usb_driver* driver,struct usb_interface* intf,void* priv)
{
    struct  device* dev = &intf->dev;
    struct  usb_device* udev;
    int retval = 0;
  //  int lpm_disable_error=0;
    if(dev->driver){
        printk("fuck dev->driver is have person\n");
        return  0;
    }

    udev = interface_to_usbdev(intf);

    dev->driver = &driver->drvwrap.driver;

    intf->needs_binding = 0;

    intf->condition = USB_INTERFACE_BOUND;

  //  lpm_disable_error = usb_unlocked_disable_lpm(udev);
    pm_suspend_ignore_children(dev,false);
    if(driver->supports_autosuspend)
        pm_runtime_enable(dev);
    else
        pm_runtime_set_active(dev);

    if(device_is_registered(dev))
        retval = device_bind_driver(dev);

    return  retval;
}



static int claimintf(struct usb_dev_state *ps, unsigned int ifnum)
{
    struct usb_device *dev = ps->dev;
    struct usb_interface *intf;
    int err;
    printk("claimintf begin()\n");
    if (ifnum >= 8*sizeof(ps->ifclaimed)){
        printk("return  process 1\n");
        return -EINVAL;
    }
    /* already claimed */
    if (test_bit(ifnum, &ps->ifclaimed)){
        printk("return  process 2\n");
        return 0;
    }

    intf = usb_ifnum_to_if(dev, ifnum);
    if (!intf){
        printk("return  process 3\n");
        err = -ENOENT;
    }
    else{
        printk("return  process 7\n");
        err = husb_driver_claim_interface(&hosless_driver, intf, ps);
    }
    if (err == 0){
        printk("return  process 4\n");
        set_bit(ifnum, &ps->ifclaimed);
    }
    printk("return  process 5 ret = %d\n",err);
    return err;
}

static int checkintf(struct usb_dev_state *ps, unsigned int ifnum)
{
    if (ps->dev->state != USB_STATE_CONFIGURED)
        return -EHOSTUNREACH;
    if (ifnum >= 8*sizeof(ps->ifclaimed))
        return -EINVAL;
    if (test_bit(ifnum, &ps->ifclaimed))
        return 0;
    /* if not yet claimed, claim it for the driver */
    dev_warn(&ps->dev->dev, "usbfs: process %d (%s) did not claim "
         "interface %u before use\n", task_pid_nr(current),
         current->comm, ifnum);
    return claimintf(ps, ifnum);
}




static int findintfep(struct usb_device *dev, unsigned int ep)
{
    unsigned int i, j, e;
    struct usb_interface *intf;
    struct usb_host_interface *alts;
    struct usb_endpoint_descriptor *endpt;

    if (ep & ~(USB_DIR_IN|0xf))
        return -EINVAL;
    if (!dev->actconfig)
        return -ESRCH;
    for (i = 0; i < dev->actconfig->desc.bNumInterfaces; i++) {
        intf = dev->actconfig->interface[i];
        for (j = 0; j < intf->num_altsetting; j++) {
            alts = &intf->altsetting[j];
            for (e = 0; e < alts->desc.bNumEndpoints; e++) {
                endpt = &alts->endpoint[e].desc;
                if (endpt->bEndpointAddress == ep)
                    return alts->desc.bInterfaceNumber;
            }
        }
    }
    return -ENOENT;
}

static int check_ctrlrecip(struct usb_dev_state *ps, unsigned int requesttype,
               unsigned int request, unsigned int index)
{
    int ret = 0;
    struct usb_host_interface *alt_setting;

    if (ps->dev->state != USB_STATE_UNAUTHENTICATED
     && ps->dev->state != USB_STATE_ADDRESS
     && ps->dev->state != USB_STATE_CONFIGURED)
        return -EHOSTUNREACH;
    if (USB_TYPE_VENDOR == (USB_TYPE_MASK & requesttype))
        return 0;

    /*
     * check for the special corner case 'get_device_id' in the printer
     * class specification, which we always want to allow as it is used
     * to query things like ink level, etc.
     */
    if (requesttype == 0xa1 && request == 0) {
        alt_setting = usb_find_alt_setting(ps->dev->actconfig,
                           index >> 8, index & 0xff);
        if (alt_setting
         && alt_setting->desc.bInterfaceClass == USB_CLASS_PRINTER)
            return 0;
    }

    index &= 0xff;
    switch (requesttype & USB_RECIP_MASK) {
    case USB_RECIP_ENDPOINT:
        if ((index & ~USB_DIR_IN) == 0)
            return 0;
        ret = findintfep(ps->dev, index);
        if (ret < 0) {
            /*
             * Some not fully compliant Win apps seem to get
             * index wrong and have the endpoint number here
             * rather than the endpoint address (with the
             * correct direction). Win does let this through,
             * so we'll not reject it here but leave it to
             * the device to not break KVM. But we warn.
             */
            ret = findintfep(ps->dev, index ^ 0x80);
            if (ret >= 0)
                dev_info(&ps->dev->dev,
                    "%s: process %i (%s) requesting ep %02x but needs %02x\n",
                    __func__, task_pid_nr(current),
                    current->comm, index, index ^ 0x80);
        }
        if (ret >= 0)
            ret = checkintf(ps, ret);
        break;

    case USB_RECIP_INTERFACE:
        ret = checkintf(ps, index);
        break;
    }
    return ret;
}

struct subsys_private {
    struct kset subsys;
    struct kset *devices_kset;
    struct list_head interfaces;
    struct mutex mutex;

    struct kset *drivers_kset;
    struct klist klist_devices;
    struct klist klist_drivers;
    struct blocking_notifier_head bus_notifier;
    unsigned int drivers_autoprobe:1;
    struct bus_type *bus;

    struct kset glue_dirs;
    struct class *class;
};
#define to_subsys_private(obj) container_of(obj, struct subsys_private, subsys.kobj)

struct driver_private {
    struct kobject kobj;
    struct klist klist_devices;
    struct klist_node knode_bus;
    struct module_kobject *mkobj;
    struct device_driver *driver;
};
struct device_private {
    struct klist klist_children;
    struct klist_node knode_parent;
    struct klist_node knode_driver;
    struct klist_node knode_bus;
    struct list_head deferred_probe;
    struct device *device;
};

static  struct  klist_node* to_klist_node(struct    list_head*  n)
{
    return  container_of(n,struct   klist_node,n_node);
}




#define to_device_private_bus(obj)	\
    container_of(obj, struct device_private, knode_bus)


#define KNODE_DEAD  1LU

static  bool    knode_dead(struct   klist_node* knode)
{
    return  (unsigned long)knode->n_klist & KNODE_DEAD;
}



struct  klist_node* hklist_next(struct  klist_iter* i)
{
    void(*put)(struct klist_node*) = i->i_klist->put;
    struct  klist_node* last = i->i_cur;
    struct  klist_node* next;
    printk("hklist_next\n");
    if(last){
        printk("last == true\n");
        next = to_klist_node(last->n_node.next);
    }else{
        printk("last == false\n");
        next = to_klist_node(i->i_klist->k_list.next);
    }
    i->i_cur = NULL;
    while(next != to_klist_node(&i->i_klist->k_list)){
        if(likely(!knode_dead(next))){
            printk("return  now\n");
            kref_get(&next->n_ref);
            i->i_cur = next;
            break;
        }
        next = to_klist_node(next->n_node.next);
    }
    printk("hklist_next end\n");
    return  i->i_cur;
}



static struct device *hnext_device(struct klist_iter *i)
{
    printk("hnext_device\n");
    struct klist_node *n = hklist_next(i);
    printk("klist_next finish\n");
    struct device *dev = NULL;
    struct device_private *dev_prv;
    if (n) {
        printk("dev_prv = to_device_private_bus()\n");
        dev_prv = to_device_private_bus(n);
        dev = dev_prv->device;
    }
    printk("return  dev = %p\n",dev);
    return dev;
}




//static void    hbus_dump_devices()
//{
//    struct  klist_iter i;
//    struct  device* dev;
//    struct  usb_device* udev;
//    struct  usb_device_descriptor*  ds;
//    int error = 0;
//    struct  bus_type*   bus = hosless_driver.drvwrap.driver.bus;
//    printk("usb_bus_type = %p\n",bus);

//    if(!bus || !bus->p){
//        printk("!bus || !bus->p\n");
//        return;
//    }
//    printk("kilist_iter_init_node()\n");
//    klist_iter_init_node(&bus->p->klist_devices,&i,NULL);
//    printk("while dev = hnext_device\n");
//    while((dev = hnext_device(&i))){
//        printk("while dev = %p\n",dev);
//        udev = container_of(dev,struct usb_device,dev);
//        if(!udev){
//            printk("udev == 0\n");
//            break;
//        }
//        ds = &udev->descriptor;
//        printk("usb-device = %s : %s\nid = %d : pd = %d\n",udev->product,udev->manufacturer,ds->idVendor,ds->idProduct);
//        if(ds->idVendor == 1027 && ds->idProduct == 24596){
//            printk("attach this usbdevice\n");
//            usb_classdev_add(udev);
//            break;
//        }
//    }
//    klist_iter_exit(&i);
//    printk("hbus_dump_exit()\n");
//}




//static  struct  usb_device* usbdev_lookup_by_devt(dev_t devt)
//{
//    struct  device* dev;
//    dev = bus_find_device(&usb_bus_type,NULL,(void*)(unsigned long)devt,match_devt);
//    if(!dev)
//        return  NULL;
//    return  container_of(dev,struct usb_device,dev);
//}




static int hosless_open(struct inode *inode, struct file *file)
{
    struct  hosless_driver_data*    hd = NULL;
    struct  usb_device* dev = NULL;
    struct  usb_dev_state*  ps;
    int ret;

    printk("hosless_open!!!!!!!!!!!!!1\n");
    if(g_dev == NULL){
        printk("hosless_open error because g_dev == NULL\n");
        return  -ENODEV;
    }
    if(g_ps != NULL && g_hd != NULL){
        printk("g_ps != NULL and g_hd != NULL current is running state\n");
        file->private_data = g_hd;
        return  0;
    }

    ret = -ENOMEM;

    ps = kmalloc(sizeof(struct usb_dev_state),GFP_KERNEL);
    hd = kmalloc(sizeof(struct hosless_driver_data),GFP_KERNEL);
    g_ps = ps;
    g_hd = hd;
    if(!ps || !hd){
        printk("kmalloc ps or hd error\n");
        return  ret;
    }
    ret = -ENODEV;
    dev = g_dev;
    if(!dev){
        printk("usbdev_lookup_by_devt error\n");
        return  ret;
    }
    if(dev->state == USB_STATE_NOTATTACHED)
        return;

    ps->dev = dev;
    ps->file = file;
    spin_lock_init(&ps->lock);

    INIT_LIST_HEAD(&ps->list);

    INIT_LIST_HEAD(&ps->async_pending);
    INIT_LIST_HEAD(&ps->async_completed);
    INIT_LIST_HEAD(&ps->hosless_completed);

    init_waitqueue_head(&ps->wait);

    file->private_data = hd;

    hd->ps = ps;
    hd->skel = 0;

    return  0;
}

static int hosless_release(struct inode *inode, struct file *file)
{
    struct  hosless_driver_data*    hd = NULL;
    struct  usb_dev_state*          ps = NULL;
    struct  hosless_get_buf*        buf= NULL;
    printk("hosless_release\n");
    printk("g_isrunning = true\n");
    printk("clear all list\n");
    printk("clear variable name = g_cur in init_urb index\n");
    g_cur = 0;
    hd = file->private_data;
    ps = hd->ps;
    g_request_data = false;
    while(!list_empty(&ps->hosless_completed)){
        buf = list_entry(ps->hosless_completed.next,struct hosless_get_buf,list);
        list_del_init(&buf->list);
    }
    INIT_LIST_HEAD(&ps->hosless_completed);
    printk("clear all list finish\n");
    return 0;
}

static int hosless_flush(struct file *file, fl_owner_t id)
{
    return 0;
}

static  unsigned int hosless_poll(struct file* file,struct poll_table_struct* wait)
{
    struct  hosless_driver_data*    hd = file->private_data;
    struct usb_dev_state*           ps = hd->ps;
    unsigned    int mask = 0;
    poll_wait(file,&ps->wait,wait);
    if(!list_empty(&ps->async_completed)){
        mask |= POLLIN;
    }else{
        printk("list_empty == 0\n");
        mask |= POLLOUT;
    }
    return  mask;
}

static const struct file_operations skel_fops = {
    .owner =	THIS_MODULE,
    .open =		hosless_open,
    .release =	hosless_release,
    .poll    =    hosless_poll,
    .unlocked_ioctl = hosless_ioctl
};





static int hosless_probe(struct usb_interface *interface,const struct usb_device_id *id)
{
    return  -ENODEV;
}

static void hosless_disconnect(struct usb_interface *interface)
{
//    printk("hosless disconnect\n");
//    struct hosless_driver_data* hd;
//    struct usb_dev_state*   ps;

//    hd = usb_get_intfdata(interface);
//    ps = hd->ps;




//    unsigned int ifnum = interface->altsetting->desc.bInterfaceNumber;

//    if(!ps)
//        return;
//    if(likely(ifnum < 8 * sizeof(ps->ifclaimed)))
//        clear_bit(ifnum,&ps->ifclaimed);
//    else
//        dev_warn(&interface->dev,"interface number %u out of range\n",ifnum);

//    usb_set_intfdata(interface,NULL);
//    destroy_async_on_interface(ps,ifnum);
}



static int hosless_suspend(struct usb_interface *intf, pm_message_t message)
{
    return 0;
}

static int hosless_resume(struct usb_interface *intf)
{
    return 0;
}



//-----------------------------------------------------------------------------------------------------------------------------------------------


static  int proc_control(struct usb_dev_state*  ps,void __user* arg)
{
    struct usb_device *dev = ps->dev;
    struct usbdevfs_ctrltransfer ctrl;
    unsigned int tmo;
    unsigned char *tbuf;
    unsigned wLength;
    int i, pipe, ret;

    if (copy_from_user(&ctrl, arg, sizeof(ctrl)))
        return -EFAULT;
    ret = check_ctrlrecip(ps, ctrl.bRequestType, ctrl.bRequest,
                  ctrl.wIndex);
    if (ret)
        return ret;
    wLength = ctrl.wLength;		/* To suppress 64k PAGE_SIZE warning */
    if (wLength > PAGE_SIZE)
        return -EINVAL;
//    ret = usbfs_increase_memory_usage(PAGE_SIZE + sizeof(struct urb) +
//            sizeof(struct usb_ctrlrequest));
//    if (ret)
//        return ret;
    tbuf = (unsigned char *)__get_free_page(GFP_KERNEL);
    if (!tbuf) {
        ret = -ENOMEM;
        goto done;
    }
    tmo = ctrl.timeout;
    snoop(&dev->dev, "control urb: bRequestType=%02x "
        "bRequest=%02x wValue=%04x "
        "wIndex=%04x wLength=%04x\n",
        ctrl.bRequestType, ctrl.bRequest, ctrl.wValue,
        ctrl.wIndex, ctrl.wLength);
    if (ctrl.bRequestType & 0x80) {
        if (ctrl.wLength && !access_ok(VERIFY_WRITE, ctrl.data,
                           ctrl.wLength)) {
            ret = -EINVAL;
            goto done;
        }
        pipe = usb_rcvctrlpipe(dev, 0);
//        snoop_urb(dev, NULL, pipe, ctrl.wLength, tmo, SUBMIT, NULL, 0);

        usb_unlock_device(dev);
        i = usb_control_msg(dev, pipe, ctrl.bRequest,
                    ctrl.bRequestType, ctrl.wValue, ctrl.wIndex,
                    tbuf, ctrl.wLength, tmo);
        usb_lock_device(dev);
//        snoop_urb(dev, NULL, pipe, max(i, 0), min(i, 0), COMPLETE,
//              tbuf, max(i, 0));
        if ((i > 0) && ctrl.wLength) {
            if (copy_to_user(ctrl.data, tbuf, i)) {
                ret = -EFAULT;
                goto done;
            }
        }
    } else {
        if (ctrl.wLength) {
            if (copy_from_user(tbuf, ctrl.data, ctrl.wLength)) {
                ret = -EFAULT;
                goto done;
            }
        }
        pipe = usb_sndctrlpipe(dev, 0);
//        snoop_urb(dev, NULL, pipe, ctrl.wLength, tmo, SUBMIT,
//            tbuf, ctrl.wLength);

        usb_unlock_device(dev);
        i = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), ctrl.bRequest,
                    ctrl.bRequestType, ctrl.wValue, ctrl.wIndex,
                    tbuf, ctrl.wLength, tmo);
        usb_lock_device(dev);
//        snoop_urb(dev, NULL, pipe, max(i, 0), min(i, 0), COMPLETE, NULL, 0);
    }
    if (i < 0 && i != -EPIPE) {
        dev_printk(KERN_DEBUG, &dev->dev, "usbfs: USBDEVFS_CONTROL "
               "failed cmd %s rqt %u rq %u len %u ret %d\n",
               current->comm, ctrl.bRequestType, ctrl.bRequest,
               ctrl.wLength, i);
    }
    ret = i;
 done:
    free_page((unsigned long) tbuf);
//    usbfs_decrease_memory_usage(PAGE_SIZE + sizeof(struct urb) +
//            sizeof(struct usb_ctrlrequest));
    return ret;
}

static int proc_bulk(struct usb_dev_state *ps, void __user *arg)
{
    struct usb_device *dev = ps->dev;
    struct usbdevfs_bulktransfer bulk;
    unsigned int tmo, len1, pipe;
    int len2;
    unsigned char *tbuf;
    int i, ret;

    if (copy_from_user(&bulk, arg, sizeof(bulk)))
        return -EFAULT;
    ret = findintfep(ps->dev, bulk.ep);
    if (ret < 0)
        return ret;
    ret = checkintf(ps, ret);
    if (ret)
        return ret;
    if (bulk.ep & USB_DIR_IN)
        pipe = usb_rcvbulkpipe(dev, bulk.ep & 0x7f);
    else
        pipe = usb_sndbulkpipe(dev, bulk.ep & 0x7f);
    if (!usb_maxpacket(dev, pipe, !(bulk.ep & USB_DIR_IN)))
        return -EINVAL;
    len1 = bulk.len;
    if (len1 >= USBFS_XFER_MAX)
        return -EINVAL;
//    ret = usbfs_increase_memory_usage(len1 + sizeof(struct urb));
//    if (ret)
//        return ret;
    if (!(tbuf = kmalloc(len1, GFP_KERNEL))) {
        ret = -ENOMEM;
        goto done;
    }
    tmo = bulk.timeout;
    if (bulk.ep & 0x80) {
        if (len1 && !access_ok(VERIFY_WRITE, bulk.data, len1)) {
            ret = -EINVAL;
            goto done;
        }
        snoop_urb(dev, NULL, pipe, len1, tmo, SUBMIT, NULL, 0);

        usb_unlock_device(dev);
        i = usb_bulk_msg(dev, pipe, tbuf, len1, &len2, tmo);
        usb_lock_device(dev);
        snoop_urb(dev, NULL, pipe, len2, i, COMPLETE, tbuf, len2);

        if (!i && len2) {
            if (copy_to_user(bulk.data, tbuf, len2)) {
                ret = -EFAULT;
                goto done;
            }
        }
    } else {
        if (len1) {
            if (copy_from_user(tbuf, bulk.data, len1)) {
                ret = -EFAULT;
                goto done;
            }
        }
        snoop_urb(dev, NULL, pipe, len1, tmo, SUBMIT, tbuf, len1);

        usb_unlock_device(dev);
        i = usb_bulk_msg(dev, pipe, tbuf, len1, &len2, tmo);
        usb_lock_device(dev);
        snoop_urb(dev, NULL, pipe, len2, i, COMPLETE, NULL, 0);
    }
    ret = (i < 0 ? i : len2);
 done:
    kfree(tbuf);
//    usbfs_decrease_memory_usage(len1 + sizeof(struct urb));
    return ret;
}


static void check_reset_of_active_ep(struct usb_device *udev,
        unsigned int epnum, char *ioctl_name)
{
    struct usb_host_endpoint **eps;
    struct usb_host_endpoint *ep;

    eps = (epnum & USB_DIR_IN) ? udev->ep_in : udev->ep_out;
    ep = eps[epnum & 0x0f];
    if (ep && !list_empty(&ep->urb_list))
        dev_warn(&udev->dev, "Process %d (%s) called USBDEVFS_%s for active endpoint 0x%02x\n",
                task_pid_nr(current), current->comm,
                ioctl_name, epnum);
}

static int proc_resetep(struct usb_dev_state *ps, void __user *arg)
{
    unsigned int ep;
    int ret;

    if (get_user(ep, (unsigned int __user *)arg))
        return -EFAULT;
    ret = findintfep(ps->dev, ep);
    if (ret < 0)
        return ret;
    ret = checkintf(ps, ret);
    if (ret)
        return ret;
    check_reset_of_active_ep(ps->dev, ep, "RESETEP");
    usb_reset_endpoint(ps->dev, ep);
    return 0;
}

static int proc_clearhalt(struct usb_dev_state *ps, void __user *arg)
{
    unsigned int ep;
    int pipe;
    int ret;

    if (get_user(ep, (unsigned int __user *)arg))
        return -EFAULT;
    ret = findintfep(ps->dev, ep);
    if (ret < 0)
        return ret;
    ret = checkintf(ps, ret);
    if (ret)
        return ret;
    check_reset_of_active_ep(ps->dev, ep, "CLEAR_HALT");
    if (ep & USB_DIR_IN)
        pipe = usb_rcvbulkpipe(ps->dev, ep & 0x7f);
    else
        pipe = usb_sndbulkpipe(ps->dev, ep & 0x7f);

    return usb_clear_halt(ps->dev, pipe);
}

static int proc_resetdevice(struct usb_dev_state *ps)
{
    return usb_reset_device(ps->dev);
}


static int proc_setconfig(struct usb_dev_state *ps, void __user *arg)
{
    int u;
    int status = 0;
    struct usb_host_config *actconfig;

    if (get_user(u, (int __user *)arg))
        return -EFAULT;

    actconfig = ps->dev->actconfig;

    /* Don't touch the device if any interfaces are claimed.
     * It could interfere with other drivers' operations, and if
     * an interface is claimed by usbfs it could easily deadlock.
     */
    if (actconfig) {
        int i;

        for (i = 0; i < actconfig->desc.bNumInterfaces; ++i) {
            if (usb_interface_claimed(actconfig->interface[i])) {
                dev_warn(&ps->dev->dev,
                    "usbfs: interface %d claimed by %s "
                    "while '%s' sets config #%d\n",
                    actconfig->interface[i]
                        ->cur_altsetting
                        ->desc.bInterfaceNumber,
                    actconfig->interface[i]
                        ->dev.driver->name,
                    current->comm, u);
                status = -EBUSY;
                break;
            }
        }
    }

    /* SET_CONFIGURATION is often abused as a "cheap" driver reset,
     * so avoid usb_set_configuration()'s kick to sysfs
     */
    if (status == 0) {
        if (actconfig && actconfig->desc.bConfigurationValue == u)
            status = usb_reset_configuration(ps->dev);
  //      else
  //          status = usb_set_configuration(ps->dev, u);
    }

    return status;
}


static int proc_do_submiturb(struct usb_dev_state *ps, struct usbdevfs_urb *uurb,
            struct usbdevfs_iso_packet_desc __user *iso_frame_desc,
            void __user *arg)
{
    static  bool    first = true;
    struct usbdevfs_iso_packet_desc *isopkt = NULL;
    struct usb_host_endpoint *ep;
    struct async *as = NULL;
    struct usb_ctrlrequest *dr = NULL;
    unsigned int u, totlen, isofrmlen;
    int i, ret, is_in, num_sgs = 0, ifnum = -1;
    int number_of_packets = 0;
    unsigned int stream_id = 0;
    void *buf;

    if (!(uurb->type == USBDEVFS_URB_TYPE_CONTROL &&
        (uurb->endpoint & ~USB_ENDPOINT_DIR_MASK) == 0)) {
        ifnum = findintfep(ps->dev, uurb->endpoint);
        if (ifnum < 0){
             printk("do_submit_urb return proc 3\n");
            return ifnum;
        }
        ret = checkintf(ps, ifnum);
        if (ret){
             printk("do_submit_urb return proc 4\n");
            return ret;
        }
    }
 //   printk("uurb->type = %d uurb->endpoint = %d\n",uurb->type,uurb->endpoint);
    ep = ep_to_host_endpoint(ps->dev, uurb->endpoint);
    if (!ep){
         printk("do_submit_urb return proc 5\n");
        return -ENOENT;
    }
    is_in = (uurb->endpoint & USB_ENDPOINT_DIR_MASK) != 0;
    u = 0;
    switch(uurb->type) {
    case USBDEVFS_URB_TYPE_CONTROL:
        if (!usb_endpoint_xfer_control(&ep->desc))
            return -EINVAL;
        /* min 8 byte setup packet */
        if (uurb->buffer_length < 8)
            return -EINVAL;
        dr = kmalloc(sizeof(struct usb_ctrlrequest), GFP_KERNEL);
        if (!dr)
            return -ENOMEM;
        if (copy_from_user(dr, uurb->buffer, 8)) {
            ret = -EFAULT;
            goto error;

        }
        if (uurb->buffer_length < (le16_to_cpup(&dr->wLength) + 8)) {
            ret = -EINVAL;
            goto error;
        }
        ret = check_ctrlrecip(ps, dr->bRequestType, dr->bRequest,
                      le16_to_cpup(&dr->wIndex));
        if (ret)
            goto error;
        uurb->buffer_length = le16_to_cpup(&dr->wLength);
        uurb->buffer += 8;
        if ((dr->bRequestType & USB_DIR_IN) && uurb->buffer_length) {
            is_in = 1;
            uurb->endpoint |= USB_DIR_IN;
        } else {
            is_in = 0;
            uurb->endpoint &= ~USB_DIR_IN;
        }
        u = sizeof(struct usb_ctrlrequest);
        break;

    case USBDEVFS_URB_TYPE_BULK:
        switch (usb_endpoint_type(&ep->desc)) {
        case USB_ENDPOINT_XFER_CONTROL:
        case USB_ENDPOINT_XFER_ISOC:
            return -EINVAL;
        case USB_ENDPOINT_XFER_INT:
            /* allow single-shot interrupt transfers */
            uurb->type = USBDEVFS_URB_TYPE_INTERRUPT;
            goto interrupt_urb;
        }
        num_sgs = DIV_ROUND_UP(uurb->buffer_length, USB_SG_SIZE);
        if (num_sgs == 1 || num_sgs > ps->dev->bus->sg_tablesize)
            num_sgs = 0;
        break;
    case USBDEVFS_URB_TYPE_INTERRUPT:
        if (!usb_endpoint_xfer_int(&ep->desc))
            return -EINVAL;
 interrupt_urb:
        break;
    default:{
         printk("do_submit_urb return proc 5\n");
        return -EINVAL;
        }
    }

    as = alloc_async(number_of_packets);
    if (!as) {
        ret = -ENOMEM;
         printk("do_submit_urb return proc 7\n");
        goto error;
    }
    if (num_sgs) {
        as->urb->sg = kmalloc(num_sgs * sizeof(struct scatterlist),
                      GFP_KERNEL);
        if (!as->urb->sg) {
            printk("kmalloc as->urb->sg error\n");
            ret = -ENOMEM;
            goto error;
        }
        as->urb->num_sgs = num_sgs;
        sg_init_table(as->urb->sg, as->urb->num_sgs);
        totlen = uurb->buffer_length;
        for (i = 0; i < as->urb->num_sgs; i++) {
            u = (totlen > USB_SG_SIZE) ? USB_SG_SIZE : totlen;
            buf = kmalloc(u, GFP_KERNEL);
            if (!buf) {
                ret = -ENOMEM;
                 printk("do_submit_urb return proc 8\n");
                goto error;
            }
            sg_set_buf(&as->urb->sg[i], buf, u);
            totlen -= u;
        }
    } else if (uurb->buffer_length > 0) {
        as->urb->transfer_buffer = kmalloc(uurb->buffer_length,GFP_KERNEL);
        if (!as->urb->transfer_buffer) {
            printk("kmalloc error size = %d\n",uurb->buffer_length);
            ret = -ENOMEM;
            goto error;
        }
        if (!is_in) {
            if (copy_from_user(as->urb->transfer_buffer,uurb->buffer,uurb->buffer_length)) {
                printk("copy_from_user error\n");
                ret = -EFAULT;
                goto error;
            }
        }
    }
    as->urb->dev = ps->dev;
    as->urb->pipe = (uurb->type << 30) |
            __create_pipe(ps->dev, uurb->endpoint & 0xf) |
            (uurb->endpoint & USB_DIR_IN);

    u = (is_in ? URB_DIR_IN : URB_DIR_OUT);
    if (uurb->flags & USBDEVFS_URB_ISO_ASAP)
        u |= URB_ISO_ASAP;
    if (uurb->flags & USBDEVFS_URB_SHORT_NOT_OK && is_in)
        u |= URB_SHORT_NOT_OK;
    if (uurb->flags & USBDEVFS_URB_NO_FSBR)
        u |= URB_NO_FSBR;
    if (uurb->flags & USBDEVFS_URB_ZERO_PACKET)
        u |= URB_ZERO_PACKET;
    if (uurb->flags & USBDEVFS_URB_NO_INTERRUPT)
        u |= URB_NO_INTERRUPT;

    as->urb->transfer_flags = u;
//    printk("uurb->flags = %d urb->transfer_flags = %d\n",uurb->flags,u);
    as->urb->transfer_buffer_length = uurb->buffer_length;
    as->urb->setup_packet = (unsigned char *)dr;
    dr = NULL;
    as->urb->start_frame = uurb->start_frame;
//    printk("urb->number_of_packets = %d urb->start_frame = %d\n",number_of_packets,uurb->start_frame);
    as->urb->number_of_packets = number_of_packets;
    as->urb->stream_id = 0;
    if (uurb->type == USBDEVFS_URB_TYPE_ISO || ps->dev->speed == USB_SPEED_HIGH)
        as->urb->interval = 1 << min(15, ep->desc.bInterval - 1);
    else
        as->urb->interval = ep->desc.bInterval;
//    printk("ep->desc.binterval = %d,as->urb->interval = %d\n",ep->desc.bInterval,as->urb->interval);
//    printk("ifnum = %d\n",ifnum);
    as->urb->context = as;
    as->urb->complete = async_completed;

    kfree(isopkt);
    isopkt = NULL;
    as->ps = ps;
    as->userurb = arg;
    if (is_in && uurb->buffer_length > 0)
        as->userbuffer = uurb->buffer;
    else
        as->userbuffer = NULL;
    as->signr = uurb->signr;
    as->ifnum = ifnum;
    as->pid = get_pid(task_pid(current));
    as->cred = get_current_cred();
    security_task_getsecid(current, &as->secid);

    async_newpending(as);

    if (usb_endpoint_xfer_bulk(&ep->desc)) {
        spin_lock_irq(&ps->lock);
        as->bulk_addr = usb_endpoint_num(&ep->desc) | ((ep->desc.bEndpointAddress & USB_ENDPOINT_DIR_MASK) >> 3);
        if (uurb->flags & USBDEVFS_URB_BULK_CONTINUATION)
            as->bulk_status = AS_CONTINUATION;
        else
            ps->disabled_bulk_eps &= ~(1 << as->bulk_addr);
        if (ps->disabled_bulk_eps & (1 << as->bulk_addr)){
//            printk("nandao jinlai guo zheli wo dou bu zhidao ? zheme shenqi?wo cao\n");
            ret = -EREMOTEIO;
        }
        else{
            ret = usb_submit_urb(as->urb, GFP_ATOMIC);
            if(ret == 0){
            }
        }
        spin_unlock_irq(&ps->lock);
    } else {
        ret = usb_submit_urb(as->urb, GFP_KERNEL);
    }

    if (ret) {
        printk("submit urb error code  = %d\n",ret);
        async_removepending(as);
        goto error;
    }
    return 0;

 error:
    kfree(isopkt);
    kfree(dr);
    if (as)
        free_async(as);
//     printk("do_submit_urb return proc 10\n");
    return ret;
}


static int proc_submiturb(struct usb_dev_state *ps, void __user *arg)
{
    struct usbdevfs_urb uurb;

    if (copy_from_user(&uurb, arg, sizeof(uurb)))
        return -EFAULT;

    return proc_do_submiturb(ps, &uurb,
            (((struct usbdevfs_urb __user *)arg)->iso_frame_desc),
            arg);
}

static int hosless_processcompl(struct async* as)
{
    struct  urb* urb = as->urb;
    struct usbdevfs_urb __user*    uurb = as->userurb;
    int    ok = 1;
    if(as->userbuffer){
        if(copy_urb_data_to_user(as->userbuffer,urb)){
//            printk("hosless_processcomple copy_urb_data_to_user error\n");
            return  -EFAULT;
        }
    }
    if(put_user(as->status,&uurb->status)){
        printk("hosless_processcomple put status error\n");
        return  -EFAULT;
    }
    if(put_user(urb->actual_length,&uurb->actual_length)){
        printk("hosless_processcomple put actual_len error\n");
        return  -EFAULT;
    }
    if(put_user(ok,&uurb->error_count)){
        printk("hosless_processcomple put ok flags error\n");
        return  -EFAULT;
    }
//    printk("transfer ok\n");
    return  0;
}

static int processcompl(struct async *as, void __user * __user *arg)
{
    struct urb *urb = as->urb;
    struct usbdevfs_urb __user *userurb = as->userurb;
    void __user *addr = as->userurb;
    unsigned int i;

    if (as->userbuffer && urb->actual_length) {
        if (copy_urb_data_to_user(as->userbuffer, urb))
            goto err_out;
    }
    if (put_user(as->status, &userurb->status))
        goto err_out;
    if (put_user(urb->actual_length, &userurb->actual_length))
        goto err_out;
    if (put_user(urb->error_count, &userurb->error_count))
        goto err_out;

//    if (usb_endpoint_xfer_isoc(&urb->ep->desc)) {
//        for (i = 0; i < urb->number_of_packets; i++) {
//            if (put_user(urb->iso_frame_desc[i].actual_length,
//                     &userurb->iso_frame_desc[i].actual_length))
//                goto err_out;
//            if (put_user(urb->iso_frame_desc[i].status,
//                     &userurb->iso_frame_desc[i].status))
//                goto err_out;
//        }
//    }

    if (put_user(addr, (void __user * __user *)arg))
        return -EFAULT;
    return 0;

err_out:
    return -EFAULT;
}

static struct async *reap_as(struct usb_dev_state *ps)
{
    DECLARE_WAITQUEUE(wait, current);
    struct async *as = NULL;
    struct usb_device *dev = ps->dev;

    add_wait_queue(&ps->wait, &wait);
    for (;;) {
        __set_current_state(TASK_INTERRUPTIBLE);
        as = async_getcompleted(ps);
        if (as)
            break;
        if (signal_pending(current))
            break;
        usb_unlock_device(dev);
        schedule();
        usb_lock_device(dev);
    }
    remove_wait_queue(&ps->wait, &wait);
    set_current_state(TASK_RUNNING);
    return as;
}

static int proc_reapurb(struct usb_dev_state *ps, void __user *arg)
{
    struct async *as = reap_as(ps);
    if (as) {
        int retval = processcompl(as, (void __user * __user *)arg);
        free_async(as);
        return retval;
    }
    if (signal_pending(current))
        return -EINTR;
    return -EIO;
}

static int proc_get_running(struct usb_dev_state*   ps,void __user* arg)
{
    int fuck = 0;
    if(g_isrunning == true){
        fuck = 1;
    }else{
        fuck = 0;
    }
    if(put_user(fuck,(int __user*)arg)){
        printk("put running state error\n");
        return  -EFAULT;
    }
    return  0;
}

static int proc_hosless_get_urb_size(struct usb_dev_state* ps,void __user* arg)
{
    int fuck = -1;
    if(g_urbsize == 0){
        if(put_user(fuck,(int __user*)arg)){
            printk("put -1 to user error in get_urb_size\n");
            return  -EFAULT;
        }
        return  0;
    }
    if(put_user(g_urbsize,(int __user*)arg)){
        printk("put_user error in get_urb_size\n");
        return  -EFAULT;
    }
    return  0;
}

#define NODATA  4096

static int proc_hosless_geturb(struct usb_dev_state* ps,void __user* arg)
{
    struct  hosless_get_buf*    buf = NULL;
    void __user*    addr = NULL;
    if(list_empty(&ps->hosless_completed)){
        return  -NODATA;
    }

    buf = list_entry(ps->hosless_completed.next,struct hosless_get_buf,list);
    list_del_init(&buf->list);

    addr = buf->uurb;

    if(copy_to_user(buf->uurb->buffer,buf->kernel_buffer,buf->actual_len)){
//        printk("copy_to_error in geturb\n");
        return  -EFAULT;
    }
    if(put_user(buf->actual_len,&buf->uurb->actual_length)){
//        printk("put_user len error in geturb\n");
        return  -EFAULT;
    }
    if(put_user(addr,(void __user* __user*)arg)){
//        printk("put_user userurb addr error in geturb\n");
        return  -EFAULT;
    }
    buf->is_ok = 0;
    return  0;
}

static int proc_reapurbnonblock(struct usb_dev_state *ps, void __user *arg)
{
    int retval;
    struct async *as;

    as = async_getcompleted(ps);
    retval = -EAGAIN;
    if (as) {
        retval = processcompl(as, (void __user * __user *)arg);
//        printk("get completed as ok free it cur urb = %d\n",g_urbsize);
        free_async(as);
    }
    return retval;
}

static int proc_disconnectsignal(struct usb_dev_state *ps, void __user *arg)
{
    struct usbdevfs_disconnectsignal ds;

    if (copy_from_user(&ds, arg, sizeof(ds)))
        return -EFAULT;
    ps->discsignr = ds.signr;
    ps->disccontext = ds.context;
    return 0;
}

static int proc_claiminterface(struct usb_dev_state *ps, void __user *arg)
{
    unsigned int ifnum;

    if (get_user(ifnum, (unsigned int __user *)arg))
        return -EFAULT;
    return claimintf(ps, ifnum);
}

static int proc_releaseinterface(struct usb_dev_state *ps, void __user *arg)
{
    unsigned int ifnum;
    int ret;

    if (get_user(ifnum, (unsigned int __user *)arg))
        return -EFAULT;
    if ((ret = releaseintf(ps, ifnum)) < 0)
        return ret;
    destroy_async_on_interface (ps, ifnum);
    return 0;
}


static int proc_claim_port(struct usb_dev_state *ps, void __user *arg)
{
    unsigned portnum;
    int rc;

    if (get_user(portnum, (unsigned __user *) arg))
        return -EFAULT;
  //  rc = usb_hub_claim_port(ps->dev, portnum, ps);
	return	0;
    if (rc == 0)
        snoop(&ps->dev->dev, "port %d claimed by process %d: %s\n",
            portnum, task_pid_nr(current), current->comm);
    return rc;
}

static int proc_release_port(struct usb_dev_state *ps, void __user *arg)
{
//    unsigned portnum;

//    if (get_user(portnum, (unsigned __user *) arg))
//        return -EFAULT;
//    return usb_hub_release_port(ps->dev, portnum, ps);
    return  0;
}






static int proc_setintf(struct usb_dev_state *ps, void __user *arg)
{
    struct usbdevfs_setinterface setintf;
    int ret;

    if (copy_from_user(&setintf, arg, sizeof(setintf)))
        return -EFAULT;
    if ((ret = checkintf(ps, setintf.interface)))
        return ret;

    destroy_async_on_interface(ps, setintf.interface);

    return usb_set_interface(ps->dev, setintf.interface,
            setintf.altsetting);
}



static int hosless_init_urb(struct usb_dev_state* ps,struct usbdevfs_urb __user* uurb)
{
    struct  async*   as;
    int     type = 3;
    int     endpoint = 129;
    int     transfer_flags = 513;

    int     totlen = 0;
    int     i = 0;
    int     u = 0;
    unsigned char*  buf = 0;
    as = alloc_async(0);
    if(!as){
        printk("hosless|||||| init_all_urb() alloc_async error\n");
        return  -ENOMEM;
    }
    as->async_index = g_cur;
    g_async[g_cur++] = as;
    as->urb->sg = kmalloc(4 * sizeof(struct scatterlist),GFP_KERNEL);
    if(!as->urb->sg){
        printk("kmalloc as->urb->sg error\n");
        return  -ENOMEM;
    }
    as->urb->num_sgs = 4;
    sg_init_table(as->urb->sg,as->urb->num_sgs);
    totlen = 65536;
    for(i = 0; i < as->urb->num_sgs;++i){
        u = (totlen > USB_SG_SIZE ) ? USB_SG_SIZE : totlen;
        buf = kmalloc(u,GFP_KERNEL);
        if(!buf){
            printk("kmalloc buf error\n");
            return  -ENOMEM;
        }
        sg_set_buf(&as->urb->sg[i],buf,u);
        totlen -= u;
    }
    as->urb->dev = ps->dev;
    as->urb->pipe = (type << 30 ) | __create_pipe(ps->dev,endpoint & 0xf) | (endpoint & USB_DIR_IN);

    as->urb->transfer_flags = transfer_flags;
    as->urb->transfer_buffer_length = 65536;
    as->urb->setup_packet = 0;
    as->urb->start_frame = 0;
    as->urb->number_of_packets = 0;
    as->urb->stream_id = 0;
    as->urb->interval = 1 << min(15,0 - 1);
    as->urb->context = as;
    as->urb->complete = hosless_async_completed;
    as->ps = ps;
    as->userbuffer = uurb->buffer;
    as->userurb     = uurb;
    as->ifnum = 0;
    as->pid     = get_pid(task_pid(current));
    as->cred   = get_current_cred();
    security_task_getsecid(current,&as->secid);
    async_newpending(as);
//    printk("hosless-----------------------urb dma dump------------------------begin()\n");
//    struct  usb_hcd*    hcd = bus_to_hcd(as->urb->dev->bus);
//    if(hcd == NULL){
//        printk("hcd == NULL\n");
//        return 0 ;
//    }
//    if(hcd->driver == NULL){
//        printk("hcd->driver == NULL\n");
//        return  0;
//    }
//    if(hcd->driver->map_urb_for_dma){
//        printk("hcd->driver->map_urb_for_dma have this function\n");
//    }else{
//        printk("use ehci->map_urb_for_dma()\n");
//    }
//    if(hcd->self.uses_pio_for_control){
//        printk("urb pio for control\n");
//    }
//    if(as->urb->transfer_flags & URB_NO_TRANSFER_DMA_MAP){
//        printk("urb transfer_flags set URB_NO_TRANSFER_DMA_MAP\n");
//    }
//    if(hcd->self.uses_dma){
//        printk("hcd->self.uses_dma == true\n");
//    }
//    if(hcd->driver->flags & HCD_LOCAL_MEM){
//        printk("hdc->driver=>flags set HCD_LOCAL_MEM == true\n");
//    }




//    printk("hosless-----------------------urb dma dump--------------------------end()\n");





    return  0;
}



static int proc_hosless_set_buf(struct usb_dev_state*   ps,void __user* arg)
{
    static  int first = 1;
    struct  hosless_set_buf    buf;
    int x = 0;
    if(first){
        printk("first malloc 128 x 65536 buf\n");
        for(x = 0;x < 128;++x){
            g_data[x] = kmalloc(sizeof(struct hosless_get_buf),GFP_KERNEL);
            if(!g_data[x]){
                printk("kmalloc error index = %d\n",x);
                return  -EFAULT;
            }
            g_data[x]->kernel_buffer = kmalloc(0x10000,GFP_KERNEL);
            if(!g_data[x]->kernel_buffer){
                printk("kmalloc error g_data[%d]->kernel_buffer\n");
                return  -EFAULT;
            }
        }
        first = 0;
    }

    if(copy_from_user(&buf,arg,sizeof(buf))){
        printk("copy hosless_set_buf errpr\n");
        return  -EFAULT;
    }
    if(buf.index >=128){
        printk("set_buf index range out value = %d\n",buf.index);
        return  -EFAULT;
    }
    if(buf.uurb == 0){
        printk("set_buf buffer is NULL\n");
        return  -EFAULT;
    }
    g_data[buf.index]->index = buf.index;
    g_data[buf.index]->uurb  = buf.uurb;
    g_data[buf.index]->actual_len = 0;
    g_data[buf.index]->is_ok = 0;
    list_add_tail(&g_data[buf.index]->list, &ps->hosless_completed);
    printk("proc_hosless_set_buf buf_index = %d\n",buf.index);
    return hosless_init_urb(ps,buf.uurb);
}

static int proc_hosless_stream_restart(struct usb_dev_state* ps,void __user* arg)
{
    g_request_data = true;
    g_isrunning    = true;
    printk("proc_hosless_stream_restart\n");
    return  0;
}

static int proc_hosless_stream_start(struct usb_dev_state* ps,void __user* arg)
{
    int endpoint = 129;
    int ret = 0;
    int x = 0;
    g_isrunning = true;
    g_request_data = true;
    for(x=0;x < 128;++x){
        struct async*   as = g_async[x];
        struct usb_host_endpoint*   ep = ep_to_host_endpoint(ps->dev,endpoint);
        if(!ep){
            printk("ep_to_host_endpoint error\n");
            return -EFAULT;
        }
        if (usb_endpoint_xfer_bulk(&ep->desc)) {
            spin_lock_irq(&ps->lock);
            as->bulk_addr = usb_endpoint_num(&ep->desc) | ((ep->desc.bEndpointAddress & USB_ENDPOINT_DIR_MASK) >> 3);
            ps->disabled_bulk_eps &= ~(1 << as->bulk_addr);
                ret = usb_submit_urb(as->urb, GFP_ATOMIC);
                if(ret != 0){
                    printk("in stream_start function\n");
                    printk("submit urb error code = %d index = %d\n",ret,x);
                    printk("urb = %p urb->hcpriv = %p urb->complete = %p",as->urb,as->urb->hcpriv,as->urb->complete);
                    spin_unlock_irq(&ps->lock);
                    break;
                }
            spin_unlock_irq(&ps->lock);
        }
    }
    return  0;
}

static int proc_hosless(struct usb_dev_state* ps,void __user* arg)
{
    if(list_empty(&ps->async_completed)){
        if(g_urbsize==0)
            return  -1;
        else
            return  -2;
    }
    return  1;
}

static  long    hosless_do_ioctl(struct file* file,unsigned int cmd,void __user* p)
{


    struct  hosless_driver_data*    hd = file->private_data;

    struct  usb_dev_state*  ps = hd->ps;
//    struct  inode*  inode = file_inode(file);
    struct  usb_device* dev = ps->dev;
    int ret = -ENOTTY;



    usb_lock_device(dev);
    switch(cmd){
    case USBDEVFS_CONTROL:
//        snoop(&dev->dev, "%s: CONTROL\n", __func__);
        ret = proc_control(ps, p);
//        if (ret >= 0)
//            inode->i_mtime = CURRENT_TIME;
        break;
    case USBDEVFS_BULK:
//        snoop(&dev->dev, "%s: BULK\n", __func__);
        ret = proc_bulk(ps, p);
//        if (ret >= 0)
//            inode->i_mtime = CURRENT_TIME;
        break;
    case USBDEVFS_RESETEP:
        snoop(&dev->dev, "%s: RESETEP\n", __func__);
        ret = proc_resetep(ps, p);
//        if (ret >= 0)
//            inode->i_mtime = CURRENT_TIME;
        break;
    case USBDEVFS_RESET:
        snoop(&dev->dev, "%s: RESET\n", __func__);
        ret = proc_resetdevice(ps);
        break;
    case USBDEVFS_CLEAR_HALT:
        snoop(&dev->dev, "%s: CLEAR_HALT\n", __func__);
        ret = proc_clearhalt(ps, p);
//        if (ret >= 0)
//            inode->i_mtime = CURRENT_TIME;
        break;
    case USBDEVFS_SETINTERFACE:
        snoop(&dev->dev, "%s: SETINTERFACE\n", __func__);
        ret = proc_setintf(ps, p);
        break;
    case USBDEVFS_SETCONFIGURATION:
        snoop(&dev->dev, "%s: SETCONFIGURATION\n", __func__);
        ret = 0;
//        ret = proc_setconfig(ps, p);
        break;
    case USBDEVFS_SUBMITURB:
//        snoop(&dev->dev, "%s: SUBMITURB\n", __func__);
        ret = proc_submiturb(ps, p);
//        if (ret >= 0)
//            inode->i_mtime = CURRENT_TIME;
        break;
    case USBDEVFS_REAPURB:
        snoop(&dev->dev, "%s: REAPURB\n", __func__);
        ret = proc_reapurb(ps, p);
        break;
    case USBDEVFS_REAPURBNDELAY:
 //       snoop(&dev->dev, "%s: REAPURBNDELAY\n", __func__);
        ret = proc_reapurbnonblock(ps, p);
//        printk("proc_reapurbnonblock return = %d\n",ret);
        break;
    case USBDEVFS_CLAIMINTERFACE:
        snoop(&dev->dev, "%s: CLAIMINTERFACE\n", __func__);
        ret = proc_claiminterface(ps, p);
        break;
    case USBDEVFS_RELEASEINTERFACE:
        snoop(&dev->dev, "%s: RELEASEINTERFACE\n", __func__);
        ret = proc_releaseinterface(ps, p);
        break;
    case USBDEVFS_IOCTL:
        snoop(&dev->dev, "%s: IOCTL\n", __func__);
        ret = 0;
   //     ret = proc_ioctl_default(ps, p);
        break;

    case USBDEVFS_CLAIM_PORT:
        snoop(&dev->dev, "%s: CLAIM_PORT\n", __func__);
        ret = 0;
   //     ret = proc_claim_port(ps, p);
        break;

    case USBDEVFS_RELEASE_PORT:
        snoop(&dev->dev, "%s: RELEASE_PORT\n", __func__);
       // ret = proc_release_port(ps, p);
        break;
    case USBDEVFS_GET_CAPABILITIES:
        snoop(&dev->dev,"%s : GET_CAPABILITIES\n",__func__);
        ret = 0;
        break;
    case USBDEVFS_HOSLESS_SET_BUFFER:
        ret = proc_hosless_set_buf(ps,p);
        break;
    case USBDEVFS_HOSLESS_GET_URB:
        ret = proc_hosless_geturb(ps,p);
        break;
    case USBDEVFS_HOSLESS_GETURB_SIZE:
        ret = proc_hosless_get_urb_size(ps,p);
        break;
    case USBDEVFS_HOSLESS_GET_RUNNING:
        ret = proc_get_running(ps,p);
        break;
    case USBDEVFS_HOSLESS_STREAM_START:
        ret = proc_hosless_stream_start(ps,p);
        break;
    case USBDEVFS_HOSLESS_STREAM_RESTART:
        ret = proc_hosless_stream_restart(ps,p);
        break;
    }
    usb_unlock_device(dev);
//    if(ret >=0)
//        inode->i_atime = CURRENT_TIME;

    return  ret;
}





static  long    hosless_ioctl(struct file* file,unsigned int cmd,unsigned long arg)
{
    int ret;
    ret = hosless_do_ioctl(file,cmd,(void __user*)arg);
    return  ret;
}


static void cancel_bulk_urbs(struct usb_dev_state *ps, unsigned bulk_addr)
__releases(ps->lock)
__acquires(ps->lock)
{
    struct urb *urb;
    struct async *as;

    /* Mark all the pending URBs that match bulk_addr, up to but not
     * including the first one without AS_CONTINUATION.  If such an
     * URB is encountered then a new transfer has already started so
     * the endpoint doesn't need to be disabled; otherwise it does.
     */
    list_for_each_entry(as, &ps->async_pending, asynclist) {
        if (as->bulk_addr == bulk_addr) {
            if (as->bulk_status != AS_CONTINUATION)
                goto rescan;
            as->bulk_status = AS_UNLINK;
            as->bulk_addr = 0;
        }
    }
    ps->disabled_bulk_eps |= (1 << bulk_addr);

    /* Now carefully unlink all the marked pending URBs */
 rescan:
    list_for_each_entry(as, &ps->async_pending, asynclist) {
        if (as->bulk_status == AS_UNLINK) {
            as->bulk_status = 0;		/* Only once */
            urb = as->urb;
            usb_get_urb(urb);
            spin_unlock(&ps->lock);		/* Allow completions */
            usb_unlink_urb(urb);
            usb_put_urb(urb);
            spin_lock(&ps->lock);
            goto rescan;
        }
    }
}


static struct usb_driver skel_driver = {
    .name =		"hosless",
    .probe =	hosless_probe,
    .disconnect =	hosless_disconnect,
    .suspend =	hosless_suspend,
    .resume =	hosless_resume,
};

static  struct  cdev hosless_device_cdev;
static  struct  class*  hosless_classdev_class;


#define HOSLESS_DEVICE_DEV  MKDEV(201,0)


static  int usb_classdev_add(struct usb_device* dev)
{
    struct  device* cldev;
    printk("usb_classdev_add 1\n");
    cldev = device_create(hosless_classdev_class,&dev->dev,HOSLESS_DEVICE_DEV,NULL,"LTUSB04");
    if(IS_ERR(cldev)){
//        printk("usb classdev_add error code = %d\n",PTR_ERR(cldev));
        return  PTR_ERR(cldev);
    }
//    dev->usb_classdev = cldev;
    printk("usb_classdev_add ok!\n");
    g_dev = dev;
    return  0;
	return 0;
}

static  void    usb_classdev_remove(struct usb_device* dev)
{
/*    if(dev->usb_classdev){
        device_unregister(dev->usb_classdev);
    }*/
}

static  void    usbdev_remove(struct usb_device*    dev)
{
    printk("usbdev_remove\n");
}

static  int hosless_notify(struct notifier_block*   self,unsigned long action,void* dev)
{
    switch(action){
    case USB_DEVICE_ADD:
        if(usb_classdev_add(dev))
            return  NOTIFY_BAD;
        break;
    case USB_DEVICE_REMOVE:
        usb_classdev_remove(dev);
        usbdev_remove(dev);
        break;
    }
    return  NOTIFY_OK;
}

static  struct  notifier_block hosless_nb={
    .notifier_call = hosless_notify,
};


static  int hosless_cb(struct   device* dev,void* data)
{
//   printk("hosless_cb callbakc !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!1\n");
    struct usb_device* udev = NULL;
    struct usb_device_descriptor*  ds = NULL;
    udev = container_of(dev,struct usb_device,dev);
    if(!udev){
        printk("udev == 0\n");
        return  0;
    }
    ds = &udev->descriptor;
//    printk("usb-device = %s : %s\nid = %d : pd = %d\n",udev->product,udev->manufacturer,ds->idVendor,ds->idProduct);
    if(ds->idVendor == 1027 && ds->idProduct == 24596){
        printk("attach this usbdevice\n");
        usb_classdev_add(udev);
        return  1;
    }
    return  0;
}



static  int __init  hosless_init(void)
{
    struct  urb*    s_urb;
    struct  device* cldev;
    struct  usb_device  udev;
    int result;

    char    path[256] = "/system/bin/hosless";
    char   *argv[]    = {path,NULL};
    static char* envp[] = {"PATH=/system/bin",NULL};


    printk("hosless_init 1\n");
    result = usb_register(&hosless_driver);
    if(result){
            printk("usb_register error code = %d\n,result");
            return  result;
    }

    printk("hosless_init 2\n");
    result = register_chrdev_region(HOSLESS_DEVICE_DEV,128,"haoshao");
    if(result){
        printk("hosless-lib register char gion error\n");
        return  0;
    }
    printk("hosless_init 3\n");
    cdev_init(&hosless_device_cdev,&skel_fops);
    result = cdev_add(&hosless_device_cdev,HOSLESS_DEVICE_DEV,128);
    if(result){
        printk("hosless-lib cdev_add error\n");
        return  0;
    }
    printk("hosless_init 4\n");
    hosless_classdev_class = class_create(THIS_MODULE,"haoshaop");
    if(IS_ERR(hosless_classdev_class)){
        printk("register hosless_usb_device class error\n");
        result = PTR_ERR(hosless_classdev_class);
        cdev_del(&hosless_device_cdev);
        hosless_classdev_class = NULL;
        return  result;
    }
    printk("hosless_init 5\n");
//    hosless_classdev_class->dev_kobj = NULL;
//    device_create(hosless_classdev_class,NULL,HOSLESS_DEVICE_DEV,NULL,"haoshaofuck");



    usb_register_notify(&hosless_nb);
    printk("hosless_init ok\n");



    printk("start find usb device\n");

    bus_find_device(hosless_driver.drvwrap.driver.bus,NULL,NULL,hosless_cb);

//    hbus_dump_devices();
    result = call_usermodehelper(path,argv,envp,1);
    if(result !=0){
        printk("call_usermodehelper error code = %d\n",result);
    }



    printk("start find usb device end\n");


    return  0;
}
static void __exit hosless_exit(void)
{
    printk("hosless_exit\n");
    usb_unregister_notify(&hosless_nb);
    class_destroy(hosless_classdev_class);
    cdev_del(&hosless_device_cdev);
    unregister_chrdev_region(HOSLESS_DEVICE_DEV,128);
    usb_deregister(&hosless_driver);
}


static struct usb_driver hosless_driver = {
    .name =		"haoshao",
    .probe =	hosless_probe,
    .disconnect =	hosless_disconnect,
    .suspend =	hosless_suspend,
    .resume =	hosless_resume,
};


module_init(hosless_init);
module_exit(hosless_exit);

//module_usb_driver(skel_driver);

MODULE_LICENSE("GPL");
