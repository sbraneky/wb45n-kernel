/*
 * GCMP: Galois/Counter Mode Protocol FIPS required checks.
 *
 * Copyright (c) 2019 Laird Connectivity - Boris Krasnovskiy
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include <crypto/internal/aead.h>
#include <crypto/internal/skcipher.h>
#include <crypto/null.h>
#include <crypto/scatterwalk.h>
#include <crypto/gcm.h>
#include "internal.h"
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>

#define CRYPTO_TFM_RES_MASK		0xfff00000

struct crypto_gcmp_ctx {
	struct crypto_aead *child;
	u64 pn64;
	u8 iv[GCM_AES_IV_SIZE] __attribute__((aligned(4)));
	bool check_failed, mac_set;
};

struct crypto_gcmp_req_ctx {
	struct aead_request subreq;
};

void crypto_gcmp_set_if_name(struct crypto_aead *parent, char *ifname)
{
	struct crypto_gcmp_ctx *ctx = crypto_aead_ctx(parent);

	 struct net_device *dev = __dev_get_by_name(&init_net, ifname);

	if (!ctx->mac_set && dev) {
		ctx->mac_set = true;
		memcpy(ctx->iv, dev->dev_addr, ETH_ALEN);
	}
}
EXPORT_SYMBOL_GPL(crypto_gcmp_set_if_name);

static int crypto_gcmp_setkey(struct crypto_aead *parent, const u8 *key,
				 unsigned int keylen)
{
	struct crypto_gcmp_ctx *ctx = crypto_aead_ctx(parent);
	struct crypto_aead *child = ctx->child;
	int err;

	crypto_aead_clear_flags(child, CRYPTO_TFM_REQ_MASK);
	crypto_aead_set_flags(child, crypto_aead_get_flags(parent) &
				     CRYPTO_TFM_REQ_MASK);
	err = crypto_aead_setkey(child, key, keylen);
	crypto_aead_set_flags(parent, crypto_aead_get_flags(child) &
				      CRYPTO_TFM_RES_MASK);

	ctx->check_failed = false;
	ctx->pn64 = 0;

	return err;
}

static int crypto_gcmp_setauthsize(struct crypto_aead *parent,
				      unsigned int authsize)
{
	struct crypto_gcmp_ctx *ctx = crypto_aead_ctx(parent);

	return crypto_aead_setauthsize(ctx->child, authsize);
}

static struct aead_request *crypto_gcmp_crypt(struct aead_request *req)
{
	struct crypto_gcmp_req_ctx *rctx = aead_request_ctx(req);
	struct crypto_aead *aead = crypto_aead_reqtfm(req);
	struct crypto_gcmp_ctx *ctx = crypto_aead_ctx(aead);
	struct aead_request *subreq = &rctx->subreq;
	struct crypto_aead *child = ctx->child;

	aead_request_set_tfm(subreq, child);
	aead_request_set_callback(subreq, req->base.flags, req->base.complete,
				  req->base.data);
	aead_request_set_crypt(subreq, req->src, req->dst,
			       req->cryptlen, req->iv);
	aead_request_set_ad(subreq, req->assoclen);

	return subreq;
}

static int crypto_gcmp_encrypt(struct aead_request *req)
{
	struct crypto_aead *aead = crypto_aead_reqtfm(req);
	struct crypto_gcmp_ctx *ctx = crypto_aead_ctx(aead);
	u64 pn64 = ctx->pn64;
	u8 *pn = ctx->iv + ETH_ALEN;

	if (ctx->check_failed || !ctx->mac_set || pn64 >= 0xffffffffffff)
		return -EINVAL;

	ctx->pn64 = ++pn64;

	pn[5] = pn64;
	pn[4] = pn64 >> 8;
	pn[3] = pn64 >> 16;
	pn[2] = pn64 >> 24;
	pn[1] = pn64 >> 32;
	pn[0] = pn64 >> 40;

	if (memcmp(ctx->iv, req->iv, GCM_AES_IV_SIZE)) {
		ctx->check_failed = true;
		pr_err("gcmp iv check fail\n");
		return -EINVAL;
	}

	req = crypto_gcmp_crypt(req);

	return crypto_aead_encrypt(req);
}

static int crypto_gcmp_decrypt(struct aead_request *req)
{
	req = crypto_gcmp_crypt(req);

	return crypto_aead_decrypt(req);
}

static int crypto_gcmp_init_tfm(struct crypto_aead *tfm)
{
	struct aead_instance *inst = aead_alg_instance(tfm);
	struct crypto_aead_spawn *spawn = aead_instance_ctx(inst);
	struct crypto_gcmp_ctx *ctx = crypto_aead_ctx(tfm);
	struct crypto_aead *aead;
	unsigned long align;

	aead = crypto_spawn_aead(spawn);
	if (IS_ERR(aead))
		return PTR_ERR(aead);

	ctx->child = aead;

	align = crypto_aead_alignmask(aead);
	align &= ~(crypto_tfm_ctx_alignment() - 1);
	crypto_aead_set_reqsize(
		tfm,
		sizeof(struct crypto_gcmp_req_ctx) +
		ALIGN(crypto_aead_reqsize(aead), crypto_tfm_ctx_alignment()) +
		align + 24);

	return 0;
}

static void crypto_gcmp_exit_tfm(struct crypto_aead *tfm)
{
	struct crypto_gcmp_ctx *ctx = crypto_aead_ctx(tfm);

	crypto_free_aead(ctx->child);
}

static void crypto_gcmp_free(struct aead_instance *inst)
{
	crypto_drop_aead(aead_instance_ctx(inst));
	kfree(inst);
}

static int crypto_gcmp_create(struct crypto_template *tmpl,
				 struct rtattr **tb)
{
	struct aead_instance *inst;
	struct crypto_aead_spawn *spawn;
	struct aead_alg *alg;
	const char *gcm_name;
	int err;
	u32 mask;

	err = crypto_check_attr_type(tb, CRYPTO_ALG_TYPE_AEAD, &mask);
	if (err)
		return err;

	gcm_name = crypto_attr_alg_name(tb[1]);
	if (IS_ERR(gcm_name))
		return PTR_ERR(gcm_name);

	inst = kzalloc(sizeof(*inst) + sizeof(*spawn), GFP_KERNEL);
	if (!inst)
		return -ENOMEM;

	spawn = aead_instance_ctx(inst);
	err = crypto_grab_aead(spawn, aead_crypto_instance(inst),
			       gcm_name, 0, mask);
	if (err)
		goto out_free_inst;

	alg = crypto_spawn_aead_alg(spawn);

	if (snprintf(inst->alg.base.cra_name, CRYPTO_MAX_ALG_NAME,
		     "gcmp(%s)", alg->base.cra_name) >=
	    CRYPTO_MAX_ALG_NAME ||
	    snprintf(inst->alg.base.cra_driver_name, CRYPTO_MAX_ALG_NAME,
		     "gcmp(%s)", alg->base.cra_driver_name) >=
	    CRYPTO_MAX_ALG_NAME)
		goto out_drop_alg;

	inst->alg.base.cra_flags = alg->base.cra_flags & CRYPTO_ALG_ASYNC;
	inst->alg.base.cra_priority = alg->base.cra_priority;
	inst->alg.base.cra_blocksize = 1;
	inst->alg.base.cra_alignmask = alg->base.cra_alignmask;

	inst->alg.base.cra_ctxsize = sizeof(struct crypto_gcmp_ctx);

	inst->alg.ivsize = GCM_AES_IV_SIZE;
	inst->alg.chunksize = crypto_aead_alg_chunksize(alg);
	inst->alg.maxauthsize = crypto_aead_alg_maxauthsize(alg);

	inst->alg.init = crypto_gcmp_init_tfm;
	inst->alg.exit = crypto_gcmp_exit_tfm;

	inst->alg.setkey = crypto_gcmp_setkey;
	inst->alg.setauthsize = crypto_gcmp_setauthsize;
	inst->alg.encrypt = crypto_gcmp_encrypt;
	inst->alg.decrypt = crypto_gcmp_decrypt;

	inst->free = crypto_gcmp_free;

	err = aead_register_instance(tmpl, inst);
	if (err)
		goto out_drop_alg;

out:
	return err;

out_drop_alg:
	crypto_drop_aead(spawn);
out_free_inst:
	kfree(inst);
	goto out;
}

static struct crypto_template crypto_gcmp_tmpl = {
	.name = "gcmp",
	.create = crypto_gcmp_create,
	.module = THIS_MODULE,
};

static int __init crypto_gcmp_module_init(void)
{
	return crypto_register_template(&crypto_gcmp_tmpl);
}

static void __exit crypto_gcmp_module_exit(void)
{
	crypto_unregister_template(&crypto_gcmp_tmpl);
}

module_init(crypto_gcmp_module_init);
module_exit(crypto_gcmp_module_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Galois/Counter Mode Protocol FIPS checks");
MODULE_AUTHOR("Boris Krasnovskiy");
MODULE_ALIAS_CRYPTO("gcmp");
