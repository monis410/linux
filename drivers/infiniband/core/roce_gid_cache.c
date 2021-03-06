/*
 * Copyright (c) 2015, Mellanox Technologies inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/slab.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <rdma/ib_cache.h>
#include <net/addrconf.h>

#include "core_priv.h"

union ib_gid zgid;
static const struct ib_gid_attr zattr;
static const struct ib_gid_attr roce_gid_type_msk = {
	.gid_type = -1
};

static const struct ib_gid_attr roce_gid_type_netdev_msk = {
	.gid_type = -1,
	.ndev = (void *)-1
};

static inline int start_port(struct ib_device *ib_dev)
{
	return (ib_dev->node_type == RDMA_NODE_IB_SWITCH) ? 0 : 1;
}

struct dev_put_rcu {
	struct rcu_head		rcu;
	struct net_device	*ndev;
};

static void put_ndev(struct rcu_head *rcu)
{
	struct dev_put_rcu *put_rcu = container_of(rcu, struct dev_put_rcu, rcu);

	dev_put(put_rcu->ndev);
	kfree(put_rcu);
}

static int write_gid(struct ib_device *ib_dev, u8 port,
		     struct ib_roce_gid_cache *cache, int ix,
		     const union ib_gid *gid,
		     const struct ib_gid_attr *attr)
{
	unsigned int orig_seq;
	int ret;
	struct dev_put_rcu	*put_rcu;
	struct net_device *old_net_dev;

	orig_seq = ACCESS_ONCE(cache->data_vec[ix].seq);
	ACCESS_ONCE(cache->data_vec[ix].seq) = -1;
	/* Ensure that all readers will see invalid sequence
	 * identifier before starting the actual GID update.
	 */
	smp_wmb();

	ret = ib_dev->modify_gid(ib_dev, port, ix, gid, attr,
				 &cache->data_vec[ix].context);

	old_net_dev = cache->data_vec[ix].attr.ndev;
	if (old_net_dev && old_net_dev != attr->ndev) {
		put_rcu = kmalloc(sizeof(*put_rcu), GFP_KERNEL);
		if (put_rcu) {
			put_rcu->ndev = old_net_dev;
			call_rcu(&put_rcu->rcu, put_ndev);
		} else {
			pr_warn("roce_gid_cache: can't allocate rcu context, using synchronize\n");
			synchronize_rcu();
			dev_put(old_net_dev);
		}
	}
	/* if modify_gid failed, just delete the old gid */
	if (ret) {
		gid = &zgid;
		attr = &zattr;
		cache->data_vec[ix].context = NULL;
	}
	memcpy(&cache->data_vec[ix].gid, gid, sizeof(*gid));
	memcpy(&cache->data_vec[ix].attr, attr, sizeof(*attr));
	if (cache->data_vec[ix].attr.ndev &&
	    cache->data_vec[ix].attr.ndev != old_net_dev)
		dev_hold(cache->data_vec[ix].attr.ndev);

	/* Ensure that all cached gid data updating is finished before
	 * marking the entry as available.
	 */
	smp_wmb();

	if (++orig_seq == (unsigned int)-1)
		orig_seq = 0;
	ACCESS_ONCE(cache->data_vec[ix].seq) = orig_seq + 1;

	if (!ret) {
		struct ib_event event;

		event.device		= ib_dev;
		event.element.port_num	= port;
		event.event		= IB_EVENT_GID_CHANGE;

		ib_dispatch_event(&event);
	}
	return ret;
}

int find_gid(struct ib_roce_gid_cache *cache, union ib_gid *gid,
	     const struct ib_gid_attr *val, const struct ib_gid_attr *msk)
{
	int i;
	unsigned int orig_seq;

	for (i = 0; i < cache->sz; i++) {
		struct ib_gid_attr *attr = &cache->data_vec[i].attr;

		orig_seq = ACCESS_ONCE(cache->data_vec[i].seq);
		if (orig_seq == -1)
			continue;
		/* Make sure the sequence number we remeber was read
		 * before the gid cache entry content is read.
		 */
		smp_rmb();

		if (msk->gid_type && attr->gid_type != (val->gid_type & msk->gid_type))
			continue;

		if (memcmp(gid, &cache->data_vec[i].gid, sizeof(*gid)))
			continue;

		if (msk->ndev &&
		    (uintptr_t)attr->ndev !=
		    ((uintptr_t)msk->ndev & (uintptr_t)val->ndev))
			continue;

		/* We have a match, verify that the data we
		 * compared is valid. Make sure that the
		 * sequence number we read is the last to be
		 * read.
		 */
		smp_rmb();
		if (orig_seq == ACCESS_ONCE(cache->data_vec[i].seq))
			return i;
		/* The sequence number changed under our feet,
		 * the GID entry is invalid. Continue to the
		 * next entry.
		 */
	}

	return -1;
}

static void make_default_gid(struct  net_device *dev, union ib_gid *gid)
{
	gid->global.subnet_prefix = cpu_to_be64(0xfe80000000000000LL);
	addrconf_ifid_eui48(&gid->raw[8], dev);
}

int roce_add_gid(struct ib_device *ib_dev, u8 port,
		 union ib_gid *gid, struct ib_gid_attr *attr)
{
	struct ib_roce_gid_cache *cache;
	int ix;
	int ret = 0;

	if (!ib_dev->cache.roce_gid_cache)
		return -ENOSYS;

	cache = ib_dev->cache.roce_gid_cache[port - 1];

	if (!cache->active)
		return -ENOSYS;

	mutex_lock(&cache->lock);

	ix = find_gid(cache, gid, attr, &roce_gid_type_msk);
	if (ix >= 0)
		goto out_unlock;

	ix = find_gid(cache, &zgid, NULL, &zattr);
	if (ix < 0) {
		ret = -ENOSPC;
		goto out_unlock;
	}

	write_gid(ib_dev, port, cache, ix, gid, attr);

out_unlock:
	mutex_unlock(&cache->lock);
	return ret;
}

int roce_del_gid(struct ib_device *ib_dev, u8 port,
		 union ib_gid *gid, struct ib_gid_attr *attr)
{
	struct ib_roce_gid_cache *cache;
	union ib_gid default_gid;
	int ix;

	if (!ib_dev->cache.roce_gid_cache)
		return 0;

	cache  = ib_dev->cache.roce_gid_cache[port - 1];

	if (!cache->active)
		return -ENOSYS;

	if (attr->ndev) {
		/* Deleting default GIDs in not permitted */
		make_default_gid(attr->ndev, &default_gid);
		if (!memcmp(gid, &default_gid, sizeof(*gid)))
			return -EPERM;
	}

	mutex_lock(&cache->lock);

	ix = find_gid(cache, gid, attr, &roce_gid_type_netdev_msk);
	if (ix < 0)
		goto out_unlock;

	write_gid(ib_dev, port, cache, ix, &zgid, &zattr);

out_unlock:
	mutex_unlock(&cache->lock);
	return 0;
}

int roce_del_all_netdev_gids(struct ib_device *ib_dev, u8 port,
			     struct net_device *ndev)
{
	struct ib_roce_gid_cache *cache;
	int ix;

	if (!ib_dev->cache.roce_gid_cache)
		return 0;

	cache  = ib_dev->cache.roce_gid_cache[port - 1];

	if (!cache->active)
		return -ENOSYS;

	mutex_lock(&cache->lock);

	for (ix = 0; ix < cache->sz; ix++)
		if (cache->data_vec[ix].attr.ndev == ndev)
			write_gid(ib_dev, port, cache, ix, &zgid, &zattr);

	mutex_unlock(&cache->lock);
	return 0;
}

int roce_gid_cache_get_gid(struct ib_device *ib_dev, u8 port, int index,
			   union ib_gid *gid, struct ib_gid_attr *attr)
{
	struct ib_roce_gid_cache *cache;
	union ib_gid local_gid;
	struct ib_gid_attr local_attr;
	unsigned int orig_seq;

	if (!ib_dev->cache.roce_gid_cache)
		return -EINVAL;

	cache = ib_dev->cache.roce_gid_cache[port - 1];

	if (!cache->active)
		return -ENOSYS;

	if (index < 0 || index >= cache->sz)
		return -EINVAL;

	orig_seq = ACCESS_ONCE(cache->data_vec[index].seq);
	/* Make sure we read the sequence number before copying the
	 * gid to local storage. */
	smp_rmb();

	memcpy(&local_gid, &cache->data_vec[index].gid, sizeof(local_gid));
	memcpy(&local_attr, &cache->data_vec[index].attr, sizeof(local_attr));
	/* Ensure the local copy completed reading before verifying
	 * the new sequence number. */
	smp_rmb();

	if (orig_seq == -1 ||
	    orig_seq != ACCESS_ONCE(cache->data_vec[index].seq))
		return -EAGAIN;

	memcpy(gid, &local_gid, sizeof(*gid));
	if (attr)
		memcpy(attr, &local_attr, sizeof(*attr));
	return 0;
}

int _roce_gid_cache_find_gid(struct ib_device *ib_dev, union ib_gid *gid,
			     const struct ib_gid_attr *val,
			     const struct ib_gid_attr *msk,
			     u8 *port, u16 *index)
{
	struct ib_roce_gid_cache *cache;
	u8 p;
	int local_index;

	if (!ib_dev->cache.roce_gid_cache)
		return -ENOENT;

	for (p = 0; p < ib_dev->phys_port_cnt; p++) {
		if (rdma_port_get_link_layer(ib_dev, p + start_port(ib_dev)) !=
		    IB_LINK_LAYER_ETHERNET)
			continue;
		cache = ib_dev->cache.roce_gid_cache[p];
		if (!cache->active)
			continue;
		local_index = find_gid(cache, gid, val, msk);
		if (local_index >= 0) {
			if (index)
				*index = local_index;
			if (port)
				*port = p + start_port(ib_dev);
			return 0;
		}
	}

	return -ENOENT;
}

int roce_gid_cache_find_gid(struct ib_device *ib_dev, union ib_gid *gid,
			    enum ib_gid_type gid_type, u8 *port, u16 *index)
{
	struct ib_gid_attr gid_type_val = {.gid_type = gid_type};

	return _roce_gid_cache_find_gid(ib_dev, gid, &gid_type_val, &roce_gid_type_msk,
					port, index);
}

static struct ib_roce_gid_cache *alloc_roce_gid_cache(int sz)
{
	struct ib_roce_gid_cache *cache =
		kzalloc(sizeof(struct ib_roce_gid_cache), GFP_KERNEL);
	if (!cache)
		return NULL;

	cache->data_vec = kcalloc(sz, sizeof(*cache->data_vec), GFP_KERNEL);
	if (!cache->data_vec)
		goto err_free_cache;

	mutex_init(&cache->lock);

	cache->sz = sz;

	return cache;

err_free_cache:
	kfree(cache);
	return NULL;
}

static void free_roce_gid_cache(struct ib_roce_gid_cache *cache)
{
	int i;

	if (!cache)
		return;

	for (i = 0; i < cache->sz; ++i) {
		if (cache->data_vec[i].attr.ndev)
			dev_put(cache->data_vec[i].attr.ndev);
	}
	kfree(cache->data_vec);
	kfree(cache);
}

static void set_roce_gid_cache_active(struct ib_roce_gid_cache *cache, int active)
{
	if (!cache)
		return;

	cache->active = active;
}

void roce_gid_cache_set_default_gid(struct ib_device *ib_dev, u8 port,
				    struct net_device *ndev,
				    unsigned long gid_type_mask)
{
	union ib_gid gid;
	struct ib_gid_attr gid_attr;
	struct ib_roce_gid_cache *cache;
	unsigned int i;
	unsigned int success;

	cache  = ib_dev->cache.roce_gid_cache[port - 1];

	if (!cache)
		return;

	make_default_gid(ndev, &gid);
	memset(&gid_attr, 0, sizeof(gid_attr));
	gid_attr.ndev = ndev;
	for (i = 0, success = 0; i < IB_GID_TYPE_SIZE; i++) {
		if (1UL << i & ~gid_type_mask)
			continue;
		gid_attr.gid_type = i;
		if (write_gid(ib_dev, port, cache, success, &zgid, &zattr)) {
			pr_warn("roce_gid_cache: can't delete index %d for default gid %pI6\n",
				success, gid.raw);
			continue;
		}
		if (write_gid(ib_dev, port, cache, success, &gid, &gid_attr))
			pr_warn("roce_gid_cache: unable to add default gid %pI6\n",
				gid.raw);
		else
			success++;
	}
}

static int roce_gid_cache_setup_one(struct ib_device *ib_dev)
{
	u8 port;
	int err = 0;

	if (!ib_dev->modify_gid)
		return -ENOSYS;

	ib_dev->cache.roce_gid_cache =
		kcalloc(ib_dev->phys_port_cnt,
			sizeof(*ib_dev->cache.roce_gid_cache), GFP_KERNEL);

	if (!ib_dev->cache.roce_gid_cache) {
		pr_warn("failed to allocate roce addr cache for %s\n",
			ib_dev->name);
		return -ENOMEM;
	}

	for (port = 0; port < ib_dev->phys_port_cnt; port++) {
		ib_dev->cache.roce_gid_cache[port] =
			alloc_roce_gid_cache(ib_dev->gid_tbl_len[port]);
		if (!ib_dev->cache.roce_gid_cache[port]) {
			err = -ENOMEM;
			goto rollback_cache_setup;
		}
	}
	return 0;

rollback_cache_setup:
	for (port = 0; port < ib_dev->phys_port_cnt; port++)
		free_roce_gid_cache(ib_dev->cache.roce_gid_cache[port]);

	kfree(ib_dev->cache.roce_gid_cache);
	ib_dev->cache.roce_gid_cache = NULL;

	return err;
}

static void roce_gid_cache_cleanup_one(struct ib_device *ib_dev)
{
	u8 port;

	if (!ib_dev->cache.roce_gid_cache)
		return;

	for (port = 0; port < ib_dev->phys_port_cnt; port++)
		free_roce_gid_cache(ib_dev->cache.roce_gid_cache[port]);

	kfree(ib_dev->cache.roce_gid_cache);
	ib_dev->cache.roce_gid_cache = NULL;
}

static void roce_gid_cache_set_active_state(struct ib_device *ib_dev, int active)
{
	u8 port;

	if (!ib_dev->cache.roce_gid_cache)
		return;

	for (port = 0; port < ib_dev->phys_port_cnt; port++)
		set_roce_gid_cache_active(ib_dev->cache.roce_gid_cache[port], active);
}

int roce_gid_cache_is_active(struct ib_device *ib_dev, u8 port)
{
	return ib_dev->cache.roce_gid_cache &&
		ib_dev->cache.roce_gid_cache[port - 1]->active;
}

static void roce_gid_cache_client_setup_one(struct ib_device *ib_dev)
{
	if (!roce_gid_cache_setup_one(ib_dev)) {
		struct work_struct *work = kzalloc(sizeof(*work), GFP_KERNEL);

		if (!work) {
			roce_gid_cache_cleanup_one(ib_dev);
			return;
		}
		roce_gid_cache_set_active_state(ib_dev, 1);
		INIT_WORK(work, roce_rescan_devices);
		schedule_work(work);
	}
}

static void roce_gid_cache_client_cleanup_one(struct ib_device *ib_dev)
{
	roce_gid_cache_set_active_state(ib_dev, 0);
	/* Make sure no gid update task is still referencing this device */
	flush_workqueue(roce_gid_mgmt_wq);
	flush_workqueue(system_wq);

	roce_gid_cache_cleanup_one(ib_dev);
}

static struct ib_client cache_client = {
	.name   = "roce_gid_cache",
	.add    = roce_gid_cache_client_setup_one,
	.remove = roce_gid_cache_client_cleanup_one
};

int __init roce_gid_cache_setup(void)
{
	roce_gid_mgmt_init();

	return ib_register_client(&cache_client);
}

void __exit roce_gid_cache_cleanup(void)
{
	ib_unregister_client(&cache_client);

	roce_gid_mgmt_cleanup();
}
