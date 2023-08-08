// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2023 MediaTek Inc. All Rights Reserved.
 *
 * Author: Ren-Ting Wang <ren-ting.wang@mediatek.com>
 */

#include <linux/of.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include "ctrl.h"
#include "firmware.h"
#include "hpdma.h"
#include "hwspinlock.h"
#include "internal.h"
#include "mbox.h"
#include "mcu.h"
#include "netsys.h"
#include "net-event.h"
#include "ser.h"
#include "tdma.h"
#include "trm-mcu.h"
#include "trm.h"
#include "tunnel.h"
#include "wdt.h"

struct device *tops_dev;

static int mtk_tops_post_init(struct platform_device *pdev)
{
	int ret = 0;

	/* kick core */
	ret = mtk_tops_mcu_bring_up(pdev);
	if (ret) {
		TOPS_ERR("mcu post init failed: %d\n", ret);
		return ret;
	}

	/* offload tunnel protocol initialization */
	ret = mtk_tops_tnl_offload_proto_setup(pdev);
	if (ret) {
		TOPS_ERR("tnl offload protocol init failed: %d\n", ret);
		goto err_mcu_tear_down;
	}

	ret = mtk_tops_netevent_register(pdev);
	if (ret) {
		TOPS_ERR("netevent register fail: %d\n", ret);
		goto err_offload_proto_tear_down;
	}

	/* create sysfs file */
	ret = mtk_tops_ctrl_init(pdev);
	if (ret) {
		TOPS_ERR("ctrl init failed: %d\n", ret);
		goto err_netevent_unregister;
	}

	ret = mtk_tops_ser_init(pdev);
	if (ret) {
		TOPS_ERR("ser init failed: %d\n", ret);
		goto err_ctrl_deinit;
	}

	ret = mtk_tops_wdt_init(pdev);
	if (ret) {
		TOPS_ERR("wdt init failed: %d\n", ret);
		goto err_ser_deinit;
	}

	return ret;

err_ser_deinit:
	mtk_tops_ser_deinit(pdev);

err_ctrl_deinit:
	mtk_tops_ctrl_deinit(pdev);

err_netevent_unregister:
	mtk_tops_netevent_unregister(pdev);

err_offload_proto_tear_down:
	mtk_tops_tnl_offload_proto_teardown(pdev);

err_mcu_tear_down:
	mtk_tops_mcu_tear_down(pdev);

	return ret;
}

static int mtk_tops_probe(struct platform_device *pdev)
{
	int ret = 0;

	tops_dev = &pdev->dev;

	ret = mtk_tops_hwspinlock_init(pdev);
	if (ret) {
		TOPS_ERR("hwspinlock init failed: %d\n", ret);
		return ret;
	}

	ret = mtk_tops_fw_init(pdev);
	if (ret) {
		TOPS_ERR("firmware init failed: %d\n", ret);
		return ret;
	}

	ret = mtk_tops_mcu_init(pdev);
	if (ret) {
		TOPS_ERR("mcu init failed: %d\n", ret);
		return ret;
	}

	ret = mtk_tops_netsys_init(pdev);
	if (ret) {
		TOPS_ERR("netsys init failed: %d\n", ret);
		goto err_mcu_deinit;
	}

	ret = mtk_tops_tdma_init(pdev);
	if (ret) {
		TOPS_ERR("tdma init failed: %d\n", ret);
		goto err_netsys_deinit;
	}

	ret = mtk_tops_tnl_offload_init(pdev);
	if (ret) {
		TOPS_ERR("tunnel table init failed: %d\n", ret);
		goto err_tdma_deinit;
	}

	ret = mtk_tops_post_init(pdev);
	if (ret)
		goto err_tnl_offload_deinit;

	TOPS_ERR("init done\n");
	return ret;

err_tnl_offload_deinit:
	mtk_tops_tnl_offload_deinit(pdev);

err_tdma_deinit:
	mtk_tops_tdma_deinit(pdev);

err_netsys_deinit:
	mtk_tops_netsys_deinit(pdev);

err_mcu_deinit:
	mtk_tops_mcu_deinit(pdev);

	return ret;
}

static int mtk_tops_remove(struct platform_device *pdev)
{
	mtk_tops_wdt_deinit(pdev);

	mtk_tops_ser_deinit(pdev);

	mtk_tops_ctrl_deinit(pdev);

	mtk_tops_netevent_unregister(pdev);

	mtk_tops_tnl_offload_proto_teardown(pdev);

	mtk_tops_mcu_tear_down(pdev);

	mtk_tops_tnl_offload_deinit(pdev);

	mtk_tops_tdma_deinit(pdev);

	mtk_tops_netsys_deinit(pdev);

	mtk_tops_mcu_deinit(pdev);

	return 0;
}

static const struct of_device_id tops_match[] = {
	{ .compatible = "mediatek,tops", },
	{ },
};
MODULE_DEVICE_TABLE(of, tops_match);

static struct platform_driver mtk_tops_driver = {
	.probe = mtk_tops_probe,
	.remove = mtk_tops_remove,
	.driver = {
		.name = "mediatek,tops",
		.owner = THIS_MODULE,
		.of_match_table = tops_match,
	},
};

static int __init mtk_tops_init(void)
{
	mtk_tops_mbox_init();

	mtk_tops_hpdma_init();

	mtk_tops_trm_init();

	return platform_driver_register(&mtk_tops_driver);
}

static void __exit mtk_tops_exit(void)
{
	platform_driver_unregister(&mtk_tops_driver);

	mtk_tops_trm_exit();

	mtk_tops_hpdma_exit();

	mtk_tops_mbox_exit();
}

module_init(mtk_tops_init);
module_exit(mtk_tops_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MediaTek TOPS Driver");
MODULE_AUTHOR("Ren-Ting Wang <ren-ting.wang@mediatek.com>");
