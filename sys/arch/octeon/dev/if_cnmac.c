/*	$OpenBSD: if_cnmac.c,v 1.61 2016/11/05 05:14:18 visa Exp $	*/

/*
 * Copyright (c) 2007 Internet Initiative Japan, Inc.
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
#include "bpfilter.h"

/*
 * XXXSEIL
 * If no free send buffer is available, free all the sent buffer and bail out.
 */
#define OCTEON_ETH_SEND_QUEUE_CHECK

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/pool.h>
#include <sys/proc.h>
#include <sys/mbuf.h>
#include <sys/malloc.h>
#include <sys/kernel.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/errno.h>
#include <sys/device.h>
#include <sys/queue.h>
#include <sys/conf.h>
#include <sys/stdint.h> /* uintptr_t */
#include <sys/syslog.h>
#include <sys/endian.h>
#include <sys/atomic.h>

#include <net/if.h>
#include <net/if_media.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>

#if NBPFILTER > 0
#include <net/bpf.h>
#endif

#include <machine/bus.h>
#include <machine/intr.h>
#include <machine/octeonvar.h>
#include <machine/octeon_model.h>

#include <dev/mii/mii.h>
#include <dev/mii/miivar.h>

#include <octeon/dev/cn30xxciureg.h>
#include <octeon/dev/cn30xxnpireg.h>
#include <octeon/dev/cn30xxgmxreg.h>
#include <octeon/dev/cn30xxipdreg.h>
#include <octeon/dev/cn30xxpipreg.h>
#include <octeon/dev/cn30xxpowreg.h>
#include <octeon/dev/cn30xxfaureg.h>
#include <octeon/dev/cn30xxfpareg.h>
#include <octeon/dev/cn30xxbootbusreg.h>
#include <octeon/dev/cn30xxfpavar.h>
#include <octeon/dev/cn30xxgmxvar.h>
#include <octeon/dev/cn30xxfauvar.h>
#include <octeon/dev/cn30xxpowvar.h>
#include <octeon/dev/cn30xxipdvar.h>
#include <octeon/dev/cn30xxpipvar.h>
#include <octeon/dev/cn30xxpkovar.h>
#include <octeon/dev/cn30xxsmivar.h>
#include <octeon/dev/iobusvar.h>
#include <octeon/dev/if_cnmacvar.h>

#ifdef OCTEON_ETH_DEBUG
#define	OCTEON_ETH_KASSERT(x)	KASSERT(x)
#define	OCTEON_ETH_KDASSERT(x)	KDASSERT(x)
#else
#define	OCTEON_ETH_KASSERT(x)
#define	OCTEON_ETH_KDASSERT(x)
#endif

/*
 * Set the PKO to think command buffers are an odd length.  This makes it so we
 * never have to divide a comamnd across two buffers.
 */
#define OCTEON_POOL_NWORDS_CMD	\
	    (((uint32_t)OCTEON_POOL_SIZE_CMD / sizeof(uint64_t)) - 1)
#define FPA_COMMAND_BUFFER_POOL_NWORDS	OCTEON_POOL_NWORDS_CMD	/* XXX */

void	octeon_eth_buf_init(struct octeon_eth_softc *);

int	octeon_eth_match(struct device *, void *, void *);
void	octeon_eth_attach(struct device *, struct device *, void *);
void	octeon_eth_pip_init(struct octeon_eth_softc *);
void	octeon_eth_ipd_init(struct octeon_eth_softc *);
void	octeon_eth_pko_init(struct octeon_eth_softc *);
void	octeon_eth_smi_init(struct octeon_eth_softc *);

void	octeon_eth_board_mac_addr(uint8_t *);

int	octeon_eth_mii_readreg(struct device *, int, int);
void	octeon_eth_mii_writereg(struct device *, int, int, int);
void	octeon_eth_mii_statchg(struct device *);

int	octeon_eth_mediainit(struct octeon_eth_softc *);
void	octeon_eth_mediastatus(struct ifnet *, struct ifmediareq *);
int	octeon_eth_mediachange(struct ifnet *);

void	octeon_eth_send_queue_flush_prefetch(struct octeon_eth_softc *);
void	octeon_eth_send_queue_flush_fetch(struct octeon_eth_softc *);
void	octeon_eth_send_queue_flush(struct octeon_eth_softc *);
int	octeon_eth_send_queue_is_full(struct octeon_eth_softc *);
void	octeon_eth_send_queue_add(struct octeon_eth_softc *,
	    struct mbuf *, uint64_t *);
void	octeon_eth_send_queue_del(struct octeon_eth_softc *,
	    struct mbuf **, uint64_t **);
int	octeon_eth_buf_free_work(struct octeon_eth_softc *, uint64_t *);
void	octeon_eth_buf_ext_free(caddr_t, u_int, void *);

int	octeon_eth_ioctl(struct ifnet *, u_long, caddr_t);
void	octeon_eth_watchdog(struct ifnet *);
int	octeon_eth_init(struct ifnet *);
int	octeon_eth_stop(struct ifnet *, int);
void	octeon_eth_start(struct ifnet *);

int	octeon_eth_send_cmd(struct octeon_eth_softc *, uint64_t, uint64_t);
uint64_t octeon_eth_send_makecmd_w1(int, paddr_t);
uint64_t octeon_eth_send_makecmd_w0(uint64_t, uint64_t, size_t, int, int);
int	octeon_eth_send_makecmd_gbuf(struct octeon_eth_softc *,
	    struct mbuf *, uint64_t *, int *);
int	octeon_eth_send_makecmd(struct octeon_eth_softc *,
	    struct mbuf *, uint64_t *, uint64_t *, uint64_t *);
int	octeon_eth_send_buf(struct octeon_eth_softc *,
	    struct mbuf *, uint64_t *);
int	octeon_eth_send(struct octeon_eth_softc *, struct mbuf *);

int	octeon_eth_reset(struct octeon_eth_softc *);
int	octeon_eth_configure(struct octeon_eth_softc *);
int	octeon_eth_configure_common(struct octeon_eth_softc *);

void	octeon_eth_free_task(void *);
void	octeon_eth_tick_free(void *arg);
void	octeon_eth_tick_misc(void *);

int	octeon_eth_recv_mbuf(struct octeon_eth_softc *,
	    uint64_t *, struct mbuf **, int *);
int	octeon_eth_recv_check(struct octeon_eth_softc *, uint64_t);
int	octeon_eth_recv(struct octeon_eth_softc *, uint64_t *);
void	octeon_eth_recv_intr(void *, uint64_t *);

int	octeon_eth_mbuf_alloc(int);

/* device driver context */
struct	octeon_eth_softc *octeon_eth_gsc[GMX_PORT_NUNITS];
void	*octeon_eth_pow_recv_ih;

/* device parameters */
int	octeon_eth_param_pko_cmd_w0_n2 = 1;

const struct cfattach cnmac_ca =
    { sizeof(struct octeon_eth_softc), octeon_eth_match, octeon_eth_attach };

struct cfdriver cnmac_cd = { NULL, "cnmac", DV_IFNET };

/* ---- buffer management */

const struct octeon_eth_pool_param {
	int			poolno;
	size_t			size;
	size_t			nelems;
} octeon_eth_pool_params[] = {
#define	_ENTRY(x)	{ OCTEON_POOL_NO_##x, OCTEON_POOL_SIZE_##x, OCTEON_POOL_NELEMS_##x }
	_ENTRY(WQE),
	_ENTRY(CMD),
	_ENTRY(SG)
#undef	_ENTRY
};
struct cn30xxfpa_buf	*octeon_eth_pools[8/* XXX */];
#define	octeon_eth_fb_wqe	octeon_eth_pools[OCTEON_POOL_NO_WQE]
#define	octeon_eth_fb_cmd	octeon_eth_pools[OCTEON_POOL_NO_CMD]
#define	octeon_eth_fb_sg	octeon_eth_pools[OCTEON_POOL_NO_SG]

uint64_t octeon_eth_mac_addr = 0;
uint32_t octeon_eth_mac_addr_offset = 0;

int	octeon_eth_mbufs_to_alloc;

void
octeon_eth_buf_init(struct octeon_eth_softc *sc)
{
	static int once;
	int i;
	const struct octeon_eth_pool_param *pp;
	struct cn30xxfpa_buf *fb;

	if (once == 1)
		return;
	once = 1;

	for (i = 0; i < (int)nitems(octeon_eth_pool_params); i++) {
		pp = &octeon_eth_pool_params[i];
		cn30xxfpa_buf_init(pp->poolno, pp->size, pp->nelems, &fb);
		octeon_eth_pools[pp->poolno] = fb;
	}
}

/* ---- autoconf */

int
octeon_eth_match(struct device *parent, void *match, void *aux)
{
	struct cfdata *cf = (struct cfdata *)match;
	struct cn30xxgmx_attach_args *ga = aux;

	if (strcmp(cf->cf_driver->cd_name, ga->ga_name) != 0) {
		return 0;
	}
	return 1;
}

void
octeon_eth_attach(struct device *parent, struct device *self, void *aux)
{
	struct octeon_eth_softc *sc = (void *)self;
	struct cn30xxgmx_attach_args *ga = aux;
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;
	uint8_t enaddr[ETHER_ADDR_LEN];

	KASSERT(MCLBYTES >= OCTEON_POOL_SIZE_PKT + CACHE_LINE_SIZE);

	atomic_add_int(&octeon_eth_mbufs_to_alloc,
	    octeon_eth_mbuf_alloc(OCTEON_ETH_MBUFS_PER_PORT));

	sc->sc_regt = ga->ga_regt;
	sc->sc_dmat = ga->ga_dmat;
	sc->sc_port = ga->ga_portno;
	sc->sc_port_type = ga->ga_port_type;
	sc->sc_gmx = ga->ga_gmx;
	sc->sc_gmx_port = ga->ga_gmx_port;
	sc->sc_phy_addr = ga->ga_phy_addr;

	sc->sc_init_flag = 0;

	/*
	 * XXX
	 * Setting PIP_IP_OFFSET[OFFSET] to 8 causes panic ... why???
	 */
	sc->sc_ip_offset = 0/* XXX */;

	octeon_eth_board_mac_addr(enaddr);
	printf(", address %s\n", ether_sprintf(enaddr));

	octeon_eth_gsc[sc->sc_port] = sc;

	ml_init(&sc->sc_sendq);
	sc->sc_soft_req_thresh = 15/* XXX */;
	sc->sc_ext_callback_cnt = 0;

	cn30xxgmx_stats_init(sc->sc_gmx_port);

	task_set(&sc->sc_free_task, octeon_eth_free_task, sc);
	timeout_set(&sc->sc_tick_misc_ch, octeon_eth_tick_misc, sc);
	timeout_set(&sc->sc_tick_free_ch, octeon_eth_tick_free, sc);

	cn30xxfau_op_init(&sc->sc_fau_done,
	    OCTEON_CVMSEG_ETHER_OFFSET(sc->sc_dev.dv_unit, csm_ether_fau_done),
	    OCT_FAU_REG_ADDR_END - (8 * (sc->sc_dev.dv_unit + 1))/* XXX */);
	cn30xxfau_op_set_8(&sc->sc_fau_done, 0);

	octeon_eth_pip_init(sc);
	octeon_eth_ipd_init(sc);
	octeon_eth_pko_init(sc);
	octeon_eth_smi_init(sc);

	sc->sc_gmx_port->sc_ipd = sc->sc_ipd;
	sc->sc_gmx_port->sc_port_mii = &sc->sc_mii;
	sc->sc_gmx_port->sc_port_ac = &sc->sc_arpcom;

	/* XXX */
	sc->sc_pow = &cn30xxpow_softc;

	octeon_eth_mediainit(sc);

	strncpy(ifp->if_xname, sc->sc_dev.dv_xname, sizeof(ifp->if_xname));
	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_xflags = IFXF_MPSAFE;
	ifp->if_ioctl = octeon_eth_ioctl;
	ifp->if_start = octeon_eth_start;
	ifp->if_watchdog = octeon_eth_watchdog;
	ifp->if_hardmtu = OCTEON_ETH_MAX_MTU;
	IFQ_SET_MAXLEN(&ifp->if_snd, max(GATHER_QUEUE_SIZE, IFQ_MAXLEN));

	ifp->if_capabilities = IFCAP_VLAN_MTU | IFCAP_CSUM_TCPv4 |
	    IFCAP_CSUM_UDPv4 | IFCAP_CSUM_TCPv6 | IFCAP_CSUM_UDPv6;

	cn30xxgmx_set_mac_addr(sc->sc_gmx_port, enaddr);
	cn30xxgmx_set_filter(sc->sc_gmx_port);

	if_attach(ifp);

	memcpy(sc->sc_arpcom.ac_enaddr, enaddr, ETHER_ADDR_LEN);
	ether_ifattach(ifp);

#if 1
	octeon_eth_buf_init(sc);
#endif

	if (octeon_eth_pow_recv_ih == NULL)
		octeon_eth_pow_recv_ih = cn30xxpow_intr_establish(
		    OCTEON_POW_GROUP_PIP, IPL_NET | IPL_MPSAFE,
		    octeon_eth_recv_intr, NULL, NULL, cnmac_cd.cd_name);
}

/* ---- submodules */

/* XXX */
void
octeon_eth_pip_init(struct octeon_eth_softc *sc)
{
	struct cn30xxpip_attach_args pip_aa;

	pip_aa.aa_port = sc->sc_port;
	pip_aa.aa_regt = sc->sc_regt;
	pip_aa.aa_tag_type = POW_TAG_TYPE_ORDERED/* XXX */;
	pip_aa.aa_receive_group = OCTEON_POW_GROUP_PIP;
	pip_aa.aa_ip_offset = sc->sc_ip_offset;
	cn30xxpip_init(&pip_aa, &sc->sc_pip);
}

/* XXX */
void
octeon_eth_ipd_init(struct octeon_eth_softc *sc)
{
	struct cn30xxipd_attach_args ipd_aa;

	ipd_aa.aa_port = sc->sc_port;
	ipd_aa.aa_regt = sc->sc_regt;
	ipd_aa.aa_first_mbuff_skip = 0/* XXX */;
	ipd_aa.aa_not_first_mbuff_skip = 0/* XXX */;
	cn30xxipd_init(&ipd_aa, &sc->sc_ipd);
}

/* XXX */
void
octeon_eth_pko_init(struct octeon_eth_softc *sc)
{
	struct cn30xxpko_attach_args pko_aa;

	pko_aa.aa_port = sc->sc_port;
	pko_aa.aa_regt = sc->sc_regt;
	pko_aa.aa_cmdptr = &sc->sc_cmdptr;
	pko_aa.aa_cmd_buf_pool = OCTEON_POOL_NO_CMD;
	pko_aa.aa_cmd_buf_size = OCTEON_POOL_NWORDS_CMD;
	cn30xxpko_init(&pko_aa, &sc->sc_pko);
}

void
octeon_eth_smi_init(struct octeon_eth_softc *sc)
{
	struct cn30xxsmi_attach_args smi_aa;

	smi_aa.aa_port = sc->sc_port;
	smi_aa.aa_regt = sc->sc_regt;
	cn30xxsmi_init(&smi_aa, &sc->sc_smi);
	cn30xxsmi_set_clock(sc->sc_smi, 0x1464ULL); /* XXX */
}

/* ---- XXX */

void
octeon_eth_board_mac_addr(uint8_t *enaddr)
{
	int id;

	/* Initialize MAC addresses from the global address base. */
	if (octeon_eth_mac_addr == 0) {
		memcpy((uint8_t *)&octeon_eth_mac_addr + 2,
		    octeon_boot_info->mac_addr_base, 6);

		/*
		 * Should be allowed to fail hard if couldn't read the
		 * mac_addr_base address...
		 */
		if (octeon_eth_mac_addr == 0)
			return;

		/*
		 * Calculate the offset from the mac_addr_base that will be used
		 * for the next sc->sc_port.
		 */
		id = octeon_get_chipid();

		switch (octeon_model_family(id)) {
		case OCTEON_MODEL_FAMILY_CN56XX:
			octeon_eth_mac_addr_offset = 1;
			break;
		/*
		case OCTEON_MODEL_FAMILY_CN52XX:
		case OCTEON_MODEL_FAMILY_CN63XX:
			octeon_eth_mac_addr_offset = 2;
			break;
		*/
		default:
			octeon_eth_mac_addr_offset = 0;
			break;
		}

		enaddr += octeon_eth_mac_addr_offset;
	}

	/* No more MAC addresses to assign. */
	if (octeon_eth_mac_addr_offset >= octeon_boot_info->mac_addr_count)
		return;

	if (enaddr)
		memcpy(enaddr, (uint8_t *)&octeon_eth_mac_addr + 2, 6);

	octeon_eth_mac_addr++;
	octeon_eth_mac_addr_offset++;
}

/* ---- media */

int
octeon_eth_mii_readreg(struct device *self, int phy_no, int reg)
{
	struct octeon_eth_softc *sc = (struct octeon_eth_softc *)self;
	return cn30xxsmi_read(sc->sc_smi, phy_no, reg);
}

void
octeon_eth_mii_writereg(struct device *self, int phy_no, int reg, int value)
{
	struct octeon_eth_softc *sc = (struct octeon_eth_softc *)self;
	cn30xxsmi_write(sc->sc_smi, phy_no, reg, value);
}

void
octeon_eth_mii_statchg(struct device *self)
{
	struct octeon_eth_softc *sc = (struct octeon_eth_softc *)self;
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;

	cn30xxpko_port_enable(sc->sc_pko, 0);
	cn30xxgmx_port_enable(sc->sc_gmx_port, 0);

	octeon_eth_reset(sc);

	if (ISSET(ifp->if_flags, IFF_RUNNING))
		cn30xxgmx_set_filter(sc->sc_gmx_port);

	cn30xxpko_port_enable(sc->sc_pko, 1);
	cn30xxgmx_port_enable(sc->sc_gmx_port, 1);
}

int
octeon_eth_mediainit(struct octeon_eth_softc *sc)
{
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;
	struct mii_softc *child;

	sc->sc_mii.mii_ifp = ifp;
	sc->sc_mii.mii_readreg = octeon_eth_mii_readreg;
	sc->sc_mii.mii_writereg = octeon_eth_mii_writereg;
	sc->sc_mii.mii_statchg = octeon_eth_mii_statchg;
	ifmedia_init(&sc->sc_mii.mii_media, 0, octeon_eth_mediachange,
	    octeon_eth_mediastatus);

	mii_attach(&sc->sc_dev, &sc->sc_mii,
	    0xffffffff, sc->sc_phy_addr, MII_OFFSET_ANY, MIIF_DOPAUSE);

	child = LIST_FIRST(&sc->sc_mii.mii_phys);
	if (child == NULL) {
                /* No PHY attached. */
		ifmedia_add(&sc->sc_mii.mii_media, IFM_ETHER | IFM_MANUAL,
			    0, NULL);
		ifmedia_set(&sc->sc_mii.mii_media, IFM_ETHER | IFM_MANUAL);
	} else {
		ifmedia_set(&sc->sc_mii.mii_media, IFM_ETHER | IFM_AUTO);
	}

	return 0;
}

void
octeon_eth_mediastatus(struct ifnet *ifp, struct ifmediareq *ifmr)
{
	struct octeon_eth_softc *sc = ifp->if_softc;

	mii_pollstat(&sc->sc_mii);
	ifmr->ifm_status = sc->sc_mii.mii_media_status;
	ifmr->ifm_active = sc->sc_mii.mii_media_active;
	ifmr->ifm_active = (sc->sc_mii.mii_media_active & ~IFM_ETH_FMASK) |
	    sc->sc_gmx_port->sc_port_flowflags;
}

int
octeon_eth_mediachange(struct ifnet *ifp)
{
	struct octeon_eth_softc *sc = ifp->if_softc;

	if ((ifp->if_flags & IFF_UP) == 0)
		return 0;

	return mii_mediachg(&sc->sc_mii);
}

/* ---- send buffer garbage collection */

void
octeon_eth_send_queue_flush_prefetch(struct octeon_eth_softc *sc)
{
	OCTEON_ETH_KASSERT(sc->sc_prefetch == 0);
	cn30xxfau_op_inc_fetch_8(&sc->sc_fau_done, 0);
	sc->sc_prefetch = 1;
}

void
octeon_eth_send_queue_flush_fetch(struct octeon_eth_softc *sc)
{
#ifndef  OCTEON_ETH_DEBUG
	if (!sc->sc_prefetch)
		return;
#endif
	OCTEON_ETH_KASSERT(sc->sc_prefetch == 1);
	sc->sc_hard_done_cnt = cn30xxfau_op_inc_read_8(&sc->sc_fau_done);
	OCTEON_ETH_KASSERT(sc->sc_hard_done_cnt <= 0);
	sc->sc_prefetch = 0;
}

void
octeon_eth_send_queue_flush(struct octeon_eth_softc *sc)
{
	const int64_t sent_count = sc->sc_hard_done_cnt;
	int i;

	OCTEON_ETH_KASSERT(sent_count <= 0);

	for (i = 0; i < 0 - sent_count; i++) {
		struct mbuf *m;
		uint64_t *gbuf;

		octeon_eth_send_queue_del(sc, &m, &gbuf);

		cn30xxfpa_buf_put_paddr(octeon_eth_fb_sg, XKPHYS_TO_PHYS(gbuf));

		m_freem(m);
	}

	cn30xxfau_op_add_8(&sc->sc_fau_done, i);
}

int
octeon_eth_send_queue_is_full(struct octeon_eth_softc *sc)
{
#ifdef OCTEON_ETH_SEND_QUEUE_CHECK
	int64_t nofree_cnt;

	nofree_cnt = ml_len(&sc->sc_sendq) + sc->sc_hard_done_cnt; 

	if (__predict_false(nofree_cnt == GATHER_QUEUE_SIZE - 1)) {
		octeon_eth_send_queue_flush(sc);
		return 1;
	}

#endif
	return 0;
}

void
octeon_eth_send_queue_add(struct octeon_eth_softc *sc, struct mbuf *m,
    uint64_t *gbuf)
{
	OCTEON_ETH_KASSERT(m->m_flags & M_PKTHDR);

	m->m_pkthdr.ph_cookie = gbuf;
	ml_enqueue(&sc->sc_sendq, m);

	if (m->m_ext.ext_free_fn != 0)
		sc->sc_ext_callback_cnt++;
}

void
octeon_eth_send_queue_del(struct octeon_eth_softc *sc, struct mbuf **rm,
    uint64_t **rgbuf)
{
	struct mbuf *m;
	m = ml_dequeue(&sc->sc_sendq);
	OCTEON_ETH_KASSERT(m != NULL);

	*rm = m;
	*rgbuf = m->m_pkthdr.ph_cookie;

	if (m->m_ext.ext_free_fn != 0) {
		sc->sc_ext_callback_cnt--;
		OCTEON_ETH_KASSERT(sc->sc_ext_callback_cnt >= 0);
	}
}

int
octeon_eth_buf_free_work(struct octeon_eth_softc *sc, uint64_t *work)
{
	paddr_t addr, pktbuf;
	uint64_t word3;
	unsigned int back, nbufs;

	nbufs = (work[2] & PIP_WQE_WORD2_IP_BUFS) >>
	    PIP_WQE_WORD2_IP_BUFS_SHIFT;
	word3 = work[3];
	while (nbufs-- > 0) {
		addr = word3 & PIP_WQE_WORD3_ADDR, CCA_CACHED;
		back = (word3 & PIP_WQE_WORD3_BACK) >>
		    PIP_WQE_WORD3_BACK_SHIFT;
		pktbuf = (addr & ~(CACHE_LINE_SIZE - 1)) -
		    back * CACHE_LINE_SIZE;

		cn30xxfpa_store(pktbuf, OCTEON_POOL_NO_PKT,
		    OCTEON_POOL_SIZE_PKT / CACHE_LINE_SIZE);

		if (nbufs > 0)
			memcpy(&word3, (void *)PHYS_TO_XKPHYS(addr -
			    sizeof(word3), CCA_CACHED), sizeof(word3));
	}

	cn30xxfpa_buf_put_paddr(octeon_eth_fb_wqe, XKPHYS_TO_PHYS(work));

	return 0;
}

/* ---- ifnet interfaces */

int
octeon_eth_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct octeon_eth_softc *sc = ifp->if_softc;
	struct ifreq *ifr = (struct ifreq *)data;
	int s, error = 0;

	s = splnet();

	switch (cmd) {
	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;
		if (!(ifp->if_flags & IFF_RUNNING))
			octeon_eth_init(ifp);
		break;

	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP) {
			if (ifp->if_flags & IFF_RUNNING)
				error = ENETRESET;
			else
				octeon_eth_init(ifp);
		} else {
			if (ifp->if_flags & IFF_RUNNING)
				octeon_eth_stop(ifp, 0);
		}
		break;

	case SIOCSIFMEDIA:
		/* Flow control requires full-duplex mode. */
		if (IFM_SUBTYPE(ifr->ifr_media) == IFM_AUTO ||
		    (ifr->ifr_media & IFM_FDX) == 0) {
			ifr->ifr_media &= ~IFM_ETH_FMASK;
		}
		if (IFM_SUBTYPE(ifr->ifr_media) != IFM_AUTO) {
			if ((ifr->ifr_media & IFM_ETH_FMASK) == IFM_FLOW) {
				ifr->ifr_media |=
				    IFM_ETH_TXPAUSE | IFM_ETH_RXPAUSE;
			}
			sc->sc_gmx_port->sc_port_flowflags = 
				ifr->ifr_media & IFM_ETH_FMASK;
		}
		/* FALLTHROUGH */
	case SIOCGIFMEDIA:
		error = ifmedia_ioctl(ifp, ifr, &sc->sc_mii.mii_media, cmd);
		break;

	default:
		error = ether_ioctl(ifp, &sc->sc_arpcom, cmd, data);
	}

	if (error == ENETRESET) {
		if (ISSET(ifp->if_flags, IFF_RUNNING))
			cn30xxgmx_set_filter(sc->sc_gmx_port);
		error = 0;
	}

	if_start(ifp);

	splx(s);
	return (error);
}

/* ---- send (output) */

uint64_t
octeon_eth_send_makecmd_w0(uint64_t fau0, uint64_t fau1, size_t len, int segs,
    int ipoffp1)
{
	return cn30xxpko_cmd_word0(
		OCT_FAU_OP_SIZE_64,		/* sz1 */
		OCT_FAU_OP_SIZE_64,		/* sz0 */
		1, fau1, 1, fau0,		/* s1, reg1, s0, reg0 */
		0,				/* le */
		octeon_eth_param_pko_cmd_w0_n2,	/* n2 */
		1, 0,				/* q, r */
		(segs == 1) ? 0 : 1,		/* g */
		ipoffp1, 0, 1,			/* ipoffp1, ii, df */
		segs, (int)len);		/* segs, totalbytes */
}

uint64_t 
octeon_eth_send_makecmd_w1(int size, paddr_t addr)
{
	return cn30xxpko_cmd_word1(
		0, 0,				/* i, back */
		OCTEON_POOL_NO_SG,		/* pool */
		size, addr);			/* size, addr */
}

#define KVTOPHYS(addr)	if_cnmac_kvtophys((vaddr_t)(addr))
paddr_t if_cnmac_kvtophys(vaddr_t);

paddr_t
if_cnmac_kvtophys(vaddr_t kva)
{
	if (IS_XKPHYS(kva))
		return XKPHYS_TO_PHYS(kva);
	else if (kva >= CKSEG0_BASE && kva < CKSEG0_BASE + CKSEG_SIZE)
		return CKSEG0_TO_PHYS(kva);
	else if (kva >= CKSEG1_BASE && kva < CKSEG1_BASE + CKSEG_SIZE)
		return CKSEG1_TO_PHYS(kva);

	panic("%s: non-direct mapped address %p", __func__, (void *)kva);
}

int
octeon_eth_send_makecmd_gbuf(struct octeon_eth_softc *sc, struct mbuf *m0,
    uint64_t *gbuf, int *rsegs)
{
	struct mbuf *m;
	int segs = 0;

	for (m = m0; m != NULL; m = m->m_next) {
		if (__predict_false(m->m_len == 0))
			continue;

		if (segs >= OCTEON_POOL_SIZE_SG / sizeof(uint64_t))
			goto defrag;
		gbuf[segs] = octeon_eth_send_makecmd_w1(m->m_len,
		    KVTOPHYS(m->m_data));
		segs++;
	}

	*rsegs = segs;

	return 0;

defrag:
	if (m_defrag(m0, M_DONTWAIT) != 0)
		return 1;
	gbuf[0] = octeon_eth_send_makecmd_w1(m0->m_len, KVTOPHYS(m0->m_data));
	*rsegs = 1;
	return 0;
}

int
octeon_eth_send_makecmd(struct octeon_eth_softc *sc, struct mbuf *m,
    uint64_t *gbuf, uint64_t *rpko_cmd_w0, uint64_t *rpko_cmd_w1)
{
	uint64_t pko_cmd_w0, pko_cmd_w1;
	int ipoffp1;
	int segs;
	int result = 0;

	if (octeon_eth_send_makecmd_gbuf(sc, m, gbuf, &segs)) {
		log(LOG_WARNING, "%s: large number of transmission"
		    " data segments", sc->sc_dev.dv_xname);
		result = 1;
		goto done;
	}

	/* Get the IP packet offset for TCP/UDP checksum offloading. */
	ipoffp1 = (m->m_pkthdr.csum_flags & (M_TCP_CSUM_OUT | M_UDP_CSUM_OUT))
	    ? (ETHER_HDR_LEN + 1) : 0;

	/*
	 * segs == 1	-> link mode (single continuous buffer)
	 *		   WORD1[size] is number of bytes pointed by segment
	 *
	 * segs > 1	-> gather mode (scatter-gather buffer)
	 *		   WORD1[size] is number of segments
	 */
	pko_cmd_w0 = octeon_eth_send_makecmd_w0(sc->sc_fau_done.fd_regno,
	    0, m->m_pkthdr.len, segs, ipoffp1);
	pko_cmd_w1 = octeon_eth_send_makecmd_w1(
	    (segs == 1) ? m->m_pkthdr.len : segs,
	    (segs == 1) ? 
		KVTOPHYS(m->m_data) :
		XKPHYS_TO_PHYS(gbuf));

	*rpko_cmd_w0 = pko_cmd_w0;
	*rpko_cmd_w1 = pko_cmd_w1;

done:
	return result;
}

int
octeon_eth_send_cmd(struct octeon_eth_softc *sc, uint64_t pko_cmd_w0,
    uint64_t pko_cmd_w1)
{
	uint64_t *cmdptr;
	int result = 0;

	cmdptr = (uint64_t *)PHYS_TO_XKPHYS(sc->sc_cmdptr.cmdptr, CCA_CACHED);
	cmdptr += sc->sc_cmdptr.cmdptr_idx;

	OCTEON_ETH_KASSERT(cmdptr != NULL);

	*cmdptr++ = pko_cmd_w0;
	*cmdptr++ = pko_cmd_w1;

	OCTEON_ETH_KASSERT(sc->sc_cmdptr.cmdptr_idx + 2 <= FPA_COMMAND_BUFFER_POOL_NWORDS - 1);

	if (sc->sc_cmdptr.cmdptr_idx + 2 == FPA_COMMAND_BUFFER_POOL_NWORDS - 1) {
		paddr_t buf;

		buf = cn30xxfpa_buf_get_paddr(octeon_eth_fb_cmd);
		if (buf == 0) {
			log(LOG_WARNING,
			    "%s: cannot allocate command buffer from free pool allocator\n",
			    sc->sc_dev.dv_xname);
			result = 1;
			goto done;
		}
		*cmdptr++ = buf;
		sc->sc_cmdptr.cmdptr = (uint64_t)buf;
		sc->sc_cmdptr.cmdptr_idx = 0;
	} else {
		sc->sc_cmdptr.cmdptr_idx += 2;
	}

	cn30xxpko_op_doorbell_write(sc->sc_port, sc->sc_port, 2);

done:
	return result;
}

int
octeon_eth_send_buf(struct octeon_eth_softc *sc, struct mbuf *m,
    uint64_t *gbuf)
{
	int result = 0, error;
	uint64_t pko_cmd_w0, pko_cmd_w1;

	error = octeon_eth_send_makecmd(sc, m, gbuf, &pko_cmd_w0, &pko_cmd_w1);
	if (error != 0) {
		/* already logging */
		result = error;
		goto done;
	}

	error = octeon_eth_send_cmd(sc, pko_cmd_w0, pko_cmd_w1);
	if (error != 0) {
		/* already logging */
		result = error;
	}

done:
	return result;
}

int
octeon_eth_send(struct octeon_eth_softc *sc, struct mbuf *m)
{
	paddr_t gaddr = 0;
	uint64_t *gbuf = NULL;
	int result = 0, error;

	gaddr = cn30xxfpa_buf_get_paddr(octeon_eth_fb_sg);
	if (gaddr == 0) {
		log(LOG_WARNING,
		    "%s: cannot allocate gather buffer from free pool allocator\n",
		    sc->sc_dev.dv_xname);
		result = 1;
		goto done;
	}

	gbuf = (uint64_t *)(uintptr_t)PHYS_TO_XKPHYS(gaddr, CCA_CACHED);

	error = octeon_eth_send_buf(sc, m, gbuf);
	if (error != 0) {
		/* already logging */
		cn30xxfpa_buf_put_paddr(octeon_eth_fb_sg, gaddr);
		result = error;
		goto done;
	}

	octeon_eth_send_queue_add(sc, m, gbuf);

done:
	return result;
}

void
octeon_eth_start(struct ifnet *ifp)
{
	struct octeon_eth_softc *sc = ifp->if_softc;
	struct mbuf *m;

	if (__predict_false(!cn30xxgmx_link_status(sc->sc_gmx_port))) {
		ifq_purge(&ifp->if_snd);
		return;
	}

	/*
	 * performance tuning
	 * presend iobdma request 
	 */
	octeon_eth_send_queue_flush_prefetch(sc);

	for (;;) {
		octeon_eth_send_queue_flush_fetch(sc); /* XXX */

		/*
		 * XXXSEIL
		 * If no free send buffer is available, free all the sent buffer
		 * and bail out.
		 */
		if (octeon_eth_send_queue_is_full(sc)) {
			ifq_set_oactive(&ifp->if_snd);
			timeout_add(&sc->sc_tick_free_ch, 1);
			return;
		}

		m = ifq_dequeue(&ifp->if_snd);
		if (m == NULL)
			return;

#if NBPFILTER > 0
		if (ifp->if_bpf != NULL)
			bpf_mtap(ifp->if_bpf, m, BPF_DIRECTION_OUT);
#endif

		/* XXX */
		if (ml_len(&sc->sc_sendq) > sc->sc_soft_req_thresh)
			octeon_eth_send_queue_flush(sc);
		if (octeon_eth_send(sc, m)) {
			ifp->if_oerrors++;
			m_freem(m);
			log(LOG_WARNING,
		  	  "%s: failed to transmit packet\n",
		    	  sc->sc_dev.dv_xname);
		}
		/* XXX */

		/*
		 * send next iobdma request 
		 */
		octeon_eth_send_queue_flush_prefetch(sc);
	}

	octeon_eth_send_queue_flush_fetch(sc);
}

void
octeon_eth_watchdog(struct ifnet *ifp)
{
	struct octeon_eth_softc *sc = ifp->if_softc;

	printf("%s: device timeout\n", sc->sc_dev.dv_xname);

	octeon_eth_stop(ifp, 0);

	octeon_eth_configure(sc);

	SET(ifp->if_flags, IFF_RUNNING);
	ifp->if_timer = 0;

	ifq_restart(&ifp->if_snd);
}

int
octeon_eth_init(struct ifnet *ifp)
{
	struct octeon_eth_softc *sc = ifp->if_softc;

	/* XXX don't disable commonly used parts!!! XXX */
	if (sc->sc_init_flag == 0) {
		/* Cancel any pending I/O. */
		octeon_eth_stop(ifp, 0);

		/* Initialize the device */
		octeon_eth_configure(sc);

		cn30xxpko_enable(sc->sc_pko);
		cn30xxipd_enable(sc->sc_ipd);

		sc->sc_init_flag = 1;
	} else {
		cn30xxgmx_port_enable(sc->sc_gmx_port, 1);
	}
	octeon_eth_mediachange(ifp);

	cn30xxgmx_set_mac_addr(sc->sc_gmx_port, sc->sc_arpcom.ac_enaddr);
	cn30xxgmx_set_filter(sc->sc_gmx_port);

	timeout_add_sec(&sc->sc_tick_misc_ch, 1);
	timeout_add_sec(&sc->sc_tick_free_ch, 1);

	SET(ifp->if_flags, IFF_RUNNING);
	ifq_clr_oactive(&ifp->if_snd);

	return 0;
}

int
octeon_eth_stop(struct ifnet *ifp, int disable)
{
	struct octeon_eth_softc *sc = ifp->if_softc;

	CLR(ifp->if_flags, IFF_RUNNING);

	timeout_del(&sc->sc_tick_misc_ch);
	timeout_del(&sc->sc_tick_free_ch);

	mii_down(&sc->sc_mii);

	cn30xxgmx_port_enable(sc->sc_gmx_port, 0);

	intr_barrier(octeon_eth_pow_recv_ih);
	ifq_barrier(&ifp->if_snd);

	ifq_clr_oactive(&ifp->if_snd);
	ifp->if_timer = 0;

	return 0;
}

/* ---- misc */

#define PKO_INDEX_MASK	((1ULL << 12/* XXX */) - 1)

int
octeon_eth_reset(struct octeon_eth_softc *sc)
{
	cn30xxgmx_reset_speed(sc->sc_gmx_port);
	cn30xxgmx_reset_flowctl(sc->sc_gmx_port);
	cn30xxgmx_reset_timing(sc->sc_gmx_port);
	cn30xxgmx_reset_board(sc->sc_gmx_port);

	return 0;
}

int
octeon_eth_configure(struct octeon_eth_softc *sc)
{
	cn30xxgmx_port_enable(sc->sc_gmx_port, 0);

	octeon_eth_reset(sc);

	octeon_eth_configure_common(sc);

	cn30xxpko_port_config(sc->sc_pko);
	cn30xxpko_port_enable(sc->sc_pko, 1);
	cn30xxpip_port_config(sc->sc_pip);

	cn30xxgmx_tx_stats_rd_clr(sc->sc_gmx_port, 1);
	cn30xxgmx_rx_stats_rd_clr(sc->sc_gmx_port, 1);

	cn30xxgmx_port_enable(sc->sc_gmx_port, 1);

	return 0;
}

int
octeon_eth_configure_common(struct octeon_eth_softc *sc)
{
	static int once;

	uint64_t reg;

	if (once == 1)
		return 0;
	once = 1;

#if 0
	octeon_eth_buf_init(sc);
#endif

	cn30xxipd_config(sc->sc_ipd);
	cn30xxpko_config(sc->sc_pko);

	cn30xxpow_config(sc->sc_pow, OCTEON_POW_GROUP_PIP);

	/* Set padding for packets that Octeon does not recognize as IP. */
	reg = octeon_xkphys_read_8(PIP_GBL_CFG);
	reg &= ~PIP_GBL_CFG_NIP_SHF_MASK;
	reg |= ETHER_ALIGN << PIP_GBL_CFG_NIP_SHF_SHIFT;
	octeon_xkphys_write_8(PIP_GBL_CFG, reg);

	return 0;
}

int
octeon_eth_mbuf_alloc(int n)
{
	struct mbuf *m;
	paddr_t pktbuf;

	while (n > 0) {
		m = MCLGETI(NULL, M_NOWAIT, NULL,
		    OCTEON_POOL_SIZE_PKT + CACHE_LINE_SIZE);
		if (m == NULL || !ISSET(m->m_flags, M_EXT)) {
			m_freem(m);
			break;
		}

		m->m_data = (void *)(((vaddr_t)m->m_data + CACHE_LINE_SIZE) &
		    ~(CACHE_LINE_SIZE - 1));
		((struct mbuf **)m->m_data)[-1] = m;

		pktbuf = KVTOPHYS(m->m_data);
		m->m_pkthdr.ph_cookie = (void *)pktbuf;
		cn30xxfpa_store(pktbuf, OCTEON_POOL_NO_PKT,
		    OCTEON_POOL_SIZE_PKT / CACHE_LINE_SIZE);

		n--;
	}
	return n;
}

int
octeon_eth_recv_mbuf(struct octeon_eth_softc *sc, uint64_t *work,
    struct mbuf **rm, int *nmbuf)
{
	struct mbuf *m, *m0, *mprev, **pm;
	paddr_t addr, pktbuf;
	uint64_t word1 = work[1];
	uint64_t word2 = work[2];
	uint64_t word3 = work[3];
	unsigned int back, i, nbufs;
	unsigned int left, total, size;

	cn30xxfpa_buf_put_paddr(octeon_eth_fb_wqe, XKPHYS_TO_PHYS(work));

	nbufs = (word2 & PIP_WQE_WORD2_IP_BUFS) >> PIP_WQE_WORD2_IP_BUFS_SHIFT;
	if (nbufs == 0)
		panic("%s: dynamic short packet", __func__);

	m0 = mprev = NULL;
	total = left = (word1 & PIP_WQE_WORD1_LEN) >> 48;
	for (i = 0; i < nbufs; i++) {
		addr = word3 & PIP_WQE_WORD3_ADDR;
		back = (word3 & PIP_WQE_WORD3_BACK) >> PIP_WQE_WORD3_BACK_SHIFT;
		pktbuf = (addr & ~(CACHE_LINE_SIZE - 1)) -
		    back * CACHE_LINE_SIZE;
		pm = (struct mbuf **)PHYS_TO_XKPHYS(pktbuf, CCA_CACHED) - 1;
		m = *pm;
		*pm = NULL;
		if ((paddr_t)m->m_pkthdr.ph_cookie != pktbuf)
			panic("%s: packet pool is corrupted, mbuf cookie %p != "
			    "pktbuf %p", __func__, m->m_pkthdr.ph_cookie,
			    (void *)pktbuf);

		/*
		 * Because of a hardware bug in some Octeon models the size
		 * field of word3 can be wrong. However, the hardware uses
		 * all space in a buffer before moving to the next one so
		 * it is possible to derive the size of this data segment
		 * from the size of packet data buffers.
		 */
		size = OCTEON_POOL_SIZE_PKT - (addr - pktbuf);
		if (size > left)
			size = left;

		m->m_pkthdr.ph_cookie = NULL;
		m->m_data += addr - pktbuf;
		m->m_len = size;
		left -= size;

		if (m0 == NULL)
			m0 = m;
		else {
			m->m_flags &= ~M_PKTHDR;
			mprev->m_next = m;
		}
		mprev = m;

		if (i + 1 < nbufs)
			memcpy(&word3, (void *)PHYS_TO_XKPHYS(addr -
			    sizeof(word3), CCA_CACHED), sizeof(word3));
	}

	m0->m_pkthdr.len = total;
	*rm = m0;
	*nmbuf = nbufs;

	return 0;
}

int
octeon_eth_recv_check(struct octeon_eth_softc *sc, uint64_t word2)
{
	static struct timeval rxerr_log_interval = { 0, 250000 };
	uint64_t opecode;

	if (__predict_true(!ISSET(word2, PIP_WQE_WORD2_NOIP_RE)))
		return 0;

	opecode = word2 & PIP_WQE_WORD2_NOIP_OPECODE;
	if ((sc->sc_arpcom.ac_if.if_flags & IFF_DEBUG) &&
	    ratecheck(&sc->sc_rxerr_log_last, &rxerr_log_interval))
		log(LOG_DEBUG, "%s: rx error (%lld)\n", sc->sc_dev.dv_xname,
		    opecode);

	/* XXX harmless error? */
	if (opecode == PIP_WQE_WORD2_RE_OPCODE_OVRRUN)
		return 0;

	return 1;
}

int
octeon_eth_recv(struct octeon_eth_softc *sc, uint64_t *work)
{
	struct ifnet *ifp;
	struct mbuf_list ml = MBUF_LIST_INITIALIZER();
	struct mbuf *m;
	uint64_t word2;
	int nmbuf;

	OCTEON_ETH_KASSERT(sc != NULL);
	OCTEON_ETH_KASSERT(work != NULL);

	word2 = work[2];
	ifp = &sc->sc_arpcom.ac_if;

	OCTEON_ETH_KASSERT(ifp != NULL);

	if (!(ifp->if_flags & IFF_RUNNING))
		goto drop;

	if (__predict_false(octeon_eth_recv_check(sc, word2) != 0)) {
		ifp->if_ierrors++;
		goto drop;
	}

	if (__predict_false(octeon_eth_recv_mbuf(sc, work, &m, &nmbuf) != 0)) {
		ifp->if_ierrors++;
		goto drop;
	}

	/* work[0] .. work[3] may not be valid any more */

	OCTEON_ETH_KASSERT(m != NULL);

	cn30xxipd_offload(word2, &m->m_pkthdr.csum_flags);

	ml_enqueue(&ml, m);
	if_input(ifp, &ml);

	nmbuf = octeon_eth_mbuf_alloc(nmbuf);
	if (nmbuf != 0)
		atomic_add_int(&octeon_eth_mbufs_to_alloc, nmbuf);

	return 0;

drop:
	octeon_eth_buf_free_work(sc, work);
	return 1;
}

void
octeon_eth_recv_intr(void *data, uint64_t *work)
{
	struct octeon_eth_softc *sc;
	int port;

	OCTEON_ETH_KASSERT(work != NULL);

	port = (work[1] & PIP_WQE_WORD1_IPRT) >> 42;

	OCTEON_ETH_KASSERT(port < GMX_PORT_NUNITS);

	sc = octeon_eth_gsc[port];

	OCTEON_ETH_KASSERT(sc != NULL);
	OCTEON_ETH_KASSERT(port == sc->sc_port);

	/* XXX process all work queue entries anyway */

	(void)octeon_eth_recv(sc, work);
}

/* ---- tick */

void
octeon_eth_free_task(void *arg)
{
	struct octeon_eth_softc *sc = arg;
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;
	int resched = 1;
	int timeout;

	if (ml_len(&sc->sc_sendq) > 0) {
		octeon_eth_send_queue_flush_prefetch(sc);
		octeon_eth_send_queue_flush_fetch(sc);
		octeon_eth_send_queue_flush(sc);
	}

	if (ifq_is_oactive(&ifp->if_snd)) {
		ifq_clr_oactive(&ifp->if_snd);
		octeon_eth_start(ifp);

		if (ifq_is_oactive(&ifp->if_snd))
			/* The start routine did rescheduling already. */
			resched = 0;
	}

	if (resched) {
		timeout = (sc->sc_ext_callback_cnt > 0) ? 1 : hz;
		timeout_add(&sc->sc_tick_free_ch, timeout);
	}
}

/*
 * octeon_eth_tick_free
 *
 * => garbage collect send gather buffer / mbuf
 * => called at softclock
 */
void
octeon_eth_tick_free(void *arg)
{
	struct octeon_eth_softc *sc = arg;
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;
	int to_alloc;

	ifq_serialize(&ifp->if_snd, &sc->sc_free_task);

	if (octeon_eth_mbufs_to_alloc != 0) {
		to_alloc = atomic_swap_uint(&octeon_eth_mbufs_to_alloc, 0);
		to_alloc = octeon_eth_mbuf_alloc(to_alloc);
		if (to_alloc != 0)
			atomic_add_int(&octeon_eth_mbufs_to_alloc, to_alloc);
	}
}

/*
 * octeon_eth_tick_misc
 *
 * => collect statistics
 * => check link status
 * => called at softclock
 */
void
octeon_eth_tick_misc(void *arg)
{
	struct octeon_eth_softc *sc = arg;
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;
	int s;

	s = splnet();

	cn30xxgmx_stats(sc->sc_gmx_port);
	cn30xxpip_stats(sc->sc_pip, ifp, sc->sc_port);
	mii_tick(&sc->sc_mii);

	splx(s);

	timeout_add_sec(&sc->sc_tick_misc_ch, 1);
}
