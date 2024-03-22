// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023 MediaTek Inc.
 *
 * Author: Chris.Chou <chris.chou@mediatek.com>
 *         Ren-Ting Wang <ren-ting.wang@mediatek.com>
 */

#include <crypto/aes.h>
#include <crypto/hash.h>
#include <crypto/hmac.h>
#include <crypto/md5.h>
#include <linux/delay.h>

#include <crypto-eip/ddk/slad/api_pcl.h>
#include <crypto-eip/ddk/slad/api_pcl_dtl.h>
#include <crypto-eip/ddk/slad/api_pec.h>
#include <crypto-eip/ddk/slad/api_driver197_init.h>

#include "crypto-eip/crypto-eip.h"
#include "crypto-eip/ddk-wrapper.h"
#include "crypto-eip/internal.h"

LIST_HEAD(result_list);

void crypto_free_sa(void *sa_pointer)
{
	DMABuf_Handle_t SAHandle = {0};

	SAHandle.p = sa_pointer;
	PEC_SA_UnRegister(PEC_INTERFACE_ID, SAHandle, DMABuf_NULLHandle,
				DMABuf_NULLHandle);
	DMABuf_Release(SAHandle);
}

void crypto_free_token(void *token)
{
	DMABuf_Handle_t TokenHandle = {0};

	TokenHandle.p = token;
	DMABuf_Release(TokenHandle);
}

/* TODO: to be remove*/
void crypto_free_pkt(void *pkt)
{
	DMABuf_Handle_t PktHandle = {0};

	PktHandle.p = pkt;
	DMABuf_Release(PktHandle);
}

void crypto_free_sglist(void *sglist)
{
	PEC_Status_t res;
	unsigned int count;
	unsigned int size;
	DMABuf_Handle_t SGListHandle = {0};
	DMABuf_Handle_t ParticleHandle = {0};
	int i;
	uint8_t *Particle_p;

	SGListHandle.p = sglist;
	res = PEC_SGList_GetCapacity(SGListHandle, &count);
	if (res != PEC_STATUS_OK)
		return;
	for (i = 0; i < count; i++) {
		PEC_SGList_Read(SGListHandle,
						i,
						&ParticleHandle,
						&size,
						&Particle_p);
		DMABuf_Particle_Release(ParticleHandle);
	}

	PEC_SGList_Destroy(SGListHandle);
}

static bool crypto_iotoken_create(IOToken_Input_Dscr_t * const dscr_p,
				  void * const ext_p, u32 *data_p,
				  PEC_CommandDescriptor_t * const pec_cmd_dscr)
{
	int IOTokenRc;

	dscr_p->InPacket_ByteCount = pec_cmd_dscr->SrcPkt_ByteCount;
	dscr_p->Ext_p = ext_p;

	IOTokenRc = IOToken_Create(dscr_p, data_p);
	if (IOTokenRc < 0) {
		CRYPTO_ERR("IOToken_Create error %d\n", IOTokenRc);
		return false;
	}

	pec_cmd_dscr->InputToken_p = data_p;

	return true;
}

unsigned int crypto_pe_busy_get_one(IOToken_Output_Dscr_t *const OutTokenDscr_p,
			       u32 *OutTokenData_p,
			       PEC_ResultDescriptor_t *RD_p)
{
	int LoopCounter = MTK_EIP197_INLINE_NOF_TRIES;
	int IOToken_Rc;
	PEC_Status_t pecres;

	ZEROINIT(*OutTokenDscr_p);
	ZEROINIT(*RD_p);

	/* Link data structures */
	RD_p->OutputToken_p = OutTokenData_p;

	while (LoopCounter > 0) {
		/* Try to get the processed packet from the driver */
		unsigned int Counter = 0;

		pecres = PEC_Packet_Get(PEC_INTERFACE_ID, RD_p, 1, &Counter);
		if (pecres != PEC_STATUS_OK) {
			/* IO error */
			CRYPTO_ERR("PEC_Packet_Get error %d\n", pecres);
			return 0;
		}

		if (Counter) {
			IOToken_Rc = IOToken_Parse(OutTokenData_p, OutTokenDscr_p);
			if (IOToken_Rc < 0) {
				/* IO error */
				CRYPTO_ERR("IOToken_Parse error %d\n", IOToken_Rc);
				return 0;
			}

			if (OutTokenDscr_p->ErrorCode != 0) {
				/* Packet process error */
				CRYPTO_ERR("Result descriptor error 0x%x\n",
					OutTokenDscr_p->ErrorCode);
				return 0;
			}

			/* packet received */
			return Counter;
		}

		/* Wait for MTK_EIP197_PKT_GET_TIMEOUT_MS milliseconds */
		udelay(MTK_EIP197_PKT_GET_TIMEOUT_MS);
		LoopCounter--;
	}

	/* IO error (timeout, not result packet received) */
	return 0;
}

unsigned int crypto_pe_get_one(IOToken_Output_Dscr_t *const OutTokenDscr_p,
			       u32 *OutTokenData_p,
			       PEC_ResultDescriptor_t *RD_p)
{
	int IOToken_Rc;
	unsigned int Counter = 0;
	PEC_Status_t pecres;

	ZEROINIT(*OutTokenDscr_p);
	ZEROINIT(*RD_p);

	RD_p->OutputToken_p = OutTokenData_p;

	/* Try to get the processed packet from the driver */
	pecres = PEC_Packet_Get(PEC_INTERFACE_ID, RD_p, 1, &Counter);
	if (pecres != PEC_STATUS_OK) {
		/* IO error */
		CRYPTO_ERR("PEC_Packet_Get error %d\n", pecres);
		return 0;
	}

	if (Counter) {
		IOToken_Rc = IOToken_Parse(OutTokenData_p, OutTokenDscr_p);
		if (IOToken_Rc < 0) {
			/* IO error */
			CRYPTO_ERR("IOToken_Parse error %d\n", IOToken_Rc);
			return 0;
		}
		if (OutTokenDscr_p->ErrorCode != 0) {
			/* Packet process error */
			CRYPTO_ERR("Result descriptor error 0x%x\n",
				OutTokenDscr_p->ErrorCode);
			return 0;
		}
		/* packet received */
		return Counter;
	}

	/* IO error (timeout, not result packet received) */
	return 0;
}

SABuilder_Crypto_Mode_t lookaside_match_alg_mode(enum mtk_crypto_cipher_mode mode)
{
	switch (mode) {
	case MTK_CRYPTO_MODE_CBC:
		return SAB_CRYPTO_MODE_CBC;
	case MTK_CRYPTO_MODE_ECB:
		return SAB_CRYPTO_MODE_ECB;
	case MTK_CRYPTO_MODE_OFB:
		return SAB_CRYPTO_MODE_OFB;
	case MTK_CRYPTO_MODE_CFB:
		return SAB_CRYPTO_MODE_CFB;
	case MTK_CRYPTO_MODE_CTR:
		return SAB_CRYPTO_MODE_CTR;
	case MTK_CRYPTO_MODE_GCM:
		return SAB_CRYPTO_MODE_GCM;
	case MTK_CRYPTO_MODE_GMAC:
		return SAB_CRYPTO_MODE_GMAC;
	case MTK_CRYPTO_MODE_CCM:
		return SAB_CRYPTO_MODE_CCM;
	default:
		return SAB_CRYPTO_MODE_BASIC;
	}
}

SABuilder_Crypto_t lookaside_match_alg_name(enum mtk_crypto_alg alg)
{
	switch (alg) {
	case MTK_CRYPTO_AES:
		return SAB_CRYPTO_AES;
	case MTK_CRYPTO_DES:
		return SAB_CRYPTO_DES;
	case MTK_CRYPTO_3DES:
		return SAB_CRYPTO_3DES;
	default:
		return SAB_CRYPTO_NULL;
	}
}

SABuilder_Auth_t aead_hash_match(enum mtk_crypto_alg alg)
{
	switch (alg) {
	case MTK_CRYPTO_ALG_SHA1:
		return SAB_AUTH_HMAC_SHA1;
	case MTK_CRYPTO_ALG_SHA224:
		return SAB_AUTH_HMAC_SHA2_224;
	case MTK_CRYPTO_ALG_SHA256:
		return SAB_AUTH_HMAC_SHA2_256;
	case MTK_CRYPTO_ALG_SHA384:
		return SAB_AUTH_HMAC_SHA2_384;
	case MTK_CRYPTO_ALG_SHA512:
		return SAB_AUTH_HMAC_SHA2_512;
	case MTK_CRYPTO_ALG_MD5:
		return SAB_AUTH_HMAC_MD5;
	case MTK_CRYPTO_ALG_GCM:
		return SAB_AUTH_AES_GCM;
	case MTK_CRYPTO_ALG_GMAC:
		return SAB_AUTH_AES_GMAC;
	case MTK_CRYPTO_ALG_CCM:
		return SAB_AUTH_AES_CCM;
	default:
		return SAB_AUTH_NULL;
	}
}

void mtk_crypto_interrupt_handler(void)
{
	struct mtk_crypto_result *rd;
	struct mtk_crypto_context *ctx;
	IOToken_Output_Dscr_t OutTokenDscr;
	PEC_ResultDescriptor_t Res;
	uint32_t OutputToken[IOTOKEN_OUT_WORD_COUNT];
	int ret = 0;

	while (true) {
		if (list_empty(&result_list))
			return;
		rd = list_first_entry(&result_list, struct mtk_crypto_result, list);

		if (crypto_pe_get_one(&OutTokenDscr, OutputToken, &Res) < 1) {
			PEC_NotifyFunction_t CBFunc;

			CBFunc = mtk_crypto_interrupt_handler;
			if (OutTokenDscr.ErrorCode == 0) {
				PEC_ResultNotify_Request(PEC_INTERFACE_ID, CBFunc, 1);
				return;
			} else if (OutTokenDscr.ErrorCode & BIT(9)) {
				ret = -EBADMSG;
			} else if (OutTokenDscr.ErrorCode == 0x4003) {
				ret = 0;
			} else
				ret = 1;

			CRYPTO_ERR("error from crypto_pe_get_one: %d\n", ret);
		}

		ctx = crypto_tfm_ctx(rd->async->tfm);
		ret = ctx->handle_result(rd, ret);

		spin_lock_bh(&add_lock);
		list_del(&rd->list);
		spin_unlock_bh(&add_lock);
		kfree(rd);
	}
}

int crypto_aead_cipher(struct crypto_async_request *async, struct mtk_crypto_cipher_req *mtk_req,
		       struct scatterlist *src, struct scatterlist *dst, unsigned int cryptlen,
		       unsigned int assoclen, unsigned int digestsize, u8 *iv, unsigned int ivsize)
{
	struct mtk_crypto_cipher_ctx *ctx = crypto_tfm_ctx(async->tfm);
	struct mtk_crypto_result *result;
	struct scatterlist *sg;
	unsigned int totlen_src;
	unsigned int totlen_dst;
	unsigned int src_pkt =  cryptlen + assoclen;
	unsigned int pass_assoc = 0;
	int pass_id;
	int rc;
	int i;
	SABuilder_Params_t params;
	SABuilder_Params_Basic_t ProtocolParams;
	unsigned int SAWords = 0;

	DMABuf_Status_t DMAStatus;
	DMABuf_Properties_t DMAProperties = {0, 0, 0, 0};
	DMABuf_HostAddress_t SAHostAddress;
	DMABuf_HostAddress_t TokenHostAddress;
	DMABuf_HostAddress_t PktHostAddress;

	DMABuf_Handle_t SAHandle = {0};
	DMABuf_Handle_t TokenHandle = {0};
	DMABuf_Handle_t SrcSGListHandle = {0};
	DMABuf_Handle_t DstSGListHandle = {0};

	unsigned int TCRWords = 0;
	void *TCRData = 0;
	unsigned int TokenWords = 0;
	unsigned int TokenHeaderWord;
	unsigned int TokenMaxWords = 0;

	TokenBuilder_Params_t TokenParams;
	PEC_CommandDescriptor_t Cmd;
	PEC_NotifyFunction_t CBFunc;
	unsigned int count;

	IOToken_Input_Dscr_t InTokenDscr;
	IOToken_Output_Dscr_t OutTokenDscr;
	uint32_t InputToken[IOTOKEN_IN_WORD_COUNT];
	void *InTokenDscrExt_p = NULL;
	uint8_t gcm_iv[16] = {0};
	uint8_t *aad = NULL;

#ifdef CRYPTO_IOTOKEN_EXT
	IOToken_Input_Dscr_Ext_t InTokenDscrExt;

	ZEROINIT(InTokenDscrExt);
	InTokenDscrExt_p = &InTokenDscrExt;
#endif
	ZEROINIT(InTokenDscr);
	ZEROINIT(OutTokenDscr);

	/* Init SA */
	if (mtk_req->direction == MTK_CRYPTO_ENCRYPT) {
		totlen_src = cryptlen + assoclen;
		totlen_dst = totlen_src + digestsize;
		rc = SABuilder_Init_Basic(&params, &ProtocolParams, SAB_DIRECTION_OUTBOUND);
	} else {
		totlen_src = cryptlen + assoclen;
		totlen_dst = totlen_src - digestsize;
		rc = SABuilder_Init_Basic(&params, &ProtocolParams, SAB_DIRECTION_INBOUND);
	}
	if (rc) {
		CRYPTO_ERR("SABuilder_Init_Basic failed: %d\n", rc);
		goto error_exit;
	}

	/* Build SA */
	params.CryptoAlgo = lookaside_match_alg_name(ctx->alg);
	params.CryptoMode = lookaside_match_alg_mode(ctx->mode);
	params.KeyByteCount = ctx->key_len;
	params.Key_p = (uint8_t *) ctx->key;
	if (params.CryptoMode == SAB_CRYPTO_MODE_GCM && ctx->aead == EIP197_AEAD_TYPE_IPSEC_ESP) {
		params.Nonce_p = (uint8_t *) &ctx->nonce;
		params.IVSrc = SAB_IV_SRC_TOKEN;
		params.flags |= SAB_FLAG_COPY_IV;
		memcpy(gcm_iv, &ctx->nonce, 4);
		memcpy(gcm_iv + 4, iv, ivsize);
		gcm_iv[15] = 1;
	} else if (params.CryptoMode == SAB_CRYPTO_MODE_GMAC) {
		params.Nonce_p = (uint8_t *) &ctx->nonce;
		params.IVSrc = SAB_IV_SRC_TOKEN;
		memcpy(gcm_iv, &ctx->nonce, 4);
		memcpy(gcm_iv + 4, iv, ivsize);
		gcm_iv[15] = 1;
	} else if (params.CryptoMode == SAB_CRYPTO_MODE_GCM) {
		params.IVSrc = SAB_IV_SRC_TOKEN;
		memcpy(gcm_iv, iv, ivsize);
		gcm_iv[15] = 1;
	} else if (params.CryptoMode == SAB_CRYPTO_MODE_CCM) {
		params.IVSrc = SAB_IV_SRC_SA;
		params.Nonce_p = (uint8_t *) &ctx->nonce + 1;
		params.IV_p = iv;
	} else {
		params.IVSrc = SAB_IV_SRC_SA;
		params.IV_p = iv;
	}

	if (params.CryptoMode == SAB_CRYPTO_MODE_CTR)
		params.Nonce_p = (uint8_t *) &ctx->nonce;

	params.AuthAlgo = aead_hash_match(ctx->hash_alg);
	params.AuthKey1_p = (uint8_t *) ctx->ipad;
	params.AuthKey2_p = (uint8_t *) ctx->opad;

	ProtocolParams.ICVByteCount = digestsize;

	rc = SABuilder_GetSizes(&params, &SAWords, NULL, NULL);
	if (rc) {
		CRYPTO_ERR("SA not created because of size errors: %d\n", rc);
		goto error_remove_sg;
	}

	DMAProperties.fCached = true;
	DMAProperties.Alignment = MTK_EIP197_INLINE_DMA_ALIGNMENT_BYTE_COUNT;
	DMAProperties.Bank = MTK_EIP197_INLINE_BANK_TRANSFORM;
	DMAProperties.Size = MAX(4*SAWords, 256);

	DMAStatus = DMABuf_Alloc(DMAProperties, &SAHostAddress, &SAHandle);
	if (DMAStatus != DMABUF_STATUS_OK) {
		rc = 1;
		CRYPTO_ERR("Allocation of SA failed: %d\n", DMAStatus);
		goto error_remove_sg;
	}

	rc = SABuilder_BuildSA(&params, (u32 *)SAHostAddress.p, NULL, NULL);
	if (rc) {
		CRYPTO_ERR("SA not created because of errors: %d\n", rc);
		goto error_remove_sg;
	}

	/* Check dst buffer has enough size */
	mtk_req->nr_src = sg_nents_for_len(src, totlen_src);
	mtk_req->nr_dst = sg_nents_for_len(dst, totlen_dst);

	if (src == dst) {
		mtk_req->nr_src = max(mtk_req->nr_src, mtk_req->nr_dst);
		mtk_req->nr_dst = mtk_req->nr_src;
		if (unlikely((totlen_src || totlen_dst) && (mtk_req->nr_src <= 0))) {
			CRYPTO_ERR("In-place buffer not large enough\n");
			return -EINVAL;
		}
		dma_map_sg(crypto_dev, src, mtk_req->nr_src, DMA_BIDIRECTIONAL);
	} else {
		if (unlikely(totlen_src && (mtk_req->nr_src <= 0))) {
			CRYPTO_ERR("Source buffer not large enough\n");
			return -EINVAL;
		}
		dma_map_sg(crypto_dev, src, mtk_req->nr_src, DMA_TO_DEVICE);

		if (unlikely(totlen_dst && (mtk_req->nr_dst <= 0))) {
			CRYPTO_ERR("Dest buffer not large enough\n");
			dma_unmap_sg(crypto_dev, src, mtk_req->nr_src, DMA_TO_DEVICE);
			return -EINVAL;
		}
		dma_map_sg(crypto_dev, dst, mtk_req->nr_dst, DMA_FROM_DEVICE);
	}

	if (params.CryptoMode == SAB_CRYPTO_MODE_CCM ||
		(params.CryptoMode == SAB_CRYPTO_MODE_GCM &&
		 ctx->aead == EIP197_AEAD_TYPE_IPSEC_ESP)) {

		aad = kmalloc(assoclen, GFP_KERNEL);
		if (!aad)
			goto error_remove_sg;
		sg_copy_to_buffer(src, mtk_req->nr_src, aad, assoclen);
		src_pkt -= assoclen;
		pass_assoc = assoclen;
	}

	/* Assign sg list */
	rc = PEC_SGList_Create(MAX(mtk_req->nr_src, 1), &SrcSGListHandle);
	if (rc != PEC_STATUS_OK) {
		CRYPTO_ERR("PEC_SGList_Create src failed with rc = %d\n", rc);
		goto error_remove_sg;
	}

	pass_id = 0;
	DMAProperties.fCached = true;
	DMAProperties.Alignment = MTK_EIP197_INLINE_DMA_ALIGNMENT_BYTE_COUNT;
	DMAProperties.Bank = MTK_EIP197_INLINE_BANK_PACKET;
	for_each_sg(src, sg, mtk_req->nr_src, i) {
		int len = sg_dma_len(sg);
		DMABuf_Handle_t sg_handle;
		DMABuf_HostAddress_t host;

		if (totlen_src < len)
			len = totlen_src;

		if (pass_assoc) {
			if (pass_assoc >= len) {
				pass_assoc -= len;
				pass_id++;
				continue;
			}
			DMAProperties.Size = MAX(len - pass_assoc, 1);
			rc = DMABuf_Particle_Alloc(DMAProperties, sg_dma_address(sg) + pass_assoc,
							&host, &sg_handle);
			if (rc != DMABUF_STATUS_OK) {
				CRYPTO_ERR("DMABuf_Particle_Alloc failed rc = %d\n", rc);
				goto error_remove_sg;
			}
			rc = PEC_SGList_Write(SrcSGListHandle, i - pass_id, sg_handle,
						len - pass_assoc);
			if (rc != PEC_STATUS_OK)
				pr_notice("PEC_SGList_Write failed rc = %d\n", rc);
			pass_assoc = 0;
		} else {
			DMAProperties.Size = MAX(len, 1);
			rc = DMABuf_Particle_Alloc(DMAProperties, sg_dma_address(sg),
							&host, &sg_handle);
			if (rc != DMABUF_STATUS_OK) {
				CRYPTO_ERR("DMABuf_Particle_Alloc failed rc = %d\n", rc);
				goto error_remove_sg;
			}

			rc = PEC_SGList_Write(SrcSGListHandle, i - pass_id, sg_handle, len);
			if (rc != PEC_STATUS_OK)
				pr_notice("PEC_SGList_Write failed rc = %d\n", rc);
		}

		totlen_src -= len;
		if (!totlen_src)
			break;
	}

	/* Alloc sg list for result */
	rc = PEC_SGList_Create(MAX(mtk_req->nr_dst, 1), &DstSGListHandle);
	if (rc != PEC_STATUS_OK) {
		CRYPTO_ERR("PEC_SGList_Create dst failed with rc = %d\n", rc);
		goto error_remove_sg;
	}

	for_each_sg(dst, sg, mtk_req->nr_dst, i) {
		int len = sg_dma_len(sg);
		DMABuf_Handle_t sg_handle;
		DMABuf_HostAddress_t host;

		if (len > totlen_dst)
			len = totlen_dst;

		DMAProperties.Size = MAX(len, 1);
		rc = DMABuf_Particle_Alloc(DMAProperties, sg_dma_address(sg), &host, &sg_handle);
		if (rc != DMABUF_STATUS_OK) {
			CRYPTO_ERR("DMABuf_Particle_Alloc failed rc = %d\n", rc);
			goto error_remove_sg;
		}
		rc = PEC_SGList_Write(DstSGListHandle, i, sg_handle, len);
		if (rc != PEC_STATUS_OK)
			pr_notice("PEC_SGList_Write failed rc = %d\n", rc);

		if (unlikely(!len))
			break;
		totlen_dst -= len;
	}

	/* Build Token */
	rc = TokenBuilder_GetContextSize(&params, &TCRWords);
	if (rc) {
		CRYPTO_ERR("TokenBuilder_GetContextSize returned errors: %d\n", rc);
		goto error_remove_sg;
	}

	TCRData = kmalloc(4 * TCRWords, GFP_KERNEL);
	if (!TCRData) {
		rc = 1;
		CRYPTO_ERR("Allocation of TCR failed\n");
		goto error_remove_sg;
	}

	rc = TokenBuilder_BuildContext(&params, TCRData);
	if (rc) {
		CRYPTO_ERR("TokenBuilder_BuildContext failed: %d\n", rc);
		goto error_remove_sg;
	}

	rc = TokenBuilder_GetSize(TCRData, &TokenMaxWords);
	if (rc) {
		CRYPTO_ERR("TokenBuilder_GetSize failed: %d\n", rc);
		goto error_remove_sg;
	}

	DMAProperties.fCached = true;
	DMAProperties.Alignment = MTK_EIP197_INLINE_DMA_ALIGNMENT_BYTE_COUNT;
	DMAProperties.Bank = MTK_EIP197_INLINE_BANK_TOKEN;
	DMAProperties.Size = 4*TokenMaxWords;

	DMAStatus = DMABuf_Alloc(DMAProperties, &TokenHostAddress, &TokenHandle);
	if (DMAStatus != DMABUF_STATUS_OK) {
		rc = 1;
		CRYPTO_ERR("Allocation of token builder failed: %d\n", DMAStatus);
		goto error_remove_sg;
	}

	rc = PEC_SA_Register(PEC_INTERFACE_ID, SAHandle, DMABuf_NULLHandle,
				DMABuf_NULLHandle);
	if (rc != PEC_STATUS_OK) {
		CRYPTO_ERR("PEC_SA_Register failed: %d\n", rc);
		goto error_remove_sg;
	}

	ZEROINIT(TokenParams);

	if (params.CryptoMode == SAB_CRYPTO_MODE_GCM || params.CryptoMode == SAB_CRYPTO_MODE_GMAC)
		TokenParams.IV_p = gcm_iv;

	if ((params.CryptoMode == SAB_CRYPTO_MODE_GCM && ctx->aead == EIP197_AEAD_TYPE_IPSEC_ESP) ||
	     params.CryptoMode == SAB_CRYPTO_MODE_CCM) {
		TokenParams.AdditionalValue = assoclen - ivsize;
		TokenParams.AAD_p = aad;
	} else if (params.CryptoMode != SAB_CRYPTO_MODE_GMAC)
		TokenParams.AdditionalValue = assoclen;


	PktHostAddress.p = kmalloc(sizeof(uint8_t), GFP_KERNEL);
	rc = TokenBuilder_BuildToken(TCRData, (uint8_t *)PktHostAddress.p, src_pkt,
					&TokenParams, (uint32_t *)TokenHostAddress.p,
					&TokenWords, &TokenHeaderWord);
	kfree(PktHostAddress.p);
	if (rc != TKB_STATUS_OK) {
		CRYPTO_ERR("Token builder failed: %d\n", rc);
		goto error_exit_unregister;
	}

	ZEROINIT(Cmd);
	Cmd.Token_Handle = TokenHandle;
	Cmd.Token_WordCount = TokenWords;
	Cmd.SrcPkt_Handle = SrcSGListHandle;
	Cmd.SrcPkt_ByteCount = src_pkt;
	Cmd.DstPkt_Handle = DstSGListHandle;
	Cmd.SA_Handle1 = SAHandle;
	Cmd.SA_Handle2 = DMABuf_NULLHandle;

	#if defined(CRYPTO_IOTOKEN_EXT)
	InTokenDscrExt.HW_Services = IOTOKEN_CMD_PKT_LAC;
#endif
	InTokenDscr.TknHdrWordInit = TokenHeaderWord;

	if (!crypto_iotoken_create(&InTokenDscr,
				   InTokenDscrExt_p,
				   InputToken,
				   &Cmd)) {
		rc = 1;
		goto error_exit_unregister;
	}

	rc = PEC_Packet_Put(PEC_INTERFACE_ID, &Cmd, 1, &count);
	if (rc != PEC_STATUS_OK && count != 1)
		goto error_exit_unregister;

	result = kmalloc(sizeof(struct mtk_crypto_result), GFP_KERNEL);
	if (!result) {
		rc = 1;
		CRYPTO_ERR("No memory for result\n");
		goto error_exit_unregister;
	}
	INIT_LIST_HEAD(&result->list);
	result->eip.sa = SAHandle.p;
	result->eip.token = TokenHandle.p;
	result->eip.token_context = TCRData;
	result->eip.pkt_handle = SrcSGListHandle.p;
	result->async = async;
	result->dst = DstSGListHandle.p;

	spin_lock_bh(&add_lock);
	list_add_tail(&result->list, &result_list);
	spin_unlock_bh(&add_lock);
	CBFunc = mtk_crypto_interrupt_handler;
	rc = PEC_ResultNotify_Request(PEC_INTERFACE_ID, CBFunc, 1);
	if (rc != PEC_STATUS_OK) {
		CRYPTO_ERR("PEC_ResultNotify_Request failed with rc = %d\n", rc);
		goto error_exit_unregister;
	}

	return rc;

error_exit_unregister:
	PEC_SA_UnRegister(PEC_INTERFACE_ID, SAHandle, DMABuf_NULLHandle,
				DMABuf_NULLHandle);
error_remove_sg:
	if (src == dst) {
		dma_unmap_sg(crypto_dev, src, mtk_req->nr_src, DMA_BIDIRECTIONAL);
	} else {
		dma_unmap_sg(crypto_dev, src, mtk_req->nr_src, DMA_TO_DEVICE);
		dma_unmap_sg(crypto_dev, dst, mtk_req->nr_dst, DMA_FROM_DEVICE);
	}

	if (aad != NULL)
		kfree(aad);

	crypto_free_sglist(SrcSGListHandle.p);
	crypto_free_sglist(DstSGListHandle.p);

error_exit:
	DMABuf_Release(SAHandle);
	DMABuf_Release(TokenHandle);

	if (TCRData != NULL)
		kfree(TCRData);

	return rc;
}

int crypto_basic_cipher(struct crypto_async_request *async, struct mtk_crypto_cipher_req *mtk_req,
			struct scatterlist *src, struct scatterlist *dst, unsigned int cryptlen,
			unsigned int assoclen, unsigned int digestsize, u8 *iv, unsigned int ivsize)
{
	struct mtk_crypto_cipher_ctx *ctx = crypto_tfm_ctx(async->tfm);
	struct skcipher_request *areq = skcipher_request_cast(async);
	struct crypto_skcipher *skcipher = crypto_skcipher_reqtfm(areq);
	struct mtk_crypto_result *result;
	struct scatterlist *sg;
	unsigned int totlen_src = cryptlen + assoclen;
	unsigned int totlen_dst = totlen_src;
	unsigned int blksize = crypto_skcipher_blocksize(skcipher);
	int rc;
	int i;
	SABuilder_Params_t params;
	SABuilder_Params_Basic_t ProtocolParams;
	unsigned int SAWords = 0;

	DMABuf_Status_t DMAStatus;
	DMABuf_Properties_t DMAProperties = {0, 0, 0, 0};
	DMABuf_HostAddress_t SAHostAddress;
	DMABuf_HostAddress_t TokenHostAddress;
	DMABuf_HostAddress_t PktHostAddress;

	DMABuf_Handle_t SAHandle = {0};
	DMABuf_Handle_t TokenHandle = {0};
	DMABuf_Handle_t SrcSGListHandle = {0};
	DMABuf_Handle_t DstSGListHandle = {0};

	unsigned int TCRWords = 0;
	void *TCRData = 0;
	unsigned int TokenWords = 0;
	unsigned int TokenHeaderWord;
	unsigned int TokenMaxWords = 0;

	TokenBuilder_Params_t TokenParams;
	PEC_CommandDescriptor_t Cmd;
	unsigned int count;

	IOToken_Input_Dscr_t InTokenDscr;
	IOToken_Output_Dscr_t OutTokenDscr;
	uint32_t InputToken[IOTOKEN_IN_WORD_COUNT];
	void *InTokenDscrExt_p = NULL;
	PEC_NotifyFunction_t CBFunc;

#ifdef CRYPTO_IOTOKEN_EXT
	IOToken_Input_Dscr_Ext_t InTokenDscrExt;

	ZEROINIT(InTokenDscrExt);
	InTokenDscrExt_p = &InTokenDscrExt;
#endif
	ZEROINIT(InTokenDscr);
	ZEROINIT(OutTokenDscr);

	/* If the data is not aligned with block size, return invalid */
	if (!IS_ALIGNED(cryptlen, blksize))
		return -EINVAL;

	/* Init SA */
	if (mtk_req->direction == MTK_CRYPTO_ENCRYPT)
		rc = SABuilder_Init_Basic(&params, &ProtocolParams, SAB_DIRECTION_OUTBOUND);
	else
		rc = SABuilder_Init_Basic(&params, &ProtocolParams, SAB_DIRECTION_INBOUND);
	if (rc) {
		CRYPTO_ERR("SABuilder_Init_Basic failed: %d\n", rc);
		goto error_exit;
	}

	/* Build SA */
	params.CryptoAlgo = lookaside_match_alg_name(ctx->alg);
	params.CryptoMode = lookaside_match_alg_mode(ctx->mode);
	params.KeyByteCount = ctx->key_len;
	params.Key_p = (uint8_t *) ctx->key;
	params.IVSrc = SAB_IV_SRC_SA;
	if (params.CryptoMode == SAB_CRYPTO_MODE_CTR)
		params.Nonce_p = (uint8_t *) &ctx->nonce;
	params.IV_p = iv;

	rc = SABuilder_GetSizes(&params, &SAWords, NULL, NULL);
	if (rc) {
		CRYPTO_ERR("SA not created because of size errors: %d\n", rc);
		goto error_exit;
	}

	DMAProperties.fCached = true;
	DMAProperties.Alignment = MTK_EIP197_INLINE_DMA_ALIGNMENT_BYTE_COUNT;
	DMAProperties.Bank = MTK_EIP197_INLINE_BANK_TRANSFORM;
	DMAProperties.Size = MAX(4*SAWords, 256);

	DMAStatus = DMABuf_Alloc(DMAProperties, &SAHostAddress, &SAHandle);
	if (DMAStatus != DMABUF_STATUS_OK) {
		rc = 1;
		CRYPTO_ERR("Allocation of SA failed: %d\n", DMAStatus);
		goto error_exit;
	}

	rc = SABuilder_BuildSA(&params, (u32 *)SAHostAddress.p, NULL, NULL);
	if (rc) {
		CRYPTO_ERR("SA not created because of errors: %d\n", rc);
		goto error_exit;
	}

	/* Build Token */
	rc = TokenBuilder_GetContextSize(&params, &TCRWords);
	if (rc) {
		CRYPTO_ERR("TokenBuilder_GetContextSize returned errors: %d\n", rc);
		goto error_exit;
	}

	TCRData = kmalloc(4 * TCRWords, GFP_KERNEL);
	if (!TCRData) {
		rc = 1;
		CRYPTO_ERR("Allocation of TCR failed\n");
		goto error_exit;
	}

	rc = TokenBuilder_BuildContext(&params, TCRData);
	if (rc) {
		CRYPTO_ERR("TokenBuilder_BuildContext failed: %d\n", rc);
		goto error_exit;
	}

	rc = TokenBuilder_GetSize(TCRData, &TokenMaxWords);
	if (rc) {
		CRYPTO_ERR("TokenBuilder_GetSize failed: %d\n", rc);
		goto error_exit;
	}

	DMAProperties.fCached = true;
	DMAProperties.Alignment = MTK_EIP197_INLINE_DMA_ALIGNMENT_BYTE_COUNT;
	DMAProperties.Bank = MTK_EIP197_INLINE_BANK_TOKEN;
	DMAProperties.Size = 4*TokenMaxWords;

	DMAStatus = DMABuf_Alloc(DMAProperties, &TokenHostAddress, &TokenHandle);
	if (DMAStatus != DMABUF_STATUS_OK) {
		rc = 1;
		CRYPTO_ERR("Allocation of token builder failed: %d\n", DMAStatus);
		goto error_exit;
	}

	rc = PEC_SA_Register(PEC_INTERFACE_ID, SAHandle, DMABuf_NULLHandle,
				DMABuf_NULLHandle);
	if (rc != PEC_STATUS_OK) {
		CRYPTO_ERR("PEC_SA_Register failed: %d\n", rc);
		goto error_exit;
	}

	/* Check buffer has enough size for output */
	mtk_req->nr_src = sg_nents_for_len(src, totlen_src);
	mtk_req->nr_dst = sg_nents_for_len(dst, totlen_dst);

	if (src == dst) {
		mtk_req->nr_src = max(mtk_req->nr_src, mtk_req->nr_dst);
		mtk_req->nr_dst = mtk_req->nr_src;
		if (unlikely((totlen_src || totlen_dst) && (mtk_req->nr_src <= 0))) {
			CRYPTO_ERR("In-place buffer not large enough\n");
			return -EINVAL;
		}
		dma_map_sg(crypto_dev, src, mtk_req->nr_src, DMA_BIDIRECTIONAL);
	} else {
		if (unlikely(totlen_src && (mtk_req->nr_src <= 0))) {
			CRYPTO_ERR("Source buffer not large enough\n");
			return -EINVAL;
		}
		dma_map_sg(crypto_dev, src, mtk_req->nr_src, DMA_TO_DEVICE);

		if (unlikely(totlen_dst && (mtk_req->nr_dst <= 0))) {
			CRYPTO_ERR("Dest buffer not large enough\n");
			dma_unmap_sg(crypto_dev, src, mtk_req->nr_src, DMA_TO_DEVICE);
			return -EINVAL;
		}
		dma_map_sg(crypto_dev, dst, mtk_req->nr_dst, DMA_FROM_DEVICE);
	}

	rc = PEC_SGList_Create(MAX(mtk_req->nr_src, 1), &SrcSGListHandle);
	if (rc != PEC_STATUS_OK) {
		CRYPTO_ERR("PEC_SGList_Create src failed with rc = %d\n", rc);
		goto error_remove_sg;
	}

	DMAProperties.fCached = true;
	DMAProperties.Alignment = MTK_EIP197_INLINE_DMA_ALIGNMENT_BYTE_COUNT;
	DMAProperties.Bank = MTK_EIP197_INLINE_BANK_PACKET;
	for_each_sg(src, sg, mtk_req->nr_src, i) {
		int len = sg_dma_len(sg);
		DMABuf_Handle_t sg_handle;
		DMABuf_HostAddress_t host;

		if (totlen_src < len)
			len = totlen_src;

		DMAProperties.Size = MAX(len, 1);
		rc = DMABuf_Particle_Alloc(DMAProperties, sg_dma_address(sg), &host, &sg_handle);
		if (rc != DMABUF_STATUS_OK) {
			CRYPTO_ERR("DMABuf_Particle_Alloc failed rc = %d\n", rc);
			goto error_remove_sg;
		}
		rc = PEC_SGList_Write(SrcSGListHandle, i, sg_handle, len);
		if (rc != PEC_STATUS_OK)
			pr_notice("PEC_SGList_Write failed rc = %d\n", rc);

		totlen_src -= len;
		if (!totlen_src)
			break;
	}

	rc = PEC_SGList_Create(MAX(mtk_req->nr_dst, 1), &DstSGListHandle);
	if (rc != PEC_STATUS_OK) {
		CRYPTO_ERR("PEC_SGList_Create dst failed with rc = %d\n", rc);
		goto error_remove_sg;
	}

	for_each_sg(dst, sg, mtk_req->nr_dst, i) {
		int len = sg_dma_len(sg);
		DMABuf_Handle_t sg_handle;
		DMABuf_HostAddress_t host;

		if (len > totlen_dst)
			len = totlen_dst;

		DMAProperties.Size = MAX(len, 1);
		rc = DMABuf_Particle_Alloc(DMAProperties, sg_dma_address(sg), &host, &sg_handle);
		if (rc != DMABUF_STATUS_OK) {
			CRYPTO_ERR("DMABuf_Particle_Alloc failed rc = %d\n", rc);
			goto error_remove_sg;
		}
		rc = PEC_SGList_Write(DstSGListHandle, i, sg_handle, len);

		if (unlikely(!len))
			break;
		totlen_dst -= len;
	}

	if (params.CryptoMode == SAB_CRYPTO_MODE_CBC &&
			mtk_req->direction == MTK_CRYPTO_DECRYPT)
		sg_pcopy_to_buffer(src, mtk_req->nr_src, iv, ivsize, cryptlen - ivsize);

	PktHostAddress.p = kmalloc(sizeof(uint8_t), GFP_KERNEL);
	ZEROINIT(TokenParams);
	rc = TokenBuilder_BuildToken(TCRData, (uint8_t *)PktHostAddress.p, cryptlen,
					&TokenParams, (uint32_t *)TokenHostAddress.p,
					&TokenWords, &TokenHeaderWord);
	kfree(PktHostAddress.p);
	if (rc != TKB_STATUS_OK) {
		CRYPTO_ERR("Token builder failed: %d\n", rc);
		goto error_remove_sg;
	}

	ZEROINIT(Cmd);
	Cmd.Token_Handle = TokenHandle;
	Cmd.Token_WordCount = TokenWords;
	Cmd.SrcPkt_Handle = SrcSGListHandle;
	Cmd.SrcPkt_ByteCount = cryptlen;
	Cmd.DstPkt_Handle = DstSGListHandle;
	Cmd.SA_Handle1 = SAHandle;
	Cmd.SA_Handle2 = DMABuf_NULLHandle;

#if defined(CRYPTO_IOTOKEN_EXT)
	InTokenDscrExt.HW_Services = IOTOKEN_CMD_PKT_LAC;
#endif
	InTokenDscr.TknHdrWordInit = TokenHeaderWord;

	if (!crypto_iotoken_create(&InTokenDscr,
				   InTokenDscrExt_p,
				   InputToken,
				   &Cmd)) {
		rc = 1;
		goto error_remove_sg;
	}

	rc = PEC_Packet_Put(PEC_INTERFACE_ID, &Cmd, 1, &count);
	if (rc != PEC_STATUS_OK && count != 1) {
		rc = 1;
		CRYPTO_ERR("PEC_Packet_Put error: %d\n", rc);
		goto error_remove_sg;
	}

	result = kmalloc(sizeof(struct mtk_crypto_result), GFP_KERNEL);
	if (!result) {
		rc = 1;
		CRYPTO_ERR("No memory for result\n");
		goto error_remove_sg;
	}
	INIT_LIST_HEAD(&result->list);
	result->eip.sa = SAHandle.p;
	result->eip.token = TokenHandle.p;
	result->eip.token_context = TCRData;
	result->eip.pkt_handle = SrcSGListHandle.p;
	result->async = async;
	result->dst = DstSGListHandle.p;

	spin_lock_bh(&add_lock);
	list_add_tail(&result->list, &result_list);
	spin_unlock_bh(&add_lock);
	CBFunc = mtk_crypto_interrupt_handler;
	rc = PEC_ResultNotify_Request(PEC_INTERFACE_ID, CBFunc, 1);
	if (rc != PEC_STATUS_OK) {
		CRYPTO_ERR("PEC_ResultNotify_Request failed with rc = %d\n", rc);
		goto error_remove_sg;
	}
	return 0;

error_remove_sg:
	if (src == dst) {
		dma_unmap_sg(crypto_dev, src, mtk_req->nr_src, DMA_BIDIRECTIONAL);
	} else {
		dma_unmap_sg(crypto_dev, src, mtk_req->nr_src, DMA_TO_DEVICE);
		dma_unmap_sg(crypto_dev, dst, mtk_req->nr_dst, DMA_FROM_DEVICE);
	}

	crypto_free_sglist(SrcSGListHandle.p);
	crypto_free_sglist(DstSGListHandle.p);

	PEC_SA_UnRegister(PEC_INTERFACE_ID, SAHandle, DMABuf_NULLHandle,
				DMABuf_NULLHandle);

error_exit:
	DMABuf_Release(SAHandle);
	DMABuf_Release(TokenHandle);

	if (TCRData != NULL)
		kfree(TCRData);

	return rc;
}

SABuilder_Auth_t lookaside_match_hash(enum mtk_crypto_alg alg)
{
	switch (alg) {
	case MTK_CRYPTO_ALG_SHA1:
		return SAB_AUTH_HASH_SHA1;
	case MTK_CRYPTO_ALG_SHA224:
		return SAB_AUTH_HASH_SHA2_224;
	case MTK_CRYPTO_ALG_SHA256:
		return SAB_AUTH_HASH_SHA2_256;
	case MTK_CRYPTO_ALG_SHA384:
		return SAB_AUTH_HASH_SHA2_384;
	case MTK_CRYPTO_ALG_SHA512:
		return SAB_AUTH_HASH_SHA2_512;
	case MTK_CRYPTO_ALG_MD5:
		return SAB_AUTH_HASH_MD5;
	case MTK_CRYPTO_ALG_XCBC:
		return SAB_AUTH_AES_XCBC_MAC;
	case MTK_CRYPTO_ALG_CMAC_128:
		return SAB_AUTH_AES_CMAC_128;
	case MTK_CRYPTO_ALG_CMAC_192:
		return SAB_AUTH_AES_CMAC_192;
	case MTK_CRYPTO_ALG_CMAC_256:
		return SAB_AUTH_AES_CMAC_256;
	default:
		return SAB_AUTH_NULL;
	}
}

int crypto_ahash_token_req(struct crypto_async_request *async, struct mtk_crypto_ahash_req *mtk_req,
				uint8_t *Input_p, unsigned int InputByteCount, bool finish)
{
	struct mtk_crypto_result *result;

	DMABuf_Properties_t DMAProperties = {0, 0, 0, 0};
	DMABuf_HostAddress_t TokenHostAddress;
	DMABuf_HostAddress_t PktHostAddress;
	DMABuf_Status_t DMAStatus;

	DMABuf_Handle_t TokenHandle = {0};
	DMABuf_Handle_t PktHandle = {0};
	DMABuf_Handle_t SAHandle = {0};

	unsigned int TokenMaxWords = 0;
	unsigned int TokenHeaderWord;
	unsigned int TokenWords = 0;
	void *TCRData = 0;

	TokenBuilder_Params_t TokenParams;
	PEC_CommandDescriptor_t Cmd;
	PEC_NotifyFunction_t CBFunc;

	unsigned int count;
	int rc;

	u32 InputToken[IOTOKEN_IN_WORD_COUNT];
	IOToken_Output_Dscr_t OutTokenDscr;
	IOToken_Input_Dscr_t InTokenDscr;
	void *InTokenDscrExt_p = NULL;

#ifdef CRYPTO_IOTOKEN_EXT
	IOToken_Input_Dscr_Ext_t InTokenDscrExt;

	ZEROINIT(InTokenDscrExt);
	InTokenDscrExt_p = &InTokenDscrExt;
#endif
	ZEROINIT(InTokenDscr);
	ZEROINIT(OutTokenDscr);

	TCRData = mtk_req->token_context;
	rc = TokenBuilder_GetSize(TCRData, &TokenMaxWords);
	if (rc) {
		CRYPTO_ERR("TokenBuilder_GetSize failed: %d\n", rc);
		goto error_exit;
	}

	DMAProperties.fCached = true;
	DMAProperties.Alignment = MTK_EIP197_INLINE_DMA_ALIGNMENT_BYTE_COUNT;
	DMAProperties.Bank = MTK_EIP197_INLINE_BANK_TOKEN;
	DMAProperties.Size = 4*TokenMaxWords;

	DMAStatus = DMABuf_Alloc(DMAProperties, &TokenHostAddress, &TokenHandle);
	if (DMAStatus != DMABUF_STATUS_OK) {
		rc = 1;
		CRYPTO_ERR("Allocation of token builder failed: %d\n", DMAStatus);
		goto error_exit;
	}

	DMAProperties.fCached = true;
	DMAProperties.Alignment = MTK_EIP197_INLINE_DMA_ALIGNMENT_BYTE_COUNT;
	DMAProperties.Bank = MTK_EIP197_INLINE_BANK_PACKET;
	DMAProperties.Size = MAX(InputByteCount, mtk_req->digest_sz);

	DMAStatus = DMABuf_Alloc(DMAProperties, &PktHostAddress, &PktHandle);
	if (DMAStatus != DMABUF_STATUS_OK) {
		rc = 1;
		CRYPTO_ERR("Allocation of source packet buffer failed: %d\n",
			   DMAStatus);
		goto error_exit;
	}
	memcpy(PktHostAddress.p, Input_p, InputByteCount);

	ZEROINIT(TokenParams);
	TokenParams.PacketFlags |= TKB_PACKET_FLAG_HASHAPPEND;
	if (finish)
		TokenParams.PacketFlags |= TKB_PACKET_FLAG_HASHFINAL;

	rc = TokenBuilder_BuildToken(TCRData, (u8 *) PktHostAddress.p,
				     InputByteCount, &TokenParams,
				     (u32 *) TokenHostAddress.p,
				     &TokenWords, &TokenHeaderWord);
	if (rc != TKB_STATUS_OK) {
		CRYPTO_ERR("Token builder failed: %d\n", rc);
		goto error_exit_unregister;
	}

	SAHandle.p = mtk_req->sa_pointer;
	ZEROINIT(Cmd);
	Cmd.Token_Handle = TokenHandle;
	Cmd.Token_WordCount = TokenWords;
	Cmd.SrcPkt_Handle = PktHandle;
	Cmd.SrcPkt_ByteCount = InputByteCount;
	Cmd.DstPkt_Handle = PktHandle;
	Cmd.SA_Handle1 = SAHandle;
	Cmd.SA_Handle2 = DMABuf_NULLHandle;

#if defined(CRYPTO_IOTOKEN_EXT)
	InTokenDscrExt.HW_Services  = IOTOKEN_CMD_PKT_LAC;
#endif
	InTokenDscr.TknHdrWordInit = TokenHeaderWord;

	if (!crypto_iotoken_create(&InTokenDscr,
				   InTokenDscrExt_p,
				   InputToken,
				   &Cmd)) {
		rc = 1;
		goto error_exit_unregister;
	}

	rc = PEC_Packet_Put(PEC_INTERFACE_ID, &Cmd, 1, &count);
	if (rc != PEC_STATUS_OK && count != 1) {
		rc = 1;
		CRYPTO_ERR("PEC_Packet_Put error: %d\n", rc);
		goto error_exit_unregister;
	}

	result = kmalloc(sizeof(struct mtk_crypto_result), GFP_KERNEL);
	if (!result) {
		rc = 1;
		CRYPTO_ERR("No memory for result\n");
		goto error_exit_unregister;
	}
	INIT_LIST_HEAD(&result->list);
	result->eip.token = TokenHandle.p;
	result->eip.pkt_handle = PktHandle.p;
	result->async = async;
	result->dst = PktHostAddress.p;

	spin_lock_bh(&add_lock);
	list_add_tail(&result->list, &result_list);
	spin_unlock_bh(&add_lock);
	CBFunc = mtk_crypto_interrupt_handler;
	rc = PEC_ResultNotify_Request(PEC_INTERFACE_ID, CBFunc, 1);

	return rc;

error_exit_unregister:
	PEC_SA_UnRegister(PEC_INTERFACE_ID, SAHandle, DMABuf_NULLHandle,
				DMABuf_NULLHandle);

error_exit:
	DMABuf_Release(SAHandle);
	DMABuf_Release(TokenHandle);
	DMABuf_Release(PktHandle);

	if (TCRData != NULL)
		kfree(TCRData);

	return rc;
}

int crypto_ahash_aes_cbc(struct crypto_async_request *async, struct mtk_crypto_ahash_req *mtk_req,
				uint8_t *Input_p, unsigned int InputByteCount)
{
	struct mtk_crypto_ahash_ctx *ctx = crypto_tfm_ctx(async->tfm);
	struct mtk_crypto_result *result;
	SABuilder_Params_Basic_t ProtocolParams;
	SABuilder_Params_t params;
	unsigned int SAWords = 0;
	int rc;

	DMABuf_Properties_t DMAProperties = {0, 0, 0, 0};
	DMABuf_HostAddress_t TokenHostAddress;
	DMABuf_HostAddress_t PktHostAddress;
	DMABuf_HostAddress_t SAHostAddress;
	DMABuf_Status_t DMAStatus;

	DMABuf_Handle_t TokenHandle = {0};
	DMABuf_Handle_t PktHandle = {0};
	DMABuf_Handle_t SAHandle = {0};

	unsigned int TokenMaxWords = 0;
	unsigned int TokenHeaderWord;
	unsigned int TokenWords = 0;
	unsigned int TCRWords = 0;
	void *TCRData = 0;

	TokenBuilder_Params_t TokenParams;
	PEC_CommandDescriptor_t Cmd;
	PEC_NotifyFunction_t CBFunc;

	unsigned int count;
	int i;

	u32 InputToken[IOTOKEN_IN_WORD_COUNT];
	IOToken_Output_Dscr_t OutTokenDscr;
	IOToken_Input_Dscr_t InTokenDscr;
	void *InTokenDscrExt_p = NULL;

#ifdef CRYPTO_IOTOKEN_EXT
	IOToken_Input_Dscr_Ext_t InTokenDscrExt;

	ZEROINIT(InTokenDscrExt);
	InTokenDscrExt_p = &InTokenDscrExt;
#endif
	ZEROINIT(InTokenDscr);
	ZEROINIT(OutTokenDscr);

	if (!IS_ALIGNED(InputByteCount, 16)) {
		pr_notice("not aligned: %d\n", InputByteCount);
		return -EINVAL;
	}
	rc = SABuilder_Init_Basic(&params, &ProtocolParams, SAB_DIRECTION_OUTBOUND);
	if (rc) {
		CRYPTO_ERR("SABuilder_Init_Basic failed: %d\n", rc);
		goto error_exit;
	}

	params.CryptoAlgo = SAB_CRYPTO_AES;
	params.CryptoMode = SAB_CRYPTO_MODE_CBC;
	params.KeyByteCount = ctx->key_sz - 2 * AES_BLOCK_SIZE;
	params.Key_p = (uint8_t *) ctx->ipad + 2 * AES_BLOCK_SIZE;
	params.IVSrc = SAB_IV_SRC_SA;
	params.IV_p = (uint8_t *) mtk_req->state;

	if (ctx->alg == MTK_CRYPTO_ALG_XCBC) {
		for (i = 0; i < params.KeyByteCount; i = i + 4) {
			swap(params.Key_p[i], params.Key_p[i+3]);
			swap(params.Key_p[i+1], params.Key_p[i+2]);
		}
	}

	rc = SABuilder_GetSizes(&params, &SAWords, NULL, NULL);
	if (rc) {
		CRYPTO_ERR("SA not created because of size errors: %d\n", rc);
		goto error_exit;
	}

	DMAProperties.fCached = true;
	DMAProperties.Alignment = MTK_EIP197_INLINE_DMA_ALIGNMENT_BYTE_COUNT;
	DMAProperties.Bank = MTK_EIP197_INLINE_BANK_TRANSFORM;
	DMAProperties.Size = MAX(4*SAWords, 256);

	DMAStatus = DMABuf_Alloc(DMAProperties, &SAHostAddress, &SAHandle);
	if (DMAStatus != DMABUF_STATUS_OK) {
		rc = 1;
		CRYPTO_ERR("Allocation of SA failed: %d\n", DMAStatus);
		goto error_exit;
	}

	rc = SABuilder_BuildSA(&params, (u32 *)SAHostAddress.p, NULL, NULL);
	if (rc) {
		CRYPTO_ERR("SA not created because of errors: %d\n", rc);
		goto error_exit;
	}

	rc = TokenBuilder_GetContextSize(&params, &TCRWords);
	if (rc) {
		CRYPTO_ERR("TokenBuilder_GetContextSize returned errors: %d\n", rc);
		goto error_exit;
	}

	TCRData = kmalloc(4 * TCRWords, GFP_KERNEL);
	if (!TCRData) {
		rc = 1;
		CRYPTO_ERR("Allocation of TCR failed\n");
		goto error_exit;
	}

	rc = TokenBuilder_BuildContext(&params, TCRData);
	if (rc) {
		CRYPTO_ERR("TokenBuilder_BuildContext failed: %d\n", rc);
		goto error_exit;
	}

	rc = TokenBuilder_GetSize(TCRData, &TokenMaxWords);
	if (rc) {
		CRYPTO_ERR("TokenBuilder_GetSize failed: %d\n", rc);
		goto error_exit;
	}

	DMAProperties.fCached = true;
	DMAProperties.Alignment = MTK_EIP197_INLINE_DMA_ALIGNMENT_BYTE_COUNT;
	DMAProperties.Bank = MTK_EIP197_INLINE_BANK_TOKEN;
	DMAProperties.Size = 4*TokenMaxWords;

	DMAStatus = DMABuf_Alloc(DMAProperties, &TokenHostAddress, &TokenHandle);
	if (DMAStatus != DMABUF_STATUS_OK) {
		rc = 1;
		CRYPTO_ERR("Allocation of token builder failed: %d\n", DMAStatus);
		goto error_exit;
	}

	DMAProperties.fCached = true;
	DMAProperties.Alignment = MTK_EIP197_INLINE_DMA_ALIGNMENT_BYTE_COUNT;
	DMAProperties.Bank = MTK_EIP197_INLINE_BANK_PACKET;
	DMAProperties.Size = MAX(InputByteCount, 1);

	DMAStatus = DMABuf_Alloc(DMAProperties, &PktHostAddress, &PktHandle);
	if (DMAStatus != DMABUF_STATUS_OK) {
		rc = 1;
		CRYPTO_ERR("Allocation of source packet buffer failed: %d\n", DMAStatus);
		goto error_exit;
	}

	rc = PEC_SA_Register(PEC_INTERFACE_ID, SAHandle, DMABuf_NULLHandle,
				DMABuf_NULLHandle);
	if (rc != PEC_STATUS_OK) {
		CRYPTO_ERR("PEC_SA_Register failed: %d\n", rc);
		goto error_exit;
	}

	memcpy(PktHostAddress.p, Input_p, InputByteCount);

	ZEROINIT(TokenParams);
	rc = TokenBuilder_BuildToken(TCRData, (uint8_t *) PktHostAddress.p, InputByteCount,
					&TokenParams, (uint32_t *) TokenHostAddress.p,
					&TokenWords, &TokenHeaderWord);
	if (rc != TKB_STATUS_OK) {
		CRYPTO_ERR("Token builder failed: %d\n", rc);
		goto error_exit_unregister;
	}

	ZEROINIT(Cmd);
	Cmd.Token_Handle = TokenHandle;
	Cmd.Token_WordCount = TokenWords;
	Cmd.SrcPkt_Handle = PktHandle;
	Cmd.SrcPkt_ByteCount = InputByteCount;
	Cmd.DstPkt_Handle = PktHandle;
	Cmd.SA_Handle1 = SAHandle;
	Cmd.SA_Handle2 = DMABuf_NULLHandle;

#if defined(CRYPTO_IOTOKEN_EXT)
	InTokenDscrExt.HW_Services = IOTOKEN_CMD_PKT_LAC;
#endif
	InTokenDscr.TknHdrWordInit = TokenHeaderWord;

	if (!crypto_iotoken_create(&InTokenDscr,
				   InTokenDscrExt_p,
				   InputToken,
				   &Cmd)) {
		rc = 1;
		goto error_exit_unregister;
	}

	rc = PEC_Packet_Put(PEC_INTERFACE_ID, &Cmd, 1, &count);
	if (rc != PEC_STATUS_OK && count != 1) {
		rc = 1;
		CRYPTO_ERR("PEC_Packet_Put error: %d\n", rc);
		goto error_exit_unregister;
	}

	result = kmalloc(sizeof(struct mtk_crypto_result), GFP_KERNEL);
	if (!result) {
		rc = 1;
		CRYPTO_ERR("No memory for result\n");
		goto error_exit_unregister;
	}
	INIT_LIST_HEAD(&result->list);
	result->eip.sa = SAHandle.p;
	result->eip.token = TokenHandle.p;
	result->eip.token_context = TCRData;
	result->eip.pkt_handle = PktHandle.p;
	result->async = async;
	result->dst = PktHostAddress.p;
	result->size = InputByteCount;

	spin_lock_bh(&add_lock);
	list_add_tail(&result->list, &result_list);
	spin_unlock_bh(&add_lock);

	CBFunc = mtk_crypto_interrupt_handler;
	rc = PEC_ResultNotify_Request(PEC_INTERFACE_ID, CBFunc, 1);
	if (rc != PEC_STATUS_OK) {
		CRYPTO_ERR("PEC_ResultNotify_Request failed with rc = %d\n", rc);
		goto error_exit_unregister;
	}
	return 0;

error_exit_unregister:
	PEC_SA_UnRegister(PEC_INTERFACE_ID, SAHandle, DMABuf_NULLHandle,
				DMABuf_NULLHandle);

error_exit:
	DMABuf_Release(SAHandle);
	DMABuf_Release(TokenHandle);
	DMABuf_Release(PktHandle);

	if (TCRData != NULL)
		kfree(TCRData);

	return rc;
}

int crypto_first_ahash_req(struct crypto_async_request *async,
			   struct mtk_crypto_ahash_req *mtk_req, uint8_t *Input_p,
			   unsigned int InputByteCount, bool finish)
{
	struct mtk_crypto_ahash_ctx *ctx = crypto_tfm_ctx(async->tfm);
	struct mtk_crypto_result *result;
	SABuilder_Params_Basic_t ProtocolParams;
	SABuilder_Params_t params;
	unsigned int SAWords = 0;
	static uint8_t DummyAuthKey[64];
	int rc;

	DMABuf_Properties_t DMAProperties = {0, 0, 0, 0};
	DMABuf_HostAddress_t TokenHostAddress;
	DMABuf_HostAddress_t PktHostAddress;
	DMABuf_HostAddress_t SAHostAddress;
	DMABuf_Status_t DMAStatus;

	DMABuf_Handle_t TokenHandle = {0};
	DMABuf_Handle_t PktHandle = {0};
	DMABuf_Handle_t SAHandle = {0};

	unsigned int TokenMaxWords = 0;
	unsigned int TokenHeaderWord;
	unsigned int TokenWords = 0;
	unsigned int TCRWords = 0;
	void *TCRData = 0;

	TokenBuilder_Params_t TokenParams;
	PEC_CommandDescriptor_t Cmd;
	PEC_NotifyFunction_t CBFunc;

	unsigned int count;
	int i;

	u32 InputToken[IOTOKEN_IN_WORD_COUNT];
	IOToken_Output_Dscr_t OutTokenDscr;
	IOToken_Input_Dscr_t InTokenDscr;
	void *InTokenDscrExt_p = NULL;

#ifdef CRYPTO_IOTOKEN_EXT
	IOToken_Input_Dscr_Ext_t InTokenDscrExt;

	ZEROINIT(InTokenDscrExt);
	InTokenDscrExt_p = &InTokenDscrExt;
#endif
	ZEROINIT(InTokenDscr);
	ZEROINIT(OutTokenDscr);

	rc = SABuilder_Init_Basic(&params, &ProtocolParams, SAB_DIRECTION_OUTBOUND);
	if (rc) {
		CRYPTO_ERR("SABuilder_Init_Basic failed: %d\n", rc);
		goto error_exit;
	}

	params.IV_p = (uint8_t *) ctx->ipad;
	params.AuthAlgo = lookaside_match_hash(ctx->alg);
	params.AuthKey1_p = DummyAuthKey;
	if (params.AuthAlgo == SAB_AUTH_AES_XCBC_MAC) {
		params.AuthKey1_p = (uint8_t *) ctx->ipad + 2 * AES_BLOCK_SIZE;
		params.AuthKey2_p = (uint8_t *) ctx->ipad;
		params.AuthKey3_p = (uint8_t *) ctx->ipad + AES_BLOCK_SIZE;

		for (i = 0; i < AES_BLOCK_SIZE; i = i + 4) {
			swap(params.AuthKey1_p[i], params.AuthKey1_p[i+3]);
			swap(params.AuthKey1_p[i+1], params.AuthKey1_p[i+2]);

			swap(params.AuthKey2_p[i], params.AuthKey2_p[i+3]);
			swap(params.AuthKey2_p[i+1], params.AuthKey2_p[i+2]);

			swap(params.AuthKey3_p[i], params.AuthKey3_p[i+3]);
			swap(params.AuthKey3_p[i+1], params.AuthKey3_p[i+2]);
		}
	}

	if (!finish)
		params.flags |= SAB_FLAG_HASH_SAVE | SAB_FLAG_HASH_INTERMEDIATE;

	params.flags |= SAB_FLAG_SUPPRESS_PAYLOAD;
	ProtocolParams.ICVByteCount = mtk_req->digest_sz;

	rc = SABuilder_GetSizes(&params, &SAWords, NULL, NULL);
	if (rc) {
		CRYPTO_ERR("SA not created because of size errors: %d\n", rc);
		goto error_exit;
	}

	DMAProperties.fCached = true;
	DMAProperties.Alignment = MTK_EIP197_INLINE_DMA_ALIGNMENT_BYTE_COUNT;
	DMAProperties.Bank = MTK_EIP197_INLINE_BANK_TRANSFORM;
	DMAProperties.Size = MAX(4*SAWords, 256);

	DMAStatus = DMABuf_Alloc(DMAProperties, &SAHostAddress, &SAHandle);
	if (DMAStatus != DMABUF_STATUS_OK) {
		rc = 1;
		CRYPTO_ERR("Allocation of SA failed: %d\n", DMAStatus);
		goto error_exit;
	}

	rc = SABuilder_BuildSA(&params, (u32 *)SAHostAddress.p, NULL, NULL);
	if (rc) {
		CRYPTO_ERR("SA not created because of errors: %d\n", rc);
		goto error_exit;
	}

	rc = TokenBuilder_GetContextSize(&params, &TCRWords);
	if (rc) {
		CRYPTO_ERR("TokenBuilder_GetContextSize returned errors: %d\n", rc);
		goto error_exit;
	}

	TCRData = kmalloc(4 * TCRWords, GFP_KERNEL);
	if (!TCRData) {
		rc = 1;
		CRYPTO_ERR("Allocation of TCR failed\n");
		goto error_exit;
	}

	rc = TokenBuilder_BuildContext(&params, TCRData);
	if (rc) {
		CRYPTO_ERR("TokenBuilder_BuildContext failed: %d\n", rc);
		goto error_exit;
	}
	mtk_req->token_context = TCRData;

	rc = TokenBuilder_GetSize(TCRData, &TokenMaxWords);
	if (rc) {
		CRYPTO_ERR("TokenBuilder_GetSize failed: %d\n", rc);
		goto error_exit;
	}

	DMAProperties.fCached = true;
	DMAProperties.Alignment = MTK_EIP197_INLINE_DMA_ALIGNMENT_BYTE_COUNT;
	DMAProperties.Bank = MTK_EIP197_INLINE_BANK_TOKEN;
	DMAProperties.Size = 4*TokenMaxWords;

	DMAStatus = DMABuf_Alloc(DMAProperties, &TokenHostAddress, &TokenHandle);
	if (DMAStatus != DMABUF_STATUS_OK) {
		rc = 1;
		CRYPTO_ERR("Allocation of token builder failed: %d\n", DMAStatus);
		goto error_exit;
	}

	DMAProperties.fCached = true;
	DMAProperties.Alignment = MTK_EIP197_INLINE_DMA_ALIGNMENT_BYTE_COUNT;
	DMAProperties.Bank = MTK_EIP197_INLINE_BANK_PACKET;
	DMAProperties.Size = MAX(InputByteCount, mtk_req->digest_sz);

	DMAStatus = DMABuf_Alloc(DMAProperties, &PktHostAddress, &PktHandle);
	if (DMAStatus != DMABUF_STATUS_OK) {
		rc = 1;
		CRYPTO_ERR("Allocation of source packet buffer failed: %d\n",
			   DMAStatus);
		goto error_exit;
	}

	rc = PEC_SA_Register(PEC_INTERFACE_ID, SAHandle, DMABuf_NULLHandle,
				DMABuf_NULLHandle);
	if (rc != PEC_STATUS_OK) {
		CRYPTO_ERR("PEC_SA_Register failed: %d\n", rc);
		goto error_exit;
	}
	memcpy(PktHostAddress.p, Input_p, InputByteCount);

	ZEROINIT(TokenParams);
	TokenParams.PacketFlags |= (TKB_PACKET_FLAG_HASHFIRST
				    | TKB_PACKET_FLAG_HASHAPPEND);
	if (finish)
		TokenParams.PacketFlags |= TKB_PACKET_FLAG_HASHFINAL;

	rc = TokenBuilder_BuildToken(TCRData, (u8 *) PktHostAddress.p,
				     InputByteCount, &TokenParams,
				     (u32 *) TokenHostAddress.p,
				     &TokenWords, &TokenHeaderWord);
	if (rc != TKB_STATUS_OK) {
		CRYPTO_ERR("Token builder failed: %d\n", rc);
		goto error_exit_unregister;
	}

	ZEROINIT(Cmd);
	Cmd.Token_Handle = TokenHandle;
	Cmd.Token_WordCount = TokenWords;
	Cmd.SrcPkt_Handle = PktHandle;
	Cmd.SrcPkt_ByteCount = InputByteCount;
	Cmd.DstPkt_Handle = PktHandle;
	Cmd.SA_Handle1 = SAHandle;
	Cmd.SA_Handle2 = DMABuf_NULLHandle;

	mtk_req->sa_pointer = SAHandle.p;

#if defined(CRYPTO_IOTOKEN_EXT)
	InTokenDscrExt.HW_Services  = IOTOKEN_CMD_PKT_LAC;
#endif
	InTokenDscr.TknHdrWordInit = TokenHeaderWord;

	if (!crypto_iotoken_create(&InTokenDscr,
				   InTokenDscrExt_p,
				   InputToken,
				   &Cmd)) {
		rc = 1;
		goto error_exit_unregister;
	}

	rc = PEC_Packet_Put(PEC_INTERFACE_ID, &Cmd, 1, &count);
	if (rc != PEC_STATUS_OK && count != 1) {
		rc = 1;
		CRYPTO_ERR("PEC_Packet_Put error: %d\n", rc);
		goto error_exit_unregister;
	}

	result = kmalloc(sizeof(struct mtk_crypto_result), GFP_KERNEL);
	if (!result) {
		rc = 1;
		CRYPTO_ERR("No memory for result\n");
		goto error_exit_unregister;
	}
	INIT_LIST_HEAD(&result->list);
	result->eip.token = TokenHandle.p;
	result->eip.pkt_handle = PktHandle.p;
	result->async = async;
	result->dst = PktHostAddress.p;

	spin_lock_bh(&add_lock);
	list_add_tail(&result->list, &result_list);
	spin_unlock_bh(&add_lock);
	CBFunc = mtk_crypto_interrupt_handler;
	rc = PEC_ResultNotify_Request(PEC_INTERFACE_ID, CBFunc, 1);

	return rc;

error_exit_unregister:
	PEC_SA_UnRegister(PEC_INTERFACE_ID, SAHandle, DMABuf_NULLHandle,
				DMABuf_NULLHandle);

error_exit:
	DMABuf_Release(SAHandle);
	DMABuf_Release(TokenHandle);
	DMABuf_Release(PktHandle);

	if (TCRData != NULL)
		kfree(TCRData);

	return rc;
}

bool crypto_basic_hash(SABuilder_Auth_t HashAlgo, uint8_t *Input_p,
				unsigned int InputByteCount, uint8_t *Output_p,
				unsigned int OutputByteCount, bool fFinalize)
{
	SABuilder_Params_Basic_t ProtocolParams;
	SABuilder_Params_t params;
	unsigned int SAWords = 0;
	static uint8_t DummyAuthKey[64];
	int rc;

	DMABuf_Properties_t DMAProperties = {0, 0, 0, 0};
	DMABuf_HostAddress_t TokenHostAddress;
	DMABuf_HostAddress_t PktHostAddress;
	DMABuf_HostAddress_t SAHostAddress;
	DMABuf_Status_t DMAStatus;

	DMABuf_Handle_t TokenHandle = {0};
	DMABuf_Handle_t PktHandle = {0};
	DMABuf_Handle_t SAHandle = {0};

	unsigned int TokenMaxWords = 0;
	unsigned int TokenHeaderWord;
	unsigned int TokenWords = 0;
	unsigned int TCRWords = 0;
	void *TCRData = 0;

	TokenBuilder_Params_t TokenParams;
	PEC_CommandDescriptor_t Cmd;
	PEC_ResultDescriptor_t Res;
	unsigned int count;

	u32 OutputToken[IOTOKEN_IN_WORD_COUNT];
	u32 InputToken[IOTOKEN_IN_WORD_COUNT];
	IOToken_Output_Dscr_t OutTokenDscr;
	IOToken_Input_Dscr_t InTokenDscr;
	void *InTokenDscrExt_p = NULL;

#ifdef CRYPTO_IOTOKEN_EXT
	IOToken_Input_Dscr_Ext_t InTokenDscrExt;

	ZEROINIT(InTokenDscrExt);
	InTokenDscrExt_p = &InTokenDscrExt;
#endif
	ZEROINIT(InTokenDscr);
	ZEROINIT(OutTokenDscr);

	rc = SABuilder_Init_Basic(&params, &ProtocolParams, SAB_DIRECTION_OUTBOUND);
	if (rc) {
		CRYPTO_ERR("SABuilder_Init_Basic failed: %d\n", rc);
		goto error_exit;
	}

	params.AuthAlgo = HashAlgo;
	params.AuthKey1_p = DummyAuthKey;

	if (!fFinalize)
		params.flags |= SAB_FLAG_HASH_SAVE | SAB_FLAG_HASH_INTERMEDIATE;
	params.flags |= SAB_FLAG_SUPPRESS_PAYLOAD;
	ProtocolParams.ICVByteCount = OutputByteCount;

	rc = SABuilder_GetSizes(&params, &SAWords, NULL, NULL);
	if (rc) {
		CRYPTO_ERR("SA not created because of size errors: %d\n", rc);
		goto error_exit;
	}

	DMAProperties.fCached = true;
	DMAProperties.Alignment = MTK_EIP197_INLINE_DMA_ALIGNMENT_BYTE_COUNT;
	DMAProperties.Bank = MTK_EIP197_INLINE_BANK_TRANSFORM;
	DMAProperties.Size = MAX(4*SAWords, 256);

	DMAStatus = DMABuf_Alloc(DMAProperties, &SAHostAddress, &SAHandle);
	if (DMAStatus != DMABUF_STATUS_OK) {
		rc = 1;
		CRYPTO_ERR("Allocation of SA failed: %d\n", DMAStatus);
		goto error_exit;
	}

	rc = SABuilder_BuildSA(&params, (u32 *)SAHostAddress.p, NULL, NULL);
	if (rc) {
		CRYPTO_ERR("SA not created because of errors: %d\n", rc);
		goto error_exit;
	}

	rc = TokenBuilder_GetContextSize(&params, &TCRWords);
	if (rc) {
		CRYPTO_ERR("TokenBuilder_GetContextSize returned errors: %d\n", rc);
		goto error_exit;
	}

	TCRData = kmalloc(4 * TCRWords, GFP_KERNEL);
	if (!TCRData) {
		rc = 1;
		CRYPTO_ERR("Allocation of TCR failed\n");
		goto error_exit;
	}

	rc = TokenBuilder_BuildContext(&params, TCRData);
	if (rc) {
		CRYPTO_ERR("TokenBuilder_BuildContext failed: %d\n", rc);
		goto error_exit;
	}

	rc = TokenBuilder_GetSize(TCRData, &TokenMaxWords);
	if (rc) {
		CRYPTO_ERR("TokenBuilder_GetSize failed: %d\n", rc);
		goto error_exit;
	}

	DMAProperties.fCached = true;
	DMAProperties.Alignment = MTK_EIP197_INLINE_DMA_ALIGNMENT_BYTE_COUNT;
	DMAProperties.Bank = MTK_EIP197_INLINE_BANK_TOKEN;
	DMAProperties.Size = 4*TokenMaxWords;

	DMAStatus = DMABuf_Alloc(DMAProperties, &TokenHostAddress, &TokenHandle);
	if (DMAStatus != DMABUF_STATUS_OK) {
		rc = 1;
		CRYPTO_ERR("Allocation of token builder failed: %d\n", DMAStatus);
		goto error_exit;
	}

	DMAProperties.fCached = true;
	DMAProperties.Alignment = MTK_EIP197_INLINE_DMA_ALIGNMENT_BYTE_COUNT;
	DMAProperties.Bank = MTK_EIP197_INLINE_BANK_PACKET;
	DMAProperties.Size = MAX(InputByteCount, OutputByteCount);

	DMAStatus = DMABuf_Alloc(DMAProperties, &PktHostAddress, &PktHandle);
	if (DMAStatus != DMABUF_STATUS_OK) {
		rc = 1;
		CRYPTO_ERR("Allocation of source packet buffer failed: %d\n",
			   DMAStatus);
		goto error_exit;
	}

	rc = PEC_SA_Register(PEC_INTERFACE_ID, SAHandle, DMABuf_NULLHandle,
				DMABuf_NULLHandle);
	if (rc != PEC_STATUS_OK) {
		CRYPTO_ERR("PEC_SA_Register failed: %d\n", rc);
		goto error_exit;
	}

	memcpy(PktHostAddress.p, Input_p, InputByteCount);

	ZEROINIT(TokenParams);
	TokenParams.PacketFlags |= (TKB_PACKET_FLAG_HASHFIRST
				    | TKB_PACKET_FLAG_HASHAPPEND);
	if (fFinalize)
		TokenParams.PacketFlags |= TKB_PACKET_FLAG_HASHFINAL;

	rc = TokenBuilder_BuildToken(TCRData, (u8 *) PktHostAddress.p,
				     InputByteCount, &TokenParams,
				     (u32 *) TokenHostAddress.p,
				     &TokenWords, &TokenHeaderWord);
	if (rc != TKB_STATUS_OK) {
		CRYPTO_ERR("Token builder failed: %d\n", rc);
		goto error_exit_unregister;
	}

	ZEROINIT(Cmd);
	Cmd.Token_Handle = TokenHandle;
	Cmd.Token_WordCount = TokenWords;
	Cmd.SrcPkt_Handle = PktHandle;
	Cmd.SrcPkt_ByteCount = InputByteCount;
	Cmd.DstPkt_Handle = PktHandle;
	Cmd.SA_Handle1 = SAHandle;
	Cmd.SA_Handle2 = DMABuf_NULLHandle;

#if defined(CRYPTO_IOTOKEN_EXT)
	InTokenDscrExt.HW_Services  = IOTOKEN_CMD_PKT_LAC;
#endif
	InTokenDscr.TknHdrWordInit = TokenHeaderWord;

	if (!crypto_iotoken_create(&InTokenDscr,
				   InTokenDscrExt_p,
				   InputToken,
				   &Cmd)) {
		rc = 1;
		goto error_exit_unregister;
	}

	rc = PEC_Packet_Put(PEC_INTERFACE_ID, &Cmd, 1, &count);
	if (rc != PEC_STATUS_OK && count != 1) {
		rc = 1;
		CRYPTO_ERR("PEC_Packet_Put error: %d\n", rc);
		goto error_exit_unregister;
	}

	if (crypto_pe_busy_get_one(&OutTokenDscr, OutputToken, &Res) < 1) {
		rc = 1;
		CRYPTO_ERR("error from crypto_pe_busy_get_one\n");
		goto error_exit_unregister;
	}
	memcpy(Output_p, PktHostAddress.p, OutputByteCount);

error_exit_unregister:
	PEC_SA_UnRegister(PEC_INTERFACE_ID, SAHandle, DMABuf_NULLHandle,
				DMABuf_NULLHandle);

error_exit:
	DMABuf_Release(SAHandle);
	DMABuf_Release(TokenHandle);
	DMABuf_Release(PktHandle);

	if (TCRData != NULL)
		kfree(TCRData);

	return rc == 0;
}

bool crypto_hmac_precompute(SABuilder_Auth_t AuthAlgo,
			    uint8_t *AuthKey_p,
			    unsigned int AuthKeyByteCount,
			    uint8_t *Inner_p,
			    uint8_t *Outer_p)
{
	SABuilder_Auth_t HashAlgo;
	unsigned int blocksize, hashsize, digestsize;
	static uint8_t pad_block[128], hashed_key[128];
	unsigned int i;

	switch (AuthAlgo) {
	case SAB_AUTH_HMAC_MD5:
		HashAlgo = SAB_AUTH_HASH_MD5;
		blocksize = 64;
		hashsize = 16;
		digestsize = 16;
		break;
	case SAB_AUTH_HMAC_SHA1:
		HashAlgo = SAB_AUTH_HASH_SHA1;
		blocksize = 64;
		hashsize = 20;
		digestsize = 20;
		break;
	case SAB_AUTH_HMAC_SHA2_224:
		HashAlgo = SAB_AUTH_HASH_SHA2_224;
		blocksize = 64;
		hashsize = 28;
		digestsize = 32;
		break;
	case SAB_AUTH_HMAC_SHA2_256:
		HashAlgo = SAB_AUTH_HASH_SHA2_256;
		blocksize = 64;
		hashsize = 32;
		digestsize = 32;
		break;
	case SAB_AUTH_HMAC_SHA2_384:
		HashAlgo = SAB_AUTH_HASH_SHA2_384;
		blocksize = 128;
		hashsize = 48;
		digestsize = 64;
		break;
	case SAB_AUTH_HMAC_SHA2_512:
		HashAlgo = SAB_AUTH_HASH_SHA2_512;
		blocksize = 128;
		hashsize = 64;
		digestsize = 64;
		break;
	default:
		CRYPTO_ERR("Unknown HMAC algorithm\n");
		return false;
	}

	memset(hashed_key, 0, blocksize);
	if (AuthKeyByteCount <= blocksize) {
		memcpy(hashed_key, AuthKey_p, AuthKeyByteCount);
	} else {
		if (!crypto_basic_hash(HashAlgo, AuthKey_p, AuthKeyByteCount,
				       hashed_key, hashsize, true))
			return false;
	}

	for (i = 0; i < blocksize; i++)
		pad_block[i] = hashed_key[i] ^ 0x36;

	if (!crypto_basic_hash(HashAlgo, pad_block, blocksize,
			       Inner_p, digestsize, false))
		return false;

	for (i = 0; i < blocksize; i++)
		pad_block[i] = hashed_key[i] ^ 0x5c;

	if (!crypto_basic_hash(HashAlgo, pad_block, blocksize,
			       Outer_p, digestsize, false))
		return false;

	return true;
}

static SABuilder_Crypto_t set_crypto_algo(struct xfrm_algo *ealg)
{
	if (strcmp(ealg->alg_name, "cbc(des)") == 0)
		return SAB_CRYPTO_DES;
	else if (strcmp(ealg->alg_name, "cbc(aes)") == 0)
		return SAB_CRYPTO_AES;
	else if (strcmp(ealg->alg_name, "cbc(des3_ede)") == 0)
		return SAB_CRYPTO_3DES;

	return SAB_CRYPTO_NULL;
}

static bool set_auth_algo(struct xfrm_algo_auth *aalg, SABuilder_Params_t *params,
			  uint8_t *inner, uint8_t *outer)
{
	if (strcmp(aalg->alg_name, "hmac(sha1)") == 0) {
		params->AuthAlgo = SAB_AUTH_HMAC_SHA1;
		inner = kcalloc(SHA1_DIGEST_SIZE, sizeof(uint8_t), GFP_KERNEL);
		outer = kcalloc(SHA1_DIGEST_SIZE, sizeof(uint8_t), GFP_KERNEL);
		crypto_hmac_precompute(SAB_AUTH_HMAC_SHA1, &aalg->alg_key[0],
					aalg->alg_key_len / 8, inner, outer);

		params->AuthKey1_p = inner;
		params->AuthKey2_p = outer;
	} else if (strcmp(aalg->alg_name, "hmac(sha256)") == 0) {
		params->AuthAlgo = SAB_AUTH_HMAC_SHA2_256;
		inner = kcalloc(SHA256_DIGEST_SIZE, sizeof(uint8_t), GFP_KERNEL);
		outer = kcalloc(SHA256_DIGEST_SIZE, sizeof(uint8_t), GFP_KERNEL);
		crypto_hmac_precompute(SAB_AUTH_HMAC_SHA2_256, &aalg->alg_key[0],
					aalg->alg_key_len / 8, inner, outer);
		params->AuthKey1_p = inner;
		params->AuthKey2_p = outer;
	} else if (strcmp(aalg->alg_name, "hmac(sha384)") == 0) {
		params->AuthAlgo = SAB_AUTH_HMAC_SHA2_384;
		inner = kcalloc(SHA384_DIGEST_SIZE, sizeof(uint8_t), GFP_KERNEL);
		outer = kcalloc(SHA384_DIGEST_SIZE, sizeof(uint8_t), GFP_KERNEL);
		crypto_hmac_precompute(SAB_AUTH_HMAC_SHA2_384, &aalg->alg_key[0],
					aalg->alg_key_len / 8, inner, outer);
		params->AuthKey1_p = inner;
		params->AuthKey2_p = outer;
	} else if (strcmp(aalg->alg_name, "hmac(sha512)") == 0) {
		params->AuthAlgo = SAB_AUTH_HMAC_SHA2_512;
		inner = kcalloc(SHA512_DIGEST_SIZE, sizeof(uint8_t), GFP_KERNEL);
		outer = kcalloc(SHA512_DIGEST_SIZE, sizeof(uint8_t), GFP_KERNEL);
		crypto_hmac_precompute(SAB_AUTH_HMAC_SHA2_512, &aalg->alg_key[0],
					aalg->alg_key_len / 8, inner, outer);
		params->AuthKey1_p = inner;
		params->AuthKey2_p = outer;
	} else if (strcmp(aalg->alg_name, "hmac(md5)") == 0) {
		params->AuthAlgo = SAB_AUTH_HMAC_MD5;
		inner = kcalloc(MD5_DIGEST_SIZE, sizeof(uint8_t), GFP_KERNEL);
		outer = kcalloc(MD5_DIGEST_SIZE, sizeof(uint8_t), GFP_KERNEL);
		crypto_hmac_precompute(SAB_AUTH_HMAC_MD5, &aalg->alg_key[0],
					aalg->alg_key_len / 8, inner, outer);
		params->AuthKey1_p = inner;
		params->AuthKey2_p = outer;
	} else {
		return false;
	}

	return true;
}

u32 *mtk_ddk_tr_ipsec_build(struct mtk_xfrm_params *xfrm_params, u32 ipsec_mode)
{
	struct xfrm_state *xs = xfrm_params->xs;
	SABuilder_Params_IPsec_t ipsec_params;
	SABuilder_Status_t sa_status;
	SABuilder_Params_t params;
	bool set_auth_success = false;
	unsigned int SAWords = 0;
	uint8_t *inner, *outer;

	DMABuf_Status_t dma_status;
	DMABuf_Properties_t dma_properties = {0, 0, 0, 0};
	DMABuf_HostAddress_t sa_host_addr;

	DMABuf_Handle_t sa_handle = {0};

	sa_status = SABuilder_Init_ESP(&params,
				       &ipsec_params,
				       be32_to_cpu(xs->id.spi),
				       ipsec_mode,
				       SAB_IPSEC_IPV4,
				       xfrm_params->dir);

	if (sa_status != SAB_STATUS_OK) {
		pr_err("SABuilder_Init_ESP failed\n");
		sa_handle.p = NULL;
		return (u32 *) sa_handle.p;
	}

	/* No support for aead now */
	if (xs->aead) {
		CRYPTO_ERR("AEAD not supported\n");
		sa_handle.p = NULL;
		return (u32 *) sa_handle.p;
	}

	/* Add crypto key and parameters */
	params.CryptoAlgo = set_crypto_algo(xs->ealg);
	params.CryptoMode = SAB_CRYPTO_MODE_CBC;
	params.KeyByteCount = xs->ealg->alg_key_len / 8;
	params.Key_p = xs->ealg->alg_key;

	/* Add authentication key and parameters */
	set_auth_success = set_auth_algo(xs->aalg, &params, inner, outer);
	if (set_auth_success != true) {
		CRYPTO_ERR("Set Auth Algo failed\n");
		sa_handle.p = NULL;
		return (u32 *) sa_handle.p;
	}

	ipsec_params.IPsecFlags |= (SAB_IPSEC_PROCESS_IP_HEADERS
				    | SAB_IPSEC_EXT_PROCESSING);
	if (ipsec_mode == SAB_IPSEC_TUNNEL) {
		ipsec_params.SrcIPAddr_p = (uint8_t *) &xs->props.saddr.a4;
		ipsec_params.DestIPAddr_p = (uint8_t *) &xs->id.daddr.a4;
	}

	sa_status = SABuilder_GetSizes(&params, &SAWords, NULL, NULL);
	if (sa_status != SAB_STATUS_OK) {
		CRYPTO_ERR("SA not created because of size errors\n");
		sa_handle.p = NULL;
		return (u32 *) sa_handle.p;
	}

	dma_properties.fCached = true;
	dma_properties.Alignment = MTK_EIP197_INLINE_DMA_ALIGNMENT_BYTE_COUNT;
	dma_properties.Bank = MTK_EIP197_INLINE_BANK_TRANSFORM;
	dma_properties.Size = SAWords * sizeof(u32);

	dma_status = DMABuf_Alloc(dma_properties, &sa_host_addr, &sa_handle);
	if (dma_status != DMABUF_STATUS_OK) {
		CRYPTO_ERR("Allocation of SA failed\n");
		/* goto error_exit; */
		sa_handle.p = NULL;
		return (u32 *) sa_handle.p;
	}

	sa_status = SABuilder_BuildSA(&params, (u32 *) sa_host_addr.p, NULL, NULL);
	if (sa_status != SAB_STATUS_OK) {
		CRYPTO_ERR("SA not created because of errors\n");
		sa_handle.p = NULL;
		return (u32 *) sa_handle.p;
	}

	kfree(inner);
	kfree(outer);
	return (u32 *) sa_host_addr.p;
}

int mtk_ddk_pec_init(void)
{
	PEC_InitBlock_t pec_init_blk = {0, 0, false};
	PEC_Capabilities_t pec_cap;
	PEC_Status_t pec_sta;
	u32 i = MTK_EIP197_INLINE_NOF_TRIES;

	while (i) {
		pec_sta = PEC_Init(PEC_INTERFACE_ID, &pec_init_blk);
		if (pec_sta == PEC_STATUS_OK) {
			CRYPTO_INFO("PEC_INIT ok!\n");
			break;
		} else if (pec_sta != PEC_STATUS_OK && pec_sta != PEC_STATUS_BUSY) {
			return pec_sta;
		}

		mdelay(MTK_EIP197_INLINE_RETRY_DELAY_MS);
		i--;
	}

	if (!i) {
		CRYPTO_ERR("PEC could not be initialized: %d\n", pec_sta);
		return pec_sta;
	}

	pec_sta = PEC_Capabilities_Get(&pec_cap);
	if (pec_sta != PEC_STATUS_OK) {
		CRYPTO_ERR("PEC capability could not be obtained: %d\n", pec_sta);
		return pec_sta;
	}

	CRYPTO_INFO("PEC Capabilities: %s\n", pec_cap.szTextDescription);

	return 0;
}

void mtk_ddk_pec_deinit(void)
{
}
