/* SocketCAN driver for Microchip CAN BUS Analyzer Tool
 *
 * Copyright (C) Mobica Limited
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published
 * by the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.
 *
 * This driver is inspired by the 4.6.2 version of drivers/net/can/usb/usb_8dev.c
 */

#include <linux/signal.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/usb.h>

#include <linux/can.h>
#include <linux/can/dev.h>
#include <linux/can/error.h>
#include <linux/can/led.h>
#include "mcba_usb.h"

/* table of devices that work with this driver */
static const struct usb_device_id mcba_usb_table[] = {
    { USB_DEVICE(MCBA_VENDOR_ID, MCBA_PRODUCT_ID) },
    { }                 /* Terminating entry */
};

MODULE_DEVICE_TABLE(usb, mcba_usb_table);

int debug = 0;
module_param(debug, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
MODULE_PARM_DESC(debug, "Debug USB and/or CAN PICs");

/* Required by can-dev but not for the sake of driver as CANBUS is USB based */
static const struct can_bittiming_const mcba_bittiming_const = {
        .name = "mcba_usb",
        .tseg1_min = 1,
        .tseg1_max = 8,
        .tseg2_min = 1,
        .tseg2_max = 8,
        .sjw_max = 4,
        .brp_min = 2,
        .brp_max = 128,
        .brp_inc = 2,
};

static ssize_t termination_show(struct device *dev,
                                struct device_attribute *attr, char *buf)
{
    struct net_device *netdev = to_net_dev(dev);
    struct mcba_priv *priv = netdev_priv(netdev);

    return sprintf(buf, "%hhu\n", priv->termination_state);
}

static ssize_t termination_store(struct device *dev,
                                 struct device_attribute *attr,
                                 const char *buf, size_t count)
{
    struct net_device *netdev = to_net_dev(dev);
    struct mcba_priv *priv = netdev_priv(netdev);
    u8 tmp_termination = 0;

    sscanf(buf, "%hhu", &tmp_termination);

    if((0 == tmp_termination) || (1 == tmp_termination))
    {
        priv->termination_state = tmp_termination;
        mcba_usb_xmit_termination(priv, priv->termination_state);
    }

    return count;
}


static struct device_attribute termination_attr = {
    .attr = {
        .name = "termination",
        .mode = 0666 },
    .show	= termination_show,
    .store	= termination_store
};

static void mcba_usb_process_can(struct mcba_priv *priv,
                                 struct mcba_usb_msg_can *msg)
{
    struct can_frame *cf;
    struct sk_buff *skb;
    struct net_device_stats *stats = &priv->netdev->stats;

    skb = alloc_can_skb(priv->netdev, &cf);
    if (!skb)
        return;

    if(MCBA_RX_IS_EXID(msg))
        cf->can_id = MCBA_CAN_GET_EID(msg);
    else
        cf->can_id = MCBA_CAN_GET_SID(msg);

    if(MCBA_RX_IS_RTR(msg))
        cf->can_id |= MCBA_CAN_RTR_MASK;

    cf->can_dlc = msg->dlc & MCBA_DLC_MASK;

    memcpy(cf->data, msg->data, cf->can_dlc);

    stats->rx_packets++;
    stats->rx_bytes += cf->can_dlc;
    netif_rx(skb);
}

static void mcba_usb_process_keep_alive_usb(struct mcba_priv *priv,
                                        struct mcba_usb_msg_keep_alive_usb *msg)
{
    if(MCBA_IS_USB_DEBUG())
    {
        netdev_info(priv->netdev,
                    "USB_KA: termination %hhu, ver_maj %hhu, soft_min %hhu\n",
                    msg->termination_state, msg->soft_ver_major,
                    msg->soft_ver_minor);
    }

    if((MCBA_VER_UNDEFINED == priv->pic_usb_sw_ver_major) &&
       (MCBA_VER_UNDEFINED == priv->pic_usb_sw_ver_minor))
    {
        netdev_info(priv->netdev, "PIC USB version %hhu.%hhu\n",
                    msg->soft_ver_major, msg->soft_ver_minor);

        if(!(MCBA_VER_USB_MAJOR == msg->soft_ver_major) &&
            (MCBA_VER_USB_MINOR == msg->soft_ver_minor))
        {
            netdev_warn(priv->netdev,
                       "Driver tested against PIC USB %hhu.%hhu version only\n",
                        MCBA_VER_USB_MAJOR, MCBA_VER_USB_MINOR);
        }
    }

    priv->pic_usb_sw_ver_major = msg->soft_ver_major;
    priv->pic_usb_sw_ver_minor = msg->soft_ver_minor;
    priv->termination_state = msg->termination_state;
}

static void mcba_usb_process_keep_alive_can(struct mcba_priv *priv,
                                        struct mcba_usb_msg_keep_alive_can *msg)
{
    if(MCBA_IS_CAN_DEBUG())
    {
        netdev_info(priv->netdev,
                    "CAN_KA: tx_err_cnt %hhu, rx_err_cnt %hhu, "
                    "rx_buff_ovfl %hhu, tx_bus_off %hhu, can_bitrate %hu, "
                    "rx_lost %hu, can_stat %hhu, soft_ver %hhu.%hhu, "
                    "debug_mode %hhu, test_complete %hhu, test_result %hhu\n",
                    msg->tx_err_cnt, msg->rx_err_cnt, msg->rx_buff_ovfl,
                    msg->tx_bus_off,
                    ((msg->can_bitrate_hi << 8) + msg->can_bitrate_lo),
                    ((msg->rx_lost_hi >> 8) + msg->rx_lost_lo),
                    msg->can_stat, msg->soft_ver_major, msg->soft_ver_minor,
                    msg->debug_mode, msg->test_complete, msg->test_result);
    }

    if((MCBA_VER_UNDEFINED == priv->pic_can_sw_ver_major) &&
       (MCBA_VER_UNDEFINED == priv->pic_can_sw_ver_minor))
    {
        netdev_info(priv->netdev,
                    "PIC CAN version %hhu.%hhu\n",
                    msg->soft_ver_major, msg->soft_ver_minor);

        if(!(MCBA_VER_CAN_MAJOR == msg->soft_ver_major) &&
            (MCBA_VER_CAN_MINOR == msg->soft_ver_minor))
        {
            netdev_warn(priv->netdev,
                       "Driver tested against PIC CAN %hhu.%hhu version only\n",
                        MCBA_VER_CAN_MAJOR, MCBA_VER_CAN_MINOR);
        }
    }

    priv->bec.txerr = msg->tx_err_cnt;
    priv->bec.rxerr = msg->rx_err_cnt;

    priv->pic_can_sw_ver_major = msg->soft_ver_major;
    priv->pic_can_sw_ver_minor = msg->soft_ver_minor;
}

static void mcba_usb_process_rx(struct mcba_priv *priv,
                                struct mcba_usb_msg *msg)
{
    switch(msg->cmdId)
    {
    case MBCA_CMD_I_AM_ALIVE_FROM_CAN:
        mcba_usb_process_keep_alive_can(priv,
                                     (struct mcba_usb_msg_keep_alive_can *)msg);
        break;

    case MBCA_CMD_I_AM_ALIVE_FROM_USB:
        mcba_usb_process_keep_alive_usb(priv,
                                     (struct mcba_usb_msg_keep_alive_usb *)msg);
        break;

    case MBCA_CMD_RECEIVE_MESSAGE:
        mcba_usb_process_can(priv, (struct mcba_usb_msg_can *)msg);
        break;

    case MBCA_CMD_NOTHING_TO_SEND:
        /* Side effect of communication between PIC_USB and PIC_CAN.
         * PIC_CAN is telling us that it has nothing to send
        */
        break;

    case MBCA_CMD_TRANSMIT_MESSAGE_RSP:
        /* Transmission response from the device containing timestamp */
        break;

    default:
        netdev_warn(priv->netdev, "Unsupported msg (0x%hhX)", msg->cmdId);
        break;
    }
}


/* Callback for reading data from device
 *
 * Check urb status, call read function and resubmit urb read operation.
 */
static void mcba_usb_read_bulk_callback(struct urb *urb)
{
    struct mcba_priv *priv = urb->context;
    struct net_device *netdev;
    int retval;
    int pos = 0;

    netdev = priv->netdev;

    if (!netif_device_present(netdev))
            return;

    switch (urb->status) {
    case 0: /* success */
        break;

    case -ENOENT:
    case -ESHUTDOWN:
        return;

    default:
        netdev_info(netdev, "Rx URB aborted (%d)\n",
                 urb->status);

        goto resubmit_urb;
    }

    while (pos < urb->actual_length) {
        struct mcba_usb_msg *msg;

        if (pos + sizeof(struct mcba_usb_msg) > urb->actual_length) {
            netdev_err(priv->netdev, "format error\n");
            break;
        }

        msg = (struct mcba_usb_msg *)(urb->transfer_buffer + pos);
        mcba_usb_process_rx(priv, msg);

        pos += sizeof(struct mcba_usb_msg);
    }

resubmit_urb:

    usb_fill_bulk_urb(urb, priv->udev,
                      usb_rcvbulkpipe(priv->udev, MCBA_USB_EP_OUT),
                      urb->transfer_buffer, MCBA_USB_RX_BUFF_SIZE,
                      mcba_usb_read_bulk_callback, priv);

    retval = usb_submit_urb(urb, GFP_ATOMIC);

    if (retval == -ENODEV)
        netif_device_detach(netdev);
    else if (retval)
        netdev_err(netdev, "failed resubmitting read bulk urb: %d\n", retval);
}

/* Start USB device */
static int mcba_usb_start(struct mcba_priv *priv)
{
    struct net_device *netdev = priv->netdev;
    int err, i;

    for (i = 0; i < MCBA_MAX_RX_URBS; i++) {
        struct urb *urb = NULL;
        u8 *buf;

        /* create a URB, and a buffer for it */
        urb = usb_alloc_urb(0, GFP_KERNEL);
        if (!urb) {
                netdev_err(netdev, "No memory left for URBs\n");
                err = -ENOMEM;
                break;
        }

        buf = usb_alloc_coherent(priv->udev, MCBA_USB_RX_BUFF_SIZE, GFP_KERNEL,
                                 &urb->transfer_dma);
        if (!buf) {
                netdev_err(netdev, "No memory left for USB buffer\n");
                usb_free_urb(urb);
                err = -ENOMEM;
                break;
        }

        usb_fill_bulk_urb(urb, priv->udev,
                          usb_rcvbulkpipe(priv->udev,
                                          MCBA_USB_EP_IN),
                          buf, MCBA_USB_RX_BUFF_SIZE,
                          mcba_usb_read_bulk_callback, priv);
        urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
        usb_anchor_urb(urb, &priv->rx_submitted);

        err = usb_submit_urb(urb, GFP_KERNEL);
        if (err) {
                usb_unanchor_urb(urb);
                usb_free_coherent(priv->udev, MCBA_USB_RX_BUFF_SIZE, buf,
                                  urb->transfer_dma);
                usb_free_urb(urb);
                break;
        }

        /* Drop reference, USB core will take care of freeing it */
        usb_free_urb(urb);
    }

    /* Did we submit any URBs */
    if (i == 0) {
            netdev_warn(netdev, "couldn't setup read URBs\n");
            return err;
    }

    /* Warn if we've couldn't transmit all the URBs */
    if (i < MCBA_MAX_RX_URBS)
            netdev_warn(netdev, "rx performance may be slow\n");

    priv->can.state = CAN_STATE_ERROR_ACTIVE;

    mcba_init_ctx(priv);
    mcba_usb_xmit_read_fw_ver(priv, MCBA_VER_REQ_USB);
    mcba_usb_xmit_read_fw_ver(priv, MCBA_VER_REQ_CAN);

    return err;
}



static inline void mcba_init_ctx(struct mcba_priv *priv)
{
    int i = 0;

    for (i = 0; i < MCBA_MAX_TX_URBS; i++) {
        priv->tx_context[i].ndx = MCBA_CTX_FREE;
    }
}


static inline struct mcba_usb_ctx *mcba_usb_get_free_ctx(struct mcba_priv *priv)
{
    int i = 0;
    struct mcba_usb_ctx *ctx = 0;

    for (i = 0; i < MCBA_MAX_TX_URBS; i++) {
        if (priv->tx_context[i].ndx == MCBA_CTX_FREE) {
            ctx = &priv->tx_context[i];
            ctx->ndx = i;
            ctx->priv = priv;
            break;
        }
    }

    return ctx;
}


static inline void mcba_usb_free_ctx(struct mcba_usb_ctx *ctx)
{
    ctx->ndx = MCBA_CTX_FREE;
    ctx->priv = 0;
    ctx->dlc = 0;
    ctx->can = false;
}

static void mcba_usb_write_bulk_callback(struct urb *urb)
{
    struct mcba_usb_ctx *ctx = urb->context;
    struct net_device *netdev;

    BUG_ON(!ctx);

    netdev = ctx->priv->netdev;

    if(ctx->can) {
        if (!netif_device_present(netdev))
            return;

        netdev->stats.tx_packets++;
        netdev->stats.tx_bytes += ctx->dlc;

        can_get_echo_skb(netdev, ctx->ndx);

        netif_wake_queue(netdev);
    }

    /* free up our allocated buffer */
    usb_free_coherent(urb->dev, urb->transfer_buffer_length,
              urb->transfer_buffer, urb->transfer_dma);

    if (urb->status)
        netdev_info(netdev, "Tx URB aborted (%d)\n",
             urb->status);

    /* Release context */
    mcba_usb_free_ctx(ctx);
}

/* Send data to device */
static netdev_tx_t mcba_usb_start_xmit(struct sk_buff *skb,
                      struct net_device *netdev)
{
    struct mcba_priv *priv = netdev_priv(netdev);
    struct can_frame *cf = (struct can_frame *) skb->data;
    struct mcba_usb_msg_can usb_msg;

    usb_msg.cmdId = MBCA_CMD_TRANSMIT_MESSAGE_EV;
    memcpy(usb_msg.data, cf->data, sizeof(usb_msg.data));

    if(MCBA_TX_IS_EXID(cf)) {
        usb_msg.sidl = MCBA_SET_E_SIDL(cf->can_id);
        usb_msg.sidh = MCBA_SET_E_SIDH(cf->can_id);
        usb_msg.eidl = MCBA_SET_EIDL(cf->can_id);
        usb_msg.eidh = MCBA_SET_EIDH(cf->can_id);
    }
    else {
        usb_msg.sidl = MCBA_SET_S_SIDL(cf->can_id);
        usb_msg.sidh = MCBA_SET_S_SIDH(cf->can_id);
        usb_msg.eidl = 0;
        usb_msg.eidh = 0;
    }

    usb_msg.dlc = cf->can_dlc;

    if(MCBA_TX_IS_RTR(cf))
        usb_msg.dlc |= MCBA_DLC_RTR_MASK;

    return mcba_usb_xmit(priv, (struct mcba_usb_msg *)&usb_msg, skb);
}

/* Send data to device */
static void mcba_usb_xmit_cmd(struct mcba_priv *priv,
                              struct mcba_usb_msg *usb_msg)
{
    mcba_usb_xmit(priv, usb_msg, 0);
}

/* Send data to device */
static netdev_tx_t mcba_usb_xmit(struct mcba_priv *priv,
                                 struct mcba_usb_msg *usb_msg,
                                 struct sk_buff *skb)
{
    struct net_device_stats *stats = &priv->netdev->stats;
    struct mcba_usb_ctx *ctx = 0;
    struct urb *urb;
    u8 *buf;
    int err;

    ctx = mcba_usb_get_free_ctx(priv);
    if(!ctx){
        /* Slow down tx path */
        netif_stop_queue(priv->netdev);

        return NETDEV_TX_BUSY;
    }

    if(skb) {
        ctx->dlc = ((struct mcba_usb_msg_can *)usb_msg)->dlc;
        can_put_echo_skb(skb, priv->netdev, ctx->ndx);
        ctx->can = true;
    }
    else
        ctx->can = false;

    /* create a URB, and a buffer for it, and copy the data to the URB */
    urb = usb_alloc_urb(0, GFP_ATOMIC);
    if (!urb) {
        netdev_err(priv->netdev, "No memory left for URBs\n");
        goto nomem;
    }

    buf = usb_alloc_coherent(priv->udev, MCBA_USB_TX_BUFF_SIZE, GFP_ATOMIC,
                 &urb->transfer_dma);
    if (!buf) {
        netdev_err(priv->netdev, "No memory left for USB buffer\n");
        goto nomembuf;
    }

    memcpy(buf, usb_msg, MCBA_USB_TX_BUFF_SIZE);

    usb_fill_bulk_urb(urb, priv->udev,
              usb_sndbulkpipe(priv->udev, MCBA_USB_EP_OUT),
              buf, MCBA_USB_TX_BUFF_SIZE, mcba_usb_write_bulk_callback, ctx);
    urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
    usb_anchor_urb(urb, &priv->tx_submitted);

    err = usb_submit_urb(urb, GFP_ATOMIC);
    if (unlikely(err))
        goto failed;

    /* Release our reference to this URB, the USB core will eventually free
     * it entirely.
     */
    usb_free_urb(urb);

    return NETDEV_TX_OK;

failed:
    usb_unanchor_urb(urb);
    usb_free_coherent(priv->udev, MCBA_USB_TX_BUFF_SIZE, buf,
                      urb->transfer_dma);

    if (err == -ENODEV)
        netif_device_detach(priv->netdev);
    else
        netdev_warn(priv->netdev, "failed tx_urb %d\n", err);

nomembuf:
    usb_free_urb(urb);

nomem:
    can_free_echo_skb(priv->netdev, ctx->ndx);
    dev_kfree_skb(skb);
    stats->tx_dropped++;

    return NETDEV_TX_OK;
}

static void mcba_usb_xmit_change_bitrate(struct mcba_priv *priv, u16 bitrate)
{
    struct mcba_usb_msg_change_bitrate usb_msg;

    usb_msg.cmd_id =  MBCA_CMD_CHANGE_BIT_RATE;
    usb_msg.bitrate_hi = (0xff00 & bitrate) >> 8;
    usb_msg.bitrate_lo = (0xff & bitrate);

    mcba_usb_xmit_cmd(priv, (struct mcba_usb_msg *)&usb_msg);
}

static void mcba_usb_xmit_read_fw_ver(struct mcba_priv *priv, u8 pic)
{
    struct mcba_usb_msg_fw_ver usb_msg;

    usb_msg.cmdId = MBCA_CMD_READ_FW_VERSION;
    usb_msg.pic = pic;

    mcba_usb_xmit_cmd(priv, (struct mcba_usb_msg *)&usb_msg);
}

static void mcba_usb_xmit_termination(struct mcba_priv *priv, u8 termination)
{
    struct mcba_usb_msg_terminaton usb_msg;

    usb_msg.cmdId = MBCA_CMD_SETUP_TERMINATION_RESISTANCE;
    usb_msg.termination = termination;

    mcba_usb_xmit_cmd(priv, (struct mcba_usb_msg *)&usb_msg);
}

/* Open USB device */
static int mcba_usb_open(struct net_device *netdev)
{
    int err;

    /* common open */
    err = open_candev(netdev);
    if (err)
        return err;

    can_led_event(netdev, CAN_LED_EVENT_OPEN);

    netif_start_queue(netdev);

    return 0;
}

static void mcba_urb_unlink(struct mcba_priv *priv)
{
    usb_kill_anchored_urbs(&priv->rx_submitted);

    usb_kill_anchored_urbs(&priv->tx_submitted);
}

/* Close USB device */
static int mcba_usb_close(struct net_device *netdev)
{
    struct mcba_priv *priv = netdev_priv(netdev);
    int err = 0;

    priv->can.state = CAN_STATE_STOPPED;

    netif_stop_queue(netdev);

    /* Stop polling */
    mcba_urb_unlink(priv);

    close_candev(netdev);

    can_led_event(netdev, CAN_LED_EVENT_STOP);

    return err;
}

/* Set network device mode
 *
 * Maybe we should leave this function empty, because the device
 * set mode variable with open command.
 */
static int mcba_net_set_mode(struct net_device *netdev, enum can_mode mode)
{
//        struct mcba_priv *priv = netdev_priv(netdev);
        int err = 0;

        switch (mode) {
        case CAN_MODE_START:
//                err = usb_8dev_cmd_open(priv);
//                if (err)
//                        netdev_warn(netdev, "couldn't start device");
//                break;

        default:
                return -EOPNOTSUPP;
        }

        return err;
}

static int mcba_net_get_berr_counter(const struct net_device *netdev,
                                     struct can_berr_counter *bec)
{
    struct mcba_priv *priv = netdev_priv(netdev);

    bec->txerr = priv->bec.txerr;
    bec->rxerr = priv->bec.rxerr;

    return 0;
}

static const struct net_device_ops mcba_netdev_ops = {
    .ndo_open = mcba_usb_open,
    .ndo_stop = mcba_usb_close,
    .ndo_start_xmit = mcba_usb_start_xmit,
//    .ndo_change_mtu = can_change_mtu,
};

static void mcba_net_calc_bittiming(u32 sjw, u32 prop, u32 seg1,
                                    u32 seg2, u32 brp, struct can_bittiming *bt)
{
    bt->sjw = sjw;
    bt->prop_seg = prop;
    bt->phase_seg1 = seg1;
    bt->phase_seg2 = seg2;
    bt->brp = brp;
    /* nanoseconds expected */
    bt->tq = (bt->brp * 1000)/(MCBA_CAN_CLOCK/1000000);
    bt->bitrate = 1000000000/((bt->sjw + bt->prop_seg +
                               bt->phase_seg1 + bt->phase_seg2)*bt->tq);
    bt->sample_point = ((bt->sjw + bt->prop_seg + bt->phase_seg1)*1000)/
            (bt->sjw + bt->prop_seg + bt->phase_seg1 + bt->phase_seg2);
}

/* Microchip CANBUS has hardcoded bittiming values by default.
 * This fucntion sends request via USB to change the speed and align bittiming
 * values for presentation purposes only */
static int mcba_net_set_bittiming(struct net_device *netdev)
{
    struct mcba_priv *priv = netdev_priv(netdev);
    struct can_bittiming *bt = &priv->can.bittiming;

    switch(bt->bitrate)
    {
    case MCBA_BITRATE_20_KBPS_40MHZ:
        /* bittiming aligned with default Microchip CANBUS firmware */
        mcba_net_calc_bittiming(1, 5, 8, 6, 100, bt);
        mcba_usb_xmit_change_bitrate(priv, 20);
        break;

    case MCBA_BITRATE_33_3KBPS_40MHZ:
        /* bittiming aligned with default Microchip CANBUS firmware */
        mcba_net_calc_bittiming(1, 8, 8, 8, 48, bt);
        mcba_usb_xmit_change_bitrate(priv, 33);
        break;

    case MCBA_BITRATE_50KBPS_40MHZ:
        /* bittiming aligned with default Microchip CANBUS firmware */
        mcba_net_calc_bittiming(1, 8, 7, 4, 40, bt);
        mcba_usb_xmit_change_bitrate(priv, 50);
        break;

    case MCBA_BITRATE_80KBPS_40MHZ:
        /* bittiming aligned with default Microchip CANBUS firmware */
        mcba_net_calc_bittiming(1, 8, 8, 8, 20, bt);
        mcba_usb_xmit_change_bitrate(priv, 80);
        break;

    case MCBA_BITRATE_83_3KBPS_40MHZ:
        /* bittiming aligned with default Microchip CANBUS firmware */
        mcba_net_calc_bittiming(1, 8, 8, 7, 20, bt);
        mcba_usb_xmit_change_bitrate(priv, 83);
        break;

    case MCBA_BITRATE_100KBPS_40MHZ:
        /* bittiming aligned with default Microchip CANBUS firmware */
        mcba_net_calc_bittiming(1, 1, 5, 3, 40, bt);
        mcba_usb_xmit_change_bitrate(priv, 100);
        break;

    case MCBA_BITRATE_125KBPS_40MHZ:
        /* bittiming aligned with default Microchip CANBUS firmware */
        mcba_net_calc_bittiming(1, 3, 8, 8, 16, bt);
        mcba_usb_xmit_change_bitrate(priv, 125);
        break;

    case MCBA_BITRATE_150KBPS_40MHZ:
        /* bittiming aligned with default Microchip CANBUS firmware */
        mcba_net_calc_bittiming(1, 8, 6, 4, 14, bt);
        mcba_usb_xmit_change_bitrate(priv, 150);
        break;

    case MCBA_BITRATE_175KBPS_40MHZ:
        /* bittiming aligned with default Microchip CANBUS firmware */
        mcba_net_calc_bittiming(1, 8, 6, 4, 12, bt);
        mcba_usb_xmit_change_bitrate(priv, 175);
        break;

    case MCBA_BITRATE_200KBPS_40MHZ:
        /* bittiming aligned with default Microchip CANBUS firmware */
        mcba_net_calc_bittiming(1, 8, 8, 8, 8, bt);
        mcba_usb_xmit_change_bitrate(priv, 200);
        break;

    case MCBA_BITRATE_225KBPS_40MHZ:
        /* bittiming aligned with default Microchip CANBUS firmware */
        mcba_net_calc_bittiming(1, 8, 8, 5, 8, bt);
        mcba_usb_xmit_change_bitrate(priv, 225);
        break;

    case MCBA_BITRATE_250KBPS_40MHZ:
        /* bittiming aligned with default Microchip CANBUS firmware */
        mcba_net_calc_bittiming(1, 3, 8, 8, 8, bt);
        mcba_usb_xmit_change_bitrate(priv, 250);
        break;

    case MCBA_BITRATE_275KBPS_40MHZ:
        /* bittiming aligned with default Microchip CANBUS firmware */
        mcba_net_calc_bittiming(1, 8, 8, 7, 6, bt);
        mcba_usb_xmit_change_bitrate(priv, 275);
        break;

    case MCBA_BITRATE_300KBPS_40MHZ:
        /* bittiming aligned with default Microchip CANBUS firmware */
        mcba_net_calc_bittiming(1, 8, 8, 5, 6, bt);
        mcba_usb_xmit_change_bitrate(priv, 300);
        break;

    case MCBA_BITRATE_500KBPS_40MHZ:
        /* bittiming aligned with default Microchip CANBUS firmware */
        mcba_net_calc_bittiming(1, 3, 8, 8, 4, bt);
        mcba_usb_xmit_change_bitrate(priv, 500);
        break;

    case MCBA_BITRATE_625KBPS_40MHZ:
        /* bittiming aligned with default Microchip CANBUS firmware */
        mcba_net_calc_bittiming(1, 1, 4, 2, 8, bt);
        mcba_usb_xmit_change_bitrate(priv, 625);
        break;

    case MCBA_BITRATE_800KBPS_40MHZ:
        /* bittiming aligned with default Microchip CANBUS firmware */
        mcba_net_calc_bittiming(1, 8, 8, 8, 2, bt);
        mcba_usb_xmit_change_bitrate(priv, 800);
        break;

    case MCBA_BITRATE_1000KBPS_40MHZ:
        /* bittiming aligned with default Microchip CANBUS firmware */
        mcba_net_calc_bittiming(1, 3, 8, 8, 2, bt);
        mcba_usb_xmit_change_bitrate(priv, 1000);
        break;

    default:
        netdev_err(netdev, "Unsupported bittrate (%u). Use one of: 20000, "
                   "33333, 50000, 80000, 83333, 100000, 125000, 150000, "
                   "175000, 200000, 225000, 250000, 275000, 300000, 500000, "
                   "625000, 800000, 1000000\n", bt->bitrate);

        return -EINVAL;
    }

    return 0;
}

static int mcba_usb_probe(struct usb_interface *intf,
                          const struct usb_device_id *id)
{
    struct net_device *netdev;
    struct mcba_priv *priv;
    int err = -ENOMEM;
    struct usb_device *usbdev = interface_to_usbdev(intf);
    dev_info(&intf->dev, "%s: Microchip CAN BUS analizer connected\n",
             MCBA_MODULE_NAME);

    netdev = alloc_candev(sizeof(struct mcba_priv), MCBA_MAX_TX_URBS);
    if (!netdev) {
        dev_err(&intf->dev, "Couldn't alloc candev\n");
        return -ENOMEM;
    }

    priv = netdev_priv(netdev);

    priv->udev = usbdev;
    priv->netdev = netdev;

    /* Init USB device */
    priv->pic_can_sw_ver_major = MCBA_VER_UNDEFINED;
    priv->pic_can_sw_ver_minor = MCBA_VER_UNDEFINED;
    priv->pic_usb_sw_ver_major = MCBA_VER_UNDEFINED;
    priv->pic_usb_sw_ver_minor = MCBA_VER_UNDEFINED;

    init_usb_anchor(&priv->rx_submitted);
    init_usb_anchor(&priv->tx_submitted);

    usb_set_intfdata(intf, priv);

    err = mcba_usb_start(priv);
    if (err) {
        if (err == -ENODEV)
            netif_device_detach(priv->netdev);

        netdev_warn(netdev, "couldn't start device: %d\n", err);

        goto cleanup_candev;
    }

    /* Init CAN device */
    priv->can.state = CAN_STATE_STOPPED;
    priv->can.clock.freq = MCBA_CAN_CLOCK;
    priv->can.bittiming_const = &mcba_bittiming_const;
    priv->can.do_set_mode = mcba_net_set_mode;
    priv->can.do_get_berr_counter = mcba_net_get_berr_counter;
    priv->can.do_set_bittiming = mcba_net_set_bittiming;
    priv->can.ctrlmode_supported = CAN_CTRLMODE_LOOPBACK |
                      CAN_CTRLMODE_LISTENONLY |
                      CAN_CTRLMODE_ONE_SHOT;

    netdev->netdev_ops = &mcba_netdev_ops;

    netdev->flags |= IFF_ECHO; /* we support local echo */

    SET_NETDEV_DEV(netdev, &intf->dev);

    err = register_candev(netdev);
    if (err) {
        netdev_err(netdev,
            "couldn't register CAN device: %d\n", err);
        goto cleanup_candev;
    }

    err = device_create_file(&netdev->dev, &termination_attr);
    if (err)
        goto cleanup_candev;

    return err;

cleanup_candev:
    free_candev(netdev);

    return err;
}

/* Called by the usb core when driver is unloaded or device is removed */
static void mcba_usb_disconnect(struct usb_interface *intf)
{
    struct mcba_priv *priv = usb_get_intfdata(intf);

    device_remove_file(&priv->netdev->dev, &termination_attr);

    usb_set_intfdata(intf, NULL);

    if (priv) {
        netdev_info(priv->netdev, "device disconnected\n");

        unregister_netdev(priv->netdev);
        free_candev(priv->netdev);

        mcba_urb_unlink(priv);
    }
}

static struct usb_driver mcba_usb_driver = {
        .name =		MCBA_MODULE_NAME,
        .probe =	mcba_usb_probe,
        .disconnect =	mcba_usb_disconnect,
        .id_table =	mcba_usb_table,
};

module_usb_driver(mcba_usb_driver);

MODULE_AUTHOR("Remigiusz Kołłątaj <remigiusz.kollataj@mobica.com>");
MODULE_DESCRIPTION("SocketCAN driver for Microchip CAN BUS Analyzer Tool");
MODULE_LICENSE("GPL v2");
