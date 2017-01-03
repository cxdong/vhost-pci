/*
 * Vhost-pci Slave
 *
 * Copyright Intel Corp. 2016
 *
 * Authors:
 * Wei Wang    <wei.w.wang@intel.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <qemu/osdep.h>
#include <qemu/sockets.h>

#include "monitor/qdev.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/virtio/virtio-pci.h"
#include "hw/virtio/vhost-pci-slave.h"
#include "hw/virtio/vhost-user.h"

#define VHOST_PCI_FEATURE_BITS (1ULL << VIRTIO_F_VERSION_1)

#define VHOST_PCI_NET_FEATURE_BITS ((1ULL << VIRTIO_NET_F_MRG_RXBUF) | \
                                   (1ULL << VIRTIO_NET_F_CTRL_VQ) | \
                                   (1ULL << VIRTIO_NET_F_MQ))

VhostPCISlave *vp_slave;

static void vp_slave_cleanup(void)
{
    int ret;
    uint32_t i, nregions;
    PeerVqNode *pvq_node;

    if (!vp_slave->vdev) {
        return;
    }

    /*
     * if this cleanup is not invoked due to peer reset, re-initialize
     * the feature bits, so that the slave can work with a new master
     */
    if (!vp_slave->peer_reset) {
        vp_slave->feature_bits =
                1ULL << VHOST_USER_F_PROTOCOL_FEATURES;
    }

    nregions = vp_slave->pmem_msg.nregions;
    for (i = 0; i < nregions; i++) {
        ret = munmap(vp_slave->mr_map_base[i], vp_slave->mr_map_size[i]);
        if (ret < 0) {
            error_report("cleanup: failed to unmap mr");
        }
        memory_region_del_subregion(vp_slave->bar_mr, vp_slave->sub_mr + i);
    }

    if (!QLIST_EMPTY(&vp_slave->pvq_list)) {
        QLIST_FOREACH(pvq_node, &vp_slave->pvq_list, node)
            g_free(pvq_node);
    }
    QLIST_INIT(&vp_slave->pvq_list);
    vp_slave->pvq_num = 0;
    vp_slave->vdev = NULL;
}

static int vp_slave_write(CharBackend *chr_be, VhostUserMsg *msg)
{
    int size;

    if (!msg) {
        return 0;
    }

    size = msg->size + VHOST_USER_HDR_SIZE;
    msg->flags &= ~VHOST_USER_VERSION_MASK;
    msg->flags |= VHOST_USER_VERSION;

    return qemu_chr_fe_write_all(chr_be, (const uint8_t *)msg, size)
           == size ? 0 : -1;
}

static int vp_slave_get_features(CharBackend *chr_be, VhostUserMsg *msg)
{
    msg->payload.u64 = vp_slave->feature_bits;
    msg->size = sizeof(msg->payload.u64);
    msg->flags |= VHOST_USER_REPLY_MASK;

    return vp_slave_write(chr_be, msg);
}

static void vp_slave_set_features(VhostUserMsg *msg)
{
   /* Clear the protocol feature bit, which is useless for the device */
    vp_slave->feature_bits = msg->payload.u64
                             & ~(1 << VHOST_USER_F_PROTOCOL_FEATURES);
}

static int vp_slave_send_u64(int request, uint64_t u64)
{
    VhostUserMsg msg = {
        .request = request,
        .flags = VHOST_USER_VERSION,
        .payload.u64 = u64,
        .size = sizeof(msg.payload.u64),
    };

    if (vp_slave_write(&vp_slave->chr_be, &msg) < 0) {
        return -1;
    }

    return 0;
}

int vp_slave_send_feature_bits(uint64_t features)
{
    return vp_slave_send_u64(VHOST_USER_SET_FEATURES, features);
}

static DeviceState *virtio_to_pci_dev(VirtIODevice *vdev, uint16_t virtio_id)
{
    DeviceState *qdev = NULL;
    VhostPCINet *vpnet;
    VhostPCINetPCI *netpci;

    if (vdev == NULL)
        return NULL;

    switch (virtio_id) {
    case VIRTIO_ID_NET:
        vpnet = VHOST_PCI_NET(vdev);
        netpci = container_of(vpnet, VhostPCINetPCI, vdev);
        qdev = &netpci->parent_obj.pci_dev.qdev;
        break;
    default:
        error_report("virtio_to_pci_dev: device type %d not supported",
                     virtio_id);
    }

    return qdev;
}

static void vp_slave_device_del(VirtIODevice *vdev)
{
    Error *errp = NULL;
    DeviceState *qdev = virtio_to_pci_dev(vdev, vp_slave->dev_type);

    if (qdev != NULL) {
        qdev_unplug(qdev, &errp);
        vp_slave_cleanup();
    }
}

static void vp_slave_event(void *opaque, int event)
{
    switch (event) {
    case CHR_EVENT_OPENED:
        break;
    case CHR_EVENT_CLOSED:
        vp_slave_device_del(vp_slave->vdev);
        break;
    }
}

static int vp_slave_get_protocol_features(CharBackend *chr_be,
                                          VhostUserMsg *msg)
{
    msg->payload.u64 = VHOST_USER_PROTOCOL_FEATURES;
    msg->size = sizeof(msg->payload.u64);
    msg->flags |= VHOST_USER_REPLY_MASK;

    return vp_slave_write(chr_be, msg);
}

static void vp_slave_set_device_type(VhostUserMsg *msg)
{
    vp_slave->dev_type = (uint16_t)msg->payload.u64;

    switch (vp_slave->dev_type) {
    case VIRTIO_ID_NET:
        /* Don't initialize the feature bits if they have been negotiated */
        if (!vp_slave->peer_reset) {
            vp_slave->feature_bits |= (VHOST_PCI_FEATURE_BITS
                                       | VHOST_PCI_NET_FEATURE_BITS);
        }
        break;
    default:
        error_report("device type %d is not supported", vp_slave->dev_type);
    }
}

static int vp_slave_get_queue_num(CharBackend *chr_be, VhostUserMsg *msg)
{
    switch (vp_slave->dev_type) {
    case VIRTIO_ID_NET:
        msg->payload.u64 = VIRTIO_NET_CTRL_MQ_VQ_PAIRS_MAX;
        break;
    default:
        error_report("GET_QUEUE_NUM: device type %d is not supported",
                     vp_slave->dev_type);
        return -1;
    }
    msg->size = sizeof(msg->payload.u64);
    msg->flags |= VHOST_USER_REPLY_MASK;

    return vp_slave_write(chr_be, msg);
}

static uint64_t vp_slave_peer_mem_size_get(VhostUserMemory *pmem)
{
    int i;
    uint64_t total_size = 0;
    uint32_t nregions = pmem->nregions;
    VhostUserMemoryRegion *pmem_regions = pmem->regions;

    for (i = 0; i < nregions; i++) {
        total_size += pmem_regions[i].memory_size;
    }

    return total_size;
}

static int vp_slave_set_mem_table(VhostUserMsg *msg, int *fds, int fd_num)
{
    VhostUserMemory *pmem = &msg->payload.memory;
    VhostUserMemoryRegion *pmem_region = pmem->regions;
    uint32_t i, nregions = pmem->nregions;
    struct peer_mem_msg *pmem_msg = &vp_slave->pmem_msg;
    pmem_msg->nregions = nregions;
    MemoryRegion *bar_mr, *sub_mr;
    uint64_t bar_size, bar_map_offset = 0;
    void *mr_qva;

    /* Sanity Check */
    if (fd_num != nregions) {
        error_report("SET_MEM_TABLE: fd num doesn't match region num");
        return -1;
    }

    if (vp_slave->bar_mr == NULL) {
        vp_slave->bar_mr = g_malloc(sizeof(MemoryRegion));
    }
    if (vp_slave->sub_mr == NULL) {
        vp_slave->sub_mr = g_malloc(nregions * sizeof(MemoryRegion));
    }
    bar_mr = vp_slave->bar_mr;
    sub_mr = vp_slave->sub_mr;

    /*
     * The top half of the bar area holds the peer memory, and the bottom
     * half is reserved for memory hotplug
     */
    bar_size = 2 * vp_slave_peer_mem_size_get(pmem);
    bar_size = pow2ceil(bar_size);
    memory_region_init(bar_mr, NULL, "Peer Memory", bar_size);
    for (i = 0; i < nregions; i++) {
        vp_slave->mr_map_size[i] = pmem_region[i].memory_size
                                       + pmem_region[i].mmap_offset;
        vp_slave->mr_map_base[i] = mmap(NULL, vp_slave->mr_map_size[i],
                      PROT_READ | PROT_WRITE, MAP_SHARED, fds[i], 0);
        if (vp_slave->mr_map_base[i] == MAP_FAILED) {
            error_report("SET_MEM_TABLE: map peer memory region %d failed", i);
            return -1;
        }

        mr_qva = vp_slave->mr_map_base[i] + pmem_region[i].mmap_offset;
        memory_region_init_ram_ptr(&sub_mr[i], NULL, "Peer Memory",
                                   pmem_region[i].memory_size, mr_qva);
        memory_region_add_subregion(bar_mr, bar_map_offset, &sub_mr[i]);
        bar_map_offset += pmem_region[i].memory_size;
        pmem_msg->regions[i].gpa = pmem_region[i].guest_phys_addr;
        pmem_msg->regions[i].size = pmem_region[i].memory_size;
    }
    vp_slave->bar_map_offset = bar_map_offset;

    return 0;
}

static void vp_slave_alloc_pvq_node(void)
{
    PeerVqNode *pvq_node = g_malloc0(sizeof(PeerVqNode));
    QLIST_INSERT_HEAD(&vp_slave->pvq_list, pvq_node, node);
    vp_slave->pvq_num++;
}

static void vp_slave_set_vring_num(VhostUserMsg *msg)
{
    PeerVqNode *pvq_node = QLIST_FIRST(&vp_slave->pvq_list);

    pvq_node->vring_num = msg->payload.u64;
}

static void vp_slave_set_vring_base(VhostUserMsg *msg)
{
    PeerVqNode *pvq_node = QLIST_FIRST(&vp_slave->pvq_list);

    pvq_node->last_avail_idx = msg->payload.u64;
}

static int vp_slave_get_vring_base(CharBackend *chr_be, VhostUserMsg *msg)
{
    /* send back vring base to qemu */
    msg->flags |= VHOST_USER_REPLY_MASK;
    msg->size = sizeof(m.payload.state);
    msg->payload.state.num = 0;

    return vp_slave_write(chr_be, msg);
}

static void vp_slave_set_vring_addr(VhostUserMsg *msg)
{
    PeerVqNode *pvq_node = QLIST_FIRST(&vp_slave->pvq_list);
    memcpy(&pvq_node->addr, &msg->payload.addr,
           sizeof(struct vhost_vring_addr));
}

static void vp_slave_set_vring_kick(int fd)
{
    PeerVqNode *pvq_node = QLIST_FIRST(&vp_slave->pvq_list);
    if (!pvq_node)
        pvq_node->kickfd = fd;
}

static void vp_slave_set_vring_call(int fd)
{
    PeerVqNode *pvq_node = QLIST_FIRST(&vp_slave->pvq_list);
    if (pvq_node)
        pvq_node->callfd = fd;
}

static void vp_slave_set_vring_enable(VhostUserMsg *msg)
{
    struct vhost_vring_state *state = &msg->payload.state;
    PeerVqNode *pvq_node;
    QLIST_FOREACH(pvq_node, &vp_slave->pvq_list, node) {
        if (pvq_node->vring_num == state->index) {
            pvq_node->enabled = (int)state->num;
            break;
        }
    }
}

static int vp_slave_device_create(uint16_t virtio_id)
{
    Error *local_err = NULL;
    QemuOpts *opts;
    DeviceState *dev;
    char params[50];

    switch (virtio_id) {
    case VIRTIO_ID_NET:
        strcpy(params, "driver=vhost-pci-net-pci,id=vhost-pci-0");
        break;
    default:
        error_report("vhost-pci device create: device type %d not supported",
                     virtio_id);
    }

    opts = qemu_opts_parse_noisily(qemu_find_opts("device"), params, true);
    dev = qdev_device_add(opts, &local_err);
    if (!dev) {
        qemu_opts_del(opts);
        return -1;
    }
    object_unref(OBJECT(dev));
    return 0;
}

static int vp_slave_set_vhost_pci(CharBackend *chr_be, VhostUserMsg *msg)
{
    int ret;
    uint8_t cmd = (uint8_t)msg->payload.u64;

    switch (cmd) {
    case VHOST_USER_SET_VHOST_PCI_start:
        if (vp_slave->peer_reset) {
            vp_slave->peer_reset = 0;
        }
        ret = vp_slave_device_create(vp_slave->dev_type);
        if (ret < 0) {
            return ret;
        }
        break;
    case VHOST_USER_SET_VHOST_PCI_stop:
        vp_slave_device_del(vp_slave->vdev);
        ret = 0;
        break;
    default:
        error_report("slave pconnection: cmd %d not supported yet", cmd);
        return -1;
    }

    return ret;
}

static int vp_slave_can_read(void *opaque)
{
    return VHOST_USER_HDR_SIZE;
}

static void vp_slave_read(void *opaque, const uint8_t *buf, int size)
{
    int ret, fd_num, fds[MAX_GUEST_REGION];
    VhostUserMsg msg;
    uint8_t *p = (uint8_t *) &msg;
    CharBackend *chr_be = (CharBackend *)opaque;

    if (size != VHOST_USER_HDR_SIZE) {
        error_report("Wrong message size received %d", size);
        return;
    }

    memcpy(p, buf, VHOST_USER_HDR_SIZE);

    if (msg.size) {
        p += VHOST_USER_HDR_SIZE;
        size = qemu_chr_fe_read_all(chr_be, p, msg.size);
        if (size != msg.size) {
            error_report("Wrong message size received %d != %d",
                           size, msg.size);
            return;
        }
    }

    if (msg.request > VHOST_USER_MAX) {
        error_report("vhost-pci-slave read incorrect msg");
    }

    switch (msg.request) {
    case VHOST_USER_GET_FEATURES:
        ret = vp_slave_get_features(chr_be, &msg);
        if (ret < 0) {
            goto err_handling;
        }
        break;
    case VHOST_USER_SET_FEATURES:
        vp_slave_set_features(&msg);
        break;
    case VHOST_USER_GET_PROTOCOL_FEATURES:
        ret = vp_slave_get_protocol_features(chr_be, &msg);
        if (ret < 0) {
            goto err_handling;
        }
        break;
    case VHOST_USER_SET_PROTOCOL_FEATURES:
        break;
    case VHOST_USER_SET_DEVICE_ID:
        vp_slave_set_device_type(&msg);
        break;
    case VHOST_USER_GET_QUEUE_NUM:
        ret = vp_slave_get_queue_num(chr_be, &msg);
        if (ret < 0) {
            goto err_handling;
        }
        break;
    case VHOST_USER_SET_OWNER:
        break;
    case VHOST_USER_SET_MEM_TABLE:
	if (vp_slave->vdev)
		break;
        fd_num = qemu_chr_fe_get_msgfds(chr_be, fds, sizeof(fds) / sizeof(int));
        vp_slave_set_mem_table(&msg, fds, fd_num);
        break;
    case VHOST_USER_SET_VRING_NUM:
        vp_slave_alloc_pvq_node();
        vp_slave_set_vring_num(&msg);
        break;
    case VHOST_USER_SET_VRING_BASE:
        vp_slave_set_vring_base(&msg);
        break;
    case VHOST_USER_GET_VRING_BASE:
        ret = vp_slave_get_vring_base(chr_be, &msg);
        if (ret < 0) {
            goto err_handling;
        }
        break;
    case VHOST_USER_SET_VRING_ADDR:
        vp_slave_set_vring_addr(&msg);
        break;
    case VHOST_USER_SET_VRING_KICK:
        /* consume the fd */
        qemu_chr_fe_get_msgfds(chr_be, fds, 1);
        vp_slave_set_vring_kick(fds[0]);
        /*
         * This is a non-blocking eventfd.
         * The receive function forces it to be blocking,
         * so revert it back to non-blocking.
         */
        qemu_set_nonblock(fds[0]);
        break;
    case VHOST_USER_SET_VRING_CALL:
        /* consume the fd */
        qemu_chr_fe_get_msgfds(chr_be, fds, 1);
        vp_slave_set_vring_call(fds[0]);
        /*
         * This is a non-blocking eventfd.
         * The receive function forces it to be blocking,
         * so revert it back to non-blocking.
         */
        qemu_set_nonblock(fds[0]);
        break;
    case VHOST_USER_SET_VRING_ENABLE:
        vp_slave_set_vring_enable(&msg);
        break;
    case VHOST_USER_SET_LOG_BASE:
        break;
    case VHOST_USER_SET_LOG_FD:
        qemu_chr_fe_get_msgfds(chr_be, fds, 1);
        close(fds[0]);
        break;
    case VHOST_USER_SEND_RARP:
        break;
    case VHOST_USER_SET_VHOST_PCI:
        ret = vp_slave_set_vhost_pci(chr_be, &msg);
        if (ret < 0) {
            goto err_handling;
        }
        break;
    default:
        error_report("vhost-pci-slave does not support msg request = %d",
                     msg.request);
        break;
    }
    return;

err_handling:
    error_report("vhost-pci-slave handle request %d failed", msg.request);
}

static CharDriverState *vp_slave_parse_chardev(const char *id)
{
    CharDriverState *chr = qemu_chr_find(id);
    if (chr == NULL) {
        error_report("chardev \"%s\" not found", id);
        return NULL;
    }

    return chr;
}

int vhost_pci_slave_init(QemuOpts *opts)
{
    CharDriverState *chr;
    const char *chardev_id = qemu_opt_get(opts, "chardev");

    vp_slave = g_malloc(sizeof(VhostPCISlave));
    chr = vp_slave_parse_chardev(chardev_id);
    if (!chr) {
        return -1;
    }
    vp_slave->feature_bits =  1ULL << VHOST_USER_F_PROTOCOL_FEATURES;
    vp_slave->bar_mr = NULL;
    vp_slave->sub_mr = NULL;
    vp_slave->vdev = NULL;
    QLIST_INIT(&vp_slave->pvq_list);
    vp_slave->pvq_num = 0;
    vp_slave->peer_reset = 0;
    qemu_chr_fe_init(&vp_slave->chr_be, chr, &error_abort);
    qemu_chr_fe_set_handlers(&vp_slave->chr_be, vp_slave_can_read,
                             vp_slave_read, vp_slave_event,
                             &vp_slave->chr_be, NULL, true);

    return 0;
}

int vhost_pci_slave_cleanup(void)
{
    vp_slave_cleanup();
    qemu_chr_fe_deinit(&vp_slave->chr_be);
    g_free(vp_slave->sub_mr);
    g_free(vp_slave->bar_mr);
    g_free(vp_slave);

    return 0;
}
