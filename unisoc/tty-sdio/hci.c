/*
 * Copyright (C) 2016 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "include/sdio.h"
#include "include/debug.h"
#include "include/tty.h"
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/semaphore.h>
#include <linux/vmalloc.h>
#include "include/hci.h"
#include "include/sitm.h"

//#define assert(x) do{pr_err()}

extern struct bt_data_interface_t *bt_data_interface;

unsigned char own_public_addr[BD_ADDR_LEN];

unsigned char own_addr[BD_ADDR_LEN];

struct woble_dev_t
{
    unsigned char in_use;
    unsigned char type;
    unsigned short handler;
    unsigned char bd_addr[BD_ADDR_LEN];
};

static struct woble_dev_t woble_devices[WOBLE_DEVICES_SIZE];


static struct hci_cmd_t hci_cmd;

static int recv_kernel_event(const unsigned char *buf, int length, int pack_enum)
{
    unsigned short opcode = 0;
    unsigned char status = 0;
    const unsigned char *p = 0;
    int ret = -1;

    BT_DEBUG("%s +++\n", __func__);

    switch (pack_enum)
    {
    default:
    case BT_HCI_EVT_CMD_COMPLETE:
        p = buf + 4;
        STREAM_TO_UINT16(opcode, p);
        STREAM_TO_UINT8(status, p);
        break;

    case BT_HCI_EVT_CMD_STATUS:
        p = buf + 5;
        STREAM_TO_UINT16(opcode, p);
        status = buf[3];
        break;
    }

    BT_DEBUG("%s opcode: 0x%04X, status: %d\n", __func__, opcode, status);

    if (hci_cmd.opcode == opcode)
    {
        BT_DEBUG("%s hci_cmd.opcode %04X match, need up sem\n", __func__, opcode);
        up(&hci_cmd.wait);
        ret = 0;
    }

    if(opcode == 0x1009)
    {
        STREAM_TO_ARRAY(own_public_addr, p, BD_ADDR_LEN);
        pr_err("%s address %02x:%02x:%02x:%02x:%02x:%02x\n", __func__,
                own_public_addr[5], own_public_addr[4], own_public_addr[3], own_public_addr[2], own_public_addr[1], own_public_addr[0]);
    }

    BT_DEBUG("%s --- ret %d\n", __func__, ret);
    return ret;
}

int rx_data_recv(const unsigned char *buf, int count,
                 int (*upper_cb)(const unsigned char *buf, int count))
{
    const unsigned char *rxmsg = buf;
    int left_length = count;
    int pack_length = 0;
    int last = 0;

    if (count <= 0)
    {
        pr_err("%s count <0 !!!!!", __func__);
    }

    do
    {
        rxmsg = buf + (count - left_length);

        //pr_err("recv %d", left_length);
        //hex_dump_block(rxmsg, left_length);
        switch (rxmsg[PACKET_TYPE])
        {
        case HCI_EVT:
            if (left_length < 3)
            {
                pr_err("%s left_length <3 !!!!!", __func__);
            }

            pack_length = rxmsg[EVT_HEADER_SIZE];
            pack_length += 3;

            if (left_length - pack_length < 0)
            {
                pr_err("%s left_length - pack_length <0 !!!!!", __func__);

            }

            switch (rxmsg[EVT_HEADER_EVENT])
            {
            case BT_HCI_EVT_CMD_COMPLETE:
            case BT_HCI_EVT_CMD_STATUS:
                if (recv_kernel_event(rxmsg, pack_length, rxmsg[EVT_HEADER_EVENT]))
                {
                    upper_cb(rxmsg, pack_length);
                }

                break;

            default:
                upper_cb(rxmsg, pack_length);
                break;
            }

            last = left_length;
            left_length -= pack_length;
            break;


        case HCI_ACL:
            if (left_length < 5)
            {
                pr_err("%s left_length <5 !!!!!", __func__);
            }

            pack_length = rxmsg[ACL_HEADER_SIZE_HB];
            pack_length <<= 8;
            pack_length |= rxmsg[ACL_HEADER_SIZE_LB];
            pack_length += 5;

            if (left_length - pack_length < 0)
            {
                pr_err("%s left_length - pack_length <0 !!!!!", __func__);
            }

            upper_cb(rxmsg, pack_length);
            last = left_length;
            left_length -= pack_length;
            break;

        default:
            pr_err("%s unknown package left_length %d last %d data %x %x %x %x!!!!!\n", __func__, left_length, last, rxmsg[0], rxmsg[1], rxmsg[2], rxmsg[3]);

            if (0)
            {
                char dump[512] = {0};
                char hexx[4] = {0};
                int tmpi = 0;

                for (tmpi = 0; tmpi < left_length; tmpi++)
                {
                    memset(hexx, 0, sizeof(hexx));
                    sprintf(hexx, "%02x ", rxmsg[tmpi]);
                    strcat(dump, hexx);
                }

                pr_err("unknown package detail %s!!!\n", dump);

                mdbg_assert_interface("detect dirty data!!!!\n");
            }

            left_length = 0;
            break;
        }

        //pr_info("%s left: %d\n",__func__ , left_length);
    }
    while (left_length);

    return count;
}


int hci_cmd_send_sync(unsigned short opcode, struct HC_BT_HDR *py,
                      struct HC_BT_HDR *rsp)
{
    unsigned char msg_req[HCI_CMD_MAX_LEN + BYTE_ALIGNMENT] = {0}, *p;

    pr_info("%s max_len=%d+++\n", __func__, HCI_CMD_MAX_LEN);
    p = msg_req;
    UINT8_TO_STREAM(p, HCI_CMD);
    UINT16_TO_STREAM(p, opcode);

    if (py == NULL)
    {
        UINT8_TO_STREAM(p, 0);
    }
    else
    {
        UINT8_TO_STREAM(p, py->len);
        ARRAY_TO_STREAM(p, py->data, py->len);
    }

    hci_cmd.opcode = opcode;

    bt_data_interface->write(msg_req, p - msg_req + BYTE_ALIGNMENT - ((p - msg_req) % BYTE_ALIGNMENT));
    down(&hci_cmd.wait);
    hci_cmd.opcode = 0;

    pr_info("%s ---\n", __func__);
    return 0;
}


int del_woble_devices(unsigned char type, unsigned char *bd_addr)
{
    int i;

    for (i = 0; i < WOBLE_DEVICES_SIZE; ++i)
    {
        if (woble_devices[i].in_use && !memcmp(bd_addr, woble_devices[i].bd_addr, BD_ADDR_LEN)
                && woble_devices[i].type == type)
        {
            memset(woble_devices + i, 0, sizeof(woble_devices[i]));
        }
    }

    return 0;
}

int update_woble_devices(unsigned char type, unsigned short handler, unsigned char *bd_addr)
{
    int i;

    for (i = 0; i < WOBLE_DEVICES_SIZE; ++i)
    {
        if (woble_devices[i].in_use && !memcmp(bd_addr, woble_devices[i].bd_addr, BD_ADDR_LEN)
                && woble_devices[i].type == type)
        {

            woble_devices[i].in_use = 1;
            woble_devices[i].type = type;
            woble_devices[i].handler = handler;
            STREAM_TO_ARRAY(woble_devices[i].bd_addr, bd_addr, BD_ADDR_LEN);

            pr_info("%s set type: %d, handler 0x%X address: %02x:%02x:%02x:%02x:%02x:%02x\n",
                    __func__,
                    woble_devices[i].type,
                    woble_devices[i].handler,
                    woble_devices[i].bd_addr[0], woble_devices[i].bd_addr[1],
                    woble_devices[i].bd_addr[2], woble_devices[i].bd_addr[3],
                    woble_devices[i].bd_addr[4], woble_devices[i].bd_addr[5]);

            break;
        }
    }

    if (i < WOBLE_DEVICES_SIZE)
    {
        return 0;
    }

    for (i = 0; i < WOBLE_DEVICES_SIZE; i++)
    {
        if (woble_devices[i].in_use)
        {
            continue;
        }
        else
        {
            woble_devices[i].in_use = 1;
            woble_devices[i].type = type;
            woble_devices[i].handler = handler;
            STREAM_TO_ARRAY(woble_devices[i].bd_addr, bd_addr, BD_ADDR_LEN);

            pr_info("%s set type: %d, handler 0x%X address: %02x:%02x:%02x:%02x:%02x:%02x\n",
                    __func__,
                    woble_devices[i].type,
                    woble_devices[i].handler,
                    woble_devices[i].bd_addr[0], woble_devices[i].bd_addr[1],
                    woble_devices[i].bd_addr[2], woble_devices[i].bd_addr[3],
                    woble_devices[i].bd_addr[4], woble_devices[i].bd_addr[5]);
        }

        break;
    }

    if (i == WOBLE_DEVICES_SIZE)
    {
        pr_err("%s err, woble is full\n", __func__);
        return -1;
    }

    return 0;
}

void dump_woble_devices(void)
{
    int i;

    for (i = 0; i < WOBLE_DEVICES_SIZE; i++)
    {
        if (woble_devices[i].in_use)
        {
            pr_info("%s type: %d, handler 0x%x address: %02x:%02x:%02x:%02x:%02x:%02x\n",
                    __func__,
                    woble_devices[i].type, woble_devices[i].handler,
                    woble_devices[i].bd_addr[0],
                    woble_devices[i].bd_addr[1], woble_devices[i].bd_addr[2],
                    woble_devices[i].bd_addr[3], woble_devices[i].bd_addr[4],
                    woble_devices[i].bd_addr[5]);
        }
    }

    pr_info("%s own address: %02x:%02x:%02x:%02x:%02x:%02x\n", __func__,
            own_addr[0], own_addr[1], own_addr[2],
            own_addr[3], own_addr[4], own_addr[5]);
}


int clear_woble_devices(void)
{
    memset(&woble_devices, 0, sizeof(woble_devices));
    return 0;
}

int set_random_address(unsigned char *bd_addr)
{
    STREAM_TO_ARRAY(own_addr, bd_addr, BD_ADDR_LEN);
    return 0;
}


int hci_init(void)
{
    memset(&hci_cmd, 0, sizeof(struct hci_cmd_t));
    clear_woble_devices();
    sema_init(&hci_cmd.wait, 0);

    return 0;
}

int hci_destroy(void)
{
    //sema_destroy(&hci_cmd.wait);
    memset(&hci_cmd, 0, sizeof(struct hci_cmd_t));
    return 0;
}



void hci_woble_enable(void)
{
    struct HC_BT_HDR *payload = (struct HC_BT_HDR *)vmalloc(sizeof(struct HC_BT_HDR) + 3);
    unsigned char *p, *dp = 0;
    int i;
    struct HC_BT_HDR *disc_payload = (struct HC_BT_HDR *)vmalloc(sizeof(struct HC_BT_HDR) + 3);

    payload->len = 3;
    payload->data[0] = 0;
    payload->data[1] = 0;
    payload->data[2] = 1;

    hci_cmd_send_sync(BT_HCI_OP_ENABLE, payload, NULL);
    vfree(payload);

    hci_cmd_send_sync(BT_HCI_OP_RESET, NULL, NULL);

    payload = (struct HC_BT_HDR *)vmalloc(sizeof(struct HC_BT_HDR) + 100);
    p = payload->data;
    BDADDR_TO_STREAM(p, own_addr);
    UINT8_TO_STREAM(p, 0);


    for (i = 0; i < WOBLE_DEVICES_SIZE; i++)
    {
        if (woble_devices[i].in_use)
        {

            if (woble_devices[i].handler && woble_devices[i].handler != 0xffff)
            {
                disc_payload->len = 3;
                dp = disc_payload->data;
                UINT16_TO_STREAM(dp, woble_devices[i].handler);
                UINT8_TO_STREAM(dp, 0x13);
                hci_cmd_send_sync(0x0406, disc_payload, 0);
            }

            UINT8_TO_STREAM(p, woble_devices[i].type);
            BDADDR_TO_STREAM(p, woble_devices[i].bd_addr);
            payload->data[BD_ADDR_LEN]++;
            pr_info("%s type: %d, address: %02x:%02x:%02x:%02x:%02x:%02x\n",
                    __func__,
                    woble_devices[i].type, woble_devices[i].bd_addr[0],
                    woble_devices[i].bd_addr[1], woble_devices[i].bd_addr[2],
                    woble_devices[i].bd_addr[3], woble_devices[i].bd_addr[4],
                    woble_devices[i].bd_addr[5]);
        }
    }

    payload->len = p - payload->data;
    hex_dump_block(payload->data, payload->len);
    hci_cmd_send_sync(BT_HCI_OP_WOBLE, payload, NULL);
    vfree(payload);
    vfree(disc_payload);
}

void hci_cleanup(void)
{


}

const woble_config_t s_woble_config_cust = {WOBLE_MOD_ENABLE, WOBLE_SLEEP_MOD_COULD_NOT_KNOW, 0, WOBLE_SLEEP_MOD_NOT_NEED_NOTITY};

void hci_set_ap_sleep_mode(int is_shutdown, int is_resume)
{
    struct HC_BT_HDR *payload = (struct HC_BT_HDR *)vmalloc(sizeof(struct HC_BT_HDR) + 6);
    unsigned char *p;
    p = payload->data;

    pr_err("%s start\n", __func__);

    payload->len = 6;
    if (is_resume)
    {
        UINT8_TO_STREAM(p, WOBLE_MOD_DISABLE);
    }
    else
    {
        UINT8_TO_STREAM(p, WOBLE_MOD_ENABLE);
    }
    UINT8_TO_STREAM(p, 0);
    UINT16_TO_STREAM(p, s_woble_config_cust.timeout);
    UINT8_TO_STREAM(p, s_woble_config_cust.notify);
    UINT8_TO_STREAM(p, 0);
    hex_dump_block(payload->data, payload->len);
    hci_cmd_send_sync(BT_HCI_OP_SET_SLEEPMODE, payload, NULL);
    vfree(payload);
    pr_err("%s end\n", __func__);
    return;
}

void hci_set_scan_parameters(void)
{
    struct HC_BT_HDR *payload = (struct HC_BT_HDR *)vmalloc(sizeof(struct HC_BT_HDR) + 8);
    unsigned char *p;
    p = payload->data;

    pr_err("%s start\n", __func__);

    payload->len = 8;
    UINT8_TO_STREAM(p, 0x00);
    UINT8_TO_STREAM(p, 0x00);
    UINT8_TO_STREAM(p, 0x01);
    UINT8_TO_STREAM(p, 0x01);
    UINT8_TO_STREAM(p, 0x40);
    UINT8_TO_STREAM(p, 0x1f);
    UINT8_TO_STREAM(p, 0x40);
    UINT8_TO_STREAM(p, 0x1f);

    hex_dump_block(payload->data, payload->len);
    hci_cmd_send_sync(BT_HCI_OP_LE_SET_EX_SCAN_PARAMETERS, payload, NULL);
    vfree(payload);
    pr_err("%s end\n", __func__);
    return;
}

void hci_set_scan_enable(int enable)
{
    struct HC_BT_HDR *payload = (struct HC_BT_HDR *)vmalloc(sizeof(struct HC_BT_HDR) + 6);
    unsigned char *p;
    p = payload->data;

    pr_err("%s start\n", __func__);

    payload->len = 6;
    UINT8_TO_STREAM(p, enable);
    UINT8_TO_STREAM(p, 0x00);
    UINT8_TO_STREAM(p, 0x00);
    UINT8_TO_STREAM(p, 0x00);
    UINT8_TO_STREAM(p, 0x00);
    UINT8_TO_STREAM(p, 0x00);

    hex_dump_block(payload->data, payload->len);
    hci_cmd_send_sync(BT_HCI_OP_LE_SET_EX_SCAN_ENABLE, payload, NULL);
    vfree(payload);
    pr_err("%s end\n", __func__);
    return;
}

void hci_add_device_to_wakeup_list(void)
{
    struct HC_BT_HDR *payload = (struct HC_BT_HDR *)vmalloc(sizeof(struct HC_BT_HDR) + 34);
    unsigned char *p;
    unsigned char address[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    p = payload->data;

    pr_err("%s start\n", __func__);

    if (!strcmp(WOBLE_TYPE, "chuangwei"))
    {
        payload->len = 34;
        UINT8_TO_STREAM(p, 0xFF);
        ARRAY_TO_STREAM(p, address, 6);
        UINT16_TO_STREAM(p, 0x10);
        UINT8_TO_STREAM(p, 0x01);
        UINT8_TO_STREAM(p, 0x02);

        UINT8_TO_STREAM(p, 0x01);
        UINT8_TO_STREAM(p, 0xff);
        UINT8_TO_STREAM(p, 0x00);
        UINT8_TO_STREAM(p, 0x00);
        UINT8_TO_STREAM(p, 0x00);
        UINT8_TO_STREAM(p, 0x00);
        UINT8_TO_STREAM(p, 0x03);
        UINT8_TO_STREAM(p, 0x03);
        UINT8_TO_STREAM(p, 0x00);
        UINT8_TO_STREAM(p, 0x01);

        UINT8_TO_STREAM(p, 0x01);
        UINT8_TO_STREAM(p, 0xff);
        UINT8_TO_STREAM(p, 0x04);
        UINT8_TO_STREAM(p, 0x00);
        UINT8_TO_STREAM(p, 0x00);
        UINT8_TO_STREAM(p, 0x00);
        UINT8_TO_STREAM(p, 0x06);
        ARRAY_TO_STREAM(p, own_public_addr, 6);
    }
    else if (!strcmp(WOBLE_TYPE, "public"))
    {
        payload->len = 16;
        UINT8_TO_STREAM(p, 0xFF);
        ARRAY_TO_STREAM(p, address, 6);
        UINT16_TO_STREAM(p, 0x08);
        UINT8_TO_STREAM(p, 0x01);

        UINT8_TO_STREAM(p, 0x05);
        UINT8_TO_STREAM(p, 0x16);
        UINT8_TO_STREAM(p, UNISOC_WOBLE_UUID & 0xff);
        UINT8_TO_STREAM(p, (UNISOC_WOBLE_UUID >> 8) & 0xff);
        UINT8_TO_STREAM(p, 0x00);
        UINT8_TO_STREAM(p, 0x01);
    }
    hex_dump_block(payload->data, payload->len);
    hci_cmd_send_sync(BT_HCI_OP_ADD_WAKEUPLIST, payload, NULL);
    vfree(payload);
    pr_err("%s end\n", __func__);
    return;
}

void hci_set_ap_start_sleep(void)
{
    pr_err("%s start\n", __func__);

    hci_cmd_send_sync(BT_HCI_OP_SET_STARTSLEEP, NULL, NULL);
    pr_err("%s end\n", __func__);
    return;
}

