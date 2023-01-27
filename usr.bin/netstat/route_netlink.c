/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1983, 1988, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <netlink/netlink.h>
#include <netlink/netlink_route.h>
#include <netlink/netlink_snl.h>
#include <netlink/netlink_snl_route.h>

#include <netinet/in.h>
#include <netgraph/ng_socket.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <libutil.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>
#include <err.h>
#include <libxo/xo.h>
#include "netstat.h"
#include "common.h"
#include "nl_defs.h"


static void p_rtentry_netlink(struct snl_state *ss, const char *name, struct nlmsghdr *hdr);

static struct ifmap_entry *ifmap;
static size_t ifmap_size;

struct nl_parsed_link {
	uint32_t		ifi_index;
	uint32_t		ifla_mtu;
	char			*ifla_ifname;
};

#define	_IN(_field)	offsetof(struct ifinfomsg, _field)
#define	_OUT(_field)	offsetof(struct nl_parsed_link, _field)
static struct snl_attr_parser ap_link[] = {
	{ .type = IFLA_IFNAME, .off = _OUT(ifla_ifname), .cb = snl_attr_get_string },
	{ .type = IFLA_MTU, .off = _OUT(ifla_mtu), .cb = snl_attr_get_uint32 },
};
static struct snl_field_parser fp_link[] = {
	{.off_in = _IN(ifi_index), .off_out = _OUT(ifi_index), .cb = snl_field_get_uint32 },
};
#undef _IN
#undef _OUT
SNL_DECLARE_PARSER(link_parser, struct ifinfomsg, fp_link, ap_link);

/* Generate ifmap using netlink */
static struct ifmap_entry *
prepare_ifmap_netlink(struct snl_state *ss, size_t *pifmap_size)
{
	struct {
		struct nlmsghdr hdr;
		struct ifinfomsg ifmsg;
	} msg = {
		.hdr.nlmsg_type = RTM_GETLINK,
		.hdr.nlmsg_flags = NLM_F_DUMP | NLM_F_REQUEST,
		.hdr.nlmsg_seq = snl_get_seq(ss),
	};
	msg.hdr.nlmsg_len = sizeof(msg);

	if (!snl_send(ss, &msg, sizeof(msg))) {
		snl_free(ss);
		return (NULL);
	}

	struct ifmap_entry *ifmap = NULL;
	uint32_t ifmap_size = 0;
	struct nlmsghdr *hdr;
	while ((hdr = snl_read_message(ss)) != NULL && hdr->nlmsg_type != NLMSG_DONE) {
		if (hdr->nlmsg_seq != msg.hdr.nlmsg_seq)
			continue;
/*
		if (hdr->nlmsg_type == NLMSG_ERROR)
			break;
*/
		struct nl_parsed_link link = {};
		if (!snl_parse_nlmsg(ss, hdr, &link_parser, &link))
			continue;
		if (link.ifi_index >= ifmap_size) {
			size_t size = roundup2(link.ifi_index + 1, 32) * sizeof(struct ifmap_entry);
			if ((ifmap = realloc(ifmap, size)) == NULL)
				errx(2, "realloc(%zu) failed", size);
			memset(&ifmap[ifmap_size], 0,
			    size - ifmap_size *
			    sizeof(struct ifmap_entry));
			ifmap_size = roundup2(link.ifi_index + 1, 32);
		}
		if (*ifmap[link.ifi_index].ifname != '\0')
			continue;
		strlcpy(ifmap[link.ifi_index].ifname, link.ifla_ifname, IFNAMSIZ);
		ifmap[link.ifi_index].mtu = link.ifla_mtu;
	}
	*pifmap_size = ifmap_size;
	return (ifmap);
}

struct rta_mpath_nh {
	struct sockaddr	*gw;
	uint32_t	ifindex;
	uint8_t		rtnh_flags;
	uint8_t		rtnh_weight;
	uint32_t	rtax_mtu;
	uint32_t	rta_knh_id;
	uint32_t	rta_rtflags;
};

#define	_IN(_field)	offsetof(struct rtnexthop, _field)
#define	_OUT(_field)	offsetof(struct rta_mpath_nh, _field)
static const struct snl_attr_parser nla_p_mp_rtmetrics[] = {
	{ .type = NL_RTAX_MTU, .off = _OUT(rtax_mtu), .cb = snl_attr_get_uint32 },
};
SNL_DECLARE_ATTR_PARSER(metrics_mp_parser, nla_p_mp_rtmetrics);

static const struct snl_attr_parser psnh[] = {
	{ .type = NL_RTA_GATEWAY, .off = _OUT(gw), .cb = snl_attr_get_ip },
	{ .type = NL_RTA_METRICS, .arg = &metrics_mp_parser, .cb = snl_attr_get_nested },
	{ .type = NL_RTA_KNH_ID, .off = _OUT(rta_knh_id), .cb = snl_attr_get_uint32 },
	{ .type = NL_RTA_RTFLAGS, .off = _OUT(gw), .cb = snl_attr_get_uint32 },
	{ .type = NL_RTA_VIA, .off = _OUT(gw), .cb = snl_attr_get_ipvia },
};

static const struct snl_field_parser fpnh[] = {
	{ .off_in = _IN(rtnh_flags), .off_out = _OUT(rtnh_flags), .cb = snl_field_get_uint8 },
	{ .off_in = _IN(rtnh_hops), .off_out = _OUT(rtnh_weight), .cb = snl_field_get_uint8 },
	{ .off_in = _IN(rtnh_ifindex), .off_out = _OUT(ifindex), .cb = snl_field_get_uint32 },
};
#undef _IN
#undef _OUT

SNL_DECLARE_PARSER(mpath_parser, struct rtnexthop, fpnh, psnh);

struct rta_mpath {
	int num_nhops;
	struct rta_mpath_nh nhops[0];
};

static bool
nlattr_get_multipath(struct snl_state *ss, struct nlattr *nla, const void *arg, void *target)
{
	int data_len = nla->nla_len - sizeof(struct nlattr);
	struct rtnexthop *rtnh;

	int max_nhops = data_len / sizeof(struct rtnexthop);
	size_t sz = (max_nhops + 2) * sizeof(struct rta_mpath_nh);

	struct rta_mpath *mp = snl_allocz(ss, sz);
	mp->num_nhops = 0;

	for (rtnh = (struct rtnexthop *)(nla + 1); data_len > 0; ) {
		struct rta_mpath_nh *mpnh = &mp->nhops[mp->num_nhops++];

		if (!snl_parse_header(ss, rtnh, rtnh->rtnh_len, &mpath_parser, mpnh))
			return (false);

		int len = NL_ITEM_ALIGN(rtnh->rtnh_len);
		data_len -= len;
		rtnh = (struct rtnexthop *)((char *)rtnh + len);
	}
	if (data_len != 0 || mp->num_nhops == 0) {
		return (false);
	}

	*((struct rta_mpath **)target) = mp;
	return (true);
}


struct nl_parsed_route {
	struct sockaddr		*rta_dst;
	struct sockaddr		*rta_gw;
	struct nlattr		*rta_metrics;
	struct rta_mpath	*rta_multipath;
	uint32_t		rta_expires;
	uint32_t		rta_oif;
	uint32_t		rta_expire;
	uint32_t		rta_table;
	uint32_t		rta_knh_id;
	uint32_t		rta_rtflags;
	uint32_t		rtax_mtu;
	uint32_t		rtax_weight;
	uint8_t			rtm_family;
	uint8_t			rtm_type;
	uint8_t			rtm_protocol;
	uint8_t			rtm_dst_len;
};

#define	_IN(_field)	offsetof(struct rtmsg, _field)
#define	_OUT(_field)	offsetof(struct nl_parsed_route, _field)
static const struct snl_attr_parser nla_p_rtmetrics[] = {
	{ .type = NL_RTAX_MTU, .off = _OUT(rtax_mtu), .cb = snl_attr_get_uint32 },
};
SNL_DECLARE_ATTR_PARSER(metrics_parser, nla_p_rtmetrics);

static const struct snl_attr_parser ps[] = {
	{ .type = NL_RTA_DST, .off = _OUT(rta_dst), .cb = snl_attr_get_ip },
	{ .type = NL_RTA_OIF, .off = _OUT(rta_oif), .cb = snl_attr_get_uint32 },
	{ .type = NL_RTA_GATEWAY, .off = _OUT(rta_gw), .cb = snl_attr_get_ip },
	{ .type = NL_RTA_METRICS, .arg = &metrics_parser, .cb = snl_attr_get_nested },
	{ .type = NL_RTA_MULTIPATH, .off = _OUT(rta_multipath), .cb = nlattr_get_multipath },
	{ .type = NL_RTA_KNH_ID, .off = _OUT(rta_knh_id), .cb = snl_attr_get_uint32 },
	{ .type = NL_RTA_RTFLAGS, .off = _OUT(rta_rtflags), .cb = snl_attr_get_uint32 },
	{ .type = NL_RTA_TABLE, .off = _OUT(rta_table), .cb = snl_attr_get_uint32 },
	{ .type = NL_RTA_VIA, .off = _OUT(rta_gw), .cb = snl_attr_get_ipvia },
	{ .type = NL_RTA_EXPIRES, .off = _OUT(rta_expire), .cb = snl_attr_get_uint32 },
};

static const struct snl_field_parser fprt[] = {
	{.off_in = _IN(rtm_family), .off_out = _OUT(rtm_family), .cb = snl_field_get_uint8 },
	{.off_in = _IN(rtm_type), .off_out = _OUT(rtm_type), .cb = snl_field_get_uint8 },
	{.off_in = _IN(rtm_protocol), .off_out = _OUT(rtm_protocol), .cb = snl_field_get_uint8 },
	{.off_in = _IN(rtm_dst_len), .off_out = _OUT(rtm_dst_len), .cb = snl_field_get_uint8 },
};
#undef _IN
#undef _OUT
SNL_DECLARE_PARSER(rtm_parser, struct rtmsg, fprt, ps);

#define	RTF_UP		0x1
#define	RTF_GATEWAY	0x2
#define	RTF_HOST	0x4
#define	RTF_REJECT	0x8
#define	RTF_DYNAMIC	0x10
#define RTF_STATIC	0x800
#define RTF_BLACKHOLE	0x1000
#define RTF_PROTO2	0x4000
#define RTF_PROTO1	0x8000
#define RTF_PROTO3	0x40000
#define	RTF_FIXEDMTU	0x80000
#define RTF_PINNED	0x100000

static void
ip6_writemask(struct in6_addr *addr6, uint8_t mask)
{
	uint32_t *cp;

	for (cp = (uint32_t *)addr6; mask >= 32; mask -= 32)
		*cp++ = 0xFFFFFFFF;
	if (mask > 0)
		*cp = htonl(mask ? ~((1 << (32 - mask)) - 1) : 0);
}

static void
gen_mask(int family, int plen, struct sockaddr *sa)
{
	if (family == AF_INET6) {
		struct sockaddr_in6 sin6 = {
			.sin6_family = AF_INET6,
			.sin6_len = sizeof(struct sockaddr_in6),
		};
		ip6_writemask(&sin6.sin6_addr, plen);
		*((struct sockaddr_in6 *)sa) = sin6;
	} else if (family == AF_INET) {
		struct sockaddr_in sin = {
			.sin_family = AF_INET,
			.sin_len = sizeof(struct sockaddr_in),
			.sin_addr.s_addr = htonl(plen ? ~((1 << (32 - plen)) - 1) : 0),
		};
		*((struct sockaddr_in *)sa) = sin;
	}
}

struct sockaddr_dl_short {
	u_char	sdl_len;	/* Total length of sockaddr */
	u_char	sdl_family;	/* AF_LINK */
	u_short	sdl_index;	/* if != 0, system given index for interface */
	u_char	sdl_type;	/* interface type */
	u_char	sdl_nlen;	/* interface name length, no trailing 0 reqd. */
	u_char	sdl_alen;	/* link level address length */
	u_char	sdl_slen;	/* link layer selector length */
	char	sdl_data[8];	/* unused */
};

static void
p_path(struct nl_parsed_route *rt)
{
	struct sockaddr_in6 mask6;
	struct sockaddr *pmask = (struct sockaddr *)&mask6;
	char buffer[128];
	char prettyname[128];
	int protrusion;

	gen_mask(rt->rtm_family, rt->rtm_dst_len, pmask);
	protrusion = p_sockaddr("destination", rt->rta_dst, pmask, rt->rta_rtflags, wid.dst);
	protrusion = p_sockaddr("gateway", rt->rta_gw, NULL, RTF_HOST,
	    wid.gw - protrusion);
	snprintf(buffer, sizeof(buffer), "{[:-%d}{:flags/%%s}{]:} ",
	    wid.flags - protrusion);
	p_flags(rt->rta_rtflags | RTF_UP, buffer);
	/* Output path weight as non-visual property */
	xo_emit("{e:weight/%u}", rt->rtax_weight);

	memset(prettyname, 0, sizeof(prettyname));
	if (rt->rta_oif < ifmap_size) {
		strlcpy(prettyname, ifmap[rt->rta_oif].ifname,
		    sizeof(prettyname));
		if (*prettyname == '\0')
			strlcpy(prettyname, "---", sizeof(prettyname));
		if (rt->rtax_mtu == 0)
			rt->rtax_mtu = ifmap[rt->rta_oif].mtu;
	}

	if (Wflag) {
		/* XXX: use=0? */
		xo_emit("{t:nhop/%*lu} ", wid.mtu, rt->rta_knh_id);

		if (rt->rtax_mtu != 0)
			xo_emit("{t:mtu/%*lu} ", wid.mtu, rt->rtax_mtu);
		else {
			/* use interface mtu */
			xo_emit("{P:/%*s} ", wid.mtu, "");
		}

	}

	if (Wflag)
		xo_emit("{t:interface-name/%*s}", wid.iface, prettyname);
	else
		xo_emit("{t:interface-name/%*.*s}", wid.iface, wid.iface,
		    prettyname);
	if (rt->rta_expires > 0) {
		xo_emit(" {:expire-time/%*u}", wid.expire, rt->rta_expires);
	}
}

static void
p_rtentry_netlink(struct snl_state *ss, const char *name, struct nlmsghdr *hdr)
{

	struct nl_parsed_route rt = {};
	if (!snl_parse_nlmsg(ss, hdr, &rtm_parser, &rt))
		return;

	if (rt.rta_multipath != NULL) {
		uint32_t orig_rtflags = rt.rta_rtflags;
		uint32_t orig_mtu = rt.rtax_mtu;
		for (int i = 0; i < rt.rta_multipath->num_nhops; i++) {
			struct rta_mpath_nh *nhop = &rt.rta_multipath->nhops[i];

			rt.rta_gw = nhop->gw;
			rt.rta_oif = nhop->ifindex;
			rt.rtax_weight = nhop->rtnh_weight;
			rt.rta_rtflags = nhop->rta_rtflags ? nhop->rta_rtflags : orig_rtflags;
			rt.rta_knh_id = nhop->rta_knh_id;
			rt.rtax_mtu = nhop->rtax_mtu ? nhop->rtax_mtu : orig_mtu;

			xo_open_instance(name);
			p_path(&rt);
			xo_emit("\n");
			xo_close_instance(name);
		}
		return;
	}

	struct sockaddr_dl_short sdl_gw = {
		.sdl_family = AF_LINK,
		.sdl_len = sizeof(struct sockaddr_dl_short),
		.sdl_index = rt.rta_oif,
	};
	if (rt.rta_gw == NULL)
		rt.rta_gw = (struct sockaddr *)&sdl_gw;

	xo_open_instance(name);
	p_path(&rt);
	xo_emit("\n");
	xo_close_instance(name);
}

static const struct snl_hdr_parser *all_parsers[] = {
	&link_parser, &metrics_mp_parser, &mpath_parser, &metrics_parser, &rtm_parser
};

bool
p_rtable_netlink(int fibnum, int af)
{
	int fam = AF_UNSPEC;
	int need_table_close = false;
	struct nlmsghdr *hdr;

	struct snl_state ss = {};

	SNL_VERIFY_PARSERS(all_parsers);

	if (!snl_init(&ss, NETLINK_ROUTE))
		return (false);

	ifmap = prepare_ifmap_netlink(&ss, &ifmap_size);

	struct {
		struct nlmsghdr hdr;
		struct rtmsg rtmsg;
		struct nlattr nla_fibnum;
		uint32_t fibnum;
	} msg = {
		.hdr.nlmsg_type = RTM_GETROUTE,
		.hdr.nlmsg_flags = NLM_F_DUMP | NLM_F_REQUEST,
		.hdr.nlmsg_seq = snl_get_seq(&ss),
		.rtmsg.rtm_family = af,
		.nla_fibnum.nla_len = sizeof(struct nlattr) + sizeof(uint32_t),
		.nla_fibnum.nla_type = RTA_TABLE,
		.fibnum = fibnum,
	};
	msg.hdr.nlmsg_len = sizeof(msg);

	if (!snl_send(&ss, &msg, sizeof(msg))) {
		snl_free(&ss);
		return (false);
	}

	xo_open_container("route-table");
	xo_open_list("rt-family");
	while ((hdr = snl_read_message(&ss)) != NULL && hdr->nlmsg_type != NLMSG_DONE) {
		if (hdr->nlmsg_seq != msg.hdr.nlmsg_seq)
			continue;
		struct rtmsg *rtm = (struct rtmsg *)(hdr + 1);
		/* Only print family first time. */
		if (fam != rtm->rtm_family) {
			if (need_table_close) {
				xo_close_list("rt-entry");
				xo_close_instance("rt-family");
			}
			need_table_close = true;
			fam = rtm->rtm_family;
			set_wid(fam);
			xo_open_instance("rt-family");
			pr_family(fam);
			xo_open_list("rt-entry");
			pr_rthdr(fam);
		}
		p_rtentry_netlink(&ss, "rt-entry", hdr);
		snl_clear_lb(&ss);
	}
	if (need_table_close) {
		xo_close_list("rt-entry");
		xo_close_instance("rt-family");
	}
	xo_close_list("rt-family");
	xo_close_container("route-table");
	snl_free(&ss);
	return (true);
}


