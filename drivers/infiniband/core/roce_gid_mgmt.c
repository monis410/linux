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

#include "core_priv.h"

#include <linux/in.h>
#include <linux/in6.h>

/* For in6_dev_get/in6_dev_put */
#include <net/addrconf.h>

#include <rdma/ib_cache.h>
#include <rdma/ib_addr.h>

struct workqueue_struct *roce_gid_mgmt_wq;

enum gid_op_type {
	GID_DEL = 0,
	GID_ADD
};

struct  update_gid_event_work {
	struct work_struct work;
	union ib_gid       gid;
	struct ib_gid_attr gid_attr;
	enum gid_op_type gid_op;
};

#define ROCE_NETDEV_CALLBACK_SZ		2
struct netdev_event_work_cmd {
	roce_netdev_callback	cb;
	roce_netdev_filter	filter;
};

struct netdev_event_work {
	struct work_struct		work;
	struct netdev_event_work_cmd	cmds[ROCE_NETDEV_CALLBACK_SZ];
	struct net_device		*ndev;
};

struct update_gid_work {
	struct work_struct work;
	enum gid_op_type   gid_op;
	struct ib_device   *ib_dev;
	u8                 port;
	union ib_gid       gid;
	struct ib_gid_attr gid_attr;
};

static const struct {
	int flag_mask;
	enum ib_gid_type gid_type;
} PORT_CAP_TO_GID_TYPE[] = {
	{IB_PORT_ROCE_V2,   IB_GID_TYPE_ROCE_V2},
	{IB_PORT_ROCE,      IB_GID_TYPE_IB},
};

#define CAP_TO_GID_TABLE_SIZE \
	(sizeof(PORT_CAP_TO_GID_TYPE) / sizeof(PORT_CAP_TO_GID_TYPE[0]))

static unsigned long gid_type_mask_support(struct ib_device *ib_dev, u8 port)
{
	struct ib_port_attr pattr;
	int i;
	int err;
	unsigned int ret_flags = 0;

	err = ib_query_port(ib_dev, port, &pattr);
	if (err) {
		pr_warn("update_gid: ib_query_port() failed for %s, %d\n",
			ib_dev->name, err);
		return 0;
	}

	for (i = 0; i < CAP_TO_GID_TABLE_SIZE; i++)
		if (pattr.port_cap_flags & PORT_CAP_TO_GID_TYPE[i].flag_mask)
			ret_flags |= 1UL << PORT_CAP_TO_GID_TYPE[i].gid_type;

	return ret_flags;
}

static void update_gid(enum gid_op_type gid_op, struct ib_device *ib_dev,
		       u8 port, union ib_gid *gid,
		       struct ib_gid_attr *gid_attr)
{
	int i;
	unsigned long gid_type_mask = gid_type_mask_support(ib_dev, port);

	for (i = 0; i < IB_GID_TYPE_SIZE; i++) {
		if ((1UL << i) & gid_type_mask) {
			gid_attr->gid_type = i;
			switch (gid_op) {
			case GID_ADD:
				roce_add_gid(ib_dev, port,
					     gid, gid_attr);
				break;
			case GID_DEL:
				roce_del_gid(ib_dev, port,
					     gid, gid_attr);
				break;
			}
		}
	}
}

static int is_eth_port_of_netdev(struct ib_device *ib_dev, u8 port,
				 struct net_device *ndev)
{
	struct net_device *idev;
	struct net_device *rdev;
	struct net_device *mdev;

	ASSERT_RTNL();

	idev = ib_dev->get_netdev(ib_dev, port);
	if (!idev)
		return 0;

	mdev = netdev_master_upper_dev_get(idev);
	rdev = rdma_vlan_dev_real_dev(ndev);

	return (rdev ? rdev : ndev) == (mdev ? mdev : idev);
}

static int pass_all_filter(struct ib_device *ib_dev, u8 port,
			   struct net_device *ndev)
{
	return 1;
}

static void netdevice_event_work_handler(struct work_struct *_work)
{
	struct netdev_event_work *work =
		container_of(_work, struct netdev_event_work, work);
	unsigned int i;

	for (i = 0; i < sizeof(work->cmds)/sizeof(work->cmds[0]) &&
	     work->cmds[i].cb; i++) {
		rtnl_lock();
		ib_enum_roce_ports_of_netdev(work->ndev, work->cmds[i].filter,
					     work->cmds[i].cb, NULL);
		rtnl_unlock();
	}

	dev_put(work->ndev);
	kfree(work);
}

static void update_gid_ip(enum gid_op_type gid_op,
			  struct ib_device *ib_dev,
			  u8 port, struct net_device *ndev,
			  const struct sockaddr *addr)
{
	union ib_gid gid;
	struct ib_gid_attr gid_attr;

	rdma_ip2gid(addr, &gid);
	memset(&gid_attr, 0, sizeof(gid_attr));

	update_gid(gid_op, ib_dev, port, &gid, &gid_attr);
}

static void enum_netdev_default_gids(struct ib_device *ib_dev,
				     u8 port, struct net_device *ndev)
{
	unsigned long gid_type_mask;

	if (ib_dev->get_netdev(ib_dev, port) != ndev)
		return;

	gid_type_mask = gid_type_mask_support(ib_dev, port);

	roce_gid_cache_set_default_gid(ib_dev, port, ndev, gid_type_mask);
}

static void enum_netdev_ipv4_ips(struct ib_device *ib_dev,
				 u8 port, struct net_device *ndev)
{
	struct in_device *in_dev;

	if (ndev->reg_state >= NETREG_UNREGISTERING)
		return;

	in_dev = in_dev_get(ndev);
	if (!in_dev)
		return;

	for_ifa(in_dev) {
		struct sockaddr_in ip;

		ip.sin_family = AF_INET;
		ip.sin_addr.s_addr = ifa->ifa_address;
		update_gid_ip(GID_ADD, ib_dev, port, ndev,
			      (struct sockaddr *)&ip);
	}
	endfor_ifa(in_dev);

	in_dev_put(in_dev);
}

#if IS_ENABLED(CONFIG_IPV6)
static void update_gid_work_handler(struct work_struct *_work)
{
	struct update_gid_work *work =
		container_of(_work, struct update_gid_work, work);

	update_gid(work->gid_op, work->ib_dev, work->port,
		   &work->gid, &work->gid_attr);

	dev_put(work->gid_attr.ndev);
	kfree(work);
}

static void queue_update_gid_work(enum gid_op_type gid_op,
				  struct ib_device *ib_dev,
				  u8 port, struct net_device *ndev,
				  const struct sockaddr *addr)
{
	struct update_gid_work *work;

	if (!roce_gid_cache_is_active(ib_dev, port))
		return;

	work = kmalloc(sizeof(*work), GFP_ATOMIC);
	if (!work)
		return;

	INIT_WORK(&work->work, update_gid_work_handler);

	work->gid_op = gid_op;
	work->ib_dev = ib_dev;
	work->port   = port;
	rdma_ip2gid(addr, &work->gid);

	memset(&work->gid_attr, 0, sizeof(work->gid_attr));

	dev_hold(ndev);
	work->gid_attr.ndev   = ndev;

	queue_work(roce_gid_mgmt_wq, &work->work);
}

static void enum_netdev_ipv6_ips(struct ib_device *ib_dev,
				 u8 port, struct net_device *ndev)
{
	struct inet6_ifaddr *ifp;
	struct inet6_dev *in6_dev;

	if (ndev->reg_state >= NETREG_UNREGISTERING)
		return;

	in6_dev = in6_dev_get(ndev);
	if (!in6_dev)
		return;

	read_lock_bh(&in6_dev->lock);
	list_for_each_entry(ifp, &in6_dev->addr_list, if_list) {
		struct sockaddr_in6 ip;

		ip.sin6_family = AF_INET6;
		ip.sin6_addr = ifp->addr;
		queue_update_gid_work(GID_ADD, ib_dev, port, ndev,
				      (const struct sockaddr *)&ip);
	}
	read_unlock_bh(&in6_dev->lock);

	in6_dev_put(in6_dev);
}
#endif

static void add_netdev_ips(struct ib_device *ib_dev, u8 port,
			   struct net_device *ndev, void *cookie)
{
	enum_netdev_default_gids(ib_dev, port, ndev);
	enum_netdev_ipv4_ips(ib_dev, port, ndev);
#if IS_ENABLED(CONFIG_IPV6)
	enum_netdev_ipv6_ips(ib_dev, port, ndev);
#endif
}

static void del_netdev_ips(struct ib_device *ib_dev, u8 port,
			   struct net_device *ndev, void *cookie)
{
	roce_del_all_netdev_gids(ib_dev, port, ndev);
}

static int netdevice_event(struct notifier_block *this, unsigned long event,
			   void *ptr)
{
	static const struct netdev_event_work_cmd add_cmd = {
		.cb = add_netdev_ips, .filter = is_eth_port_of_netdev};
	static const struct netdev_event_work_cmd del_cmd = {
		.cb = del_netdev_ips, .filter = pass_all_filter};
	struct net_device *ndev = netdev_notifier_info_to_dev(ptr);
	struct netdev_event_work *ndev_work;
	struct netdev_event_work_cmd cmds[ROCE_NETDEV_CALLBACK_SZ] = { {0} };

	switch (event) {
	case NETDEV_REGISTER:
	case NETDEV_UP:
		cmds[0] = add_cmd;
		break;

	/* TODO: Do we want to unregister all gids also when netdev goes down?*/
	case NETDEV_UNREGISTER:
		if (ndev->reg_state < NETREG_UNREGISTERED)
			cmds[0] = del_cmd;
		else
			return NOTIFY_DONE;
		break;

	case NETDEV_CHANGEADDR:
		cmds[0] = del_cmd;
		cmds[1] = add_cmd;
		break;
	default:
		return NOTIFY_DONE;
	}

	ndev_work = kmalloc(sizeof(*ndev_work), GFP_KERNEL);
	if (!ndev_work) {
		pr_warn("roce_gid_mgmt: can't allocate work for netdevice_event\n");
		return NOTIFY_DONE;
	}

	memcpy(ndev_work->cmds, cmds, sizeof(ndev_work->cmds));
	ndev_work->ndev = ndev;
	dev_hold(ndev);
	INIT_WORK(&ndev_work->work, netdevice_event_work_handler);

	queue_work(roce_gid_mgmt_wq, &ndev_work->work);

	return NOTIFY_DONE;
}

static void callback_for_addr_gid_device_scan(struct ib_device *device,
					      u8 port,
					      struct net_device *ndev,
					      void *cookie)
{
	struct update_gid_event_work *parsed = cookie;

	return update_gid(parsed->gid_op, device,
			  port, &parsed->gid,
			  &parsed->gid_attr);
}

static void update_gid_event_work_handler(struct work_struct *_work)
{
	struct update_gid_event_work *work =
		container_of(_work, struct update_gid_event_work, work);

	rtnl_lock();
	ib_enum_roce_ports_of_netdev(work->gid_attr.ndev, is_eth_port_of_netdev,
				     callback_for_addr_gid_device_scan, work);
	rtnl_unlock();

	dev_put(work->gid_attr.ndev);
	kfree(work);
}

static int addr_event(struct notifier_block *this, unsigned long event,
		      void *ptr, sa_family_t sa_family)
{
	struct net_device *ndev;
	struct update_gid_event_work *work;
	union {
		struct in_ifaddr *v4;
		struct inet6_ifaddr *v6;
	} ifa;
	union {
		struct sockaddr     base;
		struct sockaddr_in  in;
		struct sockaddr_in6 in6;
	} sa;
	enum gid_op_type gid_op;

	ifa.v4 = ptr;
	sa.base.sa_family = sa_family;

	switch (sa_family) {
	case AF_INET:
		sa.in.sin_addr.s_addr = ifa.v4->ifa_address;
		ndev = ifa.v4->ifa_dev->dev;
		break;
#if IS_ENABLED(CONFIG_IPV6)
	case AF_INET6:
		sa.in6.sin6_addr = ifa.v6->addr;
		ndev = ifa.v6->idev->dev;
		break;
#endif
	default:
		return NOTIFY_DONE;
	}

	switch (event) {
	case NETDEV_UP:
		gid_op = GID_ADD;
		break;

	case NETDEV_DOWN:
		gid_op = GID_DEL;
		break;

	default:
		return NOTIFY_DONE;
	}

	work = kmalloc(sizeof(*work), GFP_ATOMIC);
	if (!work) {
		pr_warn("roce_gid_mgmt: Couldn't allocate work for addr_event\n");
		return NOTIFY_DONE;
	}

	INIT_WORK(&work->work, update_gid_event_work_handler);

	rdma_ip2gid(&sa.base, &work->gid);
	work->gid_op = gid_op;

	memset(&work->gid_attr, 0, sizeof(work->gid_attr));
	dev_hold(ndev);
	work->gid_attr.ndev   = ndev;

	queue_work(roce_gid_mgmt_wq, &work->work);

	return NOTIFY_DONE;
}

static void enum_all_gids(void)
{
	struct net *net;
	struct net_device *ndev;

	/* Lock the rtnl to make sure the netdevs does not move under
	 * our feet
	 */
	rtnl_lock();
	for_each_net(net)
		for_each_netdev(net, ndev)
			ib_enum_roce_ports_of_netdev(ndev, is_eth_port_of_netdev,
						     add_netdev_ips, NULL);
	rtnl_unlock();
}

static int inetaddr_event(struct notifier_block *this, unsigned long event,
			  void *ptr)
{
	return addr_event(this, event, ptr, AF_INET);
}

#if IS_ENABLED(CONFIG_IPV6)
static int inet6addr_event(struct notifier_block *this, unsigned long event,
			   void *ptr)
{
	return addr_event(this, event, ptr, AF_INET6);
}
#endif

static struct notifier_block nb_netdevice = {
	.notifier_call = netdevice_event
};

static struct notifier_block nb_inetaddr = {
	.notifier_call = inetaddr_event
};

#if IS_ENABLED(CONFIG_IPV6)
static struct notifier_block nb_inet6addr = {
	.notifier_call = inet6addr_event
};
#endif

/* This function will rescan all of the network devices in the system
 * and add their gids, as needed, to the relevant RoCE devices. Will
 * take rtnl and the IB device list mutexes. Must not be called from
 * ib_wq or deadlock will happen. */
void roce_rescan_devices(struct work_struct *work)
{
	enum_all_gids();
	kfree(work);
}

int __init roce_gid_mgmt_init(void)
{
	roce_gid_mgmt_wq = alloc_ordered_workqueue("roce_gid_mgmt_wq", 0);

	if (!roce_gid_mgmt_wq) {
		pr_warn("roce_gid_mgmt: can't allocate work queue\n");
		return -ENOMEM;
	}

	register_inetaddr_notifier(&nb_inetaddr);
#if IS_ENABLED(CONFIG_IPV6)
	register_inet6addr_notifier(&nb_inet6addr);
#endif
	/* We relay on the netdevice notifier to enumerate all
	 * existing devices in the system. Register to this notifier
	 * last to make sure we will not miss any IP add/del
	 * callbacks.
	 */
	register_netdevice_notifier(&nb_netdevice);

	return 0;
}

void __exit roce_gid_mgmt_cleanup(void)
{
#if IS_ENABLED(CONFIG_IPV6)
	unregister_inet6addr_notifier(&nb_inet6addr);
#endif
	unregister_inetaddr_notifier(&nb_inetaddr);
	unregister_netdevice_notifier(&nb_netdevice);
	/* Ensure all gid deletion tasks complete before we go down,
	 * to avoid any reference to free'd memory. By the time
	 * ib-core is removed, all physical devices have been removed,
	 * so no issue with remaining hardware contexts.
	 */
	synchronize_rcu();
	drain_workqueue(roce_gid_mgmt_wq);
	destroy_workqueue(roce_gid_mgmt_wq);
}

