/*	$NetBSD: npf.c,v 1.22 2014/07/25 08:10:40 dholland Exp $	*/

/*-
 * Copyright (c) 2009-2013 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This material is based upon work partially supported by The
 * NetBSD Foundation under a contract with Mindaugas Rasiukevicius.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * NPF main: dynamic load/initialisation and unload routines.
 */

#ifdef _KERNEL
#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: npf.c,v 1.22 2014/07/25 08:10:40 dholland Exp $");

#include <sys/param.h>
#include <sys/types.h>

#include <sys/conf.h>
#include <sys/kauth.h>
#include <sys/lwp.h>
#include <sys/module.h>
#include <sys/socketvar.h>
#include <sys/uio.h>
#endif

#include "npf_impl.h"

/*
 * Module and device structures.
 */
MODULE(MODULE_CLASS_DRIVER, npf, NULL);

void		npfattach(int);
static int	npf_dev_open(dev_t, int, int, lwp_t *);
static int	npf_dev_close(dev_t, int, int, lwp_t *);
static int	npf_dev_ioctl(dev_t, u_long, void *, int, lwp_t *);
static int	npf_dev_poll(dev_t, int, lwp_t *);
static int	npf_dev_read(dev_t, struct uio *, int);

const struct cdevsw npf_cdevsw = {
	.d_open = npf_dev_open,
	.d_close = npf_dev_close,
	.d_read = npf_dev_read,
	.d_write = nowrite,
	.d_ioctl = npf_dev_ioctl,
	.d_stop = nostop,
	.d_tty = notty,
	.d_poll = npf_dev_poll,
	.d_mmap = nommap,
	.d_kqfilter = nokqfilter,
	.d_discard = nodiscard,
	.d_flag = D_OTHER | D_MPSAFE
};

static const char *	npf_ifop_getname(ifnet_t *);
static ifnet_t *	npf_ifop_lookup(const char *);
static void		npf_ifop_flush(void *);
static const void *	npf_ifop_getmeta(ifnet_t *);
static void *		npf_ifop_setmeta(ifp_t *, void *);

static const npf_ifops_t kern_ifops = {
	.getname	= npf_ifop_getname,
	.lookup		= npf_ifop_lookup,
	.flush		= npf_ifop_flush,
	.getmeta	= npf_ifop_getmeta,
	.setmeta	= npf_ifop_setmeta,
};

static int
npf_init(void)
{
#ifdef _MODULE
	devmajor_t bmajor = NODEVMAJOR, cmajor = NODEVMAJOR;
#endif
	npf_t *npf;
	int error = 0;

	npf = npf_create(&kern_ifops);
	npf_setkernctx(npf);
	npf_pfil_register(true);

#ifdef _MODULE
	/* Attach /dev/npf device. */
	error = devsw_attach("npf", NULL, &bmajor, &npf_cdevsw, &cmajor);
	if (error) {
		/* It will call devsw_detach(), which is safe. */
		(void)npf_fini();
	}
#endif
	return error;
}

static int
npf_fini(void)
{
	npf_t *npf = npf_getkernctx();

	/* At first, detach device and remove pfil hooks. */
#ifdef _MODULE
	devsw_detach(NULL, &npf_cdevsw);
#endif
	npf_pfil_unregister(true);
	npf_destroy(npf);
	return 0;
}

/*
 * Module interface.
 */
static int
npf_modcmd(modcmd_t cmd, void *arg)
{

	switch (cmd) {
	case MODULE_CMD_INIT:
		return npf_init();
	case MODULE_CMD_FINI:
		return npf_fini();
	case MODULE_CMD_AUTOUNLOAD:
		if (npf_autounload_p()) {
			return EBUSY;
		}
		break;
	default:
		return ENOTTY;
	}
	return 0;
}

void
npfattach(int nunits)
{

	/* Void. */
}

static int
npf_dev_open(dev_t dev, int flag, int mode, lwp_t *l)
{

	/* Available only for super-user. */
	if (kauth_authorize_network(l->l_cred, KAUTH_NETWORK_FIREWALL,
	    KAUTH_REQ_NETWORK_FIREWALL_FW, NULL, NULL, NULL)) {
		return EPERM;
	}
	return 0;
}

static int
npf_dev_close(dev_t dev, int flag, int mode, lwp_t *l)
{
	return 0;
}

static int
npf_dev_ioctl(dev_t dev, u_long cmd, void *data, int flag, lwp_t *l)
{
	int error;

	/* Available only for super-user. */
	if (kauth_authorize_network(l->l_cred, KAUTH_NETWORK_FIREWALL,
	    KAUTH_REQ_NETWORK_FIREWALL_FW, NULL, NULL, NULL)) {
		return EPERM;
	}

	switch (cmd) {
	case IOC_NPF_TABLE:
		error = npfctl_table(npf, data);
		break;
	case IOC_NPF_RULE:
		error = npfctl_rule(npf, cmd, data);
		break;
	case IOC_NPF_STATS:
		error = npf_stats(npf_kernel_ctx, data);
		break;
	case IOC_NPF_SAVE:
		error = npfctl_save(npf_kernel_ctx, cmd, data);
		break;
	case IOC_NPF_SWITCH:
		error = npfctl_switch(data);
		break;
	case IOC_NPF_LOAD:
		error = npfctl_load(npf_kernel_ctx, cmd, data);
		break;
	case IOC_NPF_VERSION:
		*(int *)data = NPF_VERSION;
		error = 0;
		break;
	default:
		error = ENOTTY;
		break;
	}
	return error;
}

static int
npf_dev_poll(dev_t dev, int events, lwp_t *l)
{
	return ENOTSUP;
}

static int
npf_dev_read(dev_t dev, struct uio *uio, int flag)
{
	return ENOTSUP;
}

bool
npf_autounload_p(void)
{
	return !npf_pfil_registered_p() && npf_default_pass();
}

/*
 * Interface operations.
 */

static const char *
npf_ifop_getname(ifnet_t *ifp)
{
	return ifp->if_xname;
}

static ifnet_t *
npf_ifop_lookup(const char *name)
{
	return ifunit(name);
}

static void
npf_ifop_flush(void *arg)
{
	ifnet_t *ifp;

	KERNEL_LOCK(1, NULL);
	IFNET_FOREACH(ifp) {
		ifp->if_pf_kif = arg;
	}
	KERNEL_UNLOCK_ONE(NULL);
}

static void *
npf_ifop_getmeta(const ifnet_t *ifp)
{
	return ifp->if_pf_kif
}

static void *
npf_ifop_setmeta(ifp_t *ifp, void *arg)
{
	ifp->if_pf_kif = arg;
}
