/*
 * linux/drivers/media/video/samsung/mfc5x/mfc_dev.c
 *
 * Copyright (c) 2010 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com/
 *
 * Driver interface for Samsung MFC (Multi Function Codec - FIMV) driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>

#include <linux/sched.h>
#include <linux/firmware.h>

#include <asm/io.h>
#include <asm/uaccess.h>

#include "mfc_dev.h"
#include "mfc_user.h"
#include "mfc_reg.h"
#include "mfc_log.h"
#include "mfc_ctrl.h"
#include "mfc_buf.h"
#include "mfc_inst.h"
#include "mfc_pm.h"
#include "mfc_dec.h"
#include "mfc_mem.h"
#include "mfc_cmd.h"

#ifdef SYSMMU_MFC_ON
#include <plat/sysmmu.h>
#endif

#define MFC_MINOR	252
#define MFC_FW_NAME	"mfc_fw.bin"

static struct mfc_dev *mfcdev;

static int mfc_open(struct inode *inode, struct file *file)
{
	struct mfc_inst_ctx *mfc_ctx;
	int ret;
	enum mfc_ret_code retcode;

	mutex_lock(&mfcdev->lock);

	if (!mfcdev->fw.state) {
		mfc_err("MFC F/W not load yet\n");
		ret = -ENODEV;
		goto err_fw_state;
	}

	if (atomic_read(&mfcdev->inst_cnt) == 0) {
		ret = mfc_power_on();
		if (ret < 0) {
			mfc_err("power enable failed\n");
			goto err_pwr_enable;
		}
		/* MFC hardware initialization */
		retcode = mfc_start(mfcdev);
		if (retcode != MFC_OK) {
			mfc_err("MFC H/W init failed: %d\n", retcode);
			ret = -ENODEV;
			goto err_start_hw;
		}
/* FIMXE: TLB flush position. first instance open or every instance release */
#ifdef SYSMMU_MFC_ON
		mfc_clock_on();

		sysmmu_tlb_invalidate(SYSMMU_MFC_L);
		sysmmu_tlb_invalidate(SYSMMU_MFC_R);

		mfc_clock_off();
#endif
	}

	mfc_ctx = mfc_create_inst();
	if (!mfc_ctx) {
		mfc_err("failed to create instance context\n");
		ret = -ENOMEM;
		goto err_inst_ctx;
	}

	atomic_inc(&mfcdev->inst_cnt);
	mfc_ctx->dev = mfcdev;
	file->private_data = (struct mfc_inst_ctx *)mfc_ctx;

	mutex_unlock(&mfcdev->lock);

	return 0;

err_inst_ctx:
err_start_hw:
	if (atomic_read(&mfcdev->inst_cnt) == 0) {
		ret = mfc_power_off();
		if (ret < 0)
			mfc_err("power disable failed\n");
	}

err_pwr_enable:
err_fw_state:
	mutex_unlock(&mfcdev->lock);

	return ret;
}

static int mfc_release(struct inode *inode, struct file *file)
{
	struct mfc_inst_ctx *mfc_ctx;
	struct mfc_dev *dev;
	int ret;

	mfc_ctx = (struct mfc_inst_ctx *)file->private_data;
	dev = mfc_ctx->dev;

	mutex_lock(&dev->lock);

	/*
	 * FIXME: consider the calling sequence
	 * TLB flush -> destroy instance or destory instance -> TLB Flush
	 */

/* FIMXE: TLB flush position. first instance open or every instance release */
#ifdef SYSMMU_MFC_ON
	mfc_clock_on();

	sysmmu_tlb_invalidate(SYSMMU_MFC_L);
	sysmmu_tlb_invalidate(SYSMMU_MFC_R);

	mfc_clock_off();
#endif

	mfc_destroy_inst(mfc_ctx);

	atomic_dec(&dev->inst_cnt);

	if (atomic_read(&dev->inst_cnt) == 0) {
		ret = mfc_power_off();
		if (ret < 0) {
			mfc_err("power disable failed.\n");
			goto err_pwr_disable;
		}
	}

	ret = 0;

err_pwr_disable:
	mutex_unlock(&dev->lock);

	return ret;
}

/* FIXME: add request firmware ioctl */
static int mfc_ioctl(struct inode *inode, struct file *file,
		unsigned int cmd, unsigned long arg)
{

	struct mfc_inst_ctx *mfc_ctx = (struct mfc_inst_ctx *)file->private_data;
	int ret, ex_ret;
	struct mfc_common_args in_param;
	struct mfc_buf_alloc_arg buf_arg;
	int port;

	struct mfc_dev *dev = mfc_ctx->dev;
	int i;

	struct mfc_set_config_arg *set_cnf_arg;

	mutex_lock(&dev->lock);

	//mfc_clock_on();

	ret = copy_from_user(&in_param, (struct mfc_common_args *)arg,
			sizeof(struct mfc_common_args));
	if (ret < 0) {
		mfc_err("Inparm copy error\n");
		ret = -EIO;
		in_param.ret_code = MFC_INVALID_PARAM_FAIL;
		goto out_ioctl;
	}

	//mfc_ctx = (s3c_mfc_inst_ctx *)file->private_data;

	mutex_unlock(&dev->lock);

	/* Decoder only */
	/*
	mfc_ctx->extraDPB = MFC_MAX_EXTRA_DPB;
	mfc_ctx->displayDelay = 9999;
	mfc_ctx->FrameType = MFC_RET_FRAME_NOT_SET;
	*/

	/* FIXME: add locking */

	mfc_dbg("cmd: 0x%08x\n", cmd);

	switch (cmd) {

	case IOCTL_MFC_DEC_INIT:

		mutex_lock(&dev->lock);

		if (mfc_chk_inst_state(mfc_ctx, INST_STATE_CREATED) < 0) {
			mfc_err("invalid state: %d\n", mfc_ctx->state);
			in_param.ret_code = MFC_STATE_INVALID;
			ret = -EINVAL;

			break;
		}

		mfc_clock_on();
		in_param.ret_code = mfc_init_decoding(mfc_ctx, &(in_param.args));
		ret = in_param.ret_code;
		mfc_clock_off();

		mfc_set_inst_state(mfc_ctx, INST_STATE_DEC_INIT);

		mutex_unlock(&dev->lock);

		#if 0
		mutex_lock(&ctrl->lock);

		mfc_init_decoding();

		mfc_dbg("IOCTL_MFC_DEC_INIT\n");
		if (s3c_mfc_set_state(mfc_ctx, MFCINST_STATE_DEC_INITIALIZE) <
		    0) {
			mfc_err("MFC_RET_STATE_INVALID\n");
			in_param.ret_code = MFC_RET_STATE_INVALID;
			ret = -EINVAL;
			mutex_unlock(&ctrl->lock);
			break;
		}

		/* MFC decode init */
		in_param.ret_code =
		    s3c_mfc_init_decode(mfc_ctx, &(in_param.args));

		if (in_param.ret_code < 0) {
			ret = in_param.ret_code;
			mutex_unlock(&ctrl->lock);
			break;
		}

		if (in_param.args.dec_init.out_dpb_cnt <= 0) {
			mfc_err("MFC out_dpb_cnt error\n");
			mutex_unlock(&ctrl->lock);
			break;
		}

		/* Get frame buf size */
		frame_buf_size =
		    s3c_mfc_get_frame_buf_size(mfc_ctx, &(in_param.args));

		/* Allocate MFC buffer(Y, C, MV) */
		in_param.ret_code =
		    s3c_mfc_allocate_frame_buf(mfc_ctx, &(in_param.args),
					       frame_buf_size);
		if (in_param.ret_code < 0) {
			ret = in_param.ret_code;
			mutex_unlock(&ctrl->lock);
			break;
		}

		/* Set DPB buffer */
		in_param.ret_code =
		    s3c_mfc_set_dec_frame_buffer(mfc_ctx, &(in_param.args));
		if (in_param.ret_code < 0) {
			ret = in_param.ret_code;
			mutex_unlock(&ctrl->lock);
			break;
		}

		mutex_unlock(&ctrl->lock);
		#endif

		break;

	case IOCTL_MFC_DEC_EXE:
		mfc_clock_on();
		in_param.ret_code = mfc_exec_decoding(mfc_ctx, &(in_param.args));
		ret = in_param.ret_code;
		mfc_clock_off();

		mfc_set_inst_state(mfc_ctx, INST_STATE_DEC_EXE);

		break;

	case IOCTL_MFC_GET_IN_BUF:

		if (mfc_chk_inst_state(mfc_ctx, INST_STATE_CREATED) < 0) {
			mfc_err("invalid state: %d\n", mfc_ctx->state);
			in_param.ret_code = MFC_STATE_INVALID;
			ret = -EINVAL;

			break;
		}

		/* FIXME: consider below code */
		/*
		if ((is_dec_codec(in_param.args.mem_alloc.codec_type)) &&
			(in_param.args.mem_alloc.buff_size < (CPB_BUF_SIZE + DESC_BUF_SIZE)))
		{
			in_param.args.mem_alloc.buff_size = CPB_BUF_SIZE + DESC_BUF_SIZE;
		}
		*/

		/* allocate stream buf for decoder & current YC buf for encoder */
		#if 0
		if (in_param.args.mem_alloc.dec_enc_type == ENCODER)
#if CONFIG_VIDEO_MFC_MEM_PORT_COUNT == 1
			mfc_ctx->port_no = 0;
#else
			mfc_ctx->port_no = 1;
#endif
		else
			mfc_ctx->port_no = 0;
		#endif

		/*
		in_param.args.mem_alloc.buff_size =
			ALIGN(in_param.args.mem_alloc.buff_size, 2 * BUF_L_UNIT);
		in_param.ret_code =
			mfc_get_virt_addr(mfc_ctx, &(in_param.args));
		*/

		if (in_param.args.mem_alloc.dec_enc_type == ENCODER) {
			buf_arg.type = ENCODING;
			port = 1;
		} else {
			buf_arg.type = DECODING;
			port = 0;
		}
		buf_arg.size = in_param.args.mem_alloc.buff_size;
		//buf_arg.mapped = in_param.args.mem_alloc.mapped_addr;
		buf_arg.align = ALIGN_2KB;

		in_param.ret_code = mfc_alloc_buf(mfc_ctx, &buf_arg, port);
		in_param.args.mem_alloc.out_addr = buf_arg.user;
#ifdef CONFIG_S5P_VMEM
		in_param.args.mem_alloc.cookie = buf_arg.cookie;
#endif
		ret = in_param.ret_code;

		break;

	case IOCTL_MFC_FREE_BUF:

		/*
		if (mfc_chk_inst_state(mfc_ctx, INST_STATE_DEC_INIT) < 0) {
			mfc_err("invalid state: %d\n", mfc_ctx->state);
			in_param.ret_code = MFC_STATE_INVALID;
			ret = -EINVAL;

			break;
		}
		*/

		in_param.ret_code =
			mfc_free_buf(mfc_ctx,
				(unsigned char *)in_param.args.mem_free.u_addr);
		ret = in_param.ret_code;

		break;

	case IOCTL_MFC_GET_PHYS_ADDR:

		if (mfc_chk_inst_state(mfc_ctx, INST_STATE_CREATED) < 0) {
			mfc_err("invalid state: %d\n", mfc_ctx->state);
			in_param.ret_code = MFC_STATE_INVALID;
			ret = -EINVAL;

			break;
		}

		/*
		in_param.ret_code =
		    mfc_get_phys_addr(mfc_ctx, &(in_param.args));
		ret = in_param.ret_code;
		*/
		mfc_dbg("user addr: 0x%08x\n", in_param.args.get_phys_addr.u_addr);
		in_param.args.get_phys_addr.p_addr =
			mfc_get_buf_real(mfc_ctx->id,
				(unsigned char *)in_param.args.get_phys_addr.u_addr);
		if (in_param.args.get_phys_addr.p_addr)
			in_param.ret_code = MFC_OK;
		else
			in_param.ret_code = MFC_MEM_INVALID_ADDR_FAIL;

		ret = in_param.ret_code;

		break;

	case IOCTL_MFC_GET_MMAP_SIZE:

		if (mfc_chk_inst_state(mfc_ctx, INST_STATE_CREATED) < 0) {
			mfc_err("invalid state: %d\n", mfc_ctx->state);
			in_param.ret_code = MFC_STATE_INVALID;
			ret = -EINVAL;

			break;
		}

		in_param.ret_code = MFC_OK;
		ret = 0;
		for (i = 0; i < dev->mem_ports; i++)
			ret += mfc_mem_data_size(i);

		break;

	case IOCTL_MFC_SET_CONFIG:
		/* FIXME: mfc_chk_inst_state*/
		mutex_lock(&dev->lock);

		/* in_param.ret_code = mfc_set_config(mfc_ctx, &(in_param.args)); */

		set_cnf_arg = (struct mfc_set_config_arg *)&in_param.args;

		in_param.ret_code = mfc_set_inst_cfg(mfc_ctx, set_cnf_arg->in_config_param, set_cnf_arg->in_config_value);
		ret = in_param.ret_code;
		mutex_unlock(&dev->lock);
		break;

	default:

		/* FIXME:
		mfc_err("Requested ioctl command is not defined. (ioctl cmd=0x%08x)\n",
		     cmd);
		in_param.ret_code = MFC_INVALID_PARAM_FAIL;
		ret = -EINVAL;
		*/
		in_param.ret_code = MFC_OK;
		ret = MFC_OK;
	}

out_ioctl:
	//mfc_clock_off();

	ex_ret = copy_to_user((struct mfc_common_args *)arg,
			&in_param,
			sizeof(struct mfc_common_args));
	if (ex_ret < 0) {
		mfc_err("Outparm copy to user error\n");
		ret = -EIO;
	}

	mfc_dbg("return = %d\n", ret);

	return ret;
}

static void mfc_vm_open(struct vm_area_struct *vma)
{
	/*
	struct mfc_inst_ctx *mfc_ctx = (struct mfc_inst_ctx *)vma->vm_private_data;

	mfc_dbg("id: %d\n", mfc_ctx->id);
	*/

	/* FIXME: atomic_inc(mapped count) */
}

static void mfc_vm_close(struct vm_area_struct *vma)
{
	/*
	struct mfc_inst_ctx *mfc_ctx = (struct mfc_inst_ctx *)vma->vm_private_data;

	mfc_dbg("id: %d\n", mfc_ctx->id);
	*/

	/* FIXME: atomic_dec(mapped count) */
}

static int mfc_vm_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct mfc_inst_ctx *mfc_ctx = (struct mfc_inst_ctx *)vma->vm_private_data;
	struct page *pg = NULL;

	mfc_dbg("id: %d, pgoff: 0x%08lx, user: 0x%08lx\n",
		mfc_ctx->id, vmf->pgoff, (unsigned long)(vmf->virtual_address));

	if (mfc_ctx == NULL)
		return VM_FAULT_SIGBUS;

	mfc_dbg("addr: 0x%08lx\n", 
		(unsigned long)(_mfc_get_buf_addr(mfc_ctx->id, vmf->virtual_address)));

	pg = vmalloc_to_page(_mfc_get_buf_addr(mfc_ctx->id, vmf->virtual_address));

	if (!pg)
		return VM_FAULT_SIGBUS;

	vmf->page = pg;

	return 0;
}

static const struct vm_operations_struct mfc_vm_ops = {
	.open	= mfc_vm_open,
	.close	= mfc_vm_close,
	.fault	= mfc_vm_fault
};

static int mfc_mmap(struct file *filp, struct vm_area_struct *vma)
{
	unsigned long user_size = vma->vm_end - vma->vm_start;
	unsigned long real_size;
	struct mfc_inst_ctx *mfc_ctx = (struct mfc_inst_ctx *)filp->private_data;
#ifndef CONFIG_S5P_VMEM
	unsigned long pfn;
	unsigned long remap_offset, remap_size;
	struct mfc_dev *dev = mfc_ctx->dev;
#ifdef SYSMMU_MFC_ON
	char *ptr;
	unsigned long start, size;
#endif
#endif
	mfc_dbg("vm_start: 0x%08lx, vm_end: 0x%08lx, size: %ld(%ldMB)\n",
		vma->vm_start, vma->vm_end, user_size, (user_size >> 20));

	real_size = (unsigned long)(mfc_mem_data_size(0) + mfc_mem_data_size(1));

	mfc_dbg("port 0 size: %d, port 1 size: %d, total: %ld\n",
		mfc_mem_data_size(0),
		mfc_mem_data_size(1),
		real_size);

	/*
	 * if memory size required from appl. mmap() is bigger than max data memory
	 * size allocated in the driver.
	 */
	if (user_size > real_size) {
		mfc_err("user requeste mem(%ld) is bigger than available mem(%ld)\n",
			user_size, real_size);
		return -EINVAL;
	}
#ifdef SYSMMU_MFC_ON
#ifdef CONFIG_S5P_VMEM
	vma->vm_flags |= VM_RESERVED | VM_IO;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	vma->vm_ops = &mfc_vm_ops;
	vma->vm_private_data = mfc_ctx;

	mfc_ctx->userbase = vma->vm_start;
#else
	if (dev->mem_ports == 1) {
		remap_offset = 0;
		remap_size = user_size;

		vma->vm_flags |= VM_RESERVED | VM_IO;
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

		/*
		 * Port 0 mapping for stream buf & frame buf (chroma + MV + luma)
		 */
		ptr = (char *)mfc_mem_data_base(0);
		start = remap_offset;
		size = remap_size;
		while (size > 0) {
			pfn = vmalloc_to_pfn(ptr);
			if (remap_pfn_range(vma, vma->vm_start + start, pfn,
				PAGE_SIZE, vma->vm_page_prot)) {

				mfc_err("failed to remap port 0\n");
				return -EAGAIN;
			}

			start += PAGE_SIZE;
			ptr += PAGE_SIZE;
			size -= PAGE_SIZE;
		}
	} else {
		remap_offset = 0;
		remap_size = min((unsigned long)mfc_mem_data_size(0), user_size);

		vma->vm_flags |= VM_RESERVED | VM_IO;
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

		/*
		 * Port 0 mapping for stream buf & frame buf (chroma + MV)
		 */
		ptr = (char *)mfc_mem_data_base(0);
		start = remap_offset;
		size = remap_size;
		while (size > 0) {
			pfn = vmalloc_to_pfn(ptr);
			if (remap_pfn_range(vma, vma->vm_start + start, pfn,
				PAGE_SIZE, vma->vm_page_prot)) {

				mfc_err("failed to remap port 0\n");
				return -EAGAIN;
			}

			start += PAGE_SIZE;
			ptr += PAGE_SIZE;
			size -= PAGE_SIZE;
		}

		remap_offset = remap_size;
		remap_size = min((unsigned long)mfc_mem_data_size(1),
			user_size - remap_offset);

		vma->vm_flags |= VM_RESERVED | VM_IO;
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

		/*
		 * Port 1 mapping for frame buf (luma)
		 */
		ptr = (void *)mfc_mem_data_base(1);
		start = remap_offset;
		size = remap_size;
		while (size > 0) {
			pfn = vmalloc_to_pfn(ptr);
			if (remap_pfn_range(vma, vma->vm_start + start, pfn,
				PAGE_SIZE, vma->vm_page_prot)) {

				mfc_err("failed to remap port 1\n");
				return -EAGAIN;
			}

			start += PAGE_SIZE;
			ptr += PAGE_SIZE;
			size -= PAGE_SIZE;
		}
	}

	mfc_ctx->userbase = vma->vm_start;

	mfc_dbg("user request mem = %ld, available data mem = %ld\n",
		  user_size, real_size);

	if ((remap_offset + remap_size) < real_size)
		mfc_warn("The MFC reserved memory dose not mmap fully [%ld: %ld]\n",
		  real_size, (remap_offset + remap_size));
#endif /* CONFIG_S5P_VMEM */
#else
	if (dev->mem_ports == 1) {
		remap_offset = 0;
		remap_size = user_size;

		vma->vm_flags |= VM_RESERVED | VM_IO;
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

		/*
		 * Port 0 mapping for stream buf & frame buf (chroma + MV + luma)
		 */
		pfn = __phys_to_pfn(mfc_mem_data_base(0));
		if (remap_pfn_range(vma, vma->vm_start + remap_offset, pfn,
			remap_size, vma->vm_page_prot)) {

			mfc_err("failed to remap port 0\n");
			return -EINVAL;
		}
	} else {
		remap_offset = 0;
		remap_size = min((unsigned long)mfc_mem_data_size(0), user_size);

		vma->vm_flags |= VM_RESERVED | VM_IO;
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

		/*
		 * Port 0 mapping for stream buf & frame buf (chroma + MV)
		 */
		pfn = __phys_to_pfn(mfc_mem_data_base(0));
		if (remap_pfn_range(vma, vma->vm_start + remap_offset, pfn,
			remap_size, vma->vm_page_prot)) {

			mfc_err("failed to remap port 0\n");
			return -EINVAL;
		}

		remap_offset = remap_size;
		remap_size = min((unsigned long)mfc_mem_data_size(1),
			user_size - remap_offset);

		vma->vm_flags |= VM_RESERVED | VM_IO;
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

		/*
		 * Port 1 mapping for frame buf (luma)
		 */
		pfn = __phys_to_pfn(mfc_mem_data_base(1));
		if (remap_pfn_range(vma, vma->vm_start + remap_offset, pfn,
			remap_size, vma->vm_page_prot)) {

			mfc_err("failed to remap port 1\n");
			return -EINVAL;
		}
	}

	mfc_ctx->userbase = vma->vm_start;

	mfc_dbg("user request mem = %ld, available data mem = %ld\n",
		  user_size, real_size);

	if ((remap_offset + remap_size) < real_size)
		mfc_warn("The MFC reserved memory dose not mmap fully [%ld: %ld]\n",
		  real_size, (remap_offset + remap_size));
#endif /* SYSMMU_MFC_ON */
	return 0;
}

static const struct file_operations mfc_fops = {
	.owner	= THIS_MODULE,
	.open	= mfc_open,
	.release= mfc_release,
	.ioctl	= mfc_ioctl,
	.mmap	= mfc_mmap,
};

static struct miscdevice mfc_miscdev = {
	.minor	= MFC_MINOR,
	.name	= "mfc",
	.fops	= &mfc_fops,
};

static void mfc_firmware_request_complete_handler(const struct firmware *fw,
						  void *context)
{
	if (fw != NULL) {
		mfcdev->fw.state = mfc_load_firmware(fw->data, fw->size);
		printk(KERN_INFO "MFC F/W loaded successfully (size: %d)\n", fw->size);

		mfcdev->fw.info = fw;
	} else {
		printk(KERN_ERR "failed to load MFC F/W, MFC will not working\n");
	}
}

/* FIXME: check every exception case (goto) */
static int __devinit mfc_probe(struct platform_device *pdev)
{
	struct resource *res;
	int ret;

	mfcdev = kzalloc(sizeof(struct mfc_dev), GFP_KERNEL);
	if (unlikely(mfcdev == NULL)) {
		dev_err(&pdev->dev, "failed to allocate control memory\n");
		return -ENOMEM;
	}

	/* init. control structure */
	sprintf(mfcdev->name, "%s", MFC_DEV_NAME);

	mutex_init(&mfcdev->lock);
	init_waitqueue_head(&mfcdev->wait_sys);
	init_waitqueue_head(&mfcdev->wait_codec[0]);
	init_waitqueue_head(&mfcdev->wait_codec[1]);
	atomic_set(&mfcdev->inst_cnt, 0);

	mfcdev->device = &pdev->dev;

	platform_set_drvdata(pdev, mfcdev);

	/* get the memory region */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (unlikely(res == NULL)) {
		dev_err(&pdev->dev, "no memory resource specified\n");
		ret = -ENOENT;
		goto err_mem_res;
	}

	mfcdev->reg.rsrc_start = res->start;
	mfcdev->reg.rsrc_len = res->end - res->start + 1;

	/* request mem region for MFC register (0x0000 ~ 0xE008) */
	res = request_mem_region(mfcdev->reg.rsrc_start,
			mfcdev->reg.rsrc_len, pdev->name);
	if (unlikely(res == NULL)) {
		dev_err(&pdev->dev, "failed to get memory region\n");
		ret = -ENOENT;
		goto err_mem_req;
	}

	/* ioremap for MFC register */
	mfcdev->reg.base = ioremap(mfcdev->reg.rsrc_start, mfcdev->reg.rsrc_len);

	if (unlikely(!mfcdev->reg.base)) {
		dev_err(&pdev->dev, "failed to ioremap memory region\n");
		ret = -EINVAL;
		goto err_mem_map;
	}

	init_reg(mfcdev->reg.base);

	mfcdev->irq = platform_get_irq(pdev, 0);
	if (unlikely(mfcdev->irq < 0)) {
		dev_err(&pdev->dev, "no irq resource specified\n");
		ret = -ENOENT;
		goto err_irq_res;
	}

	ret = request_irq(mfcdev->irq, mfc_irq, IRQF_DISABLED, mfcdev->name, mfcdev);
	if (ret) {
		dev_err(&pdev->dev, "failed to allocate irq (%d)\n", ret);
		goto err_irq_req;
	}

	/*
	 * initialize PM(power, clock) interface
	 */
	ret = mfc_init_pm(mfcdev);
	if (ret < 0) {
		printk(KERN_ERR "failed to init. mfc PM interface\n");
		goto err_pm_if;
	}

	/*
	 * initialize memory manager
	 */
	ret = mfc_init_mem_mgr(mfcdev);
	if (ret < 0) {
		printk(KERN_ERR "failed to init. mfc memory manager\n");
		goto err_mem_mgr;
	}

	/*
	 * loading firmware
	 */
	ret = request_firmware_nowait(THIS_MODULE,
				      FW_ACTION_HOTPLUG,
				      MFC_FW_NAME,
				      &pdev->dev,
				      GFP_KERNEL,
				      pdev,
				      mfc_firmware_request_complete_handler);
	if (ret) {
		dev_err(&pdev->dev, "could not load firmware (err=%d)\n", ret);
		goto err_fw_req;
	}

#ifdef SYSMMU_MFC_ON
	mfc_clock_on();

	sysmmu_on(SYSMMU_MFC_L);
	sysmmu_on(SYSMMU_MFC_R);

	sysmmu_set_tablebase_pgd(SYSMMU_MFC_L, __pa(swapper_pg_dir));
	sysmmu_set_tablebase_pgd(SYSMMU_MFC_R, __pa(swapper_pg_dir));

	mfc_clock_off();
#endif
	/*
	 * initialize buffer manager
	 */
	mfc_init_buf();

	mfc_init_decoders();

	ret = misc_register(&mfc_miscdev);

	if (ret) {
		mfc_err("MFC can't misc register on minor=%d\n", MFC_MINOR);
		goto err_misc_reg;
	}

	mfc_info("MFC(Multi Function Codec - FIMV v5.x) registered successfully\n");

	return 0;

err_misc_reg:
	mfc_final_buf();

#ifdef SYSMMU_MFC_ON
	mfc_clock_on();

	sysmmu_off(SYSMMU_MFC_L);
	sysmmu_off(SYSMMU_MFC_R);

	mfc_clock_off();
#endif
	if (mfcdev->fw.info)
		release_firmware(mfcdev->fw.info);

err_fw_req:
	mfc_final_mem_mgr(mfcdev);

err_mem_mgr:
	mfc_final_pm(mfcdev);

err_pm_if:
	free_irq(mfcdev->irq, mfcdev);

err_irq_req:
err_irq_res:
	iounmap(mfcdev->reg.base);

err_mem_map:
	release_mem_region(mfcdev->reg.rsrc_start, mfcdev->reg.rsrc_len);

err_mem_req:
err_mem_res:
	platform_set_drvdata(pdev, NULL);
	mutex_destroy(&mfcdev->lock);
	kfree(mfcdev);

	return ret;
}

/* FIXME: check mfc_remove funtionalilty */
static int __devexit mfc_remove(struct platform_device *pdev)
{
	struct mfc_dev *dev = platform_get_drvdata(pdev);

	/* FIXME: close all instance? or check active instance? */

	misc_deregister(&mfc_miscdev);

	mfc_final_buf();
#ifdef SYSMMU_MFC_ON
	mfc_clock_on();

	sysmmu_off(SYSMMU_MFC_L);
	sysmmu_off(SYSMMU_MFC_R);

	mfc_clock_off();
#endif
	if (dev->fw.info)
		release_firmware(dev->fw.info);
	mfc_final_mem_mgr(dev);
	mfc_final_pm(dev);
	free_irq(dev->irq, dev);
	iounmap(dev->reg.base);
	release_mem_region(dev->reg.rsrc_start, dev->reg.rsrc_len);
	platform_set_drvdata(pdev, NULL);
	mutex_destroy(&dev->lock);
	kfree(dev);

	return 0;
}

static int mfc_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct mfc_dev *dev = platform_get_drvdata(pdev);
	int ret;

	if (atomic_read(&dev->inst_cnt) == 0)
		return 0;

	mutex_lock(&dev->lock);

	ret = mfc_sleep(dev);

	mutex_unlock(&dev->lock);

	return ret;
}

static int mfc_resume(struct platform_device *pdev)
{
	struct mfc_dev *dev = platform_get_drvdata(pdev);
	int ret;

	if (atomic_read(&dev->inst_cnt) == 0)
		return 0;

	mutex_lock(&dev->lock);

	ret = mfc_wakeup(dev);

	mutex_unlock(&dev->lock);

	return ret;
}

static struct platform_driver mfc_driver = {
	.probe		= mfc_probe,
	.remove		= __devexit_p(mfc_remove),
	.shutdown	= NULL,
	.suspend	= mfc_suspend,
	.resume		= mfc_resume,

	.driver		= {
		.owner	= THIS_MODULE,
		.name	= "mfc",
	},
};

static int __init mfc_init(void)
{
	if (platform_driver_register(&mfc_driver) != 0) {
		printk(KERN_ERR "FIMV MFC platform device registration failed.. \n");
		return -1;
	}

	return 0;
}

static void __exit mfc_exit(void)
{
	platform_driver_unregister(&mfc_driver);
	mfc_info("FIMV MFC(Multi Function Codec) V5.x exit.\n");
}

module_init(mfc_init);
module_exit(mfc_exit);

MODULE_AUTHOR("Jeongtae, Park");
MODULE_AUTHOR("Jaeryul, Oh");
MODULE_DESCRIPTION("FIMV MFC(Multi Function Codec) V5.x Device Driver");
MODULE_LICENSE("GPL");

