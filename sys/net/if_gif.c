/*	$OpenBSD: if_gif.c,v 1.101 2017/10/25 09:24:09 mpi Exp $	*/
/*	$KAME: if_gif.c,v 1.43 2001/02/20 08:51:07 itojun Exp $	*/

/*
 * Copyright (C) 1995, 1996, 1997, and 1998 WIDE Project.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/syslog.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_types.h>
#include <net/route.h>

#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet/ip.h>
#include <netinet/ip_ether.h>
#include <netinet/ip_var.h>
#include <netinet/ip_ipip.h>
#include <netinet/ip_ipsp.h>

#ifdef INET6
#include <netinet6/in6_var.h>
#include <netinet/ip6.h>
#include <netinet6/ip6_var.h>
#endif /* INET6 */

#include <net/if_gif.h>

#include "bpfilter.h"
#if NBPFILTER > 0
#include <net/bpf.h>
#endif

#ifdef MPLS
#include <netinet/ip_ether.h>
#endif

#include "pf.h"
#if NPF > 0
#include <net/pfvar.h>
#endif

#define GIF_MTU		(1280)	/* Default MTU */
#define GIF_MTU_MIN	(1280)	/* Minimum MTU */
#define GIF_MTU_MAX	(8192)	/* Maximum MTU */

void	gifattach(int);
int	gif_clone_create(struct if_clone *, int);
int	gif_clone_destroy(struct ifnet *);
int	gif_checkloop(struct ifnet *, struct mbuf *);
void	gif_start(struct ifnet *);
int	gif_ioctl(struct ifnet *, u_long, caddr_t);
int	gif_output(struct ifnet *, struct mbuf *, struct sockaddr *,
	    struct rtentry *);

int	in_gif_output(struct ifnet *, int, struct mbuf **);
int	in6_gif_output(struct ifnet *, int, struct mbuf **);

/*
 * gif global variable definitions
 */
struct gif_softc_head gif_softc_list;
struct if_clone gif_cloner =
    IF_CLONE_INITIALIZER("gif", gif_clone_create, gif_clone_destroy);

void
gifattach(int count)
{
	LIST_INIT(&gif_softc_list);
	if_clone_attach(&gif_cloner);
}

int
gif_clone_create(struct if_clone *ifc, int unit)
{
	struct gif_softc *sc;

	sc = malloc(sizeof(*sc), M_DEVBUF, M_NOWAIT|M_ZERO);
	if (!sc)
		return (ENOMEM);

	snprintf(sc->gif_if.if_xname, sizeof sc->gif_if.if_xname,
	     "%s%d", ifc->ifc_name, unit);
	sc->gif_if.if_mtu    = GIF_MTU;
	sc->gif_if.if_flags  = IFF_POINTOPOINT | IFF_MULTICAST;
	sc->gif_if.if_xflags = IFXF_CLONED;
	sc->gif_if.if_ioctl  = gif_ioctl;
	sc->gif_if.if_start  = gif_start;
	sc->gif_if.if_output = gif_output;
	sc->gif_if.if_rtrequest = p2p_rtrequest;
	sc->gif_if.if_type   = IFT_GIF;
	IFQ_SET_MAXLEN(&sc->gif_if.if_snd, IFQ_MAXLEN);
	sc->gif_if.if_softc = sc;
	if_attach(&sc->gif_if);
	if_alloc_sadl(&sc->gif_if);

#if NBPFILTER > 0
	bpfattach(&sc->gif_if.if_bpf, &sc->gif_if, DLT_LOOP, sizeof(u_int32_t));
#endif
	NET_LOCK();
	LIST_INSERT_HEAD(&gif_softc_list, sc, gif_list);
	NET_UNLOCK();

	return (0);
}

int
gif_clone_destroy(struct ifnet *ifp)
{
	struct gif_softc *sc = ifp->if_softc;

	NET_LOCK();
	LIST_REMOVE(sc, gif_list);
	NET_UNLOCK();

	if_detach(ifp);

	if (sc->gif_psrc)
		free((caddr_t)sc->gif_psrc, M_IFADDR, 0);
	sc->gif_psrc = NULL;
	if (sc->gif_pdst)
		free((caddr_t)sc->gif_pdst, M_IFADDR, 0);
	sc->gif_pdst = NULL;
	free(sc, M_DEVBUF, sizeof(*sc));
	return (0);
}

void
gif_start(struct ifnet *ifp)
{
	struct gif_softc *sc = (struct gif_softc*)ifp;
	struct mbuf *m;

	for (;;) {
		IFQ_DEQUEUE(&ifp->if_snd, m);
		if (m == NULL)
			break;

		/* is interface up and usable? */
		if (!(ifp->if_flags & IFF_UP) ||
		    sc->gif_psrc == NULL || sc->gif_pdst == NULL ||
		    sc->gif_psrc->sa_family != sc->gif_pdst->sa_family) {
			m_freem(m);
			continue;
		}

#if NBPFILTER > 0
		if (ifp->if_bpf) {
			int offset;
			sa_family_t family;
			u_int8_t proto;

			/* must decapsulate outer header for bpf */
			switch (sc->gif_psrc->sa_family) {
			case AF_INET:
				offset = sizeof(struct ip);
				proto = mtod(m, struct ip *)->ip_p;
				break;
#ifdef INET6
			case AF_INET6:
				offset = sizeof(struct ip6_hdr);
				proto = mtod(m, struct ip6_hdr *)->ip6_nxt;
				break;
#endif
			default:
				proto = 0;
				break;
			}
			switch (proto) {
			case IPPROTO_IPV4:
				family = AF_INET;
				break;
			case IPPROTO_IPV6:
				family = AF_INET6;
				break;
			case IPPROTO_ETHERIP:
				family = AF_LINK;
				offset += sizeof(struct etherip_header);
				break;
			case IPPROTO_MPLS:
				family = AF_MPLS;
				break;
			default:
				offset = 0;
				family = sc->gif_psrc->sa_family;
				break;
			}
			m->m_data += offset;
			m->m_len -= offset;
			m->m_pkthdr.len -= offset;
			bpf_mtap_af(ifp->if_bpf, family, m, BPF_DIRECTION_OUT);
			m->m_data -= offset;
			m->m_len += offset;
			m->m_pkthdr.len += offset;
		}
#endif

		/* XXX we should cache the outgoing route */

		switch (sc->gif_psrc->sa_family) {
		case AF_INET:
			ip_output(m, NULL, NULL, 0, NULL, NULL, 0);
			break;
#ifdef INET6
		case AF_INET6:
			/*
			 * force fragmentation to minimum MTU, to avoid path
			 * MTU discovery. It is too painful to ask for resend
			 * of inner packet, to achieve path MTU discovery for
			 * encapsulated packets.
			 */
			ip6_output(m, 0, NULL, IPV6_MINMTU, 0, NULL);
			break;
#endif
		default:
			m_freem(m);
			break;
		}
	}
}

int
gif_encap(struct ifnet *ifp, struct mbuf **mp, sa_family_t af)
{
	struct gif_softc *sc = (struct gif_softc*)ifp;
	int error = 0;

	/*
	 * Remove multicast and broadcast flags or encapsulated packet
	 * ends up as multicast or broadcast packet.
	 */
	(*mp)->m_flags &= ~(M_BCAST|M_MCAST);

	/*
	 * Encapsulate packet. Add IP or IP6 header depending on tunnel AF.
	 */
	switch (sc->gif_psrc->sa_family) {
	case AF_INET:
		error = in_gif_output(ifp, af, mp);
		break;
#ifdef INET6
	case AF_INET6:
		error = in6_gif_output(ifp, af, mp);
		break;
#endif
	default:
		m_freemp(mp);
		error = EAFNOSUPPORT;
		break;
	}

	if (error)
		return (error);

	error = gif_checkloop(ifp, *mp);
	return (error);
}

int
gif_output(struct ifnet *ifp, struct mbuf *m, struct sockaddr *dst,
    struct rtentry *rt)
{
	struct gif_softc *sc = (struct gif_softc*)ifp;
	int error = 0;

	if (!(ifp->if_flags & IFF_UP) ||
	    sc->gif_psrc == NULL || sc->gif_pdst == NULL ||
	    sc->gif_psrc->sa_family != sc->gif_pdst->sa_family) {
		m_freem(m);
		error = ENETDOWN;
		goto end;
	}

	error = gif_encap(ifp, &m, dst->sa_family);
	if (error)
		goto end;

	error = if_enqueue(ifp, m);

end:
	if (error)
		ifp->if_oerrors++;
	return (error);
}

int
gif_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct gif_softc *sc  = (struct gif_softc*)ifp;
	struct ifreq     *ifr = (struct ifreq *)data;
	int error = 0, size;
	struct sockaddr *dst, *src;
	struct sockaddr *sa;
	struct gif_softc *sc2;

	switch (cmd) {
	case SIOCSIFADDR:
		break;

	case SIOCSIFDSTADDR:
		break;

	case SIOCADDMULTI:
	case SIOCDELMULTI:
		break;

	case SIOCSLIFPHYADDR:
		src = sstosa(&(((struct if_laddrreq *)data)->addr));
		dst = sstosa(&(((struct if_laddrreq *)data)->dstaddr));

		/* sa_family must be equal */
		if (src->sa_family != dst->sa_family)
			return (EINVAL);

		/* validate sa_len */
		switch (src->sa_family) {
		case AF_INET:
			if (src->sa_len != sizeof(struct sockaddr_in))
				return (EINVAL);
			break;
#ifdef INET6
		case AF_INET6:
			if (src->sa_len != sizeof(struct sockaddr_in6))
				return (EINVAL);
			break;
#endif
		default:
			return (EAFNOSUPPORT);
		}
		switch (dst->sa_family) {
		case AF_INET:
			if (dst->sa_len != sizeof(struct sockaddr_in))
				return (EINVAL);
			break;
#ifdef INET6
		case AF_INET6:
			if (dst->sa_len != sizeof(struct sockaddr_in6))
				return (EINVAL);
			break;
#endif
		default:
			return (EAFNOSUPPORT);
		}

		LIST_FOREACH(sc2, &gif_softc_list, gif_list) {
			if (sc2 == sc)
				continue;
			if (!sc2->gif_pdst || !sc2->gif_psrc)
				continue;
			if (sc2->gif_pdst->sa_family != dst->sa_family ||
			    sc2->gif_pdst->sa_len != dst->sa_len ||
			    sc2->gif_psrc->sa_family != src->sa_family ||
			    sc2->gif_psrc->sa_len != src->sa_len)
				continue;
			/* can't configure same pair of address onto two gifs */
			if (bcmp(sc2->gif_pdst, dst, dst->sa_len) == 0 &&
			    bcmp(sc2->gif_psrc, src, src->sa_len) == 0) {
				error = EADDRNOTAVAIL;
				goto bad;
			}

			/* can't configure multiple multi-dest interfaces */
#define multidest(x) \
	(satosin(x)->sin_addr.s_addr == INADDR_ANY)
#ifdef INET6
#define multidest6(x) \
	(IN6_IS_ADDR_UNSPECIFIED(&satosin6(x)->sin6_addr))
#endif
			if (dst->sa_family == AF_INET &&
			    multidest(dst) && multidest(sc2->gif_pdst)) {
				error = EADDRNOTAVAIL;
				goto bad;
			}
#ifdef INET6
			if (dst->sa_family == AF_INET6 &&
			    multidest6(dst) && multidest6(sc2->gif_pdst)) {
				error = EADDRNOTAVAIL;
				goto bad;
			}
#endif
		}

		if (sc->gif_psrc)
			free((caddr_t)sc->gif_psrc, M_IFADDR, 0);
		sa = malloc(src->sa_len, M_IFADDR, M_WAITOK);
		bcopy((caddr_t)src, (caddr_t)sa, src->sa_len);
		sc->gif_psrc = sa;

		if (sc->gif_pdst)
			free((caddr_t)sc->gif_pdst, M_IFADDR, 0);
		sa = malloc(dst->sa_len, M_IFADDR, M_WAITOK);
		bcopy((caddr_t)dst, (caddr_t)sa, dst->sa_len);
		sc->gif_pdst = sa;

		ifp->if_flags |= IFF_RUNNING;
		if_up(ifp);		/* send up RTM_IFINFO */

		error = 0;
		break;

	case SIOCDIFPHYADDR:
		if (sc->gif_psrc) {
			free((caddr_t)sc->gif_psrc, M_IFADDR, 0);
			sc->gif_psrc = NULL;
		}
		if (sc->gif_pdst) {
			free((caddr_t)sc->gif_pdst, M_IFADDR, 0);
			sc->gif_pdst = NULL;
		}
		/* change the IFF_{UP, RUNNING} flag as well? */
		break;

	case SIOCGLIFPHYADDR:
		if (sc->gif_psrc == NULL || sc->gif_pdst == NULL) {
			error = EADDRNOTAVAIL;
			goto bad;
		}

		/* copy src */
		src = sc->gif_psrc;
		dst = sstosa(&(((struct if_laddrreq *)data)->addr));
		size = sizeof(((struct if_laddrreq *)data)->addr);
		if (src->sa_len > size)
			return (EINVAL);
		bcopy((caddr_t)src, (caddr_t)dst, src->sa_len);

		/* copy dst */
		src = sc->gif_pdst;
		dst = sstosa(&(((struct if_laddrreq *)data)->dstaddr));
		size = sizeof(((struct if_laddrreq *)data)->dstaddr);
		if (src->sa_len > size)
			return (EINVAL);
		bcopy((caddr_t)src, (caddr_t)dst, src->sa_len);
		break;

	case SIOCSIFFLAGS:
		/* if_ioctl() takes care of it */
		break;

	case SIOCSIFMTU:
		if (ifr->ifr_mtu < GIF_MTU_MIN || ifr->ifr_mtu > GIF_MTU_MAX)
			error = EINVAL;
		else
			ifp->if_mtu = ifr->ifr_mtu;
		break;

	case SIOCSLIFPHYRTABLE:
		if (ifr->ifr_rdomainid < 0 ||
		    ifr->ifr_rdomainid > RT_TABLEID_MAX ||
		    !rtable_exists(ifr->ifr_rdomainid)) {
			error = EINVAL;
			break;
		}
		sc->gif_rtableid = ifr->ifr_rdomainid;
		break;
	case SIOCGLIFPHYRTABLE:
		ifr->ifr_rdomainid = sc->gif_rtableid;
		break;
	default:
		error = ENOTTY;
		break;
	}
 bad:
	return (error);
}

int
gif_checkloop(struct ifnet *ifp, struct mbuf *m)
{
	struct m_tag *mtag;

	/*
	 * gif may cause infinite recursion calls when misconfigured.
	 * We'll prevent this by detecting loops.
	 */
	for (mtag = m_tag_find(m, PACKET_TAG_GIF, NULL); mtag;
	    mtag = m_tag_find(m, PACKET_TAG_GIF, mtag)) {
		if (*(struct ifnet **)(mtag + 1) == ifp) {
			log(LOG_NOTICE, "gif_output: "
			    "recursively called too many times\n");
			m_freem(m);
			return ENETUNREACH;
		}
	}

	mtag = m_tag_get(PACKET_TAG_GIF, sizeof(struct ifnet *), M_NOWAIT);
	if (mtag == NULL) {
		m_freem(m);
		return ENOMEM;
	}
	*(struct ifnet **)(mtag + 1) = ifp;
	m_tag_prepend(m, mtag);
	return 0;
}

int
in_gif_output(struct ifnet *ifp, int family, struct mbuf **m0)
{
	struct gif_softc *sc = (struct gif_softc*)ifp;
	struct sockaddr_in *sin_src = satosin(sc->gif_psrc);
	struct sockaddr_in *sin_dst = satosin(sc->gif_pdst);
	struct tdb tdb;
	struct xformsw xfs;
	int error;
	struct mbuf *m = *m0;

	if (sin_src == NULL || sin_dst == NULL ||
	    sin_src->sin_family != AF_INET ||
	    sin_dst->sin_family != AF_INET) {
		m_freem(m);
		return EAFNOSUPPORT;
	}

#ifdef DIAGNOSTIC
	if (ifp->if_rdomain != rtable_l2(m->m_pkthdr.ph_rtableid)) {
		printf("%s: trying to send packet on wrong domain. "
		    "if %d vs. mbuf %d, AF %d\n", ifp->if_xname,
		    ifp->if_rdomain, rtable_l2(m->m_pkthdr.ph_rtableid),
		    family);
	}
#endif

	/* setup dummy tdb.  it highly depends on ipip_output() code. */
	bzero(&tdb, sizeof(tdb));
	bzero(&xfs, sizeof(xfs));
	tdb.tdb_src.sin.sin_family = AF_INET;
	tdb.tdb_src.sin.sin_len = sizeof(struct sockaddr_in);
	tdb.tdb_src.sin.sin_addr = sin_src->sin_addr;
	tdb.tdb_dst.sin.sin_family = AF_INET;
	tdb.tdb_dst.sin.sin_len = sizeof(struct sockaddr_in);
	tdb.tdb_dst.sin.sin_addr = sin_dst->sin_addr;
	tdb.tdb_xform = &xfs;
	xfs.xf_type = -1;	/* not XF_IP4 */

	switch (family) {
	case AF_INET:
		break;
#ifdef INET6
	case AF_INET6:
		break;
#endif
#if MPLS
	case AF_MPLS:
		break;
#endif
	default:
#ifdef DEBUG
	        printf("%s: warning: unknown family %d passed\n", __func__,
			family);
#endif
		m_freem(m);
		return EAFNOSUPPORT;
	}

	/* encapsulate into IPv4 packet */
	*m0 = NULL;
#ifdef MPLS
	if (family == AF_MPLS)
		error = etherip_output(m, &tdb, m0, IPPROTO_MPLS);
	else
#endif
	error = ipip_output(m, &tdb, m0, 0, 0);
	if (error)
		return error;
	else if (*m0 == NULL)
		return EFAULT;

	m = *m0;

	m->m_pkthdr.ph_rtableid = sc->gif_rtableid;
#if NPF > 0
	pf_pkt_addr_changed(m);
#endif
	return 0;
}

int
in_gif_input(struct mbuf **mp, int *offp, int proto, int af)
{
	struct mbuf *m = *mp;
	struct gif_softc *sc;
	struct ifnet *gifp = NULL;
	struct ip *ip;

	/* IP-in-IP header is caused by tunnel mode, so skip gif lookup */
	if (m->m_flags & M_TUNNEL) {
		m->m_flags &= ~M_TUNNEL;
		goto inject;
	}

	ip = mtod(m, struct ip *);

	/* this code will be soon improved. */
	LIST_FOREACH(sc, &gif_softc_list, gif_list) {
		if (sc->gif_psrc == NULL || sc->gif_pdst == NULL ||
		    sc->gif_psrc->sa_family != AF_INET ||
		    sc->gif_pdst->sa_family != AF_INET ||
		    rtable_l2(sc->gif_rtableid) !=
		    rtable_l2(m->m_pkthdr.ph_rtableid)) {
			continue;
		}

		if ((sc->gif_if.if_flags & IFF_UP) == 0)
			continue;

		if (in_hosteq(satosin(sc->gif_psrc)->sin_addr, ip->ip_dst) &&
		    in_hosteq(satosin(sc->gif_pdst)->sin_addr, ip->ip_src)) {
			gifp = &sc->gif_if;
			break;
		}
	}

	if (gifp) {
		m->m_pkthdr.ph_ifidx = gifp->if_index;
		m->m_pkthdr.ph_rtableid = gifp->if_rdomain;
		gifp->if_ipackets++;
		gifp->if_ibytes += m->m_pkthdr.len;
		/* We have a configured GIF */
		return ipip_input_if(mp, offp, proto, af, gifp);
	}

inject:
	/* No GIF interface was configured */
	return ipip_input(mp, offp, proto, af);
}

#ifdef INET6
int
in6_gif_output(struct ifnet *ifp, int family, struct mbuf **m0)
{
	struct gif_softc *sc = (struct gif_softc*)ifp;
	struct sockaddr_in6 *sin6_src = satosin6(sc->gif_psrc);
	struct sockaddr_in6 *sin6_dst = satosin6(sc->gif_pdst);
	struct tdb tdb;
	struct xformsw xfs;
	int error;
	struct mbuf *m = *m0;

	if (sin6_src == NULL || sin6_dst == NULL ||
	    sin6_src->sin6_family != AF_INET6 ||
	    sin6_dst->sin6_family != AF_INET6) {
		m_freem(m);
		return EAFNOSUPPORT;
	}

	/* setup dummy tdb.  it highly depends on ipip_output() code. */
	bzero(&tdb, sizeof(tdb));
	bzero(&xfs, sizeof(xfs));
	tdb.tdb_src.sin6.sin6_family = AF_INET6;
	tdb.tdb_src.sin6.sin6_len = sizeof(struct sockaddr_in6);
	tdb.tdb_src.sin6.sin6_addr = sin6_src->sin6_addr;
	tdb.tdb_src.sin6.sin6_scope_id = sin6_src->sin6_scope_id;
	tdb.tdb_dst.sin6.sin6_family = AF_INET6;
	tdb.tdb_dst.sin6.sin6_len = sizeof(struct sockaddr_in6);
	tdb.tdb_dst.sin6.sin6_addr = sin6_dst->sin6_addr;
	tdb.tdb_src.sin6.sin6_scope_id = sin6_dst->sin6_scope_id;
	tdb.tdb_xform = &xfs;
	xfs.xf_type = -1;	/* not XF_IP4 */

	switch (family) {
	case AF_INET:
		break;
#ifdef INET6
	case AF_INET6:
		break;
#endif
#ifdef MPLS
	case AF_MPLS:
		break;
#endif
	default:
#ifdef DEBUG
		printf("%s: warning: unknown family %d passed\n", __func__,
			family);
#endif
		m_freem(m);
		return EAFNOSUPPORT;
	}

	/* encapsulate into IPv6 packet */
	*m0 = NULL;
#if MPLS
	if (family == AF_MPLS)
		error = etherip_output(m, &tdb, m0, IPPROTO_MPLS);
	else
#endif
	error = ipip_output(m, &tdb, m0, 0, 0);
	if (error)
	        return error;
	else if (*m0 == NULL)
	        return EFAULT;

	m = *m0;

#if NPF > 0
	pf_pkt_addr_changed(m);
#endif
	return 0;
}

int in6_gif_input(struct mbuf **mp, int *offp, int proto, int af)
{
	struct mbuf *m = *mp;
	struct gif_softc *sc;
	struct ifnet *gifp = NULL;
	struct ip6_hdr *ip6;
	struct sockaddr_in6 src, dst;
	struct sockaddr_in6 *psrc, *pdst;

	/* XXX What if we run transport-mode IPsec to protect gif tunnel ? */
	if (m->m_flags & (M_AUTH | M_CONF))
	        goto inject;

	ip6 = mtod(m, struct ip6_hdr *);
	in6_recoverscope(&src, &ip6->ip6_src);
	in6_recoverscope(&dst, &ip6->ip6_dst);

	LIST_FOREACH(sc, &gif_softc_list, gif_list) {
		if (sc->gif_psrc == NULL || sc->gif_pdst == NULL ||
		    sc->gif_psrc->sa_family != AF_INET6 ||
		    sc->gif_pdst->sa_family != AF_INET6) {
			continue;
		}

		if ((sc->gif_if.if_flags & IFF_UP) == 0)
			continue;

		psrc = satosin6(sc->gif_psrc);
		pdst = satosin6(sc->gif_pdst);

		if (IN6_ARE_ADDR_EQUAL(&psrc->sin6_addr, &dst.sin6_addr) &&
		    psrc->sin6_scope_id == dst.sin6_scope_id &&
		    IN6_ARE_ADDR_EQUAL(&pdst->sin6_addr, &src.sin6_addr) &&
		    pdst->sin6_scope_id == src.sin6_scope_id) {
			gifp = &sc->gif_if;
			break;
		}
	}

	if (gifp) {
	        m->m_pkthdr.ph_ifidx = gifp->if_index;
		gifp->if_ipackets++;
		gifp->if_ibytes += m->m_pkthdr.len;
		return ipip_input_if(mp, offp, proto, af, gifp);
	}

inject:
	/* No GIF tunnel configured */
	return ipip_input(mp, offp, proto, af);
}
#endif /* INET6 */
