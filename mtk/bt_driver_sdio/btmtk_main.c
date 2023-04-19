/*
 *  Copyright (c) 2016 MediaTek Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *  See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/of.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>
#if (KERNEL_VERSION(4, 14, 0) > LINUX_VERSION_CODE)
#include <linux/sched.h>
#else
#include <uapi/linux/sched/types.h>
#endif

#include "btmtk_define.h"
#include "btmtk_drv.h"
#include "btmtk_sdio.h"

/*
 * This function is called by interface specific interrupt handler.
 * It updates Power Save & Host Sleep states, and wakes up the main
 * thread.
 */
void btmtk_interrupt(struct btmtk_private *priv)
{
	priv->adapter->int_count++;

	wake_up_interruptible(&priv->main_thread.wait_q);
}
EXPORT_SYMBOL_GPL(btmtk_interrupt);

static int btmtk_tx_pkt(struct btmtk_private *priv, struct sk_buff *skb)
{
	int ret = 0;
	u32 sdio_header_len = 0;

	if (!skb) {
		BTMTK_WARN("skb is NULL return -EINVAL");
		return -EINVAL;
	}

	BTMTK_DBG("skb->len %d", skb->len);

	if (!skb->data) {
		BTMTK_WARN("skb->data is NULL return -EINVAL");
		return -EINVAL;
	}

	if (!skb->len || ((skb->len + BTM_HEADER_LEN) > MTK_TXDATA_SIZE)) {
		BTMTK_WARN("Tx Error: Bad skb length %d : %d",
						skb->len, MTK_TXDATA_SIZE);
		return -EINVAL;
	}

	if (priv->hci_snoop_save)
		priv->hci_snoop_save(bt_cb(skb)->pkt_type, skb->data, skb->len);

	sdio_header_len = skb->len + BTM_HEADER_LEN;
	memset(txbuf, 0, MTK_TXDATA_SIZE);
	txbuf[0] = (sdio_header_len & 0x0000ff);
	txbuf[1] = (sdio_header_len & 0x00ff00) >> 8;
	txbuf[2] = 0;
	txbuf[3] = 0;
	txbuf[4] = bt_cb(skb)->pkt_type;
	memcpy(&txbuf[5], &skb->data[0], skb->len);
	if (priv->hw_host_to_card)
		ret = priv->hw_host_to_card(priv, txbuf, sdio_header_len);

	BTMTK_DBG("end");
	return ret;
}

static void btmtk_init_adapter(struct btmtk_private *priv)
{
	int buf_size;

	buf_size = ALIGN_SZ(SDIO_BLOCK_SIZE, BTSDIO_DMA_ALIGN);
	priv->adapter->hw_regs_buf = kzalloc(buf_size, GFP_KERNEL);
	if (!priv->adapter->hw_regs_buf) {
		priv->adapter->hw_regs = NULL;
		BTMTK_ERR("Unable to allocate buffer for h/w_regs.");
	} else {
		priv->adapter->hw_regs =
			(u8 *)ALIGN_ADDR(priv->adapter->hw_regs_buf,
					BTSDIO_DMA_ALIGN);
		BTMTK_DBG("hw_regs_buf=%p hw_regs=%p",
			priv->adapter->hw_regs_buf, priv->adapter->hw_regs);
	}
}

static void btmtk_free_adapter(struct btmtk_private *priv)
{
	kfree(priv->adapter->hw_regs_buf);
}

/*
 * This function handles the event generated by firmware, rx data
 * received from firmware, and tx data sent from kernel.
 */

static int btmtk_service_main_thread(void *data)
{
	struct btmtk_thread *thread = data;
	struct btmtk_private *priv = thread->priv;
	struct btmtk_adapter *adapter = NULL;
	struct btmtk_sdio_card *card = NULL;
#if (KERNEL_VERSION(4, 14, 0) > LINUX_VERSION_CODE)
	wait_queue_t wait;
#else
	struct wait_queue_entry wait;
#endif
	struct sk_buff *skb;
	int ret = 0;
	int i = 0;
	ulong flags;
	struct sched_param param = { .sched_priority = 90 };/*RR 90 is the same as audio*/
	int reset_flag = 0;

	sched_setscheduler(current, SCHED_RR, &param);

	BTMTK_INFO("main_thread begin 50");
	/* mdelay(50); */

	for (i = 0; i <= 1000; i++) {
		if (kthread_should_stop()) {
			BTMTK_INFO("main_thread: break from main thread for probe_ready");
			break;
		}

		if (probe_ready)
			break;

		/* BTMTK_INFO("probe_ready %d delay 10ms~15ms", probe_ready);*/
		usleep_range(10*1000, 15*1000);

		if (i == 1000) {
			BTMTK_WARN("probe_ready %d i = %d try too many times return",
				probe_ready, i);
			return 0;
		}
	}

	if (priv->adapter)
		adapter = priv->adapter;
	else {
		BTMTK_ERR("priv->adapter is NULL return");
		return 0;
	}

	if (priv->btmtk_dev.card)
		card = priv->btmtk_dev.card;
	else {
		BTMTK_ERR("priv->btmtk_dev.card is NULL return");
		return 0;
	}

	thread->thread_status = 1;
	init_waitqueue_entry(&wait, current);
	for (;;) {
		add_wait_queue(&thread->wait_q, &wait);
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop()) {
			BTMTK_WARN("main_thread: break from main thread");
			break;
		}

		if ((((!adapter->int_count) &&
				(!priv->btmtk_dev.tx_dnld_rdy ||
				skb_queue_empty(&card->tx_queue)))) &&
				(!priv->btmtk_dev.reset_dongle)) {
			BTMTK_DBG("main_thread is sleeping...");
			schedule();
		}

		set_current_state(TASK_RUNNING);

		remove_wait_queue(&thread->wait_q, &wait);

		if (kthread_should_stop()) {
			BTMTK_WARN("break after wake up");
			break;
		}

		if (priv->btmtk_dev.reset_dongle) {
			ret = priv->hw_sdio_reset_dongle();
			if (is_mt7663(card)) {
				if (ret) {
					BTMTK_ERR(L0_RESET_TAG "hw reset dongle error <%d>", ret);
				} else {
					BTMTK_INFO(L0_RESET_TAG "hw reset dongle done");
					reset_flag = 1;
					break;
				}
			} else {
				if (ret) {
					BTMTK_ERR("btmtk_sdio_reset_dongle return %d, error", ret);
					break;
				} else {
					BTMTK_INFO("hw reset dongle done");
					break;
				}
			}
		}

		if (priv->btmtk_dev.reset_progress)
			continue;

		ret = priv->hw_set_own_back(DRIVER_OWN);
		if (ret) {
			BTMTK_ERR("set driver own return fail");
			priv->start_reset_dongle_progress();
			continue;
		}

		spin_lock_irqsave(&priv->driver_lock, flags);
		if (adapter->int_count) {
			BTMTK_DBG("go int");
			adapter->int_count = 0;
			spin_unlock_irqrestore(&priv->driver_lock, flags);
			if (priv->hw_process_int_status(priv)) {
				priv->start_reset_dongle_progress();
				continue;
			}
		} else {
			BTMTK_DBG("go tx");
			spin_unlock_irqrestore(&priv->driver_lock, flags);
		}

		if (!priv->btmtk_dev.tx_dnld_rdy) {
			BTMTK_DBG("tx_dnld_rdy == 0, continue");
			continue;
		}

		spin_lock_irqsave(&priv->driver_lock, flags);
		skb = skb_dequeue(&card->tx_queue);
		spin_unlock_irqrestore(&priv->driver_lock, flags);

		if (skb) {
			if (skb->len < 16)
				btmtk_print_buffer_content(skb->data, skb->len);
			else
				btmtk_print_buffer_content(skb->data, 16);

			ret = btmtk_tx_pkt(priv, skb);
			if (ret && (ret != (-EINVAL))) {
				BTMTK_ERR("tx pkt return fail %d", ret);
				priv->start_reset_dongle_progress();
				continue;
			}

			BTMTK_DBG("after btmtk_tx_pkt kfree_skb");
			kfree_skb(skb);
		}

		if (skb_queue_empty(&card->tx_queue)) {
			ret = priv->hw_set_own_back(FW_OWN);
			if (ret) {
				BTMTK_ERR("set fw own return fail");
				priv->start_reset_dongle_progress();
				continue;
			}
		}
	}
	BTMTK_WARN("end");
	thread->thread_status = 0;

	if (is_mt7663(card) && reset_flag == 1)
		btmtk_remove_card(priv);

	return 0;
}

struct btmtk_private *btmtk_add_card(void *data)
{
	struct btmtk_sdio_card *card = (struct btmtk_sdio_card *)data;
	struct btmtk_private *priv;

	BTMTK_INFO("begin");

	priv = card->priv;

	btmtk_init_adapter(priv);

	BTMTK_INFO("Starting kthread...");
	priv->main_thread.priv = priv;
	spin_lock_init(&priv->driver_lock);

	init_waitqueue_head(&priv->main_thread.wait_q);
	priv->main_thread.task = kthread_run(btmtk_service_main_thread,
				&priv->main_thread, "btmtk_main_service");
	if (IS_ERR(priv->main_thread.task))
		goto err_thread;

	priv->btmtk_dev.card = card;
	priv->btmtk_dev.tx_dnld_rdy = true;

	return priv;

err_thread:
	return NULL;
}
EXPORT_SYMBOL_GPL(btmtk_add_card);

int btmtk_remove_card(struct btmtk_private *priv)
{
	BTMTK_INFO("begin, stop main_thread");
	if (!IS_ERR(priv->main_thread.task) && (priv->main_thread.thread_status)) {
		kthread_stop(priv->main_thread.task);
		wake_up_interruptible(&priv->main_thread.wait_q);
		BTMTK_INFO("wake_up_interruptible main_thread done");
	}
	BTMTK_INFO("stop main_thread done");
#ifdef CONFIG_DEBUG_FS
	/*btmtk_debugfs_remove(hdev);*/
#endif

	btmtk_free_adapter(priv);

	return 0;
}
EXPORT_SYMBOL_GPL(btmtk_remove_card);

MODULE_AUTHOR("Mediatek Ltd.");
MODULE_DESCRIPTION("Mediatek Bluetooth driver ver " VERSION);
MODULE_VERSION(VERSION);
MODULE_LICENSE("GPL v2");