/*
 * DHD Bus Module for SDIO
 *
 * Copyright (C) 1999-2012, Broadcom Corporation
 * 
 *      Unless you and Broadcom execute a separate written software license
 * agreement governing use of this software, this software is licensed to you
 * under the terms of the GNU General Public License version 2 (the "GPL"),
 * available at http://www.broadcom.com/licenses/GPLv2.php, with the
 * following added to such license:
 * 
 *      As a special exception, the copyright holders of this software give you
 * permission to link this software with independent modules, and to copy and
 * distribute the resulting executable under terms of your choice, provided that
 * you also meet, for each linked independent module, the terms and conditions of
 * the license of that module.  An independent module is a module which is not
 * derived from this software.  The special exception does not apply to any
 * modifications of the software.
 * 
 *      Notwithstanding the above, under no circumstances may you combine this
 * software in any way with any other Broadcom software provided under a license
 * other than the GPL, without Broadcom's express prior written consent.
 *
 * $Id: dhd_sdio.c 366199 2012-11-01 09:04:31Z $
 */

#include <typedefs.h>
#include <osl.h>
#include <bcmsdh.h>

#ifdef BCMEMBEDIMAGE
#include BCMEMBEDIMAGE
#endif 

#include <bcmdefs.h>
#include <bcmutils.h>
#include <bcmendian.h>
#include <bcmdevs.h>
#include <linux/regulator/consumer.h>

#include <siutils.h>
#include <hndpmu.h>
#include <hndsoc.h>
#include <bcmsdpcm.h>
#if defined(DHD_DEBUG)
#include <hndrte_armtrap.h>
#include <hndrte_cons.h>
#endif 
#include <sbchipc.h>
#include <sbhnddma.h>

#include <sdio.h>
#include <sbsdio.h>
#include <sbsdpcmdev.h>
#include <bcmsdpcm.h>
#include <bcmsdbus.h>

#include <proto/ethernet.h>
#include <proto/802.1d.h>
#include <proto/802.11.h>

#include <dngl_stats.h>
#include <dhd.h>
#include <dhd_bus.h>
#include <dhd_proto.h>
#include <dhd_dbg.h>
#include <dhdioctl.h>
#include <sdiovar.h>
#include <mach/msm_watchdog.h>

#ifndef DHDSDIO_MEM_DUMP_FNAME
#define DHDSDIO_MEM_DUMP_FNAME         "mem_dump"
#endif

#define QLEN		256	
#define FCHI		(QLEN - 10)
#define FCLOW		(FCHI / 2)
#define PRIOMASK	7

#define TXRETRIES	2	

#define DHD_RXBOUND	50	

#define DHD_TXBOUND	20	

#define DHD_TXMINMAX	1	

#define MEMBLOCK	2048		
#define MAX_NVRAMBUF_SIZE	4096	
#define MAX_DATA_BUF	(32 * 1024)	

#ifndef DHD_FIRSTREAD
#define DHD_FIRSTREAD   32
#endif
#if !ISPOWEROF2(DHD_FIRSTREAD)
#error DHD_FIRSTREAD is not a power of 2!
#endif

#ifdef BCMSDIOH_TXGLOM
#define SDPCM_HDRLEN	(SDPCM_FRAMETAG_LEN + SDPCM_HWEXT_LEN + SDPCM_SWHEADER_LEN)
#else
#define SDPCM_HDRLEN	(SDPCM_FRAMETAG_LEN + SDPCM_SWHEADER_LEN)
#endif

#define SDPCM_HDRLEN_RX	(SDPCM_FRAMETAG_LEN + SDPCM_SWHEADER_LEN)

#ifdef SDTEST
#define SDPCM_RESERVE	(SDPCM_HDRLEN + SDPCM_TEST_HDRLEN + DHD_SDALIGN)
#else
#define SDPCM_RESERVE	(SDPCM_HDRLEN + DHD_SDALIGN)
#endif

#ifndef MAX_HDR_READ
#define MAX_HDR_READ	32
#endif
#if !ISPOWEROF2(MAX_HDR_READ)
#error MAX_HDR_READ is not a power of 2!
#endif

#define MAX_RX_DATASZ	2048

#define DHD_WAIT_F2RDY	3000

#if (PMU_MAX_TRANSITION_DLY <= 1000000)
#undef PMU_MAX_TRANSITION_DLY
#define PMU_MAX_TRANSITION_DLY 1000000
#endif

#define DHD_INIT_CLKCTL1	(SBSDIO_FORCE_HW_CLKREQ_OFF | SBSDIO_ALP_AVAIL_REQ)
#define DHD_INIT_CLKCTL2	(SBSDIO_FORCE_HW_CLKREQ_OFF | SBSDIO_FORCE_ALP)

#define F2SYNC	(SDIO_REQ_4BYTE | SDIO_REQ_FIXED)

#define PKTFREE2()		if ((bus->bus != SPI_BUS) || bus->usebufpool) \
					PKTFREE(bus->dhd->osh, pkt, FALSE);
DHD_SPINWAIT_SLEEP_INIT(sdioh_spinwait_sleep);
#if defined(OOB_INTR_ONLY) || defined(BCMSPI_ANDROID)
extern void bcmsdh_set_irq(int flag);
#endif 
#ifdef PROP_TXSTATUS
extern void dhd_wlfc_txcomplete(dhd_pub_t *dhd, void *txp, bool success);
extern void dhd_wlfc_trigger_pktcommit(dhd_pub_t *dhd);
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 25))
DEFINE_MUTEX(_dhd_sdio_mutex_lock_);
#endif 

#ifdef DHD_DEBUG
#define CONSOLE_LINE_MAX	192
#define CONSOLE_BUFFER_MAX	2024
typedef struct dhd_console {
	uint		count;			
	uint		log_addr;		
	hndrte_log_t	log;			
	uint		bufsize;		
	uint8		*buf;			
	uint		last;			
} dhd_console_t;
#endif 

#define	REMAP_ENAB(bus)			((bus)->remap)
#define	REMAP_ISADDR(bus, a)		(((a) >= ((bus)->orig_ramsize)) && ((a) < ((bus)->ramsize)))
#define	KSO_ENAB(bus)			((bus)->kso)
#define	SR_ENAB(bus)			((bus)->_srenab)
#define	SLPAUTO_ENAB(bus)		((SR_ENAB(bus)) && ((bus)->_slpauto))
#define	MIN_RSRC_ADDR			(SI_ENUM_BASE + 0x618)
#define	MIN_RSRC_SR			0x3
#define	CORE_CAPEXT_ADDR		(SI_ENUM_BASE + 0x64c)
#define	CORE_CAPEXT_SR_SUPPORTED_MASK	(1 << 1)
#define RCTL_MACPHY_DISABLE_MASK	(1 << 26)
#define RCTL_LOGIC_DISABLE_MASK		(1 << 27)

#define	OOB_WAKEUP_ENAB(bus)		((bus)->_oobwakeup)
#define	GPIO_DEV_SRSTATE		16	
#define	GPIO_DEV_SRSTATE_TIMEOUT	320000	
#define	GPIO_DEV_WAKEUP			17	
#define	CC_CHIPCTRL2_GPIO1_WAKEUP	(1  << 0)
#define OVERFLOW_BLKSZ512_WM		48
#define OVERFLOW_BLKSZ512_MES		80

#define CC_PMUCC3	(0x3)
typedef struct dhd_bus {
	dhd_pub_t	*dhd;

	bcmsdh_info_t	*sdh;			
	si_t		*sih;			
	char		*vars;			
	uint		varsz;			
	uint32		sbaddr;			

	sdpcmd_regs_t	*regs;			
	uint		sdpcmrev;		
	uint		armrev;			
	uint		ramrev;			
	uint32		ramsize;		
	uint32		orig_ramsize;		
	uint32		srmemsize;		

	uint32		bus;			
	uint32		hostintmask;		
	uint32		intstatus;		
	bool		dpc_sched;		
	bool		fcstate;		

	uint16		cl_devid;		
	char		*fw_path;		
	char		*nv_path;		
	const char      *nvram_params;		

	uint		blocksize;		
	uint		roundup;		

	struct pktq	txq;			
	uint8		flowcontrol;		
	uint8		tx_seq;			
	uint8		tx_max;			

	uint8		hdrbuf[MAX_HDR_READ + DHD_SDALIGN];
	uint8		*rxhdr;			
	uint16		nextlen;		
	uint8		rx_seq;			
	bool		rxskip;			

	void		*glomd;			
	void		*glom;			
	uint		glomerr;		

	uint8		*rxbuf;			
	uint		rxblen;			
	uint8		*rxctl;			
	uint8		*databuf;		
	uint8		*dataptr;		
	uint		rxlen;			

	uint8		sdpcm_ver;		

	bool		intr;			
	bool		poll;			
	bool		ipend;			
	bool		intdis;			
	uint 		intrcount;		
	uint		lastintrs;		
	uint		spurious;		
	uint		pollrate;		
	uint		polltick;		
	uint		pollcnt;		

#ifdef DHD_DEBUG
	dhd_console_t	console;		
	uint		console_addr;		
#endif 

	uint		regfails;		

	uint		clkstate;		
	bool		activity;		
	int32		idletime;		
	int32		idlecount;		
	int32		idleclock;		
	int32		sd_divisor;		
	int32		sd_mode;		
	int32		sd_rxchain;		
	bool		use_rxchain;		
	bool		sleeping;		
	uint		rxflow_mode;		
	bool		rxflow;			
	uint		prev_rxlim_hit;		
	bool		alp_only;		
	
	bool		usebufpool;

#ifdef SDTEST
	
	bool		ext_loop;
	uint8		loopid;

	
	uint		pktgen_freq;		
	uint		pktgen_count;		
	uint		pktgen_print;		
	uint		pktgen_total;		
	uint		pktgen_minlen;		
	uint		pktgen_maxlen;		
	uint		pktgen_mode;		
	uint		pktgen_stop;		

	
	uint		pktgen_tick;		
	uint		pktgen_ptick;		
	uint		pktgen_sent;		
	uint		pktgen_rcvd;		
	uint		pktgen_prev_time;	
	uint		pktgen_prev_sent;	
	uint		pktgen_prev_rcvd;	
	uint		pktgen_fail;		
	uint16		pktgen_len;		
#define PKTGEN_RCV_IDLE     (0)
#define PKTGEN_RCV_ONGOING  (1)
	uint16		pktgen_rcv_state;		
	uint		pktgen_rcvd_rcvsession;	
#endif 

	
	uint		tx_sderrs;		
	uint		fcqueued;		
	uint		rxrtx;			
	uint		rx_toolong;		
	uint		rxc_errors;		
	uint		rx_hdrfail;		
	uint		rx_badhdr;		
	uint		rx_badseq;		
	uint		fc_rcvd;		
	uint		fc_xoff;		
	uint		fc_xon;			
	uint		rxglomfail;		
	uint		rxglomframes;		
	uint		rxglompkts;		
	uint		f2rxhdrs;		
	uint		f2rxdata;		
	uint		f2txdata;		
	uint		f1regdata;		

	uint8		*ctrl_frame_buf;
	uint32		ctrl_frame_len;
	bool		ctrl_frame_stat;
	uint32		rxint_mode;	
	bool		remap;		
	bool		kso;
	bool		_slpauto;
	bool		_oobwakeup;
	bool		_srenab;
	bool        readframes;
	bool        reqbussleep;
	uint32		resetinstr;
	uint32		dongle_ram_base;
#ifdef BCMSDIOH_TXGLOM
	void		*glom_pkt_arr[SDPCM_MAXGLOM_SIZE];	
	uint16		glom_cnt;	
	uint16		glom_total_len;	
	bool		glom_enable;	
	uint8		glom_mode;	
	uint32		glomsize;	
#endif
} dhd_bus_t;

#define CLK_NONE	0
#define CLK_SDONLY	1
#define CLK_PENDING	2	
#define CLK_AVAIL	3

#define DHD_NOPMU(dhd)	(FALSE)

#define MAX_SDIO_ERR_COUNT 3
static int rx_readahead_err_cnt = 0;
static int rx_sdio_err_cnt = 0;

#ifdef DHD_DEBUG
static int qcount[NUMPRIO];
static int tx_packets[NUMPRIO];
#endif 

const uint dhd_deferred_tx = 1;

extern uint dhd_watchdog_ms;
extern void dhd_os_wd_timer(void *bus, uint wdtick);

uint dhd_txbound;
uint dhd_rxbound;
uint dhd_txminmax = DHD_TXMINMAX;

#define DONGLE_MIN_MEMSIZE (128 *1024)
int dhd_dongle_memsize;

#ifndef REPEAT_READFRAME
static bool dhd_doflow;
#else
extern bool dhd_doflow;
#endif 
static bool dhd_alignctl;

static bool sd1idle;

static bool retrydata;
#define RETRYCHAN(chan) (((chan) == SDPCM_EVENT_CHANNEL) || retrydata)

#ifdef SDIO_CRC_ERROR_FIX
static uint watermark = 48;
static uint mesbusyctrl = 80;
#else
static const uint watermark = 8;
static const uint mesbusyctrl = 0;
#endif
static const uint firstread = DHD_FIRSTREAD;

#define HDATLEN (firstread - (SDPCM_HDRLEN))

static const uint retry_limit = 2;

static bool forcealign;

#define ALIGNMENT  4

#if defined(OOB_INTR_ONLY) && defined(HW_OOB)
extern void bcmsdh_enable_hw_oob_intr(void *sdh, bool enable);
#endif

#if defined(OOB_INTR_ONLY) && defined(SDIO_ISR_THREAD)
#error OOB_INTR_ONLY is NOT working with SDIO_ISR_THREAD
#endif 
#define PKTALIGN(osh, p, len, align)					\
	do {								\
		uint datalign;						\
		datalign = (uintptr)PKTDATA((osh), (p));		\
		datalign = ROUNDUP(datalign, (align)) - datalign;	\
		ASSERT(datalign < (align));				\
		ASSERT(PKTLEN((osh), (p)) >= ((len) + datalign));	\
		if (datalign)						\
			PKTPULL((osh), (p), datalign);			\
		PKTSETLEN((osh), (p), (len));				\
	} while (0)

static const uint max_roundup = 512;

static bool dhd_readahead;

si_t		*local_sih = NULL;

#define DATAOK(bus) \
	(((uint8)(bus->tx_max - bus->tx_seq) > 1) && \
	(((uint8)(bus->tx_max - bus->tx_seq) & 0x80) == 0))

#define TXCTLOK(bus) \
	(((uint8)(bus->tx_max - bus->tx_seq) != 0) && \
	(((uint8)(bus->tx_max - bus->tx_seq) & 0x80) == 0))

#define DATABUFCNT(bus) \
	((uint8)(bus->tx_max - bus->tx_seq) - 1)

#define R_SDREG(regvar, regaddr, retryvar) \
do { \
	retryvar = 0; \
	do { \
		regvar = R_REG(bus->dhd->osh, regaddr); \
	} while (bcmsdh_regfail(bus->sdh) && (++retryvar <= retry_limit)); \
	if (retryvar) { \
		bus->regfails += (retryvar-1); \
		if (retryvar > retry_limit) { \
			DHD_ERROR(("%s: FAILED" #regvar "READ, LINE %d\n", \
			           __FUNCTION__, __LINE__)); \
			regvar = 0; \
		} \
	} \
} while (0)

#define W_SDREG(regval, regaddr, retryvar) \
do { \
	retryvar = 0; \
	do { \
		W_REG(bus->dhd->osh, regaddr, regval); \
	} while (bcmsdh_regfail(bus->sdh) && (++retryvar <= retry_limit)); \
	if (retryvar) { \
		bus->regfails += (retryvar-1); \
		if (retryvar > retry_limit) \
			DHD_ERROR(("%s: FAILED REGISTER WRITE, LINE %d\n", \
			           __FUNCTION__, __LINE__)); \
	} \
} while (0)

#define BUS_WAKE(bus) \
	do { \
		bus->idlecount = 0; \
		if ((bus)->sleeping) \
			dhdsdio_bussleep((bus), FALSE); \
	} while (0);


#define SDIO_DEVICE_HMB_RXINT		0	
#define SDIO_DEVICE_RXDATAINT_MODE_0	1	
#define SDIO_DEVICE_RXDATAINT_MODE_1	2	


#define FRAME_AVAIL_MASK(bus) 	\
	((bus->rxint_mode == SDIO_DEVICE_HMB_RXINT) ? I_HMB_FRAME_IND : I_XMTDATA_AVAIL)

#define DHD_BUS			SDIO_BUS

#define PKT_AVAILABLE(bus, intstatus)	((intstatus) & (FRAME_AVAIL_MASK(bus)))

#define HOSTINTMASK		(I_HMB_SW_MASK | I_CHIPACTIVE)

#define GSPI_PR55150_BAILOUT

#ifdef SDTEST
static void dhdsdio_testrcv(dhd_bus_t *bus, void *pkt, uint seq);
static void dhdsdio_sdtest_set(dhd_bus_t *bus, uint count);
#endif

#ifdef DHD_DEBUG
static int dhdsdio_checkdied(dhd_bus_t *bus, char *data, uint size);
static int dhd_serialconsole(dhd_bus_t *bus, bool get, bool enable, int *bcmerror);
#endif 

static int dhdsdio_devcap_set(dhd_bus_t *bus, uint8 cap);
static int dhdsdio_download_state(dhd_bus_t *bus, bool enter);

static void dhdsdio_release(dhd_bus_t *bus, osl_t *osh);
static void dhdsdio_release_malloc(dhd_bus_t *bus, osl_t *osh);
static void dhdsdio_disconnect(void *ptr);
static bool dhdsdio_chipmatch(uint16 chipid);
static bool dhdsdio_probe_attach(dhd_bus_t *bus, osl_t *osh, void *sdh,
                                 void * regsva, uint16  devid);
static bool dhdsdio_probe_malloc(dhd_bus_t *bus, osl_t *osh, void *sdh);
static bool dhdsdio_probe_init(dhd_bus_t *bus, osl_t *osh, void *sdh);
static void dhdsdio_release_dongle(dhd_bus_t *bus, osl_t *osh, bool dongle_isolation,
	bool reset_flag);

static void dhd_dongle_setmemsize(struct dhd_bus *bus, int mem_size);
static int dhd_bcmsdh_recv_buf(dhd_bus_t *bus, uint32 addr, uint fn, uint flags,
	uint8 *buf, uint nbytes,
	void *pkt, bcmsdh_cmplt_fn_t complete, void *handle);
static int dhd_bcmsdh_send_buf(dhd_bus_t *bus, uint32 addr, uint fn, uint flags,
	uint8 *buf, uint nbytes,
	void *pkt, bcmsdh_cmplt_fn_t complete, void *handle);
#ifdef BCMSDIOH_TXGLOM
static void dhd_bcmsdh_glom_post(dhd_bus_t *bus, uint8 *frame, void *pkt, uint len);
static void dhd_bcmsdh_glom_clear(dhd_bus_t *bus);
#endif

static bool dhdsdio_download_firmware(dhd_bus_t *bus, osl_t *osh, void *sdh);
static int _dhdsdio_download_firmware(dhd_bus_t *bus);

static int dhdsdio_download_code_file(dhd_bus_t *bus, char *image_path);
static int dhdsdio_download_nvram(dhd_bus_t *bus);
#ifdef BCMEMBEDIMAGE
static int dhdsdio_download_code_array(dhd_bus_t *bus);
#endif
static int dhdsdio_bussleep(dhd_bus_t *bus, bool sleep);
static int dhdsdio_clkctl(dhd_bus_t *bus, uint target, bool pendok);
static uint8 dhdsdio_sleepcsr_get(dhd_bus_t *bus);

#ifdef WLMEDIA_HTSF
#include <htsf.h>
extern uint32 dhd_get_htsf(void *dhd, int ifidx);
#endif 


static void
dhd_overflow_war(struct dhd_bus *bus)
{
	int err;
	uint8 devctl, wm, mes;

	
	if (bus->blocksize == 512) {
		wm = OVERFLOW_BLKSZ512_WM;
		mes = OVERFLOW_BLKSZ512_MES;
	} else {
		mes = bus->blocksize/4;
		wm = bus->blocksize/4;
	}


	
	bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_1, SBSDIO_WATERMARK, wm, &err);

	devctl = bcmsdh_cfg_read(bus->sdh, SDIO_FUNC_1, SBSDIO_DEVICE_CTL, &err);
	devctl |= SBSDIO_DEVCTL_F2WM_ENAB;
	bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_1, SBSDIO_DEVICE_CTL, devctl, &err);

	
	bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_1, SBSDIO_FUNC1_MESBUSYCTRL,
		(mes | SBSDIO_MESBUSYCTRL_ENAB), &err);

	DHD_INFO(("Apply overflow WAR: 0x%02x 0x%02x 0x%02x\n",
		bcmsdh_cfg_read(bus->sdh, SDIO_FUNC_1, SBSDIO_DEVICE_CTL, &err),
		bcmsdh_cfg_read(bus->sdh, SDIO_FUNC_1, SBSDIO_WATERMARK, &err),
		bcmsdh_cfg_read(bus->sdh, SDIO_FUNC_1, SBSDIO_FUNC1_MESBUSYCTRL, &err)));
}

static void
dhd_dongle_setmemsize(struct dhd_bus *bus, int mem_size)
{
	int32 min_size =  DONGLE_MIN_MEMSIZE;
	
	DHD_ERROR(("user: Restrict the dongle ram size to %d, min accepted %d\n",
		dhd_dongle_memsize, min_size));
	if ((dhd_dongle_memsize > min_size) &&
		(dhd_dongle_memsize < (int32)bus->orig_ramsize))
		bus->ramsize = dhd_dongle_memsize;
}

static int
dhdsdio_set_siaddr_window(dhd_bus_t *bus, uint32 address)
{
	int err = 0;
	bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_1, SBSDIO_FUNC1_SBADDRLOW,
	                 (address >> 8) & SBSDIO_SBADDRLOW_MASK, &err);
	if (!err)
		bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_1, SBSDIO_FUNC1_SBADDRMID,
		                 (address >> 16) & SBSDIO_SBADDRMID_MASK, &err);
	if (!err)
		bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_1, SBSDIO_FUNC1_SBADDRHIGH,
		                 (address >> 24) & SBSDIO_SBADDRHIGH_MASK, &err);
	return err;
}


#ifdef USE_OOB_GPIO1
static int
dhdsdio_oobwakeup_init(dhd_bus_t *bus)
{
	uint32 val, addr, data;

	bcmsdh_gpioouten(bus->sdh, GPIO_DEV_WAKEUP);

	addr = SI_ENUM_BASE + OFFSETOF(chipcregs_t, chipcontrol_addr);
	data = SI_ENUM_BASE + OFFSETOF(chipcregs_t, chipcontrol_data);

	
	bcmsdh_reg_write(bus->sdh, addr, 4, 2);
	val = bcmsdh_reg_read(bus->sdh, data, 4);
	val |= CC_CHIPCTRL2_GPIO1_WAKEUP;
	bcmsdh_reg_write(bus->sdh, data, 4, val);

	bus->_oobwakeup = TRUE;

	return 0;
}
#endif 

static bool
dhdsdio_sr_cap(dhd_bus_t *bus)
{
	bool cap = FALSE;
	uint32  core_capext, addr, data;

	
	

	if (bus->sih->chip == BCM4324_CHIP_ID) {
			addr = SI_ENUM_BASE + OFFSETOF(chipcregs_t, chipcontrol_addr);
			data = SI_ENUM_BASE + OFFSETOF(chipcregs_t, chipcontrol_data);
			bcmsdh_reg_write(bus->sdh, addr, 4, 3);
			core_capext = bcmsdh_reg_read(bus->sdh, data, 4);
	} else if (bus->sih->chip == BCM4330_CHIP_ID) {
			core_capext = FALSE;
	} else if (bus->sih->chip == BCM4335_CHIP_ID) {
		core_capext = TRUE;
	} else {
			core_capext = bcmsdh_reg_read(bus->sdh, CORE_CAPEXT_ADDR, 4);
			core_capext = (core_capext & CORE_CAPEXT_SR_SUPPORTED_MASK);
	}
	if (!(core_capext))
		return FALSE;

	if (bus->sih->chip == BCM4324_CHIP_ID) {
		
		cap = TRUE;
	} else if (bus->sih->chip == BCM4335_CHIP_ID) {
		uint32 enabval = 0;
		addr = SI_ENUM_BASE + OFFSETOF(chipcregs_t, chipcontrol_addr);
		data = SI_ENUM_BASE + OFFSETOF(chipcregs_t, chipcontrol_data);
		bcmsdh_reg_write(bus->sdh, addr, 4, CC_PMUCC3);
		enabval = bcmsdh_reg_read(bus->sdh, data, 4);

		if (enabval)
			cap = TRUE;
	} else {
		data = bcmsdh_reg_read(bus->sdh,
			SI_ENUM_BASE + OFFSETOF(chipcregs_t, retention_ctl), 4);
		if ((data & (RCTL_MACPHY_DISABLE_MASK | RCTL_LOGIC_DISABLE_MASK)) == 0)
			cap = TRUE;
	}

	return cap;
}

static int
dhdsdio_srwar_init(dhd_bus_t *bus)
{

	bcmsdh_gpio_init(bus->sdh);

#ifdef USE_OOB_GPIO1
	dhdsdio_oobwakeup_init(bus);
#endif


	return 0;
}

static int
dhdsdio_sr_init(dhd_bus_t *bus)
{
	uint8 val;
	int err = 0;

	if ((bus->sih->chip == BCM4334_CHIP_ID) && (bus->sih->chiprev == 2))
		dhdsdio_srwar_init(bus);

	val = bcmsdh_cfg_read(bus->sdh, SDIO_FUNC_1, SBSDIO_FUNC1_WAKEUPCTRL, NULL);
	val |= 1 << SBSDIO_FUNC1_WCTRL_HTWAIT_SHIFT;
	bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_1, SBSDIO_FUNC1_WAKEUPCTRL,
		1 << SBSDIO_FUNC1_WCTRL_HTWAIT_SHIFT, &err);
	val = bcmsdh_cfg_read(bus->sdh, SDIO_FUNC_1, SBSDIO_FUNC1_WAKEUPCTRL, NULL);

	
	dhdsdio_devcap_set(bus,
		(SDIOD_CCCR_BRCM_CARDCAP_CMD14_SUPPORT | SDIOD_CCCR_BRCM_CARDCAP_CMD14_EXT));

	bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_1,
		SBSDIO_FUNC1_CHIPCLKCSR, SBSDIO_FORCE_HT, &err);

	bus->_slpauto = dhd_slpauto ? TRUE : FALSE;

	bus->_srenab = TRUE;

	return 0;
}

static int
dhdsdio_clk_kso_init(dhd_bus_t *bus)
{
	uint8 val;
	int err = 0;

	
	bus->kso = TRUE;

	val = bcmsdh_cfg_read(bus->sdh, SDIO_FUNC_1, SBSDIO_FUNC1_SLEEPCSR, NULL);
	if (!(val & SBSDIO_FUNC1_SLEEPCSR_KSO_MASK)) {
		val |= (SBSDIO_FUNC1_SLEEPCSR_KSO_EN << SBSDIO_FUNC1_SLEEPCSR_KSO_SHIFT);
		bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_1, SBSDIO_FUNC1_SLEEPCSR, val, &err);
		if (err)
			DHD_ERROR(("%s: SBSDIO_FUNC1_SLEEPCSR err: 0x%x\n", __FUNCTION__, err));
	}

	return 0;
}

#define KSO_DBG(x)
#define KSO_WAIT_US 50
#define MAX_KSO_ATTEMPTS (PMU_MAX_TRANSITION_DLY/KSO_WAIT_US)
extern int module_remove;
extern int mmc_tuning_fail;
static int
dhdsdio_clk_kso_enab(dhd_bus_t *bus, bool on)
{
	uint8 wr_val = 0, rd_val = 0, cmp_val, bmask;
	int err = 0;
	int try_cnt = 0;

	KSO_DBG(("%s> op:%s\n", __FUNCTION__, (on ? "KSO_SET" : "KSO_CLR")));

	wr_val |= (on << SBSDIO_FUNC1_SLEEPCSR_KSO_SHIFT);

	bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_1, SBSDIO_FUNC1_SLEEPCSR, wr_val, &err);

	if (on) {
		cmp_val = SBSDIO_FUNC1_SLEEPCSR_KSO_MASK |  SBSDIO_FUNC1_SLEEPCSR_DEVON_MASK;
		bmask = cmp_val;

		msleep(3);
	} else {
		
		cmp_val = 0;
		bmask = SBSDIO_FUNC1_SLEEPCSR_KSO_MASK;
	}

	do {
		if (mmc_tuning_fail) {
			printf("mmc tuning fail, discard!\n");
			try_cnt = MAX_KSO_ATTEMPTS + 1;
			break;
		}
		rd_val = bcmsdh_cfg_read(bus->sdh, SDIO_FUNC_1, SBSDIO_FUNC1_SLEEPCSR, &err);
		if (((rd_val & bmask) == cmp_val) && !err)
			break;

		KSO_DBG(("%s> KSO wr/rd retry:%d, ERR:%x \n", __FUNCTION__, try_cnt, err));
		OSL_DELAY(KSO_WAIT_US);

		bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_1, SBSDIO_FUNC1_SLEEPCSR, wr_val, &err);
		
		if ((bus->dhd->busstate == DHD_BUS_DOWN) || bus->dhd->hang_was_sent || module_remove) {
			DHD_ERROR(("%s : bus is down. we have nothing to do\n", __FUNCTION__));
			break;
		}
	
	} while (try_cnt++ < MAX_KSO_ATTEMPTS);


	if (try_cnt > 2)
		KSO_DBG(("%s> op:%s, try_cnt:%d, rd_val:%x, ERR:%x \n",
			__FUNCTION__, (on ? "KSO_SET" : "KSO_CLR"), try_cnt, rd_val, err));

	if (try_cnt > MAX_KSO_ATTEMPTS)  {
		DHD_ERROR(("%s> op:%s, ERROR: try_cnt:%d, rd_val:%x, ERR:%x \n",
			__FUNCTION__, (on ? "KSO_SET" : "KSO_CLR"), try_cnt, rd_val, err));
		
		dhd_os_send_hang_message(bus->dhd);
	}
	return err;
}

static int
dhdsdio_clk_kso_iovar(dhd_bus_t *bus, bool on)
{
	int err = 0;

	if (on == FALSE) {

		BUS_WAKE(bus);
		dhdsdio_clkctl(bus, CLK_AVAIL, FALSE);

		DHD_ERROR(("%s: KSO disable clk: 0x%x\n", __FUNCTION__,
			bcmsdh_cfg_read(bus->sdh, SDIO_FUNC_1,
			SBSDIO_FUNC1_CHIPCLKCSR, &err)));
		dhdsdio_clk_kso_enab(bus, FALSE);
	} else {
		DHD_ERROR(("%s: KSO enable\n", __FUNCTION__));

		
		if (bus->clkstate == CLK_NONE) {
			DHD_ERROR(("%s: Request SD clk\n", __FUNCTION__));
			dhdsdio_clkctl(bus, CLK_SDONLY, FALSE);
		}

		
		dhdsdio_clk_kso_enab(bus, TRUE);
		dhdsdio_clk_kso_enab(bus, TRUE);
		OSL_DELAY(4000);

		
		SPINWAIT(((dhdsdio_sleepcsr_get(bus)) !=
			(SBSDIO_FUNC1_SLEEPCSR_KSO_MASK |
			SBSDIO_FUNC1_SLEEPCSR_DEVON_MASK)),
			(10000));

		DHD_ERROR(("%s: sleepcsr: 0x%x\n", __FUNCTION__,
			dhdsdio_sleepcsr_get(bus)));
	}

	bus->kso = on;
	BCM_REFERENCE(err);

	return 0;
}

static uint8
dhdsdio_sleepcsr_get(dhd_bus_t *bus)
{
	int err = 0;
	uint8 val = 0;

	val = bcmsdh_cfg_read(bus->sdh, SDIO_FUNC_1, SBSDIO_FUNC1_SLEEPCSR, &err);
	if (err)
		DHD_TRACE(("Failed to read SLEEPCSR: %d\n", err));

	return val;
}

uint8
dhdsdio_devcap_get(dhd_bus_t *bus)
{
	return bcmsdh_cfg_read(bus->sdh, SDIO_FUNC_0, SDIOD_CCCR_BRCM_CARDCAP, NULL);
}

static int
dhdsdio_devcap_set(dhd_bus_t *bus, uint8 cap)
{
	int err = 0;

	bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_0, SDIOD_CCCR_BRCM_CARDCAP, cap, &err);
	if (err)
		DHD_ERROR(("%s: devcap set err: 0x%x\n", __FUNCTION__, err));

	return 0;
}


static int
dhdsdio_clk_devsleep_iovar(dhd_bus_t *bus, bool on)
{
	int err = 0, retry;
	uint8 val;

	retry = 0;
	
	if ((bus->dhd->busstate == DHD_BUS_DOWN) || bus->dhd->hang_was_sent || module_remove) {
		DHD_ERROR(("%s : bus is down. we have nothing to do\n", __FUNCTION__));
		return 0;
	}
	
	if (on == TRUE) {
		

		if (!SLPAUTO_ENAB(bus))
			dhdsdio_clkctl(bus, CLK_AVAIL, FALSE);
		else {
			val = bcmsdh_cfg_read(bus->sdh, SDIO_FUNC_1, SBSDIO_FUNC1_CHIPCLKCSR, &err);
			if ((val & SBSDIO_CSR_MASK) == 0) {
				DHD_ERROR(("%s: No clock before enter sleep:0x%x\n",
					__FUNCTION__, val));

				
				bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_1, SBSDIO_FUNC1_CHIPCLKCSR,
					SBSDIO_ALP_AVAIL_REQ, &err);
				DHD_ERROR(("%s: clock before sleep:0x%x\n", __FUNCTION__,
					bcmsdh_cfg_read(bus->sdh, SDIO_FUNC_1,
					SBSDIO_FUNC1_CHIPCLKCSR, &err)));
			}
		}

		DHD_TRACE(("%s: clk before sleep: 0x%x\n", __FUNCTION__,
			bcmsdh_cfg_read(bus->sdh, SDIO_FUNC_1,
			SBSDIO_FUNC1_CHIPCLKCSR, &err)));
#ifdef USE_CMD14
		err = bcmsdh_sleep(bus->sdh, TRUE);
#else
		err = dhdsdio_clk_kso_enab(bus, FALSE);
		if (OOB_WAKEUP_ENAB(bus))
			err = bcmsdh_gpioout(bus->sdh, GPIO_DEV_WAKEUP, FALSE);  
#endif
		
		
	} else {
		
		
		
		
		if (bus->clkstate == CLK_NONE) {
			DHD_TRACE(("%s: Request SD clk\n", __FUNCTION__));
			dhdsdio_clkctl(bus, CLK_SDONLY, FALSE);
		}

		if ((bus->sih->chip == BCM4334_CHIP_ID) && (bus->sih->chiprev == 2)) {
			SPINWAIT((bcmsdh_gpioin(bus->sdh, GPIO_DEV_SRSTATE) != TRUE),
				GPIO_DEV_SRSTATE_TIMEOUT);

			if (bcmsdh_gpioin(bus->sdh, GPIO_DEV_SRSTATE) == FALSE) {
				DHD_ERROR(("ERROR: GPIO_DEV_SRSTATE still low!\n"));
			}
		}
#ifdef USE_CMD14
		err = bcmsdh_sleep(bus->sdh, FALSE);
		if (SLPAUTO_ENAB(bus) && (err != 0)) {
			OSL_DELAY(10000);
			DHD_TRACE(("%s: Resync device sleep\n", __FUNCTION__));

			
			err = bcmsdh_sleep(bus->sdh, TRUE);
			OSL_DELAY(10000);
			err = bcmsdh_sleep(bus->sdh, FALSE);

			if (err) {
				OSL_DELAY(10000);
				DHD_ERROR(("%s: CMD14 exit failed again!\n", __FUNCTION__));

				
				err = bcmsdh_sleep(bus->sdh, TRUE);
				OSL_DELAY(10000);
				err = bcmsdh_sleep(bus->sdh, FALSE);
				if (err) {
					DHD_ERROR(("%s: CMD14 exit failed twice!\n", __FUNCTION__));
					DHD_ERROR(("%s: FATAL: Device non-response!\n",
						__FUNCTION__));
					err = 0;
				}
			}
		}
#else
		if (OOB_WAKEUP_ENAB(bus))
			err = bcmsdh_gpioout(bus->sdh, GPIO_DEV_WAKEUP, TRUE);  

		do {
			err = dhdsdio_clk_kso_enab(bus, TRUE);
			if (err)
				OSL_DELAY(10000);
		} while ((err != 0) && (++retry < 3));

		if (err != 0) {
			DHD_ERROR(("ERROR: kso set failed retry: %d\n", retry));
			err = 0; 
		}
#endif 

		if (err == 0) {
			uint8 csr;

			
			SPINWAIT((((csr = dhdsdio_sleepcsr_get(bus)) &
				SBSDIO_FUNC1_SLEEPCSR_DEVON_MASK) !=
				(SBSDIO_FUNC1_SLEEPCSR_DEVON_MASK)), (20000));

			DHD_TRACE(("%s: ExitSleep sleepcsr: 0x%x\n", __FUNCTION__, csr));

			if (!(csr & SBSDIO_FUNC1_SLEEPCSR_DEVON_MASK)) {
				DHD_ERROR(("%s:ERROR: ExitSleep device NOT Ready! 0x%x\n",
					__FUNCTION__, csr));
				err = BCME_NODEVICE;
			}

			SPINWAIT((((csr = bcmsdh_cfg_read(bus->sdh, SDIO_FUNC_1,
				SBSDIO_FUNC1_CHIPCLKCSR, &err)) & SBSDIO_HT_AVAIL) !=
				(SBSDIO_HT_AVAIL)), (10000));

		}
	}

	
	if (err == 0)
		bus->kso = on ? FALSE : TRUE;
	else {
		DHD_ERROR(("%s: Sleep request failed: on:%d err:%d\n", __FUNCTION__, on, err));
	}

	return err;
}



static int
dhdsdio_htclk(dhd_bus_t *bus, bool on, bool pendok)
{
#define HT_AVAIL_ERROR_MAX 10
	static int ht_avail_error = 0;
	int err;
	uint8 clkctl, clkreq, devctl;
	bcmsdh_info_t *sdh;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	clkctl = 0;
	sdh = bus->sdh;


	if (!KSO_ENAB(bus))
		return BCME_OK;

	if (SLPAUTO_ENAB(bus)) {
		bus->clkstate = (on ? CLK_AVAIL : CLK_SDONLY);
		return BCME_OK;
	}

	if (on) {
		
		clkreq = bus->alp_only ? SBSDIO_ALP_AVAIL_REQ : SBSDIO_HT_AVAIL_REQ;



		bcmsdh_cfg_write(sdh, SDIO_FUNC_1, SBSDIO_FUNC1_CHIPCLKCSR, clkreq, &err);
		if (err) {
			ht_avail_error++;
			if (ht_avail_error < HT_AVAIL_ERROR_MAX) {
				DHD_ERROR(("%s: HT Avail request error: %d\n", __FUNCTION__, err));
			}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)
			else if (ht_avail_error == HT_AVAIL_ERROR_MAX) {
				dhd_os_send_hang_message(bus->dhd);
			}
#endif 
			return BCME_ERROR;
		} else {
			ht_avail_error = 0;
		}


		
		clkctl = bcmsdh_cfg_read(sdh, SDIO_FUNC_1, SBSDIO_FUNC1_CHIPCLKCSR, &err);
		if (err) {
			DHD_ERROR(("%s: HT Avail read error: %d\n", __FUNCTION__, err));
			return BCME_ERROR;
		}

#if !defined(OOB_INTR_ONLY)
		
		if (!SBSDIO_CLKAV(clkctl, bus->alp_only) && pendok) {
			
			devctl = bcmsdh_cfg_read(sdh, SDIO_FUNC_1, SBSDIO_DEVICE_CTL, &err);
			if (err) {
				DHD_ERROR(("%s: Devctl access error setting CA: %d\n",
				           __FUNCTION__, err));
				return BCME_ERROR;
			}

			devctl |= SBSDIO_DEVCTL_CA_INT_ONLY;
			bcmsdh_cfg_write(sdh, SDIO_FUNC_1, SBSDIO_DEVICE_CTL, devctl, &err);
			DHD_INFO(("CLKCTL: set PENDING\n"));
			bus->clkstate = CLK_PENDING;
			return BCME_OK;
		} else
#endif 
		{
			if (bus->clkstate == CLK_PENDING) {
				
				devctl = bcmsdh_cfg_read(sdh, SDIO_FUNC_1, SBSDIO_DEVICE_CTL, &err);
				devctl &= ~SBSDIO_DEVCTL_CA_INT_ONLY;
				bcmsdh_cfg_write(sdh, SDIO_FUNC_1, SBSDIO_DEVICE_CTL, devctl, &err);
			}
		}

		
		if (!SBSDIO_CLKAV(clkctl, bus->alp_only)) {
			SPINWAIT_SLEEP(sdioh_spinwait_sleep,
				((clkctl = bcmsdh_cfg_read(sdh, SDIO_FUNC_1,
			                                    SBSDIO_FUNC1_CHIPCLKCSR, &err)),
			          !SBSDIO_CLKAV(clkctl, bus->alp_only)), PMU_MAX_TRANSITION_DLY);
		}
		if (err) {
			DHD_ERROR(("%s: HT Avail request error: %d\n", __FUNCTION__, err));
			return BCME_ERROR;
		}
		if (!SBSDIO_CLKAV(clkctl, bus->alp_only)) {
			DHD_ERROR(("%s: HT Avail timeout (%d): clkctl 0x%02x\n",
			           __FUNCTION__, PMU_MAX_TRANSITION_DLY, clkctl));
			return BCME_ERROR;
		}

		
		bus->clkstate = CLK_AVAIL;
		DHD_INFO(("CLKCTL: turned ON\n"));

#if defined(DHD_DEBUG)
		if (bus->alp_only == TRUE) {
#if !defined(BCMLXSDMMC)
			if (!SBSDIO_ALPONLY(clkctl)) {
				DHD_ERROR(("%s: HT Clock, when ALP Only\n", __FUNCTION__));
			}
#endif 
		} else {
			if (SBSDIO_ALPONLY(clkctl)) {
				DHD_ERROR(("%s: HT Clock should be on.\n", __FUNCTION__));
			}
		}
#endif 

		bus->activity = TRUE;
#ifdef DHD_USE_IDLECOUNT
		bus->idlecount = 0;
#endif 
	} else {
		clkreq = 0;

		if (bus->clkstate == CLK_PENDING) {
			
			devctl = bcmsdh_cfg_read(sdh, SDIO_FUNC_1, SBSDIO_DEVICE_CTL, &err);
			devctl &= ~SBSDIO_DEVCTL_CA_INT_ONLY;
			bcmsdh_cfg_write(sdh, SDIO_FUNC_1, SBSDIO_DEVICE_CTL, devctl, &err);
		}

		bus->clkstate = CLK_SDONLY;
		if (!SR_ENAB(bus)) {
			bcmsdh_cfg_write(sdh, SDIO_FUNC_1, SBSDIO_FUNC1_CHIPCLKCSR, clkreq, &err);
			DHD_INFO(("CLKCTL: turned OFF\n"));
			if (err) {
				DHD_ERROR(("%s: Failed access turning clock off: %d\n",
				           __FUNCTION__, err));
				return BCME_ERROR;
			}
		}
	}
	return BCME_OK;
}

static int
dhdsdio_sdclk(dhd_bus_t *bus, bool on)
{
	int err;
	int32 iovalue;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	if (on) {
		if (bus->idleclock == DHD_IDLE_STOP) {
			
			iovalue = 1;
			err = bcmsdh_iovar_op(bus->sdh, "sd_clock", NULL, 0,
			                      &iovalue, sizeof(iovalue), TRUE);
			if (err) {
				DHD_ERROR(("%s: error enabling sd_clock: %d\n",
				           __FUNCTION__, err));
				return BCME_ERROR;
			}

			iovalue = bus->sd_mode;
			err = bcmsdh_iovar_op(bus->sdh, "sd_mode", NULL, 0,
			                      &iovalue, sizeof(iovalue), TRUE);
			if (err) {
				DHD_ERROR(("%s: error changing sd_mode: %d\n",
				           __FUNCTION__, err));
				return BCME_ERROR;
			}
		} else if (bus->idleclock != DHD_IDLE_ACTIVE) {
			
			iovalue = bus->sd_divisor;
			err = bcmsdh_iovar_op(bus->sdh, "sd_divisor", NULL, 0,
			                      &iovalue, sizeof(iovalue), TRUE);
			if (err) {
				DHD_ERROR(("%s: error restoring sd_divisor: %d\n",
				           __FUNCTION__, err));
				return BCME_ERROR;
			}
		}
		bus->clkstate = CLK_SDONLY;
	} else {
		
		if ((bus->sd_divisor == -1) || (bus->sd_mode == -1)) {
			DHD_TRACE(("%s: can't idle clock, divisor %d mode %d\n",
			           __FUNCTION__, bus->sd_divisor, bus->sd_mode));
			return BCME_ERROR;
		}
		if (bus->idleclock == DHD_IDLE_STOP) {
			if (sd1idle) {
				
				iovalue = 1;
				err = bcmsdh_iovar_op(bus->sdh, "sd_mode", NULL, 0,
				                      &iovalue, sizeof(iovalue), TRUE);
				if (err) {
					DHD_ERROR(("%s: error changing sd_clock: %d\n",
					           __FUNCTION__, err));
					return BCME_ERROR;
				}
			}

			iovalue = 0;
			err = bcmsdh_iovar_op(bus->sdh, "sd_clock", NULL, 0,
			                      &iovalue, sizeof(iovalue), TRUE);
			if (err) {
				DHD_ERROR(("%s: error disabling sd_clock: %d\n",
				           __FUNCTION__, err));
				return BCME_ERROR;
			}
		} else if (bus->idleclock != DHD_IDLE_ACTIVE) {
			
			iovalue = bus->idleclock;
			err = bcmsdh_iovar_op(bus->sdh, "sd_divisor", NULL, 0,
			                      &iovalue, sizeof(iovalue), TRUE);
			if (err) {
				DHD_ERROR(("%s: error changing sd_divisor: %d\n",
				           __FUNCTION__, err));
				return BCME_ERROR;
			}
		}
		bus->clkstate = CLK_NONE;
	}

	return BCME_OK;
}

static int
dhdsdio_clkctl(dhd_bus_t *bus, uint target, bool pendok)
{
	int ret = BCME_OK;
#ifdef DHD_DEBUG
	uint oldstate = bus->clkstate;
#endif 

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	
	if (bus->clkstate == target) {
		if (target == CLK_AVAIL) {
			dhd_os_wd_timer(bus->dhd, dhd_watchdog_ms);
			bus->activity = TRUE;
#ifdef DHD_USE_IDLECOUNT
			bus->idlecount = 0;
#endif 
		}
		return ret;
	}

	switch (target) {
	case CLK_AVAIL:
		
		if (bus->clkstate == CLK_NONE)
			dhdsdio_sdclk(bus, TRUE);
		
		ret = dhdsdio_htclk(bus, TRUE, pendok);
		if (ret == BCME_OK) {
			dhd_os_wd_timer(bus->dhd, dhd_watchdog_ms);
		bus->activity = TRUE;
#ifdef DHD_USE_IDLECOUNT
			bus->idlecount = 0;
#endif 
		}
		break;

	case CLK_SDONLY:
		
		if (bus->clkstate == CLK_NONE)
			ret = dhdsdio_sdclk(bus, TRUE);
		else if (bus->clkstate == CLK_AVAIL)
			ret = dhdsdio_htclk(bus, FALSE, FALSE);
		else
			DHD_ERROR(("dhdsdio_clkctl: request for %d -> %d\n",
			           bus->clkstate, target));
		if (ret == BCME_OK) {
			dhd_os_wd_timer(bus->dhd, dhd_watchdog_ms);
		}
		break;

	case CLK_NONE:
		
		if (bus->clkstate == CLK_AVAIL)
			ret = dhdsdio_htclk(bus, FALSE, FALSE);
		
		ret = dhdsdio_sdclk(bus, FALSE);
#ifdef DHD_DEBUG
		if (dhd_console_ms == 0)
#endif 
		if (bus->poll == 0)
			dhd_os_wd_timer(bus->dhd, 0);
		break;
	}
#ifdef DHD_DEBUG
	DHD_INFO(("dhdsdio_clkctl: %d -> %d\n", oldstate, bus->clkstate));
#endif 

	return ret;
}

static int
dhdsdio_bussleep(dhd_bus_t *bus, bool sleep)
{
	int err = 0;
	bcmsdh_info_t *sdh = bus->sdh;
	sdpcmd_regs_t *regs = bus->regs;
	uint retries = 0;

	DHD_INFO(("dhdsdio_bussleep: request %s (currently %s)\n",
	          (sleep ? "SLEEP" : "WAKE"),
	          (bus->sleeping ? "SLEEP" : "WAKE")));

	
	if (sleep == bus->sleeping)
		return BCME_OK;

	
	if (sleep) {
		
#ifdef CUSTOMER_HW4
		if (bus->dpc_sched || bus->rxskip || pktq_len(&bus->txq) || bus->readframes)
#else
		if (bus->dpc_sched || bus->rxskip || pktq_len(&bus->txq))
#endif 
			return BCME_BUSY;


		if (!SLPAUTO_ENAB(bus)) {
			
			bcmsdh_intr_disable(bus->sdh);

			
			dhdsdio_clkctl(bus, CLK_AVAIL, FALSE);

			
			W_SDREG(SMB_USE_OOB, &regs->tosbmailbox, retries);
			if (retries > retry_limit)
				DHD_ERROR(("CANNOT SIGNAL CHIP, WILL NOT WAKE UP!!\n"));

			
			dhdsdio_clkctl(bus, CLK_SDONLY, FALSE);

			bcmsdh_cfg_write(sdh, SDIO_FUNC_1, SBSDIO_FUNC1_CHIPCLKCSR,
				SBSDIO_FORCE_HW_CLKREQ_OFF, NULL);

			
			if (bus->sih->chip != BCM4329_CHIP_ID &&
				bus->sih->chip != BCM4319_CHIP_ID) {
				bcmsdh_cfg_write(sdh, SDIO_FUNC_1, SBSDIO_DEVICE_CTL,
					SBSDIO_DEVCTL_PADS_ISO, NULL);
			}
		} else {
			err = dhdsdio_clk_devsleep_iovar(bus, TRUE );
		}

		
		bus->sleeping = TRUE;

	} else {
		

		if (!SLPAUTO_ENAB(bus)) {
			bcmsdh_cfg_write(sdh, SDIO_FUNC_1, SBSDIO_FUNC1_CHIPCLKCSR, 0, &err);

			
			bcmsdh_cfg_write(sdh, SDIO_FUNC_1, SBSDIO_DEVICE_CTL, 0, NULL);


			
			dhdsdio_clkctl(bus, CLK_AVAIL, FALSE);

			
			W_SDREG(0, &regs->tosbmailboxdata, retries);
			if (retries <= retry_limit)
				W_SDREG(SMB_DEV_INT, &regs->tosbmailbox, retries);

			if (retries > retry_limit)
				DHD_ERROR(("CANNOT SIGNAL CHIP TO CLEAR OOB!!\n"));

			
			dhdsdio_clkctl(bus, CLK_SDONLY, FALSE);

			
			if (bus->intr && (bus->dhd->busstate == DHD_BUS_DATA)) {
				bus->intdis = FALSE;
				bcmsdh_intr_enable(bus->sdh);
			}
		} else {
			err = dhdsdio_clk_devsleep_iovar(bus, FALSE );
		}

		if (err == 0) {
			
			bus->sleeping = FALSE;
		}
	}

	return err;
}

#if defined(CUSTOMER_HW4) && defined(USE_DYNAMIC_F2_BLKSIZE)
int dhdsdio_func_blocksize(dhd_pub_t *dhd, int function_num, int block_size)
{
	int func_blk_size = function_num;
	int bcmerr = 0;
	int result;

	bcmerr = dhd_bus_iovar_op(dhd, "sd_blocksize", &func_blk_size,
		sizeof(int), &result, sizeof(int), 0);

	if (bcmerr != BCME_OK) {
		DHD_ERROR(("%s: Get F%d Block size error\n", __FUNCTION__, function_num));
		return BCME_ERROR;
	}

	if (result != block_size) {
		DHD_TRACE_HW4(("%s: F%d Block size set from %d to %d\n",
			__FUNCTION__, function_num, result, block_size));
		func_blk_size = function_num << 16 | block_size;
		bcmerr = dhd_bus_iovar_op(dhd, "sd_blocksize", &func_blk_size,
			sizeof(int32), &result, sizeof(int32), 1);
		if (bcmerr != BCME_OK) {
			DHD_ERROR(("%s: Set F2 Block size error\n", __FUNCTION__));
			return BCME_ERROR;
		}
	}

	return BCME_OK;
}
#endif 

#if defined(OOB_INTR_ONLY)
void
dhd_enable_oob_intr(struct dhd_bus *bus, bool enable)
{
#if defined(HW_OOB)
	bcmsdh_enable_hw_oob_intr(bus->sdh, enable);
#else
	sdpcmd_regs_t *regs = bus->regs;
	uint retries = 0;

	dhdsdio_clkctl(bus, CLK_AVAIL, FALSE);
	if (enable == TRUE) {

		
		W_SDREG(SMB_USE_OOB, &regs->tosbmailbox, retries);
		if (retries > retry_limit)
			DHD_ERROR(("CANNOT SIGNAL CHIP, WILL NOT WAKE UP!!\n"));

	} else {
		
		W_SDREG(0, &regs->tosbmailboxdata, retries);
		if (retries <= retry_limit)
			W_SDREG(SMB_DEV_INT, &regs->tosbmailbox, retries);
	}

	
	dhdsdio_clkctl(bus, CLK_SDONLY, FALSE);
#endif 
}
#endif 

#ifdef DHDTCPACK_SUPPRESS
extern bool dhd_use_tcpack_suppress;

void dhd_onoff_tcpack_sup(void *pub, bool on)
{
	dhd_pub_t *dhdp = (dhd_pub_t *)pub;

	if (dhd_use_tcpack_suppress != on) {

		DHD_ERROR(("dhd_onoff_tcpack_sup: %d -> %d\n", dhd_use_tcpack_suppress, on));
		dhd_use_tcpack_suppress = on;
		dhdp->tcp_ack_info_cnt = 0;
		bzero(dhdp->tcp_ack_info_tbl, sizeof(struct tcp_ack_info)*MAXTCPSTREAMS);

	} else
		DHD_ERROR(("dhd_onoff_tcpack_sup: alread %d\n", on));

	return;
}

inline void dhd_tcpack_check_xmit(dhd_pub_t *dhdp, void *pkt)
{
	uint8 i;
	tcp_ack_info_t *tcp_ack_info = NULL;
	int tbl_cnt;

	dhd_os_tcpacklock(dhdp);
	tbl_cnt = dhdp->tcp_ack_info_cnt;
	for (i = 0; i < tbl_cnt; i++) {
		tcp_ack_info = &dhdp->tcp_ack_info_tbl[i];
		if (tcp_ack_info->p_tcpackinqueue == pkt) {
			if (i < tbl_cnt-1) {
				memmove(&dhdp->tcp_ack_info_tbl[i],
					&dhdp->tcp_ack_info_tbl[i+1],
					sizeof(struct tcp_ack_info)*(tbl_cnt - (i+1)));
			}
			bzero(&dhdp->tcp_ack_info_tbl[tbl_cnt-1], sizeof(struct tcp_ack_info));
			if (--dhdp->tcp_ack_info_cnt < 0) {
				DHD_ERROR(("dhdsdio_sendfromq:(ERROR) tcp_ack_info_cnt %d"
				" Stop using tcpack_suppress\n", dhdp->tcp_ack_info_cnt));
				dhd_onoff_tcpack_sup(dhdp, FALSE);
			}
			break;
		}
	}
	dhd_os_tcpackunlock(dhdp);
}

bool
dhd_tcpack_suppress(dhd_pub_t *dhdp, void *pkt)
{
	uint8 *eh_header;
	uint16 eh_type;
	uint8 *ip_header;
	uint8 *tcp_header;
	uint32 ip_hdr_len;
	uint32 cur_framelen;
	uint8 bdc_hdr_len = BDC_HEADER_LEN;
	uint8 wlfc_hdr_len = 0;
	uint8 *data = PKTDATA(dhdp->osh, pkt);
	cur_framelen = PKTLEN(dhdp->osh, pkt);

#ifdef PROP_TXSTATUS
	
	if (dhdp->wlfc_state) {
		bdc_hdr_len = 0;
		wlfc_hdr_len = 8;
	}
#endif
	if (cur_framelen < bdc_hdr_len + ETHER_HDR_LEN) {
		DHD_TRACE(("dhd_tcpack_suppress: Too short packet length %d\n", cur_framelen));
		return FALSE;
	}

	
	eh_header = data + bdc_hdr_len;
	cur_framelen -= bdc_hdr_len;
	eh_type = eh_header[12] << 8 | eh_header[13];

	if (eh_type != ETHER_TYPE_IP) {
		DHD_TRACE(("dhd_tcpack_suppress: Not a IP packet 0x%x\n", eh_type));
		return FALSE;
	}

	DHD_TRACE(("dhd_tcpack_suppress: IP pkt! 0x%x\n", eh_type));

	ip_header = eh_header + ETHER_HDR_LEN;
	cur_framelen -= ETHER_HDR_LEN;
	ip_hdr_len = 4 * (ip_header[0] & 0x0f);

	if ((ip_header[0] & 0xf0) != 0x40) {
		DHD_TRACE(("dhd_tcpack_suppress: Not IPv4!\n"));
		return FALSE;
	}

	if (cur_framelen < ip_hdr_len) {
		DHD_ERROR(("dhd_tcpack_suppress: IP packet length %d wrong!\n", cur_framelen));
		return FALSE;
	}

	
	if (ip_header[9] != 0x06) {
		DHD_TRACE(("dhd_tcpack_suppress: Not a TCP packet 0x%x\n", ip_header[9]));
		return FALSE;
	}

	DHD_TRACE(("dhd_tcpack_suppress: TCP pkt!\n"));

	tcp_header = ip_header + ip_hdr_len;

	
	if (tcp_header[13] == 0x10) {
		uint32 tcp_seq_num = tcp_header[4] << 24 | tcp_header[5] << 16 |
			tcp_header[6] << 8 | tcp_header[7];
		uint32 tcp_ack_num = tcp_header[8] << 24 | tcp_header[9] << 16 |
			tcp_header[10] << 8 | tcp_header[11];
		uint16 ip_tcp_ttllen =  (ip_header[3] & 0xff) + (ip_header[2] << 8);
		uint32 tcp_hdr_len = 4*((tcp_header[12] & 0xf0) >> 4);
		DHD_TRACE(("dhd_tcpack_suppress: TCP ACK seq %ud ack %ud\n",
			tcp_seq_num, tcp_ack_num));


		
		if (ip_tcp_ttllen ==  ip_hdr_len + tcp_hdr_len) {
			int i;
			tcp_ack_info_t *tcp_ack_info = NULL;
			DHD_TRACE(("dhd_tcpack_suppress: TCP ACK zero length\n"));
			dhd_os_tcpacklock(dhdp);
			for (i = 0; i < dhdp->tcp_ack_info_cnt; i++) {
				if (dhdp->tcp_ack_info_tbl[i].p_tcpackinqueue &&
				!memcmp(&ip_header[12], dhdp->tcp_ack_info_tbl[i].ipaddrs, 8) &&
				!memcmp(tcp_header, dhdp->tcp_ack_info_tbl[i].tcpports, 4)) {
					tcp_ack_info = &dhdp->tcp_ack_info_tbl[i];
					break;
				}
			}

			if (i == dhdp->tcp_ack_info_cnt && i < MAXTCPSTREAMS)
				tcp_ack_info = &dhdp->tcp_ack_info_tbl[dhdp->tcp_ack_info_cnt++];

			if (!tcp_ack_info) {
				DHD_TRACE(("dhd_tcpack_suppress: No empty tcp ack info"
					"%d %d %d %d, %d %d %d %d\n",
					tcp_header[0], tcp_header[1], tcp_header[2], tcp_header[3],
					dhdp->tcp_ack_info_tbl[i].tcpports[0],
					dhdp->tcp_ack_info_tbl[i].tcpports[1],
					dhdp->tcp_ack_info_tbl[i].tcpports[2],
					dhdp->tcp_ack_info_tbl[i].tcpports[3]));
				dhd_os_tcpackunlock(dhdp);
				return FALSE;
			}

			if (tcp_ack_info->p_tcpackinqueue) {
				if (tcp_ack_num > tcp_ack_info->tcpack_number) {
					void *prevpkt = tcp_ack_info->p_tcpackinqueue;
					uint8 pushed_len = SDPCM_HDRLEN +
						(BDC_HEADER_LEN - bdc_hdr_len) + wlfc_hdr_len;
#ifdef PROP_TXSTATUS
					if (dhdp->wlfc_state &&	(PKTLEN(dhdp->osh, prevpkt) ==
						tcp_ack_info->ip_tcp_ttllen + ETHER_HDR_LEN))
						pushed_len = 0;
#endif
					if ((ip_tcp_ttllen == tcp_ack_info->ip_tcp_ttllen) &&
						(PKTLEN(dhdp->osh, pkt) ==
						PKTLEN(dhdp->osh, prevpkt) - pushed_len)) {
						bcopy(PKTDATA(dhdp->osh, pkt),
							PKTDATA(dhdp->osh, prevpkt) + pushed_len,
							PKTLEN(dhdp->osh, pkt));
						PKTFREE(dhdp->osh, pkt, FALSE);
						DHD_TRACE(("dhd_tcpack_suppress: pkt 0x%p"
							" TCP ACK replace %ud -> %ud\n", prevpkt,
							tcp_ack_info->tcpack_number, tcp_ack_num));
						tcp_ack_info->tcpack_number = tcp_ack_num;
						dhd_os_tcpackunlock(dhdp);
						return TRUE;
					} else
						DHD_TRACE(("dhd_tcpack_suppress: len mismatch"
							" %d(%d) %d(%d)\n",
							PKTLEN(dhdp->osh, pkt), ip_tcp_ttllen,
							PKTLEN(dhdp->osh, prevpkt),
							tcp_ack_info->ip_tcp_ttllen));
				} else {	
#ifdef TCPACK_TEST
					void *prevpkt = tcp_ack_info->p_tcpackinqueue;
#endif
					DHD_TRACE(("dhd_tcpack_suppress: TCP ACK number reverse"
							" prev %ud (0x%p) new %ud (0x%p)\n",
							tcp_ack_info->tcpack_number,
							tcp_ack_info->p_tcpackinqueue,
							tcp_ack_num, pkt));
#ifdef TCPACK_TEST
					if (PKTLEN(dhdp->osh, pkt) == PKTLEN(dhdp->osh, prevpkt)) {
						PKTFREE(dhdp->osh, pkt, FALSE);
						dhd_os_tcpackunlock(dhdp);
						return TRUE;
					}
#endif
				}
			} else {
				tcp_ack_info->p_tcpackinqueue = pkt;
				tcp_ack_info->tcpack_number = tcp_ack_num;
				tcp_ack_info->ip_tcp_ttllen = ip_tcp_ttllen;
				bcopy(&ip_header[12], tcp_ack_info->ipaddrs, 8);
				bcopy(tcp_header, tcp_ack_info->tcpports, 4);
			}
			dhd_os_tcpackunlock(dhdp);
		} else
			DHD_TRACE(("dhd_tcpack_suppress: TCP ACK with DATA len %d\n",
				ip_tcp_ttllen - ip_hdr_len - tcp_hdr_len));
	}
	return FALSE;
}
#endif 


static int
dhdsdio_txpkt(dhd_bus_t *bus, void *pkt, uint chan, bool free_pkt, bool queue_only)
{
	int ret;
	osl_t *osh;
	uint8 *frame;
	uint16 len, pad1 = 0;
	uint32 swheader;
	uint retries = 0;
	bcmsdh_info_t *sdh;
	void *new;
	
	int pkt_cnt;
#ifdef BCMSDIOH_TXGLOM
	uint8 *frame_tmp;
#endif
#ifdef WLMEDIA_HTSF
	char *p;
	htsfts_t *htsf_ts;
#endif

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	sdh = bus->sdh;
	osh = bus->dhd->osh;

#ifdef DHDTCPACK_SUPPRESS
	if (dhd_use_tcpack_suppress) {
		dhd_tcpack_check_xmit(bus->dhd, pkt);
	}
#endif 

	if (bus->dhd->dongle_reset) {
		ret = BCME_NOTREADY;
		goto done;
	}

	frame = (uint8*)PKTDATA(osh, pkt);

#ifdef WLMEDIA_HTSF
	if (PKTLEN(osh, pkt) >= 100) {
		p = PKTDATA(osh, pkt);
		htsf_ts = (htsfts_t*) (p + HTSF_HOSTOFFSET + 12);
		if (htsf_ts->magic == HTSFMAGIC) {
			htsf_ts->c20 = get_cycles();
			htsf_ts->t20 = dhd_get_htsf(bus->dhd->info, 0);
		}
	}
#endif 

	
	if (!((uintptr)frame & 1) && (pad1 = ((uintptr)frame % DHD_SDALIGN))) {
		if (PKTHEADROOM(osh, pkt) < pad1) {
			DHD_INFO(("%s: insufficient headroom %d for %d pad1\n",
			          __FUNCTION__, (int)PKTHEADROOM(osh, pkt), pad1));
			bus->dhd->tx_realloc++;
			new = PKTGET(osh, (PKTLEN(osh, pkt) + DHD_SDALIGN), TRUE);
			if (!new) {
				DHD_ERROR(("%s: couldn't allocate new %d-byte packet\n",
				           __FUNCTION__, PKTLEN(osh, pkt) + DHD_SDALIGN));
				ret = BCME_NOMEM;
				goto done;
			}

			PKTALIGN(osh, new, PKTLEN(osh, pkt), DHD_SDALIGN);
			bcopy(PKTDATA(osh, pkt), PKTDATA(osh, new), PKTLEN(osh, pkt));
			if (free_pkt)
				PKTFREE(osh, pkt, TRUE);
			
			free_pkt = TRUE;
			pkt = new;
			frame = (uint8*)PKTDATA(osh, pkt);
			ASSERT(((uintptr)frame % DHD_SDALIGN) == 0);
			pad1 = 0;
		} else {
			PKTPUSH(osh, pkt, pad1);
			frame = (uint8*)PKTDATA(osh, pkt);

			ASSERT((pad1 + SDPCM_HDRLEN) <= (int) PKTLEN(osh, pkt));
			bzero(frame, pad1 + SDPCM_HDRLEN);
		}
	}
	ASSERT(pad1 < DHD_SDALIGN);

	
	len = (uint16)PKTLEN(osh, pkt);
	*(uint16*)frame = htol16(len);
	*(((uint16*)frame) + 1) = htol16(~len);

#ifdef BCMSDIOH_TXGLOM
	if (bus->glom_enable) {
		uint32 hwheader1 = 0, hwheader2 = 0, act_len = len;
		uint32 real_pad = 0;

		
		swheader = ((chan << SDPCM_CHANNEL_SHIFT) & SDPCM_CHANNEL_MASK) |
			((bus->tx_seq + bus->glom_cnt) % SDPCM_SEQUENCE_WRAP) |
		        (((pad1 + SDPCM_HDRLEN) << SDPCM_DOFFSET_SHIFT) & SDPCM_DOFFSET_MASK);
		htol32_ua_store(swheader, frame + SDPCM_FRAMETAG_LEN + SDPCM_HWEXT_LEN);
		htol32_ua_store(0, frame + SDPCM_FRAMETAG_LEN + SDPCM_HWEXT_LEN + sizeof(swheader));

		if (queue_only) {
			uint8 alignment = ALIGNMENT;
#if defined(BCMLXSDMMC) && defined(CUSTOMER_HW4)
			if (bus->glom_mode == SDPCM_TXGLOM_MDESC)
				alignment = DHD_SDALIGN;
#endif 
			if (forcealign && (len & (alignment - 1)))
				len = ROUNDUP(len, alignment);
			
			hwheader1 = (act_len - SDPCM_FRAMETAG_LEN) | (0 << 24);
			hwheader2 = (len - act_len) << 16;
			htol32_ua_store(hwheader1, frame + SDPCM_FRAMETAG_LEN);
			htol32_ua_store(hwheader2, frame + SDPCM_FRAMETAG_LEN + 4);

                	real_pad = len - act_len;
                	if (PKTTAILROOM(osh, pkt) < real_pad) {
                    		DHD_INFO(("%s 1: insufficient tailroom %d"
                    			" for %d real_pad\n",
                    			__FUNCTION__, (int)PKTTAILROOM(osh, pkt), real_pad));
                    		if (skb_pad(pkt, real_pad)) {
                        		DHD_ERROR(("padding error size %d\n", real_pad));
                    		}
                	}

			
			dhd_bcmsdh_glom_post(bus, frame, pkt, len);
			
			bus->glom_pkt_arr[bus->glom_cnt] = pkt;
			bus->glom_total_len += len;
			bus->glom_cnt++;
			return BCME_OK;
		} else {
				
				if (bus->roundup && bus->blocksize &&
					((bus->glom_total_len + len) > bus->blocksize)) {
					uint16 pad2 = bus->blocksize -
						((bus->glom_total_len + len) % bus->blocksize);
					if ((pad2 <= bus->roundup) && (pad2 < bus->blocksize)) {
							len += pad2;
					} else {
					}
				} else if ((bus->glom_total_len + len) % DHD_SDALIGN) {
					len += DHD_SDALIGN
					    - ((bus->glom_total_len + len) % DHD_SDALIGN);
				}
				if (forcealign && (len & (ALIGNMENT - 1))) {
					len = ROUNDUP(len, ALIGNMENT);
				}

				
				hwheader1 = (act_len - SDPCM_FRAMETAG_LEN) | (1 << 24);
				hwheader2 = (len - act_len) << 16;
				htol32_ua_store(hwheader1, frame + SDPCM_FRAMETAG_LEN);
				htol32_ua_store(hwheader2, frame + SDPCM_FRAMETAG_LEN + 4);

                		real_pad = len - act_len;
                		if (PKTTAILROOM(osh, pkt) < real_pad) {
                    			DHD_INFO(("%s 2: insufficient tailroom %d"
                    				" for %d real_pad\n",
                    				__FUNCTION__, (int)PKTTAILROOM(osh, pkt), real_pad));
                    			if (skb_pad(pkt, real_pad)) {
                        			DHD_ERROR(("padding error size %d\n", real_pad));
                    			}
                		}

				
				dhd_bcmsdh_glom_post(bus, frame, pkt, len);
				
				bus->glom_pkt_arr[bus->glom_cnt] = pkt;
				bus->glom_cnt++;
				bus->glom_total_len += len;

				
				frame_tmp = (uint8*)PKTDATA(osh, bus->glom_pkt_arr[0]);
				*(uint16*)frame_tmp = htol16(bus->glom_total_len);
				*(((uint16*)frame_tmp) + 1) = htol16(~bus->glom_total_len);
		}
	} else
#endif 
	{
	
	swheader = ((chan << SDPCM_CHANNEL_SHIFT) & SDPCM_CHANNEL_MASK) | bus->tx_seq |
	        (((pad1 + SDPCM_HDRLEN) << SDPCM_DOFFSET_SHIFT) & SDPCM_DOFFSET_MASK);
	htol32_ua_store(swheader, frame + SDPCM_FRAMETAG_LEN);
	htol32_ua_store(0, frame + SDPCM_FRAMETAG_LEN + sizeof(swheader));

#ifdef DHD_DEBUG
	if (PKTPRIO(pkt) < ARRAYSIZE(tx_packets)) {
		tx_packets[PKTPRIO(pkt)]++;
	}
	if (DHD_BYTES_ON() &&
	    (((DHD_CTL_ON() && (chan == SDPCM_CONTROL_CHANNEL)) ||
	      (DHD_DATA_ON() && (chan != SDPCM_CONTROL_CHANNEL))))) {
		prhex("Tx Frame", frame, len);
	} else if (DHD_HDRS_ON()) {
		prhex("TxHdr", frame, MIN(len, 16));
	}
#endif

	
	if (bus->roundup && bus->blocksize && (len > bus->blocksize)) {
		uint16 pad2 = bus->blocksize - (len % bus->blocksize);
		if ((pad2 <= bus->roundup) && (pad2 < bus->blocksize))
#ifdef NOTUSED
			if (pad2 <= PKTTAILROOM(osh, pkt))
#endif 
				len += pad2;
	} else if (len % DHD_SDALIGN) {
		len += DHD_SDALIGN - (len % DHD_SDALIGN);
	}

	
	if (forcealign && (len & (ALIGNMENT - 1))) {
#ifdef NOTUSED
		if (PKTTAILROOM(osh, pkt))
#endif
			len = ROUNDUP(len, ALIGNMENT);
#ifdef NOTUSED
		else
			DHD_ERROR(("%s: sending unrounded %d-byte packet\n", __FUNCTION__, len));
#endif
	}
	}

	do {
		ret = dhd_bcmsdh_send_buf(bus, bcmsdh_cur_sbwad(sdh), SDIO_FUNC_2, F2SYNC,
		                          frame, len, pkt, NULL, NULL);
		bus->f2txdata++;
		ASSERT(ret != BCME_PENDING);

		if (ret == BCME_NODEVICE) {
			DHD_ERROR(("%s: Device asleep already\n", __FUNCTION__));
		} else if (ret < 0) {
#if 1
			
			DHD_ERROR(("%s: sdio error %d, abort command and terminate frame 3.\n",
			__FUNCTION__, ret));
			bus->dhd->busstate = DHD_BUS_DOWN;
			dhd_os_send_hang_message(bus->dhd);
#else
			
			DHD_ERROR(("%s: sdio error %d, abort command and terminate frame.\n",
			          __FUNCTION__, ret));
			bus->tx_sderrs++;

			bcmsdh_abort(sdh, SDIO_FUNC_2);
			bcmsdh_cfg_write(sdh, SDIO_FUNC_1, SBSDIO_FUNC1_FRAMECTRL,
			                 SFC_WF_TERM, NULL);
			bus->f1regdata++;

			for (i = 0; i < 3; i++) {
				uint8 hi, lo;
				hi = bcmsdh_cfg_read(sdh, SDIO_FUNC_1,
				                     SBSDIO_FUNC1_WFRAMEBCHI, NULL);
				lo = bcmsdh_cfg_read(sdh, SDIO_FUNC_1,
				                     SBSDIO_FUNC1_WFRAMEBCLO, NULL);
				bus->f1regdata += 2;
				if ((hi == 0) && (lo == 0))
					break;
			}
#endif
		}
		if (ret == 0) {
#ifdef BCMSDIOH_TXGLOM
			if (bus->glom_enable) {
				bus->tx_seq = (bus->tx_seq + bus->glom_cnt) % SDPCM_SEQUENCE_WRAP;
			} else
#endif
			{
			bus->tx_seq = (bus->tx_seq + 1) % SDPCM_SEQUENCE_WRAP;
		}
		}
	} while ((ret < 0) && retrydata && retries++ < TXRETRIES);

done:

#ifdef BCMSDIOH_TXGLOM
	if (bus->glom_enable) {
		dhd_bcmsdh_glom_clear(bus);
		pkt_cnt = bus->glom_cnt;
	} else
#endif
	{
		pkt_cnt = 1;
	}
		
	while (pkt_cnt) {
#ifdef BCMSDIOH_TXGLOM
		uint32 doff;
		if (bus->glom_enable) {
			pkt = bus->glom_pkt_arr[bus->glom_cnt - pkt_cnt];
			frame = (uint8*)PKTDATA(osh, pkt);
			doff = ltoh32_ua(frame + SDPCM_FRAMETAG_LEN + SDPCM_HWEXT_LEN);
			doff = (doff & SDPCM_DOFFSET_MASK) >> SDPCM_DOFFSET_SHIFT;
			PKTPULL(osh, pkt, doff);
		} else
#endif
		{
	PKTPULL(osh, pkt, SDPCM_HDRLEN + pad1);
		}
#ifdef PROP_TXSTATUS
	if (bus->dhd->wlfc_state) {
		dhd_os_sdunlock(bus->dhd);
		dhd_wlfc_txcomplete(bus->dhd, pkt, ret == 0);
		dhd_os_sdlock(bus->dhd);
	} else {
#endif 
#ifdef SDTEST
	if (chan != SDPCM_TEST_CHANNEL) {
		dhd_txcomplete(bus->dhd, pkt, ret != 0);
	}
#else 
	dhd_txcomplete(bus->dhd, pkt, ret != 0);
#endif 
	if (free_pkt)
		PKTFREE(osh, pkt, TRUE);

#ifdef PROP_TXSTATUS
	}
#endif
		pkt_cnt--;
	}

#ifdef BCMSDIOH_TXGLOM
	
	if (bus->glom_enable) {
		bus->glom_cnt = 0;
		bus->glom_total_len = 0;
	}
#endif
	return ret;
}

int
dhd_bus_txdata(struct dhd_bus *bus, void *pkt)
{
	int ret = BCME_ERROR;
	osl_t *osh;
	uint datalen, prec;
#ifdef DHD_TX_DUMP
	uint8 *dump_data;
	uint16 protocol;
#ifdef DHD_TX_FULL_DUMP
	int i;
#endif 
#endif 
	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	osh = bus->dhd->osh;
	datalen = PKTLEN(osh, pkt);

#ifdef SDTEST
	
	if (bus->ext_loop) {
		uint8* data;
		PKTPUSH(osh, pkt, SDPCM_TEST_HDRLEN);
		data = PKTDATA(osh, pkt);
		*data++ = SDPCM_TEST_ECHOREQ;
		*data++ = (uint8)bus->loopid++;
		*data++ = (datalen >> 0);
		*data++ = (datalen >> 8);
		datalen += SDPCM_TEST_HDRLEN;
	}
#endif 

#ifdef DHD_TX_DUMP
	dump_data = PKTDATA(osh, pkt);
	dump_data += 4; 
	protocol = (dump_data[12] << 8) | dump_data[13];
#ifdef DHD_TX_FULL_DUMP
	DHD_ERROR(("TX DUMP\n"));

	for (i = 0; i < (datalen - 4); i++) {
		DHD_ERROR(("%02X ", dump_data[i]));
		if ((i & 15) == 15)
			printk("\n");
	}
	DHD_ERROR(("\n"));

#endif 
	if (protocol == ETHER_TYPE_802_1X) {
		DHD_ERROR(("ETHER_TYPE_802_1X: ver %d, type %d, replay %d\n",
			dump_data[14], dump_data[15], dump_data[30]));
	}
#endif 

	
	PKTPUSH(osh, pkt, SDPCM_HDRLEN);
	ASSERT(ISALIGNED((uintptr)PKTDATA(osh, pkt), 2));

	prec = PRIO2PREC((PKTPRIO(pkt) & PRIOMASK));
#ifndef DHDTHREAD
	
	dhd_os_sdlock(bus->dhd);
#endif 

	
	if (dhd_deferred_tx || bus->fcstate || pktq_len(&bus->txq) || bus->dpc_sched ||
	    (!DATAOK(bus)) || (bus->flowcontrol & NBITVAL(prec)) ||
	    (bus->clkstate != CLK_AVAIL)) {
		DHD_TRACE(("%s: deferring pktq len %d\n", __FUNCTION__,
			pktq_len(&bus->txq)));
		bus->fcqueued++;

		
		dhd_os_sdlock_txq(bus->dhd);
		if (dhd_prec_enq(bus->dhd, &bus->txq, pkt, prec) == FALSE) {
			PKTPULL(osh, pkt, SDPCM_HDRLEN);
#ifndef DHDTHREAD
			dhd_os_sdunlock_txq(bus->dhd);
			dhd_os_sdunlock(bus->dhd);
#endif
#ifdef PROP_TXSTATUS
			if (bus->dhd->wlfc_state)
				dhd_wlfc_txcomplete(bus->dhd, pkt, FALSE);
			else
#endif
			dhd_txcomplete(bus->dhd, pkt, FALSE);
#ifndef DHDTHREAD
			dhd_os_sdlock(bus->dhd);
			dhd_os_sdlock_txq(bus->dhd);
#endif
#ifdef PROP_TXSTATUS
			
			if (!bus->dhd->wlfc_state)
#endif
			PKTFREE(osh, pkt, TRUE);
			ret = BCME_NORESOURCE;
		}
		else
			ret = BCME_OK;

		if ((pktq_len(&bus->txq) >= FCHI) && dhd_doflow)
			dhd_txflowcontrol(bus->dhd, ALL_INTERFACES, ON);

#ifdef DHD_DEBUG
		if (pktq_plen(&bus->txq, prec) > qcount[prec])
			qcount[prec] = pktq_plen(&bus->txq, prec);
#endif
		dhd_os_sdunlock_txq(bus->dhd);

		
		if (dhd_deferred_tx && !bus->dpc_sched) {
			bus->dpc_sched = TRUE;
			dhd_sched_dpc(bus->dhd);
		}
	} else {
#ifdef DHDTHREAD
		
		dhd_os_sdlock(bus->dhd);
#endif 

		
		BUS_WAKE(bus);
		
		dhdsdio_clkctl(bus, CLK_AVAIL, TRUE);
#ifndef SDTEST
		ret = dhdsdio_txpkt(bus, pkt, SDPCM_DATA_CHANNEL, TRUE, FALSE);
#else
		ret = dhdsdio_txpkt(bus, pkt,
		        (bus->ext_loop ? SDPCM_TEST_CHANNEL : SDPCM_DATA_CHANNEL), TRUE, FALSE);
#endif
		if (ret)
			bus->dhd->tx_errors++;
		else
			bus->dhd->dstats.tx_bytes += datalen;

		if ((bus->idletime == DHD_IDLE_IMMEDIATE) && !bus->dpc_sched) {
			bus->activity = FALSE;
			dhdsdio_clkctl(bus, CLK_NONE, TRUE);
		}

#ifdef DHDTHREAD
		dhd_os_sdunlock(bus->dhd);
#endif 
	}

#ifndef DHDTHREAD
	dhd_os_sdunlock(bus->dhd);
#endif 

	return ret;
}

static uint
dhdsdio_sendfromq(dhd_bus_t *bus, uint maxframes)
{
	void *pkt;
	uint32 intstatus = 0;
	uint retries = 0;
	int ret = 0, prec_out;
	uint cnt = 0;
	uint datalen;
	uint8 tx_prec_map;
	uint8 txpktqlen = 0;
#ifdef BCMSDIOH_TXGLOM
	uint i;
	uint8 glom_cnt;
#endif

	dhd_pub_t *dhd = bus->dhd;
	sdpcmd_regs_t *regs = bus->regs;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	if (!KSO_ENAB(bus)) {
		DHD_ERROR(("%s: Device asleep\n", __FUNCTION__));
		return BCME_NODEVICE;
	}

	tx_prec_map = ~bus->flowcontrol;

	
	for (cnt = 0; (cnt < maxframes) && DATAOK(bus); cnt++) {
#ifdef BCMSDIOH_TXGLOM
		if (bus->glom_enable) {
			void *pkttable[SDPCM_MAXGLOM_SIZE];
			dhd_os_sdlock_txq(bus->dhd);
			glom_cnt = MIN(DATABUFCNT(bus), bus->glomsize);
			glom_cnt = MIN(glom_cnt, pktq_mlen(&bus->txq, tx_prec_map));
			

			
			if (bus->glom_mode == SDPCM_TXGLOM_CPY)
			    glom_cnt = MIN(glom_cnt, 10);

			for (i = 0; i < glom_cnt; i++)
				pkttable[i] = pktq_mdeq(&bus->txq, tx_prec_map, &prec_out);

			txpktqlen = pktq_len(&bus->txq);
			dhd_os_sdunlock_txq(bus->dhd);

			if (glom_cnt == 0)
				break;
			datalen = 0;
			for (i = 0; i < glom_cnt; i++) {
				if ((pkt = pkttable[i]) == NULL) {
					
					DHD_ERROR(("No pkts in the queue for glomming\n"));
					break;
				}

				datalen += (PKTLEN(bus->dhd->osh, pkt) - SDPCM_HDRLEN);
#ifndef SDTEST
				ret = dhdsdio_txpkt(bus,
					pkt,
					SDPCM_DATA_CHANNEL,
					TRUE,
					(i == (glom_cnt-1))? FALSE: TRUE);
#else
				ret = dhdsdio_txpkt(bus,
					pkt,
					(bus->ext_loop ? SDPCM_TEST_CHANNEL : SDPCM_DATA_CHANNEL),
					TRUE,
					(i == (glom_cnt-1))? FALSE: TRUE);
#endif
			}
			cnt += i-1;
		} else
#endif 
		{
		dhd_os_sdlock_txq(bus->dhd);
		if ((pkt = pktq_mdeq(&bus->txq, tx_prec_map, &prec_out)) == NULL) {
			txpktqlen = pktq_len(&bus->txq);
			dhd_os_sdunlock_txq(bus->dhd);
			break;
		}

		txpktqlen = pktq_len(&bus->txq);
		dhd_os_sdunlock_txq(bus->dhd);
		datalen = PKTLEN(bus->dhd->osh, pkt) - SDPCM_HDRLEN;

#ifndef SDTEST
		ret = dhdsdio_txpkt(bus, pkt, SDPCM_DATA_CHANNEL, TRUE, FALSE);
#else
		ret = dhdsdio_txpkt(bus,
			pkt,
			(bus->ext_loop ? SDPCM_TEST_CHANNEL : SDPCM_DATA_CHANNEL),
			TRUE,
			FALSE);
#endif
		}

		if (ret)
			bus->dhd->tx_errors++;
		else
			bus->dhd->dstats.tx_bytes += datalen;

		
		if (!bus->intr && cnt)
		{
			
			R_SDREG(intstatus, &regs->intstatus, retries);
			bus->f2txdata++;
			if (bcmsdh_regfail(bus->sdh))
				break;
			if (intstatus & bus->hostintmask)
				bus->ipend = TRUE;
		}
	}

	
	if (dhd_doflow && dhd->up && (dhd->busstate == DHD_BUS_DATA) &&
	    dhd->txoff && (txpktqlen < FCLOW))
		dhd_txflowcontrol(dhd, ALL_INTERFACES, OFF);

	return cnt;
}

static int bus_txctl_failed_num = 0;

int rxglom_fail_count = RXGLOM_FAIL_COUNT;
int max_cntl_timeout =  MAX_CNTL_TIMEOUT;

int
dhd_bus_txctl(struct dhd_bus *bus, uchar *msg, uint msglen)
{
	uint8 *frame;
	uint16 len;
	uint32 swheader;
	uint retries = 0;
	bcmsdh_info_t *sdh = bus->sdh;
	uint8 doff = 0;
	int ret = -1;
	

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	if (bus->dhd->dongle_reset)
		return -EIO;

	
	frame = msg - SDPCM_HDRLEN;
	len = (msglen += SDPCM_HDRLEN);

	
	if (dhd_alignctl) {
		if ((doff = ((uintptr)frame % DHD_SDALIGN))) {
			frame -= doff;
			len += doff;
			msglen += doff;
			bzero(frame, doff + SDPCM_HDRLEN);
		}
		ASSERT(doff < DHD_SDALIGN);
	}
	doff += SDPCM_HDRLEN;

	
	if (bus->roundup && bus->blocksize && (len > bus->blocksize)) {
		uint16 pad = bus->blocksize - (len % bus->blocksize);
		if ((pad <= bus->roundup) && (pad < bus->blocksize))
			len += pad;
	} else if (len % DHD_SDALIGN) {
		len += DHD_SDALIGN - (len % DHD_SDALIGN);
	}

	
	if (forcealign && (len & (ALIGNMENT - 1)))
		len = ROUNDUP(len, ALIGNMENT);

	ASSERT(ISALIGNED((uintptr)frame, 2));


	
	dhd_os_sdlock(bus->dhd);

	BUS_WAKE(bus);

	
	dhdsdio_clkctl(bus, CLK_AVAIL, FALSE);

	
	*(uint16*)frame = htol16((uint16)msglen);
	*(((uint16*)frame) + 1) = htol16(~msglen);

#ifdef BCMSDIOH_TXGLOM
	if (bus->glom_enable) {
		uint32 hwheader1, hwheader2;
		
		swheader = ((SDPCM_CONTROL_CHANNEL << SDPCM_CHANNEL_SHIFT) & SDPCM_CHANNEL_MASK)
				| bus->tx_seq
				| ((doff << SDPCM_DOFFSET_SHIFT) & SDPCM_DOFFSET_MASK);
		htol32_ua_store(swheader, frame + SDPCM_FRAMETAG_LEN + SDPCM_HWEXT_LEN);
		htol32_ua_store(0, frame + SDPCM_FRAMETAG_LEN
			+ SDPCM_HWEXT_LEN + sizeof(swheader));

		hwheader1 = (msglen - SDPCM_FRAMETAG_LEN) | (1 << 24);
		hwheader2 = (len - (msglen)) << 16;
		htol32_ua_store(hwheader1, frame + SDPCM_FRAMETAG_LEN);
		htol32_ua_store(hwheader2, frame + SDPCM_FRAMETAG_LEN + 4);

		*(uint16*)frame = htol16(len);
		*(((uint16*)frame) + 1) = htol16(~(len));
	} else
#endif 
	{
	
	swheader = ((SDPCM_CONTROL_CHANNEL << SDPCM_CHANNEL_SHIFT) & SDPCM_CHANNEL_MASK)
	        | bus->tx_seq | ((doff << SDPCM_DOFFSET_SHIFT) & SDPCM_DOFFSET_MASK);
	htol32_ua_store(swheader, frame + SDPCM_FRAMETAG_LEN);
	htol32_ua_store(0, frame + SDPCM_FRAMETAG_LEN + sizeof(swheader));
	}
	if (!TXCTLOK(bus)) {
		DHD_ERROR(("%s: No bus credit bus->tx_max %d, bus->tx_seq %d\n",
			__FUNCTION__, bus->tx_max, bus->tx_seq));
		bus->ctrl_frame_stat = TRUE;
		
		bus->ctrl_frame_buf = frame;
		bus->ctrl_frame_len = len;

		if (!bus->dpc_sched) {
			bus->dpc_sched = TRUE;
			dhd_sched_dpc(bus->dhd);
		}
		if (bus->ctrl_frame_stat) {
			dhd_wait_for_event(bus->dhd, &bus->ctrl_frame_stat);
		}

		if (bus->ctrl_frame_stat == FALSE) {
			DHD_ERROR(("%s: ctrl_frame_stat == FALSE\n", __FUNCTION__));
			ret = 0;
		} else {
			bus->dhd->txcnt_timeout++;
			if (!bus->dhd->hang_was_sent) {
#if defined(CUSTOMER_HW4)||defined(CUSTOMER_HW2)
				uint32 status, retry = 0;
				R_SDREG(status, &bus->regs->intstatus, retry);
				DHD_TRACE_HW4(("%s: txcnt_timeout, INT status=0x%08X\n",
					__FUNCTION__, status));
				DHD_TRACE_HW4(("%s : tx_max : %d, tx_seq : %d, clkstate : %d \n",
					__FUNCTION__, bus->tx_max, bus->tx_seq, bus->clkstate));
#endif 
				DHD_ERROR(("%s: ctrl_frame_stat == TRUE txcnt_timeout=%d\n",
					__FUNCTION__, bus->dhd->txcnt_timeout));
			}
			ret = -1;
			bus->ctrl_frame_stat = FALSE;
			goto done;
		}
	}

	bus->dhd->txcnt_timeout = 0;

	if (ret == -1) {
#ifdef DHD_DEBUG
		if (DHD_BYTES_ON() && DHD_CTL_ON()) {
			prhex("Tx Frame", frame, len);
		} else if (DHD_HDRS_ON()) {
			prhex("TxHdr", frame, MIN(len, 16));
		}
#endif

		do {
			ret = dhd_bcmsdh_send_buf(bus, bcmsdh_cur_sbwad(sdh), SDIO_FUNC_2, F2SYNC,
			                          frame, len, NULL, NULL, NULL);
			ASSERT(ret != BCME_PENDING);

			if (ret == BCME_NODEVICE) {
				DHD_ERROR(("%s: Device asleep already\n", __FUNCTION__));
			} else if (ret < 0) {
#if 1
			
			DHD_ERROR(("%s: sdio error %d, abort command and terminate frame 2.\n",
			__FUNCTION__, ret));
			bus->dhd->busstate = DHD_BUS_DOWN;
			dhd_os_send_hang_message(bus->dhd);
#else
			
				DHD_ERROR(("%s: sdio error %d, abort command and terminate frame.\n",
				          __FUNCTION__, ret));
				bus->tx_sderrs++;

				bcmsdh_abort(sdh, SDIO_FUNC_2);

				bcmsdh_cfg_write(sdh, SDIO_FUNC_1, SBSDIO_FUNC1_FRAMECTRL,
				                 SFC_WF_TERM, NULL);
				bus->f1regdata++;

				for (i = 0; i < 3; i++) {
					uint8 hi, lo;
					hi = bcmsdh_cfg_read(sdh, SDIO_FUNC_1,
					                     SBSDIO_FUNC1_WFRAMEBCHI, NULL);
					lo = bcmsdh_cfg_read(sdh, SDIO_FUNC_1,
					                     SBSDIO_FUNC1_WFRAMEBCLO, NULL);
					bus->f1regdata += 2;
					if ((hi == 0) && (lo == 0))
						break;
				}
#endif
			}
			if (ret == 0) {
				bus->tx_seq = (bus->tx_seq + 1) % SDPCM_SEQUENCE_WRAP;
			}
		} while ((ret < 0) && retries++ < TXRETRIES);
	}

done:
	if ((bus->idletime == DHD_IDLE_IMMEDIATE) && !bus->dpc_sched) {
		bus->activity = FALSE;
		dhdsdio_clkctl(bus, CLK_NONE, TRUE);
	}

	dhd_os_sdunlock(bus->dhd);

	if (ret)
		bus->dhd->tx_ctlerrs++;
	else
		bus->dhd->tx_ctlpkts++;

	if (ret) {
		bus_txctl_failed_num++;
		DHD_ERROR(("%s: bus_txctl_failed_num = %d\n", __func__,bus_txctl_failed_num));

		
		if (bus_txctl_failed_num >= 3) {
			bus_txctl_failed_num = 0;
			DHD_ERROR(("%s: Event HANG sent up\n", __func__));
			dhd_info_send_hang_message(bus->dhd);
		}
	} else
		bus_txctl_failed_num = 0;

	if (bus->dhd->txcnt_timeout >= max_cntl_timeout) 
		return -ETIMEDOUT;

	return ret ? -EIO : 0;
}

int
dhd_bus_rxctl(struct dhd_bus *bus, uchar *msg, uint msglen)
{
	int timeleft;
	uint rxlen = 0;
	bool pending;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	if (bus->dhd->dongle_reset)
		return -EIO;

	
	timeleft = dhd_os_ioctl_resp_wait(bus->dhd, &bus->rxlen, &pending);

	dhd_os_sdlock(bus->dhd);
	rxlen = bus->rxlen;
	bcopy(bus->rxctl, msg, MIN(msglen, rxlen));
	bus->rxlen = 0;
	dhd_os_sdunlock(bus->dhd);

	if (rxlen) {
		DHD_CTL(("%s: resumed on rxctl frame, got %d expected %d\n",
		         __FUNCTION__, rxlen, msglen));
	} else if (timeleft == 0) {
		u32 status;
		int retry = 0;
		R_SDREG(status, &bus->regs->intstatus, retry);
		DHD_ERROR(("%s: resumed on timeout, INT status=0x%08X\n", __FUNCTION__, status));
        
        if (status) {
            printf("%s : [status = 0x%08X]If there is something, make like the ISR and schedule the DPC\n",__func__ ,status);
            bus->dpc_sched = TRUE;
            dhd_sched_dpc(bus->dhd);
        }
#ifdef DHD_DEBUG
			dhd_os_sdlock(bus->dhd);
			dhdsdio_checkdied(bus, NULL, 0);
			dhd_os_sdunlock(bus->dhd);
#endif 
	} else if (pending == TRUE) {
		DHD_ERROR(("%s: signal pending\n", __FUNCTION__));
		return -ERESTARTSYS;
	} else {
		DHD_CTL(("%s: resumed for unknown reason?\n", __FUNCTION__));
#ifdef DHD_DEBUG
		dhd_os_sdlock(bus->dhd);
		dhdsdio_checkdied(bus, NULL, 0);
		dhd_os_sdunlock(bus->dhd);
#endif 
	}
	if (timeleft == 0) {
		bus->dhd->rxcnt_timeout++;
		DHD_ERROR(("%s: rxcnt_timeout=%d\n", __FUNCTION__, bus->dhd->rxcnt_timeout));
	}
	else
		bus->dhd->rxcnt_timeout = 0;

	if (rxlen)
		bus->dhd->rx_ctlpkts++;
	else
		bus->dhd->rx_ctlerrs++;

	if (bus->dhd->rxcnt_timeout >= max_cntl_timeout) 
		return -ETIMEDOUT;

	if (bus->dhd->dongle_trap_occured)
		return -EREMOTEIO;

	return rxlen ? (int)rxlen : -EIO;
}

enum {
	IOV_INTR = 1,
	IOV_POLLRATE,
	IOV_SDREG,
	IOV_SBREG,
	IOV_SDCIS,
	IOV_MEMBYTES,
	IOV_MEMSIZE,
#ifdef DHD_DEBUG
	IOV_CHECKDIED,
	IOV_SERIALCONS,
#endif 
	IOV_SET_DOWNLOAD_STATE,
	IOV_SOCRAM_STATE,
	IOV_FORCEEVEN,
	IOV_SDIOD_DRIVE,
	IOV_READAHEAD,
	IOV_SDRXCHAIN,
	IOV_ALIGNCTL,
	IOV_SDALIGN,
	IOV_DEVRESET,
	IOV_CPU,
#ifdef SDIO_CRC_ERROR_FIX
	IOV_WATERMARK,
	IOV_MESBUSYCTRL,
#endif 
#ifdef SDTEST
	IOV_PKTGEN,
	IOV_EXTLOOP,
#endif 
	IOV_SPROM,
	IOV_TXBOUND,
	IOV_RXBOUND,
	IOV_TXMINMAX,
	IOV_IDLETIME,
	IOV_IDLECLOCK,
	IOV_SD1IDLE,
	IOV_SLEEP,
	IOV_DONGLEISOLATION,
	IOV_KSO,
	IOV_DEVSLEEP,
	IOV_DEVCAP,
	IOV_VARS,
#ifdef SOFTAP
	IOV_FWPATH,
#endif
	IOV_TXGLOMSIZE,
	IOV_TXGLOMMODE
};

const bcm_iovar_t dhdsdio_iovars[] = {
	{"intr",	IOV_INTR,	0,	IOVT_BOOL,	0 },
	{"sleep",	IOV_SLEEP,	0,	IOVT_BOOL,	0 },
	{"pollrate",	IOV_POLLRATE,	0,	IOVT_UINT32,	0 },
	{"idletime",	IOV_IDLETIME,	0,	IOVT_INT32,	0 },
	{"idleclock",	IOV_IDLECLOCK,	0,	IOVT_INT32,	0 },
	{"sd1idle",	IOV_SD1IDLE,	0,	IOVT_BOOL,	0 },
	{"membytes",	IOV_MEMBYTES,	0,	IOVT_BUFFER,	2 * sizeof(int) },
	{"memsize",	IOV_MEMSIZE,	0,	IOVT_UINT32,	0 },
	{"dwnldstate",	IOV_SET_DOWNLOAD_STATE,	0,	IOVT_BOOL,	0 },
	{"socram_state",	IOV_SOCRAM_STATE,	0,	IOVT_BOOL,	0 },
	{"vars",	IOV_VARS,	0,	IOVT_BUFFER,	0 },
	{"sdiod_drive",	IOV_SDIOD_DRIVE, 0,	IOVT_UINT32,	0 },
	{"readahead",	IOV_READAHEAD,	0,	IOVT_BOOL,	0 },
	{"sdrxchain",	IOV_SDRXCHAIN,	0,	IOVT_BOOL,	0 },
	{"alignctl",	IOV_ALIGNCTL,	0,	IOVT_BOOL,	0 },
	{"sdalign",	IOV_SDALIGN,	0,	IOVT_BOOL,	0 },
	{"devreset",	IOV_DEVRESET,	0,	IOVT_BOOL,	0 },
#ifdef DHD_DEBUG
	{"sdreg",	IOV_SDREG,	0,	IOVT_BUFFER,	sizeof(sdreg_t) },
	{"sbreg",	IOV_SBREG,	0,	IOVT_BUFFER,	sizeof(sdreg_t) },
	{"sd_cis",	IOV_SDCIS,	0,	IOVT_BUFFER,	DHD_IOCTL_MAXLEN },
	{"forcealign",	IOV_FORCEEVEN,	0,	IOVT_BOOL,	0 },
	{"txbound",	IOV_TXBOUND,	0,	IOVT_UINT32,	0 },
	{"rxbound",	IOV_RXBOUND,	0,	IOVT_UINT32,	0 },
	{"txminmax",	IOV_TXMINMAX,	0,	IOVT_UINT32,	0 },
	{"cpu",		IOV_CPU,	0,	IOVT_BOOL,	0 },
#ifdef DHD_DEBUG
	{"checkdied",	IOV_CHECKDIED,	0,	IOVT_BUFFER,	0 },
	{"serial",	IOV_SERIALCONS,	0,	IOVT_UINT32,	0 },
#endif 
#endif 
#ifdef SDTEST
	{"extloop",	IOV_EXTLOOP,	0,	IOVT_BOOL,	0 },
	{"pktgen",	IOV_PKTGEN,	0,	IOVT_BUFFER,	sizeof(dhd_pktgen_t) },
#ifdef SDIO_CRC_ERROR_FIX
	{"watermark",	IOV_WATERMARK,	0,	IOVT_UINT32,	0 },
	{"mesbusyctrl",	IOV_MESBUSYCTRL,	0,	IOVT_UINT32,	0 },
#endif 
#endif 
	{"devcap", IOV_DEVCAP,	0,	IOVT_UINT32,	0 },
	{"dngl_isolation", IOV_DONGLEISOLATION,	0,	IOVT_UINT32,	0 },
	{"kso",	IOV_KSO,	0,	IOVT_UINT32,	0 },
	{"devsleep", IOV_DEVSLEEP,	0,	IOVT_UINT32,	0 },
#ifdef SOFTAP
	{"fwpath", IOV_FWPATH, 0, IOVT_BUFFER, 0 },
#endif
	{"txglomsize", IOV_TXGLOMSIZE, 0, IOVT_UINT32, 0 },
	{"txglommode", IOV_TXGLOMMODE, 0, IOVT_UINT32, 0 },
	{NULL, 0, 0, 0, 0 }
};

static void
dhd_dump_pct(struct bcmstrbuf *strbuf, char *desc, uint num, uint div)
{
	uint q1, q2;

	if (!div) {
		bcm_bprintf(strbuf, "%s N/A", desc);
	} else {
		q1 = num / div;
		q2 = (100 * (num - (q1 * div))) / div;
		bcm_bprintf(strbuf, "%s %d.%02d", desc, q1, q2);
	}
}

void
dhd_bus_dump(dhd_pub_t *dhdp, struct bcmstrbuf *strbuf)
{
	dhd_bus_t *bus = dhdp->bus;

	bcm_bprintf(strbuf, "Bus SDIO structure:\n");
	bcm_bprintf(strbuf, "hostintmask 0x%08x intstatus 0x%08x sdpcm_ver %d\n",
	            bus->hostintmask, bus->intstatus, bus->sdpcm_ver);
	bcm_bprintf(strbuf, "fcstate %d qlen %d tx_seq %d, max %d, rxskip %d rxlen %d rx_seq %d\n",
	            bus->fcstate, pktq_len(&bus->txq), bus->tx_seq, bus->tx_max, bus->rxskip,
	            bus->rxlen, bus->rx_seq);
	bcm_bprintf(strbuf, "intr %d intrcount %d lastintrs %d spurious %d\n",
	            bus->intr, bus->intrcount, bus->lastintrs, bus->spurious);
	bcm_bprintf(strbuf, "pollrate %d pollcnt %d regfails %d\n",
	            bus->pollrate, bus->pollcnt, bus->regfails);

	bcm_bprintf(strbuf, "\nAdditional counters:\n");
	bcm_bprintf(strbuf, "tx_sderrs %d fcqueued %d rxrtx %d rx_toolong %d rxc_errors %d\n",
	            bus->tx_sderrs, bus->fcqueued, bus->rxrtx, bus->rx_toolong,
	            bus->rxc_errors);
	bcm_bprintf(strbuf, "rx_hdrfail %d badhdr %d badseq %d\n",
	            bus->rx_hdrfail, bus->rx_badhdr, bus->rx_badseq);
	bcm_bprintf(strbuf, "fc_rcvd %d, fc_xoff %d, fc_xon %d\n",
	            bus->fc_rcvd, bus->fc_xoff, bus->fc_xon);
	bcm_bprintf(strbuf, "rxglomfail %d, rxglomframes %d, rxglompkts %d\n",
	            bus->rxglomfail, bus->rxglomframes, bus->rxglompkts);
	bcm_bprintf(strbuf, "f2rx (hdrs/data) %d (%d/%d), f2tx %d f1regs %d\n",
	            (bus->f2rxhdrs + bus->f2rxdata), bus->f2rxhdrs, bus->f2rxdata,
	            bus->f2txdata, bus->f1regdata);
	{
		dhd_dump_pct(strbuf, "\nRx: pkts/f2rd", bus->dhd->rx_packets,
		             (bus->f2rxhdrs + bus->f2rxdata));
		dhd_dump_pct(strbuf, ", pkts/f1sd", bus->dhd->rx_packets, bus->f1regdata);
		dhd_dump_pct(strbuf, ", pkts/sd", bus->dhd->rx_packets,
		             (bus->f2rxhdrs + bus->f2rxdata + bus->f1regdata));
		dhd_dump_pct(strbuf, ", pkts/int", bus->dhd->rx_packets, bus->intrcount);
		bcm_bprintf(strbuf, "\n");

		dhd_dump_pct(strbuf, "Rx: glom pct", (100 * bus->rxglompkts),
		             bus->dhd->rx_packets);
		dhd_dump_pct(strbuf, ", pkts/glom", bus->rxglompkts, bus->rxglomframes);
		bcm_bprintf(strbuf, "\n");

		dhd_dump_pct(strbuf, "Tx: pkts/f2wr", bus->dhd->tx_packets, bus->f2txdata);
		dhd_dump_pct(strbuf, ", pkts/f1sd", bus->dhd->tx_packets, bus->f1regdata);
		dhd_dump_pct(strbuf, ", pkts/sd", bus->dhd->tx_packets,
		             (bus->f2txdata + bus->f1regdata));
		dhd_dump_pct(strbuf, ", pkts/int", bus->dhd->tx_packets, bus->intrcount);
		bcm_bprintf(strbuf, "\n");

		dhd_dump_pct(strbuf, "Total: pkts/f2rw",
		             (bus->dhd->tx_packets + bus->dhd->rx_packets),
		             (bus->f2txdata + bus->f2rxhdrs + bus->f2rxdata));
		dhd_dump_pct(strbuf, ", pkts/f1sd",
		             (bus->dhd->tx_packets + bus->dhd->rx_packets), bus->f1regdata);
		dhd_dump_pct(strbuf, ", pkts/sd",
		             (bus->dhd->tx_packets + bus->dhd->rx_packets),
		             (bus->f2txdata + bus->f2rxhdrs + bus->f2rxdata + bus->f1regdata));
		dhd_dump_pct(strbuf, ", pkts/int",
		             (bus->dhd->tx_packets + bus->dhd->rx_packets), bus->intrcount);
		bcm_bprintf(strbuf, "\n\n");
	}

#ifdef SDTEST
	if (bus->pktgen_count) {
		bcm_bprintf(strbuf, "pktgen config and count:\n");
		bcm_bprintf(strbuf, "freq %d count %d print %d total %d min %d len %d\n",
		            bus->pktgen_freq, bus->pktgen_count, bus->pktgen_print,
		            bus->pktgen_total, bus->pktgen_minlen, bus->pktgen_maxlen);
		bcm_bprintf(strbuf, "send attempts %d rcvd %d fail %d\n",
		            bus->pktgen_sent, bus->pktgen_rcvd, bus->pktgen_fail);
	}
#endif 
#ifdef DHD_DEBUG
	bcm_bprintf(strbuf, "dpc_sched %d host interrupt%spending\n",
	            bus->dpc_sched, (bcmsdh_intr_pending(bus->sdh) ? " " : " not "));
	bcm_bprintf(strbuf, "blocksize %d roundup %d\n", bus->blocksize, bus->roundup);
#endif 
	bcm_bprintf(strbuf, "clkstate %d activity %d idletime %d idlecount %d sleeping %d\n",
	            bus->clkstate, bus->activity, bus->idletime, bus->idlecount, bus->sleeping);
}

void
dhd_bus_clearcounts(dhd_pub_t *dhdp)
{
	dhd_bus_t *bus = (dhd_bus_t *)dhdp->bus;

	bus->intrcount = bus->lastintrs = bus->spurious = bus->regfails = 0;
	bus->rxrtx = bus->rx_toolong = bus->rxc_errors = 0;
	bus->rx_hdrfail = bus->rx_badhdr = bus->rx_badseq = 0;
	bus->tx_sderrs = bus->fc_rcvd = bus->fc_xoff = bus->fc_xon = 0;
	bus->rxglomfail = bus->rxglomframes = bus->rxglompkts = 0;
	bus->f2rxhdrs = bus->f2rxdata = bus->f2txdata = bus->f1regdata = 0;
}

#ifdef SDTEST
static int
dhdsdio_pktgen_get(dhd_bus_t *bus, uint8 *arg)
{
	dhd_pktgen_t pktgen;

	pktgen.version = DHD_PKTGEN_VERSION;
	pktgen.freq = bus->pktgen_freq;
	pktgen.count = bus->pktgen_count;
	pktgen.print = bus->pktgen_print;
	pktgen.total = bus->pktgen_total;
	pktgen.minlen = bus->pktgen_minlen;
	pktgen.maxlen = bus->pktgen_maxlen;
	pktgen.numsent = bus->pktgen_sent;
	pktgen.numrcvd = bus->pktgen_rcvd;
	pktgen.numfail = bus->pktgen_fail;
	pktgen.mode = bus->pktgen_mode;
	pktgen.stop = bus->pktgen_stop;

	bcopy(&pktgen, arg, sizeof(pktgen));

	return 0;
}

static int
dhdsdio_pktgen_set(dhd_bus_t *bus, uint8 *arg)
{
	dhd_pktgen_t pktgen;
	uint oldcnt, oldmode;

	bcopy(arg, &pktgen, sizeof(pktgen));
	if (pktgen.version != DHD_PKTGEN_VERSION)
		return BCME_BADARG;

	oldcnt = bus->pktgen_count;
	oldmode = bus->pktgen_mode;

	bus->pktgen_freq = pktgen.freq;
	bus->pktgen_count = pktgen.count;
	bus->pktgen_print = pktgen.print;
	bus->pktgen_total = pktgen.total;
	bus->pktgen_minlen = pktgen.minlen;
	bus->pktgen_maxlen = pktgen.maxlen;
	bus->pktgen_mode = pktgen.mode;
	bus->pktgen_stop = pktgen.stop;

	bus->pktgen_tick = bus->pktgen_ptick = 0;
	bus->pktgen_prev_time = jiffies;
	bus->pktgen_len = MAX(bus->pktgen_len, bus->pktgen_minlen);
	bus->pktgen_len = MIN(bus->pktgen_len, bus->pktgen_maxlen);

	
	if (bus->pktgen_count && (!oldcnt || oldmode != bus->pktgen_mode)) {
		bus->pktgen_sent = bus->pktgen_prev_sent = bus->pktgen_rcvd = 0;
		bus->pktgen_prev_rcvd = bus->pktgen_fail = 0;
	}

	return 0;
}
#endif 

static void
dhdsdio_devram_remap(dhd_bus_t *bus, bool val)
{
	uint8 enable, protect, remap;

	si_socdevram(bus->sih, FALSE, &enable, &protect, &remap);
	remap = val ? TRUE : FALSE;
	si_socdevram(bus->sih, TRUE, &enable, &protect, &remap);
}

static int
dhdsdio_membytes(dhd_bus_t *bus, bool write, uint32 address, uint8 *data, uint size)
{
	int bcmerror = 0;
	uint32 sdaddr;
	uint dsize;

	if (REMAP_ENAB(bus) && REMAP_ISADDR(bus, address)) {
		address -= bus->orig_ramsize;
		address += SOCDEVRAM_BP_ADDR;
	}

	
	sdaddr = address & SBSDIO_SB_OFT_ADDR_MASK;
	if ((sdaddr + size) & SBSDIO_SBWINDOW_MASK)
		dsize = (SBSDIO_SB_OFT_ADDR_LIMIT - sdaddr);
	else
		dsize = size;

	
	if ((bcmerror = dhdsdio_set_siaddr_window(bus, address))) {
		DHD_ERROR(("%s: window change failed\n", __FUNCTION__));
		goto xfer_done;
	}

	
	while (size) {
		DHD_INFO(("%s: %s %d bytes at offset 0x%08x in window 0x%08x\n",
		          __FUNCTION__, (write ? "write" : "read"), dsize, sdaddr,
		          (address & SBSDIO_SBWINDOW_MASK)));
		if ((bcmerror = bcmsdh_rwdata(bus->sdh, write, sdaddr, data, dsize))) {
			DHD_ERROR(("%s: membytes transfer failed\n", __FUNCTION__));
			break;
		}

		
		if ((size -= dsize)) {
			data += dsize;
			address += dsize;
			if ((bcmerror = dhdsdio_set_siaddr_window(bus, address))) {
				DHD_ERROR(("%s: window change failed\n", __FUNCTION__));
				break;
			}
			sdaddr = 0;
			dsize = MIN(SBSDIO_SB_OFT_ADDR_LIMIT, size);
		}

	}

xfer_done:
	
	if (dhdsdio_set_siaddr_window(bus, bcmsdh_cur_sbwad(bus->sdh))) {
		DHD_ERROR(("%s: FAILED to set window back to 0x%x\n", __FUNCTION__,
			bcmsdh_cur_sbwad(bus->sdh)));
	}

	return bcmerror;
}

#ifdef DHD_DEBUG
static int
dhdsdio_readshared(dhd_bus_t *bus, sdpcm_shared_t *sh)
{
	uint32 addr;
	int rv, i;
	uint32 shaddr = 0;

	shaddr = bus->dongle_ram_base + bus->ramsize - 4;
	i = 0;
	do {
		
		if ((rv = dhdsdio_membytes(bus, FALSE, shaddr, (uint8 *)&addr, 4)) < 0)
			return rv;

		addr = ltoh32(addr);

		DHD_INFO(("sdpcm_shared address 0x%08X\n", addr));

		/*
		 * Check if addr is valid.
		 * NVRAM length at the end of memory should have been overwritten.
		 */
		if (addr == 0 || ((~addr >> 16) & 0xffff) == (addr & 0xffff)) {
			if ((bus->srmemsize > 0) && (i++ == 0)) {
				shaddr -= bus->srmemsize;
			} else {
				DHD_ERROR(("%s: address (0x%08x) of sdpcm_shared invalid\n",
					__FUNCTION__, addr));
				return BCME_ERROR;
			}
		} else
			break;
	} while (i < 2);

	
	if ((rv = dhdsdio_membytes(bus, FALSE, addr, (uint8 *)sh, sizeof(sdpcm_shared_t))) < 0)
		return rv;

	
	sh->flags = ltoh32(sh->flags);
	sh->trap_addr = ltoh32(sh->trap_addr);
	sh->assert_exp_addr = ltoh32(sh->assert_exp_addr);
	sh->assert_file_addr = ltoh32(sh->assert_file_addr);
	sh->assert_line = ltoh32(sh->assert_line);
	sh->console_addr = ltoh32(sh->console_addr);
	sh->msgtrace_addr = ltoh32(sh->msgtrace_addr);

	if ((sh->flags & SDPCM_SHARED_VERSION_MASK) == 3 && SDPCM_SHARED_VERSION == 1)
		return BCME_OK;

	if ((sh->flags & SDPCM_SHARED_VERSION_MASK) != SDPCM_SHARED_VERSION) {
		DHD_ERROR(("%s: sdpcm_shared version %d in dhd "
		           "is different than sdpcm_shared version %d in dongle\n",
		           __FUNCTION__, SDPCM_SHARED_VERSION,
		           sh->flags & SDPCM_SHARED_VERSION_MASK));
		return BCME_ERROR;
	}

	return BCME_OK;
}

#define CONSOLE_LINE_MAX	192

static int
dhdsdio_readconsole(dhd_bus_t *bus)
{
	dhd_console_t *c = &bus->console;
	uint8 line[CONSOLE_LINE_MAX], ch;
	uint32 n, idx, addr;
	int rv;

	
	if (bus->console_addr == 0)
		return 0;

	if (!KSO_ENAB(bus))
		return 0;

	
	addr = bus->console_addr + OFFSETOF(hndrte_cons_t, log);
	if ((rv = dhdsdio_membytes(bus, FALSE, addr, (uint8 *)&c->log, sizeof(c->log))) < 0)
		return rv;

	
	if (c->buf == NULL) {
		c->bufsize = ltoh32(c->log.buf_size);
		if ((c->buf = MALLOC(bus->dhd->osh, c->bufsize)) == NULL)
			return BCME_NOMEM;
	}

	idx = ltoh32(c->log.idx);

	
	if (idx > c->bufsize)
		return BCME_ERROR;

	
	if (idx == c->last)
		return BCME_OK;

	
	addr = ltoh32(c->log.buf);
	if ((rv = dhdsdio_membytes(bus, FALSE, addr, c->buf, c->bufsize)) < 0)
		return rv;

	while (c->last != idx) {
		for (n = 0; n < CONSOLE_LINE_MAX - 2; n++) {
			if (c->last == idx) {
				if (c->last >= n)
					c->last -= n;
				else
					c->last = c->bufsize - n;
				goto break2;
			}
			ch = c->buf[c->last];
			c->last = (c->last + 1) % c->bufsize;
			if (ch == '\n')
				break;
			line[n] = ch;
		}

		if (n > 0) {
			if (line[n - 1] == '\r')
				n--;
			line[n] = 0;
			printf("CONSOLE: %s\n", line);
		}
	}
break2:

	return BCME_OK;
}

static int
dhdsdio_checkdied(dhd_bus_t *bus, char *data, uint size)
{
	int bcmerror = 0;
	uint msize = 512;
	char *mbuffer = NULL;
	char *console_buffer = NULL;
	uint maxstrlen = 256;
	char *str = NULL;
	trap_t tr;
	sdpcm_shared_t sdpcm_shared;
	struct bcmstrbuf strbuf;
	uint32 console_ptr, console_size, console_index;
	uint8 line[CONSOLE_LINE_MAX], ch;
	uint32 n, i, addr;
	int rv;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	if (DHD_NOCHECKDIED_ON())
		return 0;

	if (data == NULL) {
		size = msize;
		mbuffer = data = MALLOC(bus->dhd->osh, msize);
		if (mbuffer == NULL) {
			DHD_ERROR(("%s: MALLOC(%d) failed \n", __FUNCTION__, msize));
			bcmerror = BCME_NOMEM;
			goto done;
		}
	}

	if ((str = MALLOC(bus->dhd->osh, maxstrlen)) == NULL) {
		DHD_ERROR(("%s: MALLOC(%d) failed \n", __FUNCTION__, maxstrlen));
		bcmerror = BCME_NOMEM;
		goto done;
	}

	if ((bcmerror = dhdsdio_readshared(bus, &sdpcm_shared)) < 0)
		goto done;

	bcm_binit(&strbuf, data, size);

	bcm_bprintf(&strbuf, "msgtrace address : 0x%08X\nconsole address  : 0x%08X\n",
	            sdpcm_shared.msgtrace_addr, sdpcm_shared.console_addr);

	if ((sdpcm_shared.flags & SDPCM_SHARED_ASSERT_BUILT) == 0) {
		bcm_bprintf(&strbuf, "Assrt not built in dongle\n");
	}

	if ((sdpcm_shared.flags & (SDPCM_SHARED_ASSERT|SDPCM_SHARED_TRAP)) == 0) {
		bcm_bprintf(&strbuf, "No trap%s in dongle",
		          (sdpcm_shared.flags & SDPCM_SHARED_ASSERT_BUILT)
		          ?"/assrt" :"");
	} else {
		if (sdpcm_shared.flags & SDPCM_SHARED_ASSERT) {
			
			bcm_bprintf(&strbuf, "Dongle assert");
			if (sdpcm_shared.assert_exp_addr != 0) {
				str[0] = '\0';
				if ((bcmerror = dhdsdio_membytes(bus, FALSE,
				                                 sdpcm_shared.assert_exp_addr,
				                                 (uint8 *)str, maxstrlen)) < 0)
					goto done;

				str[maxstrlen - 1] = '\0';
				bcm_bprintf(&strbuf, " expr \"%s\"", str);
			}

			if (sdpcm_shared.assert_file_addr != 0) {
				str[0] = '\0';
				if ((bcmerror = dhdsdio_membytes(bus, FALSE,
				                                 sdpcm_shared.assert_file_addr,
				                                 (uint8 *)str, maxstrlen)) < 0)
					goto done;

				str[maxstrlen - 1] = '\0';
				bcm_bprintf(&strbuf, " file \"%s\"", str);
			}

			bcm_bprintf(&strbuf, " line %d ", sdpcm_shared.assert_line);
		}

		if (sdpcm_shared.flags & SDPCM_SHARED_TRAP) {
			bus->dhd->dongle_trap_occured = TRUE;
			if ((bcmerror = dhdsdio_membytes(bus, FALSE,
			                                 sdpcm_shared.trap_addr,
			                                 (uint8*)&tr, sizeof(trap_t))) < 0)
				goto done;

			bcm_bprintf(&strbuf,
			"Dongle trap type 0x%x @ epc 0x%x, cpsr 0x%x, spsr 0x%x, sp 0x%x,"
			            "lp 0x%x, rpc 0x%x Trap offset 0x%x, "
			"r0 0x%x, r1 0x%x, r2 0x%x, r3 0x%x, "
			"r4 0x%x, r5 0x%x, r6 0x%x, r7 0x%x\n\n",
			ltoh32(tr.type), ltoh32(tr.epc), ltoh32(tr.cpsr), ltoh32(tr.spsr),
			ltoh32(tr.r13), ltoh32(tr.r14), ltoh32(tr.pc),
			ltoh32(sdpcm_shared.trap_addr),
			ltoh32(tr.r0), ltoh32(tr.r1), ltoh32(tr.r2), ltoh32(tr.r3),
			ltoh32(tr.r4), ltoh32(tr.r5), ltoh32(tr.r6), ltoh32(tr.r7));

			addr = sdpcm_shared.console_addr + OFFSETOF(hndrte_cons_t, log);
			if ((rv = dhdsdio_membytes(bus, FALSE, addr,
				(uint8 *)&console_ptr, sizeof(console_ptr))) < 0)
				goto printbuf;

			addr = sdpcm_shared.console_addr + OFFSETOF(hndrte_cons_t, log.buf_size);
			if ((rv = dhdsdio_membytes(bus, FALSE, addr,
				(uint8 *)&console_size, sizeof(console_size))) < 0)
				goto printbuf;

			addr = sdpcm_shared.console_addr + OFFSETOF(hndrte_cons_t, log.idx);
			if ((rv = dhdsdio_membytes(bus, FALSE, addr,
				(uint8 *)&console_index, sizeof(console_index))) < 0)
				goto printbuf;

			console_ptr = ltoh32(console_ptr);
			console_size = ltoh32(console_size);
			console_index = ltoh32(console_index);

			if (console_size > CONSOLE_BUFFER_MAX ||
				!(console_buffer = MALLOC(bus->dhd->osh, console_size)))
				goto printbuf;

			if ((rv = dhdsdio_membytes(bus, FALSE, console_ptr,
				(uint8 *)console_buffer, console_size)) < 0)
				goto printbuf;

			for (i = 0, n = 0; i < console_size; i += n + 1) {
				for (n = 0; n < CONSOLE_LINE_MAX - 2; n++) {
					ch = console_buffer[(console_index + i + n) % console_size];
					if (ch == '\n')
						break;
					line[n] = ch;
				}


				if (n > 0) {
					if (line[n - 1] == '\r')
						n--;
					line[n] = 0;

					if (dhd_msg_level & DHD_ERROR_VAL)
						printf("CONSOLE: %s\n", line);
				}
			}
		}
	}

printbuf:
	if (sdpcm_shared.flags & (SDPCM_SHARED_ASSERT | SDPCM_SHARED_TRAP)) {
		DHD_ERROR(("%s: %s\n", __FUNCTION__, strbuf.origbuf));
	}


done:
	if (mbuffer)
		MFREE(bus->dhd->osh, mbuffer, msize);
	if (str)
		MFREE(bus->dhd->osh, str, maxstrlen);
	if (console_buffer)
		MFREE(bus->dhd->osh, console_buffer, console_size);

	return bcmerror;
}
#endif 


int
dhdsdio_downloadvars(dhd_bus_t *bus, void *arg, int len)
{
	int bcmerror = BCME_OK;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	
	if (bus->dhd->up) {
		bcmerror = BCME_NOTDOWN;
		goto err;
	}
	if (!len) {
		bcmerror = BCME_BUFTOOSHORT;
		goto err;
	}

	
	if (bus->vars)
		MFREE(bus->dhd->osh, bus->vars, bus->varsz);

	bus->vars = MALLOC(bus->dhd->osh, len);
	bus->varsz = bus->vars ? len : 0;
	if (bus->vars == NULL) {
		bcmerror = BCME_NOMEM;
		goto err;
	}

	
	bcopy(arg, bus->vars, bus->varsz);
err:
	return bcmerror;
}

#ifdef DHD_DEBUG

#define CC_PLL_CHIPCTRL_SERIAL_ENAB		(1  << 24)
#define CC_CHIPCTRL_JTAG_SEL			(1  << 3)
#define CC_CHIPCTRL_GPIO_SEL				(0x3)
#define CC_PLL_CHIPCTRL_SERIAL_ENAB_4334	(1  << 28)

static int
dhd_serialconsole(dhd_bus_t *bus, bool set, bool enable, int *bcmerror)
{
	int int_val;
	uint32 addr, data, uart_enab = 0;
	uint32 jtag_sel = CC_CHIPCTRL_JTAG_SEL;
	uint32 gpio_sel = CC_CHIPCTRL_GPIO_SEL;

	addr = SI_ENUM_BASE + OFFSETOF(chipcregs_t, chipcontrol_addr);
	data = SI_ENUM_BASE + OFFSETOF(chipcregs_t, chipcontrol_data);
	*bcmerror = 0;

	bcmsdh_reg_write(bus->sdh, addr, 4, 1);
	if (bcmsdh_regfail(bus->sdh)) {
		*bcmerror = BCME_SDIO_ERROR;
		return -1;
	}
	int_val = bcmsdh_reg_read(bus->sdh, data, 4);
	if (bcmsdh_regfail(bus->sdh)) {
		*bcmerror = BCME_SDIO_ERROR;
		return -1;
	}
	if (bus->sih->chip == BCM4330_CHIP_ID) {
		uart_enab = CC_PLL_CHIPCTRL_SERIAL_ENAB;
	}
	else if (bus->sih->chip == BCM4334_CHIP_ID ||
		bus->sih->chip == BCM43341_CHIP_ID ||
		0) {
		if (enable) {
			
			int_val &= ~gpio_sel;
			int_val |= jtag_sel;
		} else {
			int_val |= gpio_sel;
			int_val &= ~jtag_sel;
		}
		uart_enab = CC_PLL_CHIPCTRL_SERIAL_ENAB_4334;
	}

	if (!set)
		return (int_val & uart_enab);
	if (enable)
		int_val |= uart_enab;
	else
		int_val &= ~uart_enab;
	bcmsdh_reg_write(bus->sdh, data, 4, int_val);
	if (bcmsdh_regfail(bus->sdh)) {
		*bcmerror = BCME_SDIO_ERROR;
		return -1;
	}
	if (bus->sih->chip == BCM4330_CHIP_ID) {
		uint32 chipcontrol;
		addr = SI_ENUM_BASE + OFFSETOF(chipcregs_t, chipcontrol);
		chipcontrol = bcmsdh_reg_read(bus->sdh, addr, 4);
		chipcontrol &= ~jtag_sel;
		if (enable) {
			chipcontrol |=  jtag_sel;
			chipcontrol &= ~gpio_sel;
		}
		bcmsdh_reg_write(bus->sdh, addr, 4, chipcontrol);
	}

	return (int_val & uart_enab);
}
#endif 

static int
dhdsdio_doiovar(dhd_bus_t *bus, const bcm_iovar_t *vi, uint32 actionid, const char *name,
                void *params, int plen, void *arg, int len, int val_size)
{
	int bcmerror = 0;
	int32 int_val = 0;
	bool bool_val = 0;

	
	
	DHD_ERROR(("%s: Enter, action %d name %s params %p plen %d arg %p len %d val_size %d\n",
	           __FUNCTION__, actionid, name, params, plen, arg, len, val_size));

	if ((bcmerror = bcm_iovar_lencheck(vi, arg, len, IOV_ISSET(actionid))) != 0)
		goto exit;

	if (plen >= (int)sizeof(int_val))
		bcopy(params, &int_val, sizeof(int_val));

	bool_val = (int_val != 0) ? TRUE : FALSE;


	
	dhd_os_sdlock(bus->dhd);

	
	if (bus->dhd->dongle_reset && !(actionid == IOV_SVAL(IOV_DEVRESET) ||
	                                actionid == IOV_GVAL(IOV_DEVRESET))) {
		bcmerror = BCME_NOTREADY;
		goto exit;
	}

	if ((vi->varid == IOV_KSO) && (IOV_ISSET(actionid))) {
		dhdsdio_clk_kso_iovar(bus, bool_val);
		goto exit;
	} else if ((vi->varid == IOV_DEVSLEEP) && (IOV_ISSET(actionid))) {
		{
			dhdsdio_clk_devsleep_iovar(bus, bool_val);
			if (!SLPAUTO_ENAB(bus) && (bool_val == FALSE) && (bus->ipend)) {
				DHD_ERROR(("INT pending in devsleep 1, dpc_sched: %d\n",
					bus->dpc_sched));
				if (!bus->dpc_sched) {
					bus->dpc_sched = TRUE;
					dhd_sched_dpc(bus->dhd);
				}
			}
		}
		goto exit;
	}

	
	if (vi->varid == IOV_SLEEP) {
		if (IOV_ISSET(actionid)) {
			bcmerror = dhdsdio_bussleep(bus, bool_val);
		} else {
			int_val = (int32)bus->sleeping;
			bcopy(&int_val, arg, val_size);
		}
		goto exit;
	}

	
	if (!bus->dhd->dongle_reset) {
		BUS_WAKE(bus);
		dhdsdio_clkctl(bus, CLK_AVAIL, FALSE);
	}

	switch (actionid) {
	case IOV_GVAL(IOV_INTR):
		int_val = (int32)bus->intr;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_INTR):
		bus->intr = bool_val;
		bus->intdis = FALSE;
		if (bus->dhd->up) {
			if (bus->intr) {
				DHD_INTR(("%s: enable SDIO device interrupts\n", __FUNCTION__));
				bcmsdh_intr_enable(bus->sdh);
			} else {
				DHD_INTR(("%s: disable SDIO interrupts\n", __FUNCTION__));
				bcmsdh_intr_disable(bus->sdh);
			}
		}
		break;

	case IOV_GVAL(IOV_POLLRATE):
		int_val = (int32)bus->pollrate;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_POLLRATE):
		bus->pollrate = (uint)int_val;
		bus->poll = (bus->pollrate != 0);
		break;

	case IOV_GVAL(IOV_IDLETIME):
		int_val = bus->idletime;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_IDLETIME):
		if ((int_val < 0) && (int_val != DHD_IDLE_IMMEDIATE)) {
			bcmerror = BCME_BADARG;
		} else {
			bus->idletime = int_val;
		}
		break;

	case IOV_GVAL(IOV_IDLECLOCK):
		int_val = (int32)bus->idleclock;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_IDLECLOCK):
		bus->idleclock = int_val;
		break;

	case IOV_GVAL(IOV_SD1IDLE):
		int_val = (int32)sd1idle;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_SD1IDLE):
		sd1idle = bool_val;
		break;


	case IOV_SVAL(IOV_MEMBYTES):
	case IOV_GVAL(IOV_MEMBYTES):
	{
		uint32 address;
		uint size, dsize;
		uint8 *data;

		bool set = (actionid == IOV_SVAL(IOV_MEMBYTES));

		ASSERT(plen >= 2*sizeof(int));

		address = (uint32)int_val;
		bcopy((char *)params + sizeof(int_val), &int_val, sizeof(int_val));
		size = (uint)int_val;

		
		dsize = set ? plen - (2 * sizeof(int)) : len;
		if (dsize < size) {
			DHD_ERROR(("%s: error on %s membytes, addr 0x%08x size %d dsize %d\n",
			           __FUNCTION__, (set ? "set" : "get"), address, size, dsize));
			bcmerror = BCME_BADARG;
			break;
		}

		DHD_INFO(("%s: Request to %s %d bytes at address 0x%08x\n", __FUNCTION__,
		          (set ? "write" : "read"), size, address));

		
		if (si_setcore(bus->sih, ARMCR4_CORE_ID, 0)) {
			/* if address is 0, store the reset instruction to be written in 0 */

			if (address == 0) {
				bus->resetinstr = *(((uint32*)params) + 2);
			}
			
			address += bus->dongle_ram_base;
		} else {
		
		if ((bus->orig_ramsize) &&
		    ((address > bus->orig_ramsize) || (address + size > bus->orig_ramsize)))
		{
			uint8 enable, protect, remap;
			si_socdevram(bus->sih, FALSE, &enable, &protect, &remap);
			if (!enable || protect) {
				DHD_ERROR(("%s: ramsize 0x%08x doesn't have %d bytes at 0x%08x\n",
					__FUNCTION__, bus->orig_ramsize, size, address));
				DHD_ERROR(("%s: socram enable %d, protect %d\n",
					__FUNCTION__, enable, protect));
				bcmerror = BCME_BADARG;
				break;
			}

			if (!REMAP_ENAB(bus) && (address >= SOCDEVRAM_ARM_ADDR)) {
				uint32 devramsize = si_socdevram_size(bus->sih);
				if ((address < SOCDEVRAM_ARM_ADDR) ||
					(address + size > (SOCDEVRAM_ARM_ADDR + devramsize))) {
					DHD_ERROR(("%s: bad address 0x%08x, size 0x%08x\n",
						__FUNCTION__, address, size));
					DHD_ERROR(("%s: socram range 0x%08x,size 0x%08x\n",
						__FUNCTION__, SOCDEVRAM_ARM_ADDR, devramsize));
					bcmerror = BCME_BADARG;
					break;
				}
				
				address -= SOCDEVRAM_ARM_ADDR;
				address += SOCDEVRAM_BP_ADDR;
				DHD_INFO(("%s: Request to %s %d bytes @ Mapped address 0x%08x\n",
					__FUNCTION__, (set ? "write" : "read"), size, address));
			} else if (REMAP_ENAB(bus) && REMAP_ISADDR(bus, address) && remap) {
				DHD_ERROR(("%s: Need to disable remap for address 0x%08x\n",
					__FUNCTION__, address));
				bcmerror = BCME_ERROR;
				break;
			}
		}
		}

		
		data = set ? (uint8*)params + 2 * sizeof(int): (uint8*)arg;

		
		bcmerror = dhdsdio_membytes(bus, set, address, data, size);

		break;
	}

	case IOV_GVAL(IOV_MEMSIZE):
		int_val = (int32)bus->ramsize;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_GVAL(IOV_SDIOD_DRIVE):
		int_val = (int32)dhd_sdiod_drive_strength;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_SDIOD_DRIVE):
		dhd_sdiod_drive_strength = int_val;
		si_sdiod_drive_strength_init(bus->sih, bus->dhd->osh, dhd_sdiod_drive_strength);
		break;

	case IOV_SVAL(IOV_SET_DOWNLOAD_STATE):
		bcmerror = dhdsdio_download_state(bus, bool_val);
		break;

	case IOV_SVAL(IOV_SOCRAM_STATE):
		bcmerror = dhdsdio_download_state(bus, bool_val);
		break;

	case IOV_SVAL(IOV_VARS):
		bcmerror = dhdsdio_downloadvars(bus, arg, len);
		break;

	case IOV_GVAL(IOV_READAHEAD):
		int_val = (int32)dhd_readahead;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_READAHEAD):
		if (bool_val && !dhd_readahead)
			bus->nextlen = 0;
		dhd_readahead = bool_val;
		break;

	case IOV_GVAL(IOV_SDRXCHAIN):
		int_val = (int32)bus->use_rxchain;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_SDRXCHAIN):
		if (bool_val && !bus->sd_rxchain)
			bcmerror = BCME_UNSUPPORTED;
		else
			bus->use_rxchain = bool_val;
		break;
	case IOV_GVAL(IOV_ALIGNCTL):
		int_val = (int32)dhd_alignctl;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_ALIGNCTL):
		dhd_alignctl = bool_val;
		break;

	case IOV_GVAL(IOV_SDALIGN):
		int_val = DHD_SDALIGN;
		bcopy(&int_val, arg, val_size);
		break;

#ifdef DHD_DEBUG
	case IOV_GVAL(IOV_VARS):
		if (bus->varsz < (uint)len)
			bcopy(bus->vars, arg, bus->varsz);
		else
			bcmerror = BCME_BUFTOOSHORT;
		break;
#endif 

#ifdef DHD_DEBUG
	case IOV_GVAL(IOV_SDREG):
	{
		sdreg_t *sd_ptr;
		uint32 addr, size;

		sd_ptr = (sdreg_t *)params;

		addr = (uintptr)bus->regs + sd_ptr->offset;
		size = sd_ptr->func;
		int_val = (int32)bcmsdh_reg_read(bus->sdh, addr, size);
		if (bcmsdh_regfail(bus->sdh))
			bcmerror = BCME_SDIO_ERROR;
		bcopy(&int_val, arg, sizeof(int32));
		break;
	}

	case IOV_SVAL(IOV_SDREG):
	{
		sdreg_t *sd_ptr;
		uint32 addr, size;

		sd_ptr = (sdreg_t *)params;

		addr = (uintptr)bus->regs + sd_ptr->offset;
		size = sd_ptr->func;
		bcmsdh_reg_write(bus->sdh, addr, size, sd_ptr->value);
		if (bcmsdh_regfail(bus->sdh))
			bcmerror = BCME_SDIO_ERROR;
		break;
	}

	
	case IOV_GVAL(IOV_SBREG):
	{
		sdreg_t sdreg;
		uint32 addr, size;

		bcopy(params, &sdreg, sizeof(sdreg));

		addr = SI_ENUM_BASE + sdreg.offset;
		size = sdreg.func;
		int_val = (int32)bcmsdh_reg_read(bus->sdh, addr, size);
		if (bcmsdh_regfail(bus->sdh))
			bcmerror = BCME_SDIO_ERROR;
		bcopy(&int_val, arg, sizeof(int32));
		break;
	}

	case IOV_SVAL(IOV_SBREG):
	{
		sdreg_t sdreg;
		uint32 addr, size;

		bcopy(params, &sdreg, sizeof(sdreg));

		addr = SI_ENUM_BASE + sdreg.offset;
		size = sdreg.func;
		bcmsdh_reg_write(bus->sdh, addr, size, sdreg.value);
		if (bcmsdh_regfail(bus->sdh))
			bcmerror = BCME_SDIO_ERROR;
		break;
	}

	case IOV_GVAL(IOV_SDCIS):
	{
		*(char *)arg = 0;

		bcmstrcat(arg, "\nFunc 0\n");
		bcmsdh_cis_read(bus->sdh, 0x10, (uint8 *)arg + strlen(arg), SBSDIO_CIS_SIZE_LIMIT);
		bcmstrcat(arg, "\nFunc 1\n");
		bcmsdh_cis_read(bus->sdh, 0x11, (uint8 *)arg + strlen(arg), SBSDIO_CIS_SIZE_LIMIT);
		bcmstrcat(arg, "\nFunc 2\n");
		bcmsdh_cis_read(bus->sdh, 0x12, (uint8 *)arg + strlen(arg), SBSDIO_CIS_SIZE_LIMIT);
		break;
	}

	case IOV_GVAL(IOV_FORCEEVEN):
		int_val = (int32)forcealign;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_FORCEEVEN):
		forcealign = bool_val;
		break;

	case IOV_GVAL(IOV_TXBOUND):
		int_val = (int32)dhd_txbound;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_TXBOUND):
		dhd_txbound = (uint)int_val;
		break;

	case IOV_GVAL(IOV_RXBOUND):
		int_val = (int32)dhd_rxbound;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_RXBOUND):
		dhd_rxbound = (uint)int_val;
		break;

	case IOV_GVAL(IOV_TXMINMAX):
		int_val = (int32)dhd_txminmax;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_TXMINMAX):
		dhd_txminmax = (uint)int_val;
		break;

	case IOV_GVAL(IOV_SERIALCONS):
		int_val = dhd_serialconsole(bus, FALSE, 0, &bcmerror);
		if (bcmerror != 0)
			break;

		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_SERIALCONS):
		dhd_serialconsole(bus, TRUE, bool_val, &bcmerror);
		break;



#endif 


#ifdef SDTEST
	case IOV_GVAL(IOV_EXTLOOP):
		int_val = (int32)bus->ext_loop;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_EXTLOOP):
		bus->ext_loop = bool_val;
		break;

	case IOV_GVAL(IOV_PKTGEN):
		bcmerror = dhdsdio_pktgen_get(bus, arg);
		break;

	case IOV_SVAL(IOV_PKTGEN):
		bcmerror = dhdsdio_pktgen_set(bus, arg);
		break;
#endif 

#ifdef SDIO_CRC_ERROR_FIX
	case IOV_GVAL(IOV_WATERMARK):
		int_val = (int32)watermark;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_WATERMARK):
		watermark = (uint)int_val;
		watermark = (watermark > SBSDIO_WATERMARK_MASK) ? SBSDIO_WATERMARK_MASK : watermark;
		DHD_ERROR(("Setting watermark as 0x%x.\n", watermark));
		bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_1, SBSDIO_WATERMARK, (uint8)watermark, NULL);
		break;

	case IOV_GVAL(IOV_MESBUSYCTRL):
		int_val = (int32)mesbusyctrl;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_MESBUSYCTRL):
		mesbusyctrl = (uint)int_val;
		mesbusyctrl = (mesbusyctrl > SBSDIO_MESBUSYCTRL_MASK)
			? SBSDIO_MESBUSYCTRL_MASK : mesbusyctrl;
		DHD_ERROR(("Setting mesbusyctrl as 0x%x.\n", mesbusyctrl));
		bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_1, SBSDIO_FUNC1_MESBUSYCTRL,
			((uint8)mesbusyctrl | 0x80), NULL);
		break;
#endif 

	case IOV_GVAL(IOV_DONGLEISOLATION):
		int_val = bus->dhd->dongle_isolation;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_DONGLEISOLATION):
		bus->dhd->dongle_isolation = bool_val;
		break;

	case IOV_SVAL(IOV_DEVRESET):
		DHD_TRACE(("%s: Called set IOV_DEVRESET=%d dongle_reset=%d busstate=%d\n",
		           __FUNCTION__, bool_val, bus->dhd->dongle_reset,
		           bus->dhd->busstate));

		ASSERT(bus->dhd->osh);
		

		dhd_bus_devreset(bus->dhd, (uint8)bool_val);

		break;
#ifdef SOFTAP
	case IOV_GVAL(IOV_FWPATH):
	{
		uint32  fw_path_len;

		fw_path_len = strlen(bus->fw_path);
		DHD_INFO(("[softap] get fwpath, l=%d\n", len));

		if (fw_path_len > len-1) {
			bcmerror = BCME_BUFTOOSHORT;
			break;
		}

		if (fw_path_len) {
			bcopy(bus->fw_path, arg, fw_path_len);
			((uchar*)arg)[fw_path_len] = 0;
		}
		break;
	}

	case IOV_SVAL(IOV_FWPATH):
		DHD_INFO(("[softap] set fwpath, idx=%d\n", int_val));

		switch (int_val) {
		case 1:
			bus->fw_path = fw_path; 
			break;
		case 2:
			bus->fw_path = fw_path2;
			break;
		default:
			bcmerror = BCME_BADARG;
			break;
		}

		DHD_INFO(("[softap] new fw path: %s\n", (bus->fw_path[0] ? bus->fw_path : "NULL")));
		break;

#endif 
	case IOV_GVAL(IOV_DEVRESET):
		DHD_TRACE(("%s: Called get IOV_DEVRESET\n", __FUNCTION__));

		
		int_val = (bool) bus->dhd->dongle_reset;
		bcopy(&int_val, arg, val_size);

		break;

	case IOV_GVAL(IOV_KSO):
		int_val = dhdsdio_sleepcsr_get(bus);
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_GVAL(IOV_DEVCAP):
		int_val = dhdsdio_devcap_get(bus);
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_DEVCAP):
		dhdsdio_devcap_set(bus, (uint8) int_val);
		break;

#ifdef BCMSDIOH_TXGLOM
	case IOV_GVAL(IOV_TXGLOMSIZE):
		int_val = (int32)bus->glomsize;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_TXGLOMSIZE):
		if (int_val > SDPCM_MAXGLOM_SIZE) {
			bcmerror = BCME_ERROR;
		} else {
			bus->glomsize = (uint)int_val;
		}
		break;
	case IOV_GVAL(IOV_TXGLOMMODE):
		int_val = (int32)bus->glom_mode;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_TXGLOMMODE):
		if ((int_val != SDPCM_TXGLOM_CPY) && (int_val != SDPCM_TXGLOM_MDESC)) {
			bcmerror = BCME_RANGE;
		} else {
			if ((bus->glom_mode = bcmsdh_set_mode(bus->sdh, (uint)int_val)) != int_val)
				bcmerror = BCME_ERROR;
		}
		break;
#endif 
	default:
		bcmerror = BCME_UNSUPPORTED;
		break;
	}

exit:
	if ((bus->idletime == DHD_IDLE_IMMEDIATE) && !bus->dpc_sched) {
		bus->activity = FALSE;
		dhdsdio_clkctl(bus, CLK_NONE, TRUE);
	}

	dhd_os_sdunlock(bus->dhd);

	return bcmerror;
}

static int
dhdsdio_write_vars(dhd_bus_t *bus)
{
	int bcmerror = 0;
	uint32 varsize, phys_size;
	uint32 varaddr;
	uint8 *vbuffer;
	uint32 varsizew;
#ifdef DHD_DEBUG
	uint8 *nvram_ularray;
#endif 

	/* Even if there are no vars are to be written, we still need to set the ramsize. */
	varsize = bus->varsz ? ROUNDUP(bus->varsz, 4) : 0;
	varaddr = (bus->ramsize - 4) - varsize;

	varaddr += bus->dongle_ram_base;

	if (bus->vars) {
		if ((bus->sih->buscoretype == SDIOD_CORE_ID) && (bus->sdpcmrev == 7)) {
			if (((varaddr & 0x3C) == 0x3C) && (varsize > 4)) {
				DHD_ERROR(("PR85623WAR in place\n"));
				varsize += 4;
				varaddr -= 4;
			}
		}

		vbuffer = (uint8 *)MALLOC(bus->dhd->osh, varsize);
		if (!vbuffer)
			return BCME_NOMEM;

		bzero(vbuffer, varsize);
		bcopy(bus->vars, vbuffer, bus->varsz);

		
		bcmerror = dhdsdio_membytes(bus, TRUE, varaddr, vbuffer, varsize);
#ifdef DHD_DEBUG
		
		DHD_INFO(("Compare NVRAM dl & ul; varsize=%d\n", varsize));
		nvram_ularray = (uint8*)MALLOC(bus->dhd->osh, varsize);
		if (!nvram_ularray)
			return BCME_NOMEM;

		
		memset(nvram_ularray, 0xaa, varsize);

		
		bcmerror = dhdsdio_membytes(bus, FALSE, varaddr, nvram_ularray, varsize);
		if (bcmerror) {
				DHD_ERROR(("%s: error %d on reading %d nvram bytes at 0x%08x\n",
					__FUNCTION__, bcmerror, varsize, varaddr));
		}
		
		if (memcmp(vbuffer, nvram_ularray, varsize)) {
			DHD_ERROR(("%s: Downloaded NVRAM image is corrupted.\n", __FUNCTION__));
		} else
			DHD_ERROR(("%s: Download, Upload and compare of NVRAM succeeded.\n",
			__FUNCTION__));

		MFREE(bus->dhd->osh, nvram_ularray, varsize);
#endif 

		MFREE(bus->dhd->osh, vbuffer, varsize);
	}

	phys_size = REMAP_ENAB(bus) ? bus->ramsize : bus->orig_ramsize;

	phys_size += bus->dongle_ram_base;

	
	DHD_INFO(("Physical memory size: %d, usable memory size: %d\n",
		phys_size, bus->ramsize));
	DHD_INFO(("Vars are at %d, orig varsize is %d\n",
		varaddr, varsize));
	varsize = ((phys_size - 4) - varaddr);

	if (bcmerror) {
		varsizew = 0;
	} else {
		varsizew = varsize / 4;
		varsizew = (~varsizew << 16) | (varsizew & 0x0000FFFF);
		varsizew = htol32(varsizew);
	}

	DHD_INFO(("New varsize is %d, length token=0x%08x\n", varsize, varsizew));

	
	bcmerror = dhdsdio_membytes(bus, TRUE, (phys_size - 4),
		(uint8*)&varsizew, 4);

	return bcmerror;
}

static int
dhdsdio_download_state(dhd_bus_t *bus, bool enter)
{
	uint retries;
	int bcmerror = 0;
	int foundcr4 = 0;

	if (!bus->sih)
		return BCME_ERROR;
	if (enter) {
		bus->alp_only = TRUE;

		if (!(si_setcore(bus->sih, ARM7S_CORE_ID, 0)) &&
		    !(si_setcore(bus->sih, ARMCM3_CORE_ID, 0))) {
			if (si_setcore(bus->sih, ARMCR4_CORE_ID, 0)) {
				foundcr4 = 1;
			} else {
				DHD_ERROR(("%s: Failed to find ARM core!\n", __FUNCTION__));
				bcmerror = BCME_ERROR;
				goto fail;
			}
		}

		if (!foundcr4) {
			si_core_disable(bus->sih, 0);
			if (bcmsdh_regfail(bus->sdh)) {
				bcmerror = BCME_SDIO_ERROR;
				goto fail;
			}

			if (!(si_setcore(bus->sih, SOCRAM_CORE_ID, 0))) {
				DHD_ERROR(("%s: Failed to find SOCRAM core!\n", __FUNCTION__));
				bcmerror = BCME_ERROR;
				goto fail;
			}

			si_core_reset(bus->sih, 0, 0);
			if (bcmsdh_regfail(bus->sdh)) {
				DHD_ERROR(("%s: Failure trying reset SOCRAM core?\n",
				           __FUNCTION__));
				bcmerror = BCME_SDIO_ERROR;
				goto fail;
			}

			
			if (REMAP_ENAB(bus) && si_socdevram_remap_isenb(bus->sih))
				dhdsdio_devram_remap(bus, FALSE);

			
			if (bus->ramsize) {
				uint32 zeros = 0;
				if (dhdsdio_membytes(bus, TRUE, bus->ramsize - 4,
				                     (uint8*)&zeros, 4) < 0) {
					bcmerror = BCME_SDIO_ERROR;
					goto fail;
				}
			}
		} else {
			
			si_core_reset(bus->sih, SICF_CPUHALT, SICF_CPUHALT);
		}
	} else {
		if (!si_setcore(bus->sih, ARMCR4_CORE_ID, 0)) {
			if (!(si_setcore(bus->sih, SOCRAM_CORE_ID, 0))) {
				DHD_ERROR(("%s: Failed to find SOCRAM core!\n", __FUNCTION__));
				bcmerror = BCME_ERROR;
				goto fail;
			}

			if (!si_iscoreup(bus->sih)) {
				DHD_ERROR(("%s: SOCRAM core is down after reset?\n", __FUNCTION__));
				bcmerror = BCME_ERROR;
				goto fail;
			}

			if ((bcmerror = dhdsdio_write_vars(bus))) {
				DHD_ERROR(("%s: could not write vars to RAM\n", __FUNCTION__));
				goto fail;
			}

			if (REMAP_ENAB(bus) && !si_socdevram_remap_isenb(bus->sih))
				dhdsdio_devram_remap(bus, TRUE);

			if (!si_setcore(bus->sih, PCMCIA_CORE_ID, 0) &&
			    !si_setcore(bus->sih, SDIOD_CORE_ID, 0)) {
				DHD_ERROR(("%s: Can't change back to SDIO core?\n", __FUNCTION__));
				bcmerror = BCME_ERROR;
				goto fail;
			}
			W_SDREG(0xFFFFFFFF, &bus->regs->intstatus, retries);


			if (!(si_setcore(bus->sih, ARM7S_CORE_ID, 0)) &&
			    !(si_setcore(bus->sih, ARMCM3_CORE_ID, 0))) {
				DHD_ERROR(("%s: Failed to find ARM core!\n", __FUNCTION__));
				bcmerror = BCME_ERROR;
				goto fail;
			}
		} else {
			
			
			if ((bcmerror = dhdsdio_write_vars(bus))) {
				DHD_ERROR(("%s: could not write vars to RAM\n", __FUNCTION__));
				goto fail;
			}

			if (!si_setcore(bus->sih, PCMCIA_CORE_ID, 0) &&
			    !si_setcore(bus->sih, SDIOD_CORE_ID, 0)) {
				DHD_ERROR(("%s: Can't change back to SDIO core?\n", __FUNCTION__));
				bcmerror = BCME_ERROR;
				goto fail;
			}
			W_SDREG(0xFFFFFFFF, &bus->regs->intstatus, retries);

			
			if (!(si_setcore(bus->sih, ARMCR4_CORE_ID, 0))) {
				DHD_ERROR(("%s: Failed to find ARM CR4 core!\n", __FUNCTION__));
				bcmerror = BCME_ERROR;
				goto fail;
			}
			
			bcmerror = dhdsdio_membytes(bus, TRUE, 0,
				(uint8 *)&bus->resetinstr, sizeof(bus->resetinstr));

			
		}

		si_core_reset(bus->sih, 0, 0);
		if (bcmsdh_regfail(bus->sdh)) {
			DHD_ERROR(("%s: Failure trying to reset ARM core?\n", __FUNCTION__));
			bcmerror = BCME_SDIO_ERROR;
			goto fail;
		}

		
		bus->alp_only = FALSE;

		bus->dhd->busstate = DHD_BUS_LOAD;
	}

fail:
	
	if (!si_setcore(bus->sih, PCMCIA_CORE_ID, 0))
		si_setcore(bus->sih, SDIOD_CORE_ID, 0);

	return bcmerror;
}

int
dhd_bus_iovar_op(dhd_pub_t *dhdp, const char *name,
                 void *params, int plen, void *arg, int len, bool set)
{
	dhd_bus_t *bus = dhdp->bus;
	const bcm_iovar_t *vi = NULL;
	int bcmerror = 0;
	int val_size;
	uint32 actionid;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	ASSERT(name);
	ASSERT(len >= 0);

	
	ASSERT(set || (arg && len));

	
	ASSERT(!set || (!params && !plen));

	
	if ((vi = bcm_iovar_lookup(dhdsdio_iovars, name)) == NULL) {
		printf("%s: NOT FOUND\n", __FUNCTION__);

		dhd_os_sdlock(bus->dhd);

		BUS_WAKE(bus);

		
		dhdsdio_clkctl(bus, CLK_AVAIL, FALSE);

		bcmerror = bcmsdh_iovar_op(bus->sdh, name, params, plen, arg, len, set);

		

		
		if (set && strcmp(name, "sd_divisor") == 0) {
			if (bcmsdh_iovar_op(bus->sdh, "sd_divisor", NULL, 0,
			                    &bus->sd_divisor, sizeof(int32), FALSE) != BCME_OK) {
				bus->sd_divisor = -1;
				DHD_ERROR(("%s: fail on %s get\n", __FUNCTION__, name));
			} else {
				DHD_INFO(("%s: noted %s update, value now %d\n",
				          __FUNCTION__, name, bus->sd_divisor));
			}
		}
		
		if (set && strcmp(name, "sd_mode") == 0) {
			if (bcmsdh_iovar_op(bus->sdh, "sd_mode", NULL, 0,
			                    &bus->sd_mode, sizeof(int32), FALSE) != BCME_OK) {
				bus->sd_mode = -1;
				DHD_ERROR(("%s: fail on %s get\n", __FUNCTION__, name));
			} else {
				DHD_INFO(("%s: noted %s update, value now %d\n",
				          __FUNCTION__, name, bus->sd_mode));
			}
		}
		
		if (set && strcmp(name, "sd_blocksize") == 0) {
			int32 fnum = 2;
			if (bcmsdh_iovar_op(bus->sdh, "sd_blocksize", &fnum, sizeof(int32),
			                    &bus->blocksize, sizeof(int32), FALSE) != BCME_OK) {
				bus->blocksize = 0;
				DHD_ERROR(("%s: fail on %s get\n", __FUNCTION__, "sd_blocksize"));
			} else {
				DHD_INFO(("%s: noted %s update, value now %d\n",
				          __FUNCTION__, "sd_blocksize", bus->blocksize));

				if (bus->sih->chip == BCM4335_CHIP_ID)
					dhd_overflow_war(bus);
			}
		}
		bus->roundup = MIN(max_roundup, bus->blocksize);

		if ((bus->idletime == DHD_IDLE_IMMEDIATE) && !bus->dpc_sched) {
			bus->activity = FALSE;
			dhdsdio_clkctl(bus, CLK_NONE, TRUE);
		}

		dhd_os_sdunlock(bus->dhd);
		goto exit;
	}

	DHD_CTL(("%s: %s %s, len %d plen %d\n", __FUNCTION__,
	         name, (set ? "set" : "get"), len, plen));

	if (params == NULL) {
		params = arg;
		plen = len;
	}

	if (vi->type == IOVT_VOID)
		val_size = 0;
	else if (vi->type == IOVT_BUFFER)
		val_size = len;
	else
		
		val_size = sizeof(int);

	actionid = set ? IOV_SVAL(vi->varid) : IOV_GVAL(vi->varid);
	bcmerror = dhdsdio_doiovar(bus, vi, actionid, name, params, plen, arg, len, val_size);

exit:
	return bcmerror;
}

static bool dhd_bus_do_stop = FALSE;
void
dhd_bus_stop(struct dhd_bus *bus, bool enforce_mutex)
{
	osl_t *osh;
	uint32 local_hostintmask;
	uint8 saveclk, dat;
	uint retries;
	int err;
	if (!bus->dhd)
		return;

	osh = bus->dhd->osh;
	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	bcmsdh_waitlockfree(NULL);

	if (enforce_mutex)
		dhd_os_sdlock(bus->dhd);

	if ((bus->dhd->busstate == DHD_BUS_DOWN) || bus->dhd->hang_was_sent) {
		
		bus->dhd->busstate = DHD_BUS_DOWN;
		bus->hostintmask = 0;
		bcmsdh_intr_disable(bus->sdh);
	} else {

		BUS_WAKE(bus);
		dhd_bus_do_stop = TRUE;


		if (KSO_ENAB(bus)) {
			
			dat = bcmsdh_cfg_read(bus->sdh, SDIO_FUNC_0, SDIOD_CCCR_INTEN, NULL);
			dat &= ~(INTR_CTL_FUNC1_EN | INTR_CTL_FUNC2_EN);
			bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_0, SDIOD_CCCR_INTEN, dat, NULL);
		}

		
		bus->dhd->busstate = DHD_BUS_DOWN;

		if (KSO_ENAB(bus)) {

		
		dhdsdio_clkctl(bus, CLK_AVAIL, FALSE);

		
		W_SDREG(0, &bus->regs->hostintmask, retries);
		local_hostintmask = bus->hostintmask;
		bus->hostintmask = 0;

		
		saveclk = bcmsdh_cfg_read(bus->sdh, SDIO_FUNC_1, SBSDIO_FUNC1_CHIPCLKCSR, &err);
		if (!err) {
			bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_1, SBSDIO_FUNC1_CHIPCLKCSR,
			                 (saveclk | SBSDIO_FORCE_HT), &err);
		}
		if (err) {
			DHD_ERROR(("%s: Failed to force clock for F2: err %d\n",
			            __FUNCTION__, err));
		}

		
		DHD_INTR(("%s: disable SDIO interrupts\n", __FUNCTION__));
		bcmsdh_intr_disable(bus->sdh);
		bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_0, SDIOD_CCCR_IOEN, SDIO_FUNC_ENABLE_1, NULL);

		
		W_SDREG(local_hostintmask, &bus->regs->intstatus, retries);
		}

		
		dhdsdio_clkctl(bus, CLK_SDONLY, FALSE);
	}

	
	pktq_flush(osh, &bus->txq, TRUE, NULL, 0);

	
	if (bus->glomd)
		PKTFREE(osh, bus->glomd, FALSE);

	if (bus->glom)
		PKTFREE(osh, bus->glom, FALSE);

	bus->glom = bus->glomd = NULL;

	
	bus->rxlen = 0;
	dhd_os_ioctl_resp_wake(bus->dhd);

	
	bus->rxskip = FALSE;
	bus->tx_seq = bus->rx_seq = 0;

	dhd_bus_do_stop = FALSE;

	if (enforce_mutex)
		dhd_os_sdunlock(bus->dhd);
}

#ifdef BCMSDIOH_TXGLOM
void
dhd_txglom_enable(dhd_pub_t *dhdp, bool enable)
{
	dhd_bus_t *bus = dhdp->bus;

	char buf[256];
	uint32 rxglom;
	int32 ret;

	printf("%s: called, enable=%d\n", __FUNCTION__, enable);

	if (enable) {
		rxglom = 1;
		memset(buf, 0, sizeof(buf));
		bcm_mkiovar("bus:rxglom",
			(void *)&rxglom,
			4, buf, sizeof(buf));
		ret = dhd_wl_ioctl_cmd(dhdp,
			WLC_SET_VAR, buf,
			sizeof(buf), TRUE, 0);
		if (!(ret < 0)) {
			bus->glom_enable = TRUE;
		}
	} else {
		bus->glom_enable = FALSE;
	}
}
#endif 

int
dhd_bus_init(dhd_pub_t *dhdp, bool enforce_mutex)
{
	dhd_bus_t *bus = dhdp->bus;
	dhd_timeout_t tmo;
	uint retries = 0;
	uint8 ready, enable;
	int err, ret = 0;
	uint8 saveclk;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	ASSERT(bus->dhd);
	if (!bus->dhd)
		return 0;
		
	

	if (enforce_mutex)
		dhd_os_sdlock(bus->dhd);

	
	dhdsdio_clkctl(bus, CLK_AVAIL, FALSE);
	if (bus->clkstate != CLK_AVAIL) {
		DHD_ERROR(("%s: clock state is wrong. state = %d\n", __FUNCTION__, bus->clkstate));
		ret = -1;
		goto exit;
	}


	
	saveclk = bcmsdh_cfg_read(bus->sdh, SDIO_FUNC_1, SBSDIO_FUNC1_CHIPCLKCSR, &err);
	if (!err) {
		bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_1, SBSDIO_FUNC1_CHIPCLKCSR,
		                 (saveclk | SBSDIO_FORCE_HT), &err);
	}
	if (err) {
		DHD_ERROR(("%s: Failed to force clock for F2: err %d\n", __FUNCTION__, err));
		ret = -1;
		goto exit;
	}

	
	W_SDREG((SDPCM_PROT_VERSION << SMB_DATA_VERSION_SHIFT),
	        &bus->regs->tosbmailboxdata, retries);
	enable = (SDIO_FUNC_ENABLE_1 | SDIO_FUNC_ENABLE_2);

	bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_0, SDIOD_CCCR_IOEN, enable, NULL);

	
	dhd_timeout_start(&tmo, DHD_WAIT_F2RDY * 1000);

	ready = 0;
	while (ready != enable && !dhd_timeout_expired(&tmo))
	        ready = bcmsdh_cfg_read(bus->sdh, SDIO_FUNC_0, SDIOD_CCCR_IORDY, NULL);

	DHD_ERROR(("%s: enable 0x%02x, ready 0x%02x (waited %uus)\n",
	          __FUNCTION__, enable, ready, tmo.elapsed));


	
	if (ready == enable) {
		
		if (!(bus->regs = si_setcore(bus->sih, PCMCIA_CORE_ID, 0)))
			bus->regs = si_setcore(bus->sih, SDIOD_CORE_ID, 0);
		ASSERT(bus->regs != NULL);

		
		bus->hostintmask = HOSTINTMASK;
		
		if ((bus->sih->buscoretype == SDIOD_CORE_ID) && (bus->sdpcmrev == 4) &&
			(bus->rxint_mode != SDIO_DEVICE_HMB_RXINT)) {
			bus->hostintmask &= ~I_HMB_FRAME_IND;
			bus->hostintmask |= I_XMTDATA_AVAIL;
		}
#ifdef HTC_KlocWork
    if(bus->regs != NULL)
#endif
		W_SDREG(bus->hostintmask, &bus->regs->hostintmask, retries);
#ifdef SDIO_CRC_ERROR_FIX
		if (bus->blocksize < 512) {
			mesbusyctrl = watermark = bus->blocksize / 4;
		}
#endif 
		if (bus->sih->chip != BCM4335_CHIP_ID) {
			bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_1, SBSDIO_WATERMARK,
				(uint8)watermark, &err);
		}
#ifdef SDIO_CRC_ERROR_FIX
		bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_1, SBSDIO_FUNC1_MESBUSYCTRL,
			(uint8)mesbusyctrl|0x80, &err);
		bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_1, SBSDIO_DEVICE_CTL,
			SBSDIO_DEVCTL_EN_F2_BLK_WATERMARK, NULL);
#endif 

		
		dhdp->busstate = DHD_BUS_DATA;

		

		bus->intdis = FALSE;
		if (bus->intr) {
			DHD_INTR(("%s: enable SDIO device interrupts\n", __FUNCTION__));
			bcmsdh_intr_enable(bus->sdh);
		} else {
			DHD_INTR(("%s: disable SDIO interrupts\n", __FUNCTION__));
			bcmsdh_intr_disable(bus->sdh);
		}

	}


	else {
		
		enable = SDIO_FUNC_ENABLE_1;
		bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_0, SDIOD_CCCR_IOEN, enable, NULL);
	}

	if (dhdsdio_sr_cap(bus))
		dhdsdio_sr_init(bus);
	else
		bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_1,
			SBSDIO_FUNC1_CHIPCLKCSR, saveclk, &err);

	
	if (dhdp->busstate != DHD_BUS_DATA)
		dhdsdio_clkctl(bus, CLK_NONE, FALSE);

	
	bcmsdh_cfg_write(bus->sdh, SDIO_FUNC_0, SDIOD_CCCR_INTEN, INTR_CTL_FUNC2_EN | INTR_CTL_FUNC1_EN, NULL);
	printk("SDIOD_CCCR_INTEN is 0x%x\n", bcmsdh_cfg_read(bus->sdh, SDIO_FUNC_0, SDIOD_CCCR_INTEN, NULL));


exit:
	if (enforce_mutex)
		dhd_os_sdunlock(bus->dhd);

	return ret;
}

static void
dhdsdio_rxfail(dhd_bus_t *bus, bool abort, bool rtx)
{
	bcmsdh_info_t *sdh = bus->sdh;
	sdpcmd_regs_t *regs = bus->regs;
	uint retries = 0;
	uint16 lastrbc;
	uint8 hi, lo;
	int err;

	DHD_ERROR(("%s: %sterminate frame%s\n", __FUNCTION__,
	           (abort ? "abort command, " : ""), (rtx ? ", send NAK" : "")));

	if (!KSO_ENAB(bus)) {
		DHD_ERROR(("%s: Device asleep\n", __FUNCTION__));
		return;
	}

	if (abort) {
		bcmsdh_abort(sdh, SDIO_FUNC_2);
	}

	bcmsdh_cfg_write(sdh, SDIO_FUNC_1, SBSDIO_FUNC1_FRAMECTRL, SFC_RF_TERM, &err);
	if (err) {
		DHD_ERROR(("%s: SBSDIO_FUNC1_FRAMECTRL cmd err\n", __FUNCTION__));
		goto fail;
	}
	bus->f1regdata++;

	
	for (lastrbc = retries = 0xffff; retries > 0; retries--) {
		hi = bcmsdh_cfg_read(sdh, SDIO_FUNC_1, SBSDIO_FUNC1_RFRAMEBCHI, NULL);
		lo = bcmsdh_cfg_read(sdh, SDIO_FUNC_1, SBSDIO_FUNC1_RFRAMEBCLO, &err);
		if (err) {
			DHD_ERROR(("%s: SBSDIO_FUNC1_RFAMEBCLO cmd err\n", __FUNCTION__));
			goto fail;
		}
		bus->f1regdata += 2;

		if ((hi == 0) && (lo == 0))
			break;

		if ((hi > (lastrbc >> 8)) && (lo > (lastrbc & 0x00ff))) {
			DHD_ERROR(("%s: count growing: last 0x%04x now 0x%04x\n",
			           __FUNCTION__, lastrbc, ((hi << 8) + lo)));
		}
		lastrbc = (hi << 8) + lo;
	}

	if (!retries) {
		DHD_ERROR(("%s: count never zeroed: last 0x%04x\n", __FUNCTION__, lastrbc));
	} else {
		DHD_INFO(("%s: flush took %d iterations\n", __FUNCTION__, (0xffff - retries)));
	}

	if (rtx) {
		bus->rxrtx++;
		W_SDREG(SMB_NAK, &regs->tosbmailbox, retries);
		bus->f1regdata++;
		if (retries <= retry_limit) {
			bus->rxskip = TRUE;
		}
	}

	
	bus->nextlen = 0;

fail:
	
	if (err || bcmsdh_regfail(sdh))
		bus->dhd->busstate = DHD_BUS_DOWN;
}

static void
dhdsdio_read_control(dhd_bus_t *bus, uint8 *hdr, uint len, uint doff)
{
	bcmsdh_info_t *sdh = bus->sdh;
	uint rdlen, pad;

	int sdret;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	
	if ((bus->bus == SPI_BUS) && (!bus->usebufpool))
		goto gotpkt;

	ASSERT(bus->rxbuf);
	
	bus->rxctl = bus->rxbuf;
	if (dhd_alignctl) {
		bus->rxctl += firstread;
		if ((pad = ((uintptr)bus->rxctl % DHD_SDALIGN)))
			bus->rxctl += (DHD_SDALIGN - pad);
		bus->rxctl -= firstread;
	}
	ASSERT(bus->rxctl >= bus->rxbuf);

	
	bcopy(hdr, bus->rxctl, firstread);
	if (len <= firstread)
		goto gotpkt;

	
	if (bus->bus == SPI_BUS) {
		bcopy(hdr, bus->rxctl, len);
		goto gotpkt;
	}

	
	rdlen = len - firstread;
	if (bus->roundup && bus->blocksize && (rdlen > bus->blocksize)) {
		pad = bus->blocksize - (rdlen % bus->blocksize);
		if ((pad <= bus->roundup) && (pad < bus->blocksize) &&
		    ((len + pad) < bus->dhd->maxctl))
			rdlen += pad;
	} else if (rdlen % DHD_SDALIGN) {
		rdlen += DHD_SDALIGN - (rdlen % DHD_SDALIGN);
	}

	
	if (forcealign && (rdlen & (ALIGNMENT - 1)))
		rdlen = ROUNDUP(rdlen, ALIGNMENT);

	
	if ((rdlen + firstread) > bus->dhd->maxctl) {
		DHD_ERROR(("%s: %d-byte control read exceeds %d-byte buffer\n",
		           __FUNCTION__, rdlen, bus->dhd->maxctl));
		bus->dhd->rx_errors++;
		dhdsdio_rxfail(bus, FALSE, FALSE);
		goto done;
	}

	if ((len - doff) > bus->dhd->maxctl) {
		DHD_ERROR(("%s: %d-byte ctl frame (%d-byte ctl data) exceeds %d-byte limit\n",
		           __FUNCTION__, len, (len - doff), bus->dhd->maxctl));
		bus->dhd->rx_errors++; bus->rx_toolong++;
		dhdsdio_rxfail(bus, FALSE, FALSE);
		goto done;
	}


	
	sdret = dhd_bcmsdh_recv_buf(bus, bcmsdh_cur_sbwad(sdh), SDIO_FUNC_2, F2SYNC,
	                            (bus->rxctl + firstread), rdlen, NULL, NULL, NULL);
	bus->f2rxdata++;
	ASSERT(sdret != BCME_PENDING);

	
	if (sdret < 0) {
		DHD_ERROR(("%s: read %d control bytes failed: %d\n", __FUNCTION__, rdlen, sdret));
		bus->rxc_errors++; 
		dhdsdio_rxfail(bus, TRUE, TRUE);
		rx_sdio_err_cnt++;
		if ( rx_sdio_err_cnt > MAX_SDIO_ERR_COUNT ) {
			dhd_info_send_hang_message(bus->dhd);
		}
		goto done;
	}
	rx_sdio_err_cnt=0;

gotpkt:

#ifdef DHD_DEBUG
	if (DHD_BYTES_ON() && DHD_CTL_ON()) {
		prhex("RxCtrl", bus->rxctl, len);
	}
#endif

	
	bus->rxctl += doff;
	bus->rxlen = len - doff;

done:
	
	dhd_os_ioctl_resp_wake(bus->dhd);
}

static int rxfail_count = 0;

static uint8
dhdsdio_rxglom(dhd_bus_t *bus, uint8 rxseq)
{
	uint16 dlen, totlen;
	uint8 *dptr, num = 0;

	uint16 sublen, check;
	void *pfirst, *plast, *pnext;
	void * list_tail[DHD_MAX_IFS] = { NULL };
	void * list_head[DHD_MAX_IFS] = { NULL };
	uint8 idx;
	osl_t *osh = bus->dhd->osh;

	int errcode;
	uint8 chan, seq, doff, sfdoff;
	uint8 txmax;
	uchar reorder_info_buf[WLHOST_REORDERDATA_TOTLEN];
	uint reorder_info_len;

	int ifidx = 0;
	bool usechain = bus->use_rxchain;

	
	

	DHD_TRACE(("dhdsdio_rxglom: start: glomd %p glom %p\n", bus->glomd, bus->glom));

	
	if (bus->glomd) {
		dhd_os_sdlock_rxq(bus->dhd);

		pfirst = plast = pnext = NULL;
		dlen = (uint16)PKTLEN(osh, bus->glomd);
		dptr = PKTDATA(osh, bus->glomd);
		if (!dlen || (dlen & 1)) {
			DHD_ERROR(("%s: bad glomd len (%d), ignore descriptor\n",
			           __FUNCTION__, dlen));
			dlen = 0;
		}

		for (totlen = num = 0; dlen; num++) {
			
			sublen = ltoh16_ua(dptr);
			dlen -= sizeof(uint16);
			dptr += sizeof(uint16);
			if ((sublen < SDPCM_HDRLEN_RX) ||
			    ((num == 0) && (sublen < (2 * SDPCM_HDRLEN_RX)))) {
				DHD_ERROR(("%s: descriptor len %d bad: %d\n",
				           __FUNCTION__, num, sublen));
				pnext = NULL;
				break;
			}
			if (sublen % DHD_SDALIGN) {
				DHD_ERROR(("%s: sublen %d not a multiple of %d\n",
				           __FUNCTION__, sublen, DHD_SDALIGN));
				usechain = FALSE;
			}
			totlen += sublen;

			
			if (!dlen) {
				sublen += (ROUNDUP(totlen, bus->blocksize) - totlen);
				totlen = ROUNDUP(totlen, bus->blocksize);
			}

			
			if ((pnext = PKTGET(osh, sublen + DHD_SDALIGN, FALSE)) == NULL) {
				DHD_ERROR(("%s: PKTGET failed, num %d len %d\n",
				           __FUNCTION__, num, sublen));
				break;
			}
			ASSERT(!PKTLINK(pnext));
			if (!pfirst) {
				ASSERT(!plast);
				pfirst = plast = pnext;
			} else {
				ASSERT(plast);
				PKTSETNEXT(osh, plast, pnext);
				plast = pnext;
			}

			
			PKTALIGN(osh, pnext, sublen, DHD_SDALIGN);
		}

		
		if (pnext) {
			DHD_GLOM(("%s: allocated %d-byte packet chain for %d subframes\n",
			          __FUNCTION__, totlen, num));
			if (DHD_GLOM_ON() && bus->nextlen) {
				if (totlen != bus->nextlen) {
					DHD_GLOM(("%s: glomdesc mismatch: nextlen %d glomdesc %d "
					          "rxseq %d\n", __FUNCTION__, bus->nextlen,
					          totlen, rxseq));
				}
			}
			bus->glom = pfirst;
			pfirst = pnext = NULL;
		} else {
			if (pfirst)
				PKTFREE(osh, pfirst, FALSE);
			bus->glom = NULL;
			num = 0;
		}

		
		PKTFREE(osh, bus->glomd, FALSE);
		bus->glomd = NULL;
		bus->nextlen = 0;

		dhd_os_sdunlock_rxq(bus->dhd);
	}

	
	if (bus->glom) {
		if (DHD_GLOM_ON()) {
			DHD_GLOM(("%s: attempt superframe read, packet chain:\n", __FUNCTION__));
			for (pnext = bus->glom; pnext; pnext = PKTNEXT(osh, pnext)) {
				DHD_GLOM(("    %p: %p len 0x%04x (%d)\n",
				          pnext, (uint8*)PKTDATA(osh, pnext),
				          PKTLEN(osh, pnext), PKTLEN(osh, pnext)));
			}
		}

		pfirst = bus->glom;
		dlen = (uint16)pkttotlen(osh, pfirst);

		if (usechain) {
			errcode = dhd_bcmsdh_recv_buf(bus,
			                              bcmsdh_cur_sbwad(bus->sdh), SDIO_FUNC_2,
			                              F2SYNC, (uint8*)PKTDATA(osh, pfirst),
			                              dlen, pfirst, NULL, NULL);
		} else if (bus->dataptr) {
			errcode = dhd_bcmsdh_recv_buf(bus,
			                              bcmsdh_cur_sbwad(bus->sdh), SDIO_FUNC_2,
			                              F2SYNC, bus->dataptr,
			                              dlen, NULL, NULL, NULL);
			sublen = (uint16)pktfrombuf(osh, pfirst, 0, dlen, bus->dataptr);
			if (sublen != dlen) {
				DHD_ERROR(("%s: FAILED TO COPY, dlen %d sublen %d\n",
				           __FUNCTION__, dlen, sublen));
				errcode = -1;
			}
			pnext = NULL;
		} else {
			DHD_ERROR(("COULDN'T ALLOC %d-BYTE GLOM, FORCE FAILURE\n", dlen));
			errcode = -1;
		}
		bus->f2rxdata++;
		ASSERT(errcode != BCME_PENDING);

		
		if (errcode < 0) {
			DHD_ERROR(("%s: glom read of %d bytes failed: %d\n",
			           __FUNCTION__, dlen, errcode));
			bus->dhd->rx_errors++;

			if (bus->glomerr++ < 3) {
				dhdsdio_rxfail(bus, TRUE, TRUE);
			} else {
				bus->glomerr = 0;
				dhdsdio_rxfail(bus, TRUE, FALSE);
				if (bus->glom) {
					dhd_os_sdlock_rxq(bus->dhd);
					PKTFREE(osh, bus->glom, FALSE);
					dhd_os_sdunlock_rxq(bus->dhd);
				}
				bus->rxglomfail++;
				bus->glom = NULL;
			}
			rxfail_count++;
			if (rxfail_count > rxglom_fail_count) {
				dhd_info_send_hang_message(bus->dhd);
				rxfail_count = 0;
			}
			return 0;
		}

#ifdef DHD_DEBUG
		if (DHD_GLOM_ON()) {
			prhex("SUPERFRAME", PKTDATA(osh, pfirst),
			      MIN(PKTLEN(osh, pfirst), 48));
		}
#endif


		
		dptr = (uint8 *)PKTDATA(osh, pfirst);
		sublen = ltoh16_ua(dptr);
		check = ltoh16_ua(dptr + sizeof(uint16));

		chan = SDPCM_PACKET_CHANNEL(&dptr[SDPCM_FRAMETAG_LEN]);
		seq = SDPCM_PACKET_SEQUENCE(&dptr[SDPCM_FRAMETAG_LEN]);
		bus->nextlen = dptr[SDPCM_FRAMETAG_LEN + SDPCM_NEXTLEN_OFFSET];
		if ((bus->nextlen << 4) > MAX_RX_DATASZ) {
			DHD_INFO(("%s: got frame w/nextlen too large (%d) seq %d\n",
			          __FUNCTION__, bus->nextlen, seq));
			bus->nextlen = 0;
		}
		doff = SDPCM_DOFFSET_VALUE(&dptr[SDPCM_FRAMETAG_LEN]);
		txmax = SDPCM_WINDOW_VALUE(&dptr[SDPCM_FRAMETAG_LEN]);

		errcode = 0;
		if ((uint16)~(sublen^check)) {
			DHD_ERROR(("%s (superframe): HW hdr error: len/check 0x%04x/0x%04x\n",
			           __FUNCTION__, sublen, check));
			errcode = -1;
		} else if (ROUNDUP(sublen, bus->blocksize) != dlen) {
			DHD_ERROR(("%s (superframe): len 0x%04x, rounded 0x%04x, expect 0x%04x\n",
			           __FUNCTION__, sublen, ROUNDUP(sublen, bus->blocksize), dlen));
			errcode = -1;
		} else if (SDPCM_PACKET_CHANNEL(&dptr[SDPCM_FRAMETAG_LEN]) != SDPCM_GLOM_CHANNEL) {
			DHD_ERROR(("%s (superframe): bad channel %d\n", __FUNCTION__,
			           SDPCM_PACKET_CHANNEL(&dptr[SDPCM_FRAMETAG_LEN])));
			errcode = -1;
		} else if (SDPCM_GLOMDESC(&dptr[SDPCM_FRAMETAG_LEN])) {
			DHD_ERROR(("%s (superframe): got second descriptor?\n", __FUNCTION__));
			errcode = -1;
		} else if ((doff < SDPCM_HDRLEN_RX) ||
		           (doff > (PKTLEN(osh, pfirst) - SDPCM_HDRLEN_RX))) {
			DHD_ERROR(("%s (superframe): Bad data offset %d: HW %d pkt %d min %d\n",
				__FUNCTION__, doff, sublen, PKTLEN(osh, pfirst),
				SDPCM_HDRLEN_RX));
			errcode = -1;
		}

		
		if (rxseq != seq) {
			DHD_INFO(("%s: (superframe) rx_seq %d, expected %d\n",
			          __FUNCTION__, seq, rxseq));
			bus->rx_badseq++;
			rxseq = seq;
		}

		
		if ((uint8)(txmax - bus->tx_seq) > 0x40) {
			DHD_ERROR(("%s: got unlikely tx max %d with tx_seq %d\n",
			           __FUNCTION__, txmax, bus->tx_seq));
			txmax = bus->tx_max;
		}
		bus->tx_max = txmax;

		
		PKTPULL(osh, pfirst, doff);
		sfdoff = doff;

		
		for (num = 0, pnext = pfirst; pnext && !errcode;
		     num++, pnext = PKTNEXT(osh, pnext)) {
			dptr = (uint8 *)PKTDATA(osh, pnext);
			dlen = (uint16)PKTLEN(osh, pnext);
			sublen = ltoh16_ua(dptr);
			check = ltoh16_ua(dptr + sizeof(uint16));
			chan = SDPCM_PACKET_CHANNEL(&dptr[SDPCM_FRAMETAG_LEN]);
			doff = SDPCM_DOFFSET_VALUE(&dptr[SDPCM_FRAMETAG_LEN]);
#ifdef DHD_DEBUG
			if (DHD_GLOM_ON()) {
				prhex("subframe", dptr, 32);
			}
#endif

			if ((uint16)~(sublen^check)) {
				DHD_ERROR(("%s (subframe %d): HW hdr error: "
				           "len/check 0x%04x/0x%04x\n",
				           __FUNCTION__, num, sublen, check));
				errcode = -1;
			} else if ((sublen > dlen) || (sublen < SDPCM_HDRLEN_RX)) {
				DHD_ERROR(("%s (subframe %d): length mismatch: "
				           "len 0x%04x, expect 0x%04x\n",
				           __FUNCTION__, num, sublen, dlen));
				errcode = -1;
			} else if ((chan != SDPCM_DATA_CHANNEL) &&
			           (chan != SDPCM_EVENT_CHANNEL)) {
				DHD_ERROR(("%s (subframe %d): bad channel %d\n",
				           __FUNCTION__, num, chan));
				errcode = -1;
			} else if ((doff < SDPCM_HDRLEN_RX) || (doff > sublen)) {
				DHD_ERROR(("%s (subframe %d): Bad data offset %d: HW %d min %d\n",
				           __FUNCTION__, num, doff, sublen, SDPCM_HDRLEN_RX));
				errcode = -1;
			}
		}

		if (errcode) {
			
			if (bus->glomerr++ < 3) {
				
				PKTPUSH(osh, pfirst, sfdoff);
				dhdsdio_rxfail(bus, TRUE, TRUE);
			} else {
				bus->glomerr = 0;
				dhdsdio_rxfail(bus, TRUE, FALSE);
				dhd_os_sdlock_rxq(bus->dhd);
				PKTFREE(osh, bus->glom, FALSE);
				dhd_os_sdunlock_rxq(bus->dhd);
				bus->rxglomfail++;
				bus->glom = NULL;
			}
			bus->nextlen = 0;
			rxfail_count++;
			if (rxfail_count > rxglom_fail_count) {
				dhd_info_send_hang_message(bus->dhd);
				rxfail_count = 0;
			}
			return 0;
		}

		
		bus->glom = NULL;
		plast = NULL;

		dhd_os_sdlock_rxq(bus->dhd);
		for (num = 0; pfirst; rxseq++, pfirst = pnext) {
			pnext = PKTNEXT(osh, pfirst);
			PKTSETNEXT(osh, pfirst, NULL);

			dptr = (uint8 *)PKTDATA(osh, pfirst);
			sublen = ltoh16_ua(dptr);
			chan = SDPCM_PACKET_CHANNEL(&dptr[SDPCM_FRAMETAG_LEN]);
			seq = SDPCM_PACKET_SEQUENCE(&dptr[SDPCM_FRAMETAG_LEN]);
			doff = SDPCM_DOFFSET_VALUE(&dptr[SDPCM_FRAMETAG_LEN]);

			DHD_GLOM(("%s: Get subframe %d, %p(%p/%d), sublen %d chan %d seq %d\n",
			          __FUNCTION__, num, pfirst, PKTDATA(osh, pfirst),
			          PKTLEN(osh, pfirst), sublen, chan, seq));

			ASSERT((chan == SDPCM_DATA_CHANNEL) || (chan == SDPCM_EVENT_CHANNEL));

			if (rxseq != seq) {
				DHD_GLOM(("%s: rx_seq %d, expected %d\n",
				          __FUNCTION__, seq, rxseq));
				bus->rx_badseq++;
				rxseq = seq;
			}

#ifdef DHD_DEBUG
			if (DHD_BYTES_ON() && DHD_DATA_ON()) {
				prhex("Rx Subframe Data", dptr, dlen);
			}
#endif

			PKTSETLEN(osh, pfirst, sublen);
			PKTPULL(osh, pfirst, doff);

			reorder_info_len = sizeof(reorder_info_buf);

			if (PKTLEN(osh, pfirst) == 0) {
				PKTFREE(bus->dhd->osh, pfirst, FALSE);
				continue;
			} else if (dhd_prot_hdrpull(bus->dhd, &ifidx, pfirst, reorder_info_buf,
				&reorder_info_len) != 0) {
				DHD_ERROR(("%s: rx protocol error\n", __FUNCTION__));
				bus->dhd->rx_errors++;
				PKTFREE(osh, pfirst, FALSE);
				continue;
			}
			if (reorder_info_len) {
				uint32 free_buf_count;
				void *ppfirst;

				ppfirst = pfirst;
				
				dhd_process_pkt_reorder_info(bus->dhd, reorder_info_buf,
					reorder_info_len, &ppfirst, &free_buf_count);

				if (free_buf_count == 0) {
					continue;
				}
				else {
					void *temp;

					
					temp = ppfirst;
					while (PKTNEXT(osh, temp) != NULL) {
						temp = PKTNEXT(osh, temp);
					}
					pfirst = temp;
					if (list_tail[ifidx] == NULL) {
						list_head[ifidx] = ppfirst;
						list_tail[ifidx] = pfirst;
					}
					else {
						PKTSETNEXT(osh, list_tail[ifidx], ppfirst);
						list_tail[ifidx] = pfirst;
					}
				}

				num += (uint8)free_buf_count;
			}
			else {
				

				if (list_tail[ifidx] == NULL) {
					list_head[ifidx] = list_tail[ifidx] = pfirst;
				}
				else {
					PKTSETNEXT(osh, list_tail[ifidx], pfirst);
					list_tail[ifidx] = pfirst;
				}
				num++;
			}
#ifdef DHD_DEBUG
			if (DHD_GLOM_ON()) {
				DHD_GLOM(("%s subframe %d to stack, %p(%p/%d) nxt/lnk %p/%p\n",
				          __FUNCTION__, num, pfirst,
				          PKTDATA(osh, pfirst), PKTLEN(osh, pfirst),
				          PKTNEXT(osh, pfirst), PKTLINK(pfirst)));
				prhex("", (uint8 *)PKTDATA(osh, pfirst),
				      MIN(PKTLEN(osh, pfirst), 32));
			}
#endif 
		}
		dhd_os_sdunlock_rxq(bus->dhd);

		for (idx = 0; idx < DHD_MAX_IFS; idx++) {
			if (list_head[idx]) {
				void *temp;
				uint8 cnt = 0;
				temp = list_head[idx];
				do {
					temp = PKTNEXT(osh, temp);
					cnt++;
				} while (temp);
				if (cnt) {
					dhd_os_sdunlock(bus->dhd);
					dhd_rx_frame(bus->dhd, idx, list_head[idx], cnt, 0);
					dhd_os_sdlock(bus->dhd);
				}
			}
		}
		bus->rxglomframes++;
		bus->rxglompkts += num;
	}
	rxfail_count = 0;

	return num;
}


#ifdef SDHOST3
static bool
dhdsdio_pr94636_WAR(dhd_bus_t *bus)
{
	uint cd = 0;
	uint ld = 0;
	int bcmerror = 0;
	uint32 l_data[5];
	uint32 l_addr = (0x18002200 & SBSDIO_SB_OFT_ADDR_MASK);

	if ((bcmerror = bcmsdh_rwdata(bus->sdh, FALSE, l_addr, (uint8 *)&l_data[0], 20))) {
		DHD_ERROR(("%s: bcmsdh_rwdata failed\n", __FUNCTION__));
		return FALSE;
	}
	ld = l_data[1];
	ld = ld & 0x00001fff;
	cd = l_data[4];
	cd = cd & 0x00001fff;
	if (cd == ld)
		return TRUE;
	else
		return FALSE;
}
#endif 

static uint
#ifdef REPEAT_READFRAME
dhdsdio_readframes(dhd_bus_t *bus, uint maxframes, bool *finished, bool tx_enable)
#else
dhdsdio_readframes(dhd_bus_t *bus, uint maxframes, bool *finished)
#endif

{
	osl_t *osh = bus->dhd->osh;
	bcmsdh_info_t *sdh = bus->sdh;

	uint16 len, check;	
	uint8 chan, seq, doff;	
	uint8 fcbits;		
	uint8 delta;

	void *pkt;	
	uint16 pad;	
	uint16 rdlen;	
	uint8 rxseq;	
	uint rxleft = 0;	
	int sdret;	
	uint8 txmax;	
	bool len_consistent; 
	uint8 *rxbuf;
	int ifidx = 0;
	uint rxcount = 0; 
	uchar reorder_info_buf[WLHOST_REORDERDATA_TOTLEN];
	uint reorder_info_len;
	uint pkt_count;

#if defined(DHD_DEBUG) || defined(SDTEST)
	bool sdtest = FALSE;	
#endif

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	bus->readframes = TRUE;

	if (!KSO_ENAB(bus)) {
		DHD_ERROR(("%s: KSO off\n", __FUNCTION__));
		bus->readframes = FALSE;
		return 0;
	}

	ASSERT(maxframes);

#ifdef SDTEST
	
	if (bus->pktgen_count && (bus->pktgen_mode == DHD_PKTGEN_RECV)) {
		maxframes = bus->pktgen_count;
		sdtest = TRUE;
	}
#endif

	
	*finished = FALSE;


	for (rxseq = bus->rx_seq, rxleft = maxframes;
	     !bus->rxskip && rxleft && bus->dhd->busstate != DHD_BUS_DOWN;
	     rxseq++, rxleft--) {

#ifdef DHDTHREAD
		
		if (
#ifdef REPEAT_READFRAME
			tx_enable &&
#endif
			(bus->clkstate == CLK_AVAIL) && !bus->fcstate &&
			pktq_mlen(&bus->txq, ~bus->flowcontrol) && DATAOK(bus)) {
			dhdsdio_sendfromq(bus, dhd_txbound);
		}
#endif 

		
		if (bus->glom || bus->glomd) {
			uint8 cnt;
			DHD_GLOM(("%s: calling rxglom: glomd %p, glom %p\n",
			          __FUNCTION__, bus->glomd, bus->glom));
			cnt = dhdsdio_rxglom(bus, rxseq);
			DHD_GLOM(("%s: rxglom returned %d\n", __FUNCTION__, cnt));
			rxseq += cnt - 1;
			rxleft = (rxleft > cnt) ? (rxleft - cnt) : 1;
			continue;
		}

		
		if (dhd_readahead && bus->nextlen) {
			uint16 nextlen = bus->nextlen;
			bus->nextlen = 0;

			if (bus->bus == SPI_BUS) {
				rdlen = len = nextlen;
			}
			else {
				rdlen = len = nextlen << 4;

				
				if (bus->roundup && bus->blocksize && (rdlen > bus->blocksize)) {
					pad = bus->blocksize - (rdlen % bus->blocksize);
					if ((pad <= bus->roundup) && (pad < bus->blocksize) &&
						((rdlen + pad + firstread) < MAX_RX_DATASZ))
						rdlen += pad;
				} else if (rdlen % DHD_SDALIGN) {
					rdlen += DHD_SDALIGN - (rdlen % DHD_SDALIGN);
				}
			}

			
			dhd_os_sdlock_rxq(bus->dhd);
			if (!(pkt = PKTGET(osh, rdlen + DHD_SDALIGN, FALSE))) {
				if (bus->bus == SPI_BUS) {
					bus->usebufpool = FALSE;
					bus->rxctl = bus->rxbuf;
					if (dhd_alignctl) {
						bus->rxctl += firstread;
						if ((pad = ((uintptr)bus->rxctl % DHD_SDALIGN)))
							bus->rxctl += (DHD_SDALIGN - pad);
						bus->rxctl -= firstread;
					}
					ASSERT(bus->rxctl >= bus->rxbuf);
					rxbuf = bus->rxctl;
					
					sdret = dhd_bcmsdh_recv_buf(bus,
					                            bcmsdh_cur_sbwad(sdh),
					                            SDIO_FUNC_2,
					                            F2SYNC, rxbuf, rdlen,
					                            NULL, NULL, NULL);
					bus->f2rxdata++;
					ASSERT(sdret != BCME_PENDING);


					
					if (sdret < 0) {
						DHD_ERROR(("%s: read %d control bytes failed: %d\n",
						   __FUNCTION__, rdlen, sdret));
						
						bus->rxc_errors++;
						dhd_os_sdunlock_rxq(bus->dhd);
						dhdsdio_rxfail(bus, TRUE,
						    (bus->bus == SPI_BUS) ? FALSE : TRUE);
						continue;
					}
				} else {
					
					DHD_ERROR(("%s (nextlen): PKTGET failed: len %d rdlen %d "
					           "expected rxseq %d\n",
					           __FUNCTION__, len, rdlen, rxseq));
					
					dhd_os_sdunlock_rxq(bus->dhd);
					continue;
				}
			} else {
				if (bus->bus == SPI_BUS)
					bus->usebufpool = TRUE;

				ASSERT(!PKTLINK(pkt));
				PKTALIGN(osh, pkt, rdlen, DHD_SDALIGN);
				rxbuf = (uint8 *)PKTDATA(osh, pkt);
				
				sdret = dhd_bcmsdh_recv_buf(bus, bcmsdh_cur_sbwad(sdh),
				                            SDIO_FUNC_2,
				                            F2SYNC, rxbuf, rdlen,
				                            pkt, NULL, NULL);
				bus->f2rxdata++;
				ASSERT(sdret != BCME_PENDING);

				if (sdret < 0) {
					DHD_ERROR(("%s (nextlen): read %d bytes failed: %d\n",
					   __FUNCTION__, rdlen, sdret));
					PKTFREE(bus->dhd->osh, pkt, FALSE);
					bus->dhd->rx_errors++;
					dhd_os_sdunlock_rxq(bus->dhd);
					dhdsdio_rxfail(bus, TRUE,
					      (bus->bus == SPI_BUS) ? FALSE : TRUE);
					rx_readahead_err_cnt++;
					if ( rx_readahead_err_cnt > MAX_SDIO_ERR_COUNT ) {
						dhd_info_send_hang_message(bus->dhd);
					}
					continue;
				}
				rx_readahead_err_cnt = 0;
			}
			dhd_os_sdunlock_rxq(bus->dhd);

			
			bcopy(rxbuf, bus->rxhdr, SDPCM_HDRLEN_RX);

			
			len = ltoh16_ua(bus->rxhdr);
			check = ltoh16_ua(bus->rxhdr + sizeof(uint16));

			
			if (!(len|check)) {
				DHD_INFO(("%s (nextlen): read zeros in HW header???\n",
				           __FUNCTION__));
				dhd_os_sdlock_rxq(bus->dhd);
				PKTFREE2();
				dhd_os_sdunlock_rxq(bus->dhd);
				GSPI_PR55150_BAILOUT;
				continue;
			}

			
			if ((uint16)~(len^check)) {
				DHD_ERROR(("%s (nextlen): HW hdr error: nextlen/len/check"
				           " 0x%04x/0x%04x/0x%04x\n", __FUNCTION__, nextlen,
				           len, check));
				dhd_os_sdlock_rxq(bus->dhd);
				PKTFREE2();
				dhd_os_sdunlock_rxq(bus->dhd);
				bus->rx_badhdr++;
				dhdsdio_rxfail(bus, FALSE, FALSE);
				GSPI_PR55150_BAILOUT;
				continue;
			}

			
			if (len < SDPCM_HDRLEN_RX) {
				DHD_ERROR(("%s (nextlen): HW hdr length invalid: %d\n",
				           __FUNCTION__, len));
				dhd_os_sdlock_rxq(bus->dhd);
				PKTFREE2();
				dhd_os_sdunlock_rxq(bus->dhd);
				GSPI_PR55150_BAILOUT;
				continue;
			}

			
				len_consistent = (nextlen != (ROUNDUP(len, 16) >> 4));
			if (len_consistent) {
				
				DHD_ERROR(("%s (nextlen): mismatch, nextlen %d len %d rnd %d; "
				           "expected rxseq %d\n",
				           __FUNCTION__, nextlen, len, ROUNDUP(len, 16), rxseq));
				dhd_os_sdlock_rxq(bus->dhd);
				PKTFREE2();
				dhd_os_sdunlock_rxq(bus->dhd);
				dhdsdio_rxfail(bus, TRUE, (bus->bus == SPI_BUS) ? FALSE : TRUE);
				GSPI_PR55150_BAILOUT;
				continue;
			}


			
			chan = SDPCM_PACKET_CHANNEL(&bus->rxhdr[SDPCM_FRAMETAG_LEN]);
			seq = SDPCM_PACKET_SEQUENCE(&bus->rxhdr[SDPCM_FRAMETAG_LEN]);
			doff = SDPCM_DOFFSET_VALUE(&bus->rxhdr[SDPCM_FRAMETAG_LEN]);
			txmax = SDPCM_WINDOW_VALUE(&bus->rxhdr[SDPCM_FRAMETAG_LEN]);

				bus->nextlen =
				         bus->rxhdr[SDPCM_FRAMETAG_LEN + SDPCM_NEXTLEN_OFFSET];
				if ((bus->nextlen << 4) > MAX_RX_DATASZ) {
					DHD_INFO(("%s (nextlen): got frame w/nextlen too large"
					          " (%d), seq %d\n", __FUNCTION__, bus->nextlen,
					          seq));
					bus->nextlen = 0;
				}

				bus->dhd->rx_readahead_cnt ++;
			
			fcbits = SDPCM_FCMASK_VALUE(&bus->rxhdr[SDPCM_FRAMETAG_LEN]);

			delta = 0;
			if (~bus->flowcontrol & fcbits) {
				bus->fc_xoff++;
				delta = 1;
			}
			if (bus->flowcontrol & ~fcbits) {
				bus->fc_xon++;
				delta = 1;
			}

			if (delta) {
				bus->fc_rcvd++;
				bus->flowcontrol = fcbits;
			}

			
			if (rxseq != seq) {
				DHD_INFO(("%s (nextlen): rx_seq %d, expected %d\n",
				          __FUNCTION__, seq, rxseq));
				bus->rx_badseq++;
				rxseq = seq;
			}

			
			if ((uint8)(txmax - bus->tx_seq) > 0x40) {
					DHD_ERROR(("%s: got unlikely tx max %d with tx_seq %d\n",
						__FUNCTION__, txmax, bus->tx_seq));
					txmax = bus->tx_max;
			}
			bus->tx_max = txmax;

#ifdef DHD_DEBUG
			if (DHD_BYTES_ON() && DHD_DATA_ON()) {
				prhex("Rx Data", rxbuf, len);
			} else if (DHD_HDRS_ON()) {
				prhex("RxHdr", bus->rxhdr, SDPCM_HDRLEN_RX);
			}
#endif

			if (chan == SDPCM_CONTROL_CHANNEL) {
				if (bus->bus == SPI_BUS) {
					dhdsdio_read_control(bus, rxbuf, len, doff);
					if (bus->usebufpool) {
						dhd_os_sdlock_rxq(bus->dhd);
						PKTFREE(bus->dhd->osh, pkt, FALSE);
						dhd_os_sdunlock_rxq(bus->dhd);
					}
					continue;
				} else {
					DHD_ERROR(("%s (nextlen): readahead on control"
					           " packet %d?\n", __FUNCTION__, seq));
					
					bus->nextlen = 0;
					dhdsdio_rxfail(bus, FALSE, TRUE);
					dhd_os_sdlock_rxq(bus->dhd);
					PKTFREE2();
					dhd_os_sdunlock_rxq(bus->dhd);
					continue;
				}
			}

			if ((bus->bus == SPI_BUS) && !bus->usebufpool) {
				DHD_ERROR(("Received %d bytes on %d channel. Running out of "
				           "rx pktbuf's or not yet malloced.\n", len, chan));
				continue;
			}

			
			if ((doff < SDPCM_HDRLEN_RX) || (doff > len)) {
				DHD_ERROR(("%s (nextlen): bad data offset %d: HW len %d min %d\n",
				           __FUNCTION__, doff, len, SDPCM_HDRLEN_RX));
				dhd_os_sdlock_rxq(bus->dhd);
				PKTFREE2();
				dhd_os_sdunlock_rxq(bus->dhd);
				ASSERT(0);
				dhdsdio_rxfail(bus, FALSE, FALSE);
				continue;
			}

			
			goto deliver;
		}
		
		if (bus->bus == SPI_BUS) {
			break;
		}
#ifdef SDHOST3

		if (((((uint16)bus->sih->chip) == BCM4324_CHIP_ID) && (bus->sih->chiprev <= 1)) ||
			(((uint16)bus->sih->chip) == BCM43341_CHIP_ID) ||
			(((uint16)bus->sih->chip) == BCM4334_CHIP_ID)) {
#ifdef TX_THREAD
			dhd_os_sdlock(bus->dhd);
#endif 
			if (dhdsdio_pr94636_WAR(bus) == TRUE) {
				*finished = TRUE;
#ifdef TX_THREAD
				dhd_os_sdunlock(bus->dhd);
#endif 
				break;
			}
#ifdef TX_THREAD
			dhd_os_sdunlock(bus->dhd);
#endif 
		}
#endif 

#ifdef TX_THREAD
		dhd_os_sdlock(bus->dhd);
#endif 

		
		sdret = dhd_bcmsdh_recv_buf(bus, bcmsdh_cur_sbwad(sdh), SDIO_FUNC_2, F2SYNC,
		                            bus->rxhdr, firstread, NULL, NULL, NULL);
		bus->f2rxhdrs++;
		ASSERT(sdret != BCME_PENDING);

		if (sdret < 0) {
			DHD_ERROR(("%s: RXHEADER FAILED: %d\n", __FUNCTION__, sdret));
			bus->rx_hdrfail++;
			dhdsdio_rxfail(bus, TRUE, TRUE);
			rx_sdio_err_cnt++;
			if ( rx_sdio_err_cnt > MAX_SDIO_ERR_COUNT ) {
				dhd_info_send_hang_message(bus->dhd);
			}
			continue;
		}
		rx_sdio_err_cnt=0;

#ifdef DHD_DEBUG
		if (DHD_BYTES_ON() || DHD_HDRS_ON()) {
			prhex("RxHdr", bus->rxhdr, SDPCM_HDRLEN_RX);
		}
#endif

		
		len = ltoh16_ua(bus->rxhdr);
		check = ltoh16_ua(bus->rxhdr + sizeof(uint16));

		
		if (!(len|check)) {
			*finished = TRUE;
			break;
		}

		
		if ((uint16)~(len^check)) {
			DHD_ERROR(("%s: HW hdr error: len/check 0x%04x/0x%04x\n",
			           __FUNCTION__, len, check));
			bus->rx_badhdr++;
			dhdsdio_rxfail(bus, FALSE, FALSE);
			continue;
		}

		
		if (len < SDPCM_HDRLEN_RX) {
			DHD_ERROR(("%s: HW hdr length invalid: %d\n", __FUNCTION__, len));
			continue;
		}

		
		chan = SDPCM_PACKET_CHANNEL(&bus->rxhdr[SDPCM_FRAMETAG_LEN]);
		seq = SDPCM_PACKET_SEQUENCE(&bus->rxhdr[SDPCM_FRAMETAG_LEN]);
		doff = SDPCM_DOFFSET_VALUE(&bus->rxhdr[SDPCM_FRAMETAG_LEN]);
		txmax = SDPCM_WINDOW_VALUE(&bus->rxhdr[SDPCM_FRAMETAG_LEN]);

		
		if ((doff < SDPCM_HDRLEN_RX) || (doff > len)) {
			DHD_ERROR(("%s: Bad data offset %d: HW len %d, min %d seq %d\n",
			           __FUNCTION__, doff, len, SDPCM_HDRLEN_RX, seq));
			bus->rx_badhdr++;
			ASSERT(0);
			dhdsdio_rxfail(bus, FALSE, FALSE);
			continue;
		}

		
		bus->nextlen = bus->rxhdr[SDPCM_FRAMETAG_LEN + SDPCM_NEXTLEN_OFFSET];
		if ((bus->nextlen << 4) > MAX_RX_DATASZ) {
			DHD_INFO(("%s (nextlen): got frame w/nextlen too large (%d), seq %d\n",
			          __FUNCTION__, bus->nextlen, seq));
			bus->nextlen = 0;
		}

		
		fcbits = SDPCM_FCMASK_VALUE(&bus->rxhdr[SDPCM_FRAMETAG_LEN]);

		delta = 0;
		if (~bus->flowcontrol & fcbits) {
			bus->fc_xoff++;
			delta = 1;
		}
		if (bus->flowcontrol & ~fcbits) {
			bus->fc_xon++;
			delta = 1;
		}

		if (delta) {
			bus->fc_rcvd++;
			bus->flowcontrol = fcbits;
		}

		
		if (rxseq != seq) {
			DHD_INFO(("%s: rx_seq %d, expected %d\n", __FUNCTION__, seq, rxseq));
			bus->rx_badseq++;
			rxseq = seq;
		}

		
		if ((uint8)(txmax - bus->tx_seq) > 0x40) {
			DHD_ERROR(("%s: got unlikely tx max %d with tx_seq %d\n",
			           __FUNCTION__, txmax, bus->tx_seq));
			txmax = bus->tx_max;
		}
		bus->tx_max = txmax;

		
		if (chan == SDPCM_CONTROL_CHANNEL) {
			dhdsdio_read_control(bus, bus->rxhdr, len, doff);
			continue;
		}

		ASSERT((chan == SDPCM_DATA_CHANNEL) || (chan == SDPCM_EVENT_CHANNEL) ||
		       (chan == SDPCM_TEST_CHANNEL) || (chan == SDPCM_GLOM_CHANNEL));

		
		rdlen = (len > firstread) ? (len - firstread) : 0;

		
		if (bus->roundup && bus->blocksize && (rdlen > bus->blocksize)) {
			pad = bus->blocksize - (rdlen % bus->blocksize);
			if ((pad <= bus->roundup) && (pad < bus->blocksize) &&
			    ((rdlen + pad + firstread) < MAX_RX_DATASZ))
				rdlen += pad;
		} else if (rdlen % DHD_SDALIGN) {
			rdlen += DHD_SDALIGN - (rdlen % DHD_SDALIGN);
		}

		
		if (forcealign && (rdlen & (ALIGNMENT - 1)))
			rdlen = ROUNDUP(rdlen, ALIGNMENT);

		if ((rdlen + firstread) > MAX_RX_DATASZ) {
			
			DHD_ERROR(("%s: too long: len %d rdlen %d\n", __FUNCTION__, len, rdlen));
			bus->dhd->rx_errors++; bus->rx_toolong++;
			dhdsdio_rxfail(bus, FALSE, FALSE);
			continue;
		}

		dhd_os_sdlock_rxq(bus->dhd);
		if (!(pkt = PKTGET(osh, (rdlen + firstread + DHD_SDALIGN), FALSE))) {
			
			DHD_ERROR(("%s: PKTGET failed: rdlen %d chan %d\n",
			           __FUNCTION__, rdlen, chan));
			bus->dhd->rx_dropped++;
			dhd_os_sdunlock_rxq(bus->dhd);
			dhdsdio_rxfail(bus, FALSE, RETRYCHAN(chan));
			continue;
		}
		dhd_os_sdunlock_rxq(bus->dhd);

		ASSERT(!PKTLINK(pkt));

		
		ASSERT(firstread < (PKTLEN(osh, pkt)));
		PKTPULL(osh, pkt, firstread);
		PKTALIGN(osh, pkt, rdlen, DHD_SDALIGN);

		
		sdret = dhd_bcmsdh_recv_buf(bus, bcmsdh_cur_sbwad(sdh), SDIO_FUNC_2, F2SYNC,
		                            ((uint8 *)PKTDATA(osh, pkt)), rdlen, pkt, NULL, NULL);
		bus->f2rxdata++;
		ASSERT(sdret != BCME_PENDING);

		if (sdret < 0) {
			DHD_ERROR(("%s: read %d %s bytes failed: %d\n", __FUNCTION__, rdlen,
			           ((chan == SDPCM_EVENT_CHANNEL) ? "event" :
			            ((chan == SDPCM_DATA_CHANNEL) ? "data" : "test")), sdret));
			dhd_os_sdlock_rxq(bus->dhd);
			PKTFREE(bus->dhd->osh, pkt, FALSE);
			dhd_os_sdunlock_rxq(bus->dhd);
			bus->dhd->rx_errors++;
			dhdsdio_rxfail(bus, TRUE, RETRYCHAN(chan));
			rx_sdio_err_cnt++;
			if ( rx_sdio_err_cnt > MAX_SDIO_ERR_COUNT ) {
				dhd_info_send_hang_message(bus->dhd);
			}
			continue;
		}
		rx_sdio_err_cnt=0;

		
		PKTPUSH(osh, pkt, firstread);
		bcopy(bus->rxhdr, PKTDATA(osh, pkt), firstread);

#ifdef DHD_DEBUG
		if (DHD_BYTES_ON() && DHD_DATA_ON()) {
			prhex("Rx Data", PKTDATA(osh, pkt), len);
		}
#endif

deliver:
		
		if (chan == SDPCM_GLOM_CHANNEL) {
			if (SDPCM_GLOMDESC(&bus->rxhdr[SDPCM_FRAMETAG_LEN])) {
				DHD_GLOM(("%s: got glom descriptor, %d bytes:\n",
				          __FUNCTION__, len));
#ifdef DHD_DEBUG
				if (DHD_GLOM_ON()) {
					prhex("Glom Data", PKTDATA(osh, pkt), len);
				}
#endif
				PKTSETLEN(osh, pkt, len);
				ASSERT(doff == SDPCM_HDRLEN_RX);
				PKTPULL(osh, pkt, SDPCM_HDRLEN_RX);
				bus->glomd = pkt;
			} else {
				DHD_ERROR(("%s: glom superframe w/o descriptor!\n", __FUNCTION__));
				dhdsdio_rxfail(bus, FALSE, FALSE);
			}
			continue;
		}

		
		PKTSETLEN(osh, pkt, len);
		PKTPULL(osh, pkt, doff);

#ifdef SDTEST
		
		if (chan == SDPCM_TEST_CHANNEL) {
			dhdsdio_testrcv(bus, pkt, seq);
			continue;
		}
#endif 

		if (PKTLEN(osh, pkt) == 0) {
			dhd_os_sdlock_rxq(bus->dhd);
			PKTFREE(bus->dhd->osh, pkt, FALSE);
			dhd_os_sdunlock_rxq(bus->dhd);
			continue;
		} else if (dhd_prot_hdrpull(bus->dhd, &ifidx, pkt, reorder_info_buf,
			&reorder_info_len) != 0) {
			DHD_ERROR(("%s: rx protocol error\n", __FUNCTION__));
			dhd_os_sdlock_rxq(bus->dhd);
			PKTFREE(bus->dhd->osh, pkt, FALSE);
			dhd_os_sdunlock_rxq(bus->dhd);
			bus->dhd->rx_errors++;
			continue;
		}
		if (reorder_info_len) {
			
			dhd_process_pkt_reorder_info(bus->dhd, reorder_info_buf, reorder_info_len,
				&pkt, &pkt_count);
			if (pkt_count == 0)
				continue;
		}
		else
			pkt_count = 1;

		
		dhd_os_sdunlock(bus->dhd);
		dhd_rx_frame(bus->dhd, ifidx, pkt, pkt_count, chan);
		dhd_os_sdlock(bus->dhd);
	}
	rxcount = maxframes - rxleft;
#ifdef DHD_DEBUG
	
	if (!rxleft && !sdtest)
		DHD_DATA(("%s: hit rx limit of %d frames\n", __FUNCTION__, maxframes));
	else
#endif 
	DHD_DATA(("%s: processed %d frames\n", __FUNCTION__, rxcount));
	
	if (bus->rxskip)
		rxseq--;
	bus->rx_seq = rxseq;

	if (bus->reqbussleep)
	{
	    dhdsdio_bussleep(bus, TRUE);
		bus->reqbussleep = FALSE;
	}
	bus->readframes = FALSE;

	return rxcount;
}

static uint32
dhdsdio_hostmail(dhd_bus_t *bus)
{
	sdpcmd_regs_t *regs = bus->regs;
	uint32 intstatus = 0;
	uint32 hmb_data;
	uint8 fcbits;
	uint retries = 0;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	
	R_SDREG(hmb_data, &regs->tohostmailboxdata, retries);
	if (retries <= retry_limit)
		W_SDREG(SMB_INT_ACK, &regs->tosbmailbox, retries);
	bus->f1regdata += 2;

	
	if (hmb_data & HMB_DATA_NAKHANDLED) {
		DHD_INFO(("Dongle reports NAK handled, expect rtx of %d\n", bus->rx_seq));
		if (!bus->rxskip) {
			DHD_ERROR(("%s: unexpected NAKHANDLED!\n", __FUNCTION__));
		}
		bus->rxskip = FALSE;
		intstatus |= FRAME_AVAIL_MASK(bus);
	}

	if (hmb_data & (HMB_DATA_DEVREADY | HMB_DATA_FWREADY)) {
		bus->sdpcm_ver = (hmb_data & HMB_DATA_VERSION_MASK) >> HMB_DATA_VERSION_SHIFT;
		if (bus->sdpcm_ver != SDPCM_PROT_VERSION)
			DHD_ERROR(("Version mismatch, dongle reports %d, expecting %d\n",
			           bus->sdpcm_ver, SDPCM_PROT_VERSION));
		else
			DHD_INFO(("Dongle ready, protocol version %d\n", bus->sdpcm_ver));
		
		if ((bus->sih->buscoretype == SDIOD_CORE_ID) && (bus->sdpcmrev >= 4) &&
		    (bus->rxint_mode  == SDIO_DEVICE_RXDATAINT_MODE_1)) {
			uint32 val;

			val = R_REG(bus->dhd->osh, &bus->regs->corecontrol);
			val &= ~CC_XMTDATAAVAIL_MODE;
			val |= CC_XMTDATAAVAIL_CTRL;
			W_REG(bus->dhd->osh, &bus->regs->corecontrol, val);

			val = R_REG(bus->dhd->osh, &bus->regs->corecontrol);
		}

#ifdef DHD_DEBUG
		
		{
			sdpcm_shared_t shared;
			if (dhdsdio_readshared(bus, &shared) == 0)
				bus->console_addr = shared.console_addr;
		}
#endif 
	}

	if (hmb_data & HMB_DATA_FC) {
		fcbits = (hmb_data & HMB_DATA_FCDATA_MASK) >> HMB_DATA_FCDATA_SHIFT;

		if (fcbits & ~bus->flowcontrol)
			bus->fc_xoff++;
		if (bus->flowcontrol & ~fcbits)
			bus->fc_xon++;

		bus->fc_rcvd++;
		bus->flowcontrol = fcbits;
	}

#ifdef DHD_DEBUG
	
	if (hmb_data & HMB_DATA_FWHALT) {
		DHD_ERROR(("INTERNAL ERROR: FIRMWARE HALTED : set BUS DOWN\n"));
		dhdsdio_checkdied(bus, NULL, 0);
		bus->dhd->busstate = DHD_BUS_DOWN;
		
		bus->intstatus = 0;
		dhd_info_send_hang_message(bus->dhd);
	}
#endif 

	
	if (hmb_data & ~(HMB_DATA_DEVREADY |
	                 HMB_DATA_FWHALT |
	                 HMB_DATA_NAKHANDLED |
	                 HMB_DATA_FC |
	                 HMB_DATA_FWREADY |
	                 HMB_DATA_FCDATA_MASK |
	                 HMB_DATA_VERSION_MASK)) {
		DHD_ERROR(("Unknown mailbox data content: 0x%02x\n", hmb_data));
	}

	return intstatus;
}

#ifdef REPEAT_READFRAME
extern uint dhd_dpcpoll;
#endif

extern int dhd_dpc_prio;
extern int multi_core_locked;

static bool
dhdsdio_dpc(dhd_bus_t *bus)
{
	bcmsdh_info_t *sdh = bus->sdh;
	sdpcmd_regs_t *regs = bus->regs;
	uint32 intstatus, newstatus = 0;
	uint retries = 0;
	uint rxlimit = dhd_rxbound; 
	uint txlimit = dhd_txbound; 
	uint framecnt = 0;		  
	bool rxdone = TRUE;		  
	bool resched = FALSE;	  

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	if (bus->dhd->busstate == DHD_BUS_DOWN) {
		DHD_ERROR(("%s: Bus down, ret\n", __FUNCTION__));
		bus->intstatus = 0;
		return 0;
	}

	
	intstatus = bus->intstatus;

	dhd_os_sdlock(bus->dhd);

	if ((bus->dhd->busstate == DHD_BUS_DOWN) || bus->dhd->hang_was_sent || module_remove) {
		DHD_ERROR(("%s : bus is down. we have nothing to do\n", __FUNCTION__));
		resched = 0;
		bus->dpc_sched = resched;
		bus->intstatus = 0;
		goto exit2;
	}

	if (!SLPAUTO_ENAB(bus) && !KSO_ENAB(bus)) {
		DHD_ERROR(("%s: Device asleep\n", __FUNCTION__));
		goto exit;
	}

	
	if (!SLPAUTO_ENAB(bus) && (bus->clkstate == CLK_PENDING)) {
		int err;
		uint8 clkctl, devctl = 0;

#if 0
		
		devctl = bcmsdh_cfg_read(sdh, SDIO_FUNC_1, SBSDIO_DEVICE_CTL, &err);
		if (err) {
			DHD_ERROR(("%s: error reading DEVCTL: %d\n", __FUNCTION__, err));
			bus->dhd->busstate = DHD_BUS_DOWN;
		} else {
			ASSERT(devctl & SBSDIO_DEVCTL_CA_INT_ONLY);
		}
#endif 

		
		clkctl = bcmsdh_cfg_read(sdh, SDIO_FUNC_1, SBSDIO_FUNC1_CHIPCLKCSR, &err);
		if (err) {
			DHD_ERROR(("%s: error reading CSR: %d\n", __FUNCTION__, err));
			bus->dhd->busstate = DHD_BUS_DOWN;
		}

		DHD_INFO(("DPC: PENDING, devctl 0x%02x clkctl 0x%02x\n", devctl, clkctl));

		if (SBSDIO_HTAV(clkctl)) {
			devctl = bcmsdh_cfg_read(sdh, SDIO_FUNC_1, SBSDIO_DEVICE_CTL, &err);
			if (err) {
				DHD_ERROR(("%s: error reading DEVCTL: %d\n",
				           __FUNCTION__, err));
				bus->dhd->busstate = DHD_BUS_DOWN;
			}
			devctl &= ~SBSDIO_DEVCTL_CA_INT_ONLY;
			bcmsdh_cfg_write(sdh, SDIO_FUNC_1, SBSDIO_DEVICE_CTL, devctl, &err);
			if (err) {
				DHD_ERROR(("%s: error writing DEVCTL: %d\n",
				           __FUNCTION__, err));
				bus->dhd->busstate = DHD_BUS_DOWN;
			}
			bus->clkstate = CLK_AVAIL;
		} else {
			goto clkwait;
		}
	}

	BUS_WAKE(bus);

	
	dhdsdio_clkctl(bus, CLK_AVAIL, TRUE);
	if (bus->clkstate != CLK_AVAIL)
		goto clkwait;

	
	if (bus->ipend) {
		bus->ipend = FALSE;
		R_SDREG(newstatus, &regs->intstatus, retries);
		bus->f1regdata++;
		if (bcmsdh_regfail(bus->sdh))
			newstatus = 0;
		newstatus &= bus->hostintmask;
		bus->fcstate = !!(newstatus & I_HMB_FC_STATE);
		if (newstatus) {
			bus->f1regdata++;
			if ((bus->rxint_mode == SDIO_DEVICE_RXDATAINT_MODE_0) &&
				(newstatus == I_XMTDATA_AVAIL)) {
			}
			else
				W_SDREG(newstatus, &regs->intstatus, retries);
		}
	}

	
	intstatus |= newstatus;
	bus->intstatus = 0;

	if (intstatus & I_HMB_FC_CHANGE) {
		intstatus &= ~I_HMB_FC_CHANGE;
		W_SDREG(I_HMB_FC_CHANGE, &regs->intstatus, retries);
		R_SDREG(newstatus, &regs->intstatus, retries);
		bus->f1regdata += 2;
		bus->fcstate = !!(newstatus & (I_HMB_FC_STATE | I_HMB_FC_CHANGE));
		intstatus |= (newstatus & bus->hostintmask);
	}

	
	if (intstatus & I_CHIPACTIVE) {
		
		intstatus &= ~I_CHIPACTIVE;
	}

	
	if (intstatus & I_HMB_HOST_INT) {
		intstatus &= ~I_HMB_HOST_INT;
		intstatus |= dhdsdio_hostmail(bus);
	}

	
	if (intstatus & I_WR_OOSYNC) {
		DHD_ERROR(("Dongle reports WR_OOSYNC\n"));
		intstatus &= ~I_WR_OOSYNC;
	}

	if (intstatus & I_RD_OOSYNC) {
		DHD_ERROR(("Dongle reports RD_OOSYNC\n"));
		intstatus &= ~I_RD_OOSYNC;
	}

	if (intstatus & I_SBINT) {
		DHD_ERROR(("Dongle reports SBINT\n"));
		intstatus &= ~I_SBINT;
	}

	
	if (intstatus & I_CHIPACTIVE) {
		DHD_INFO(("Dongle reports CHIPACTIVE\n"));
		intstatus &= ~I_CHIPACTIVE;
	}

	
	if (bus->rxskip) {
		intstatus &= ~FRAME_AVAIL_MASK(bus);
	}

	
	if (PKT_AVAILABLE(bus, intstatus)) {
#ifdef REPEAT_READFRAME
		framecnt = dhdsdio_readframes(bus, rxlimit, &rxdone, true);
#else
		framecnt = dhdsdio_readframes(bus, rxlimit, &rxdone);
#endif
		if (rxdone || bus->rxskip)
			intstatus  &= ~FRAME_AVAIL_MASK(bus);
		rxlimit -= MIN(framecnt, rxlimit);
	}

	
	bus->intstatus = intstatus;

clkwait:
	if (bus->intr && bus->intdis && !bcmsdh_regfail(sdh)) {
		DHD_INTR(("%s: enable SDIO interrupts, rxdone %d framecnt %d\n",
		          __FUNCTION__, rxdone, framecnt));
		bus->intdis = FALSE;
#if defined(OOB_INTR_ONLY) || defined(BCMSPI_ANDROID)
	bcmsdh_oob_intr_set(1);
#endif 
		bcmsdh_intr_enable(sdh);
	}

#if defined(OOB_INTR_ONLY) && !defined(HW_OOB)
	R_SDREG(newstatus, &regs->intstatus, retries);
	if (bcmsdh_regfail(bus->sdh))
		newstatus = 0;
	if (newstatus & bus->hostintmask) {
		bus->ipend = TRUE;
		resched = TRUE;
	}
#endif 

#ifdef PROP_TXSTATUS
	dhd_wlfc_trigger_pktcommit(bus->dhd);
#endif

	if (TXCTLOK(bus) && bus->ctrl_frame_stat && (bus->clkstate == CLK_AVAIL))  {
		
		int ret;
		uint8* frame_seq = bus->ctrl_frame_buf + SDPCM_FRAMETAG_LEN;

#ifdef BCMSDIOH_TXGLOM
		if (bus->glom_enable)
			frame_seq += SDPCM_HWEXT_LEN;
#endif

		if (*frame_seq != bus->tx_seq) {
			DHD_INFO(("%s IOCTL frame seq lag detected!"
				" frm_seq:%d != bus->tx_seq:%d, corrected\n",
				__FUNCTION__, *frame_seq, bus->tx_seq));
			*frame_seq = bus->tx_seq;
		}

		ret = dhd_bcmsdh_send_buf(bus, bcmsdh_cur_sbwad(sdh), SDIO_FUNC_2, F2SYNC,
		                      (uint8 *)bus->ctrl_frame_buf, (uint32)bus->ctrl_frame_len,
			NULL, NULL, NULL);
		ASSERT(ret != BCME_PENDING);
		if (ret == BCME_NODEVICE) {
			DHD_ERROR(("%s: Device asleep already\n", __FUNCTION__));
		} else if (ret < 0) {
#if 1
			
			DHD_ERROR(("%s: sdio error %d, abort command and terminate frame.\n",
			__FUNCTION__, ret));
			bus->dhd->busstate = DHD_BUS_DOWN;
			dhd_os_send_hang_message(bus->dhd);
#else
			
			DHD_INFO(("%s: sdio error %d, abort command and terminate frame.\n",
			          __FUNCTION__, ret));
			bus->tx_sderrs++;

			bcmsdh_abort(sdh, SDIO_FUNC_2);

			bcmsdh_cfg_write(sdh, SDIO_FUNC_1, SBSDIO_FUNC1_FRAMECTRL,
			                 SFC_WF_TERM, NULL);
			bus->f1regdata++;

			for (i = 0; i < 3; i++) {
				uint8 hi, lo;
				hi = bcmsdh_cfg_read(sdh, SDIO_FUNC_1,
				                     SBSDIO_FUNC1_WFRAMEBCHI, NULL);
				lo = bcmsdh_cfg_read(sdh, SDIO_FUNC_1,
				                     SBSDIO_FUNC1_WFRAMEBCLO, NULL);
				bus->f1regdata += 2;
				if ((hi == 0) && (lo == 0))
					break;
			}
#endif
		}
		if (ret == 0) {
			bus->tx_seq = (bus->tx_seq + 1) % SDPCM_SEQUENCE_WRAP;
		}

		bus->ctrl_frame_stat = FALSE;
		dhd_wait_event_wakeup(bus->dhd);
	}
	
	else if ((bus->clkstate == CLK_AVAIL) && !bus->fcstate &&
	    pktq_mlen(&bus->txq, ~bus->flowcontrol) && txlimit && DATAOK(bus)) {
		framecnt = rxdone ? txlimit : MIN(txlimit, dhd_txminmax);
		framecnt = dhdsdio_sendfromq(bus, framecnt);
		txlimit -= framecnt;
	}
	
	if (bus->ctrl_frame_stat)
		resched = TRUE;

	
	
	if ((bus->dhd->busstate == DHD_BUS_DOWN) || bcmsdh_regfail(sdh)) {
		if (bus->dhd->hang_was_sent || module_remove) {
			resched = 0;
			bus->dpc_sched = resched;
			bus->intstatus = 0;
			goto exit2;
		}
		if ((bus->sih->buscorerev >= 12) && !(dhdsdio_sleepcsr_get(bus) &
			SBSDIO_FUNC1_SLEEPCSR_KSO_MASK)) {
			
			DHD_ERROR(("%s: Bus failed due to KSO\n", __FUNCTION__));
			bus->kso = FALSE;
		} else {
			DHD_ERROR(("%s: failed backplane access over SDIO, halting operation\n",
				__FUNCTION__));
			bus->dhd->busstate = DHD_BUS_DOWN;
			bus->intstatus = 0;
		}
	} else if (bus->clkstate == CLK_PENDING) {
		
	} else if (bus->intstatus || bus->ipend ||
	           (!bus->fcstate && pktq_mlen(&bus->txq, ~bus->flowcontrol) && DATAOK(bus)) ||
			PKT_AVAILABLE(bus, bus->intstatus)) {  
		resched = TRUE;
	}

	bus->dpc_sched = resched;

	
	if ((bus->idletime == DHD_IDLE_IMMEDIATE) && (bus->clkstate != CLK_PENDING)) {
		bus->activity = FALSE;
		dhdsdio_clkctl(bus, CLK_NONE, FALSE);
	}

exit:
#ifdef REPEAT_READFRAME
	if (!resched && dhd_dpcpoll) {
		resched = dhdsdio_readframes(bus, dhd_rxbound, &rxdone, true);
	}
#endif
exit2:
	dhd_os_sdunlock(bus->dhd);
	return resched;
}

int android_wifi_off = 0;
bool
dhd_bus_dpc(struct dhd_bus *bus)
{
	bool resched;

	if (android_wifi_off) {
		printf("wifi turning off, skip!\n");
		return 0;
	}
	
	DHD_TRACE(("Calling dhdsdio_dpc() from %s\n", __FUNCTION__));
	resched = dhdsdio_dpc(bus);

	return resched;
}

void
dhdsdio_isr(void *arg)
{
	dhd_bus_t *bus = (dhd_bus_t*)arg;
	bcmsdh_info_t *sdh;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	if (!bus) {
		DHD_ERROR(("%s : bus is null pointer , exit \n", __FUNCTION__));
		return;
	}
	sdh = bus->sdh;

	if ((bus->dhd->busstate == DHD_BUS_DOWN)&&(!dhd_bus_do_stop)) {
		DHD_ERROR(("%s : bus is down. we have nothing to do\n", __FUNCTION__));
		return;
	}

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	
	bus->intrcount++;
	bus->ipend = TRUE;

	
	if (!SLPAUTO_ENAB(bus)) {
		if (bus->sleeping) {
			DHD_ERROR(("INTERRUPT WHILE SLEEPING??\n"));
			return;
		} else if (!KSO_ENAB(bus)) {
			DHD_ERROR(("ISR in devsleep 1\n"));
		}
	}

	
	if (bus->intr) {
		DHD_INTR(("%s: disable SDIO interrupts\n", __FUNCTION__));
	} else {
		DHD_ERROR(("dhdsdio_isr() w/o interrupt configured!\n"));
	}

	bcmsdh_intr_disable(sdh);
	bus->intdis = TRUE;

#if defined(SDIO_ISR_THREAD)
	DHD_TRACE(("Calling dhdsdio_dpc() from %s\n", __FUNCTION__));
	DHD_OS_WAKE_LOCK(bus->dhd);
	while (dhdsdio_dpc(bus));
	DHD_OS_WAKE_UNLOCK(bus->dhd);
#else

	bus->dpc_sched = TRUE;
	dhd_sched_dpc(bus->dhd);

#endif 

}

#ifdef SDTEST
static void
dhdsdio_pktgen_init(dhd_bus_t *bus)
{
	
	if (dhd_pktgen_len) {
		bus->pktgen_maxlen = MIN(dhd_pktgen_len, MAX_PKTGEN_LEN);
		bus->pktgen_minlen = bus->pktgen_maxlen;
	} else {
		bus->pktgen_maxlen = MAX_PKTGEN_LEN;
		bus->pktgen_minlen = 0;
	}
	bus->pktgen_len = (uint16)bus->pktgen_minlen;

	
	bus->pktgen_freq = 1;
	bus->pktgen_print = dhd_watchdog_ms ? (10000 / dhd_watchdog_ms) : 0;
	bus->pktgen_count = (dhd_pktgen * dhd_watchdog_ms + 999) / 1000;

	
	bus->pktgen_mode = DHD_PKTGEN_ECHO;
	bus->pktgen_stop = 1;
}

static void
dhdsdio_pktgen(dhd_bus_t *bus)
{
	void *pkt;
	uint8 *data;
	uint pktcount;
	uint fillbyte;
	osl_t *osh = bus->dhd->osh;
	uint16 len;
	ulong time_lapse;
	uint sent_pkts;
	uint rcvd_pkts;

	
	if (bus->pktgen_print && (++bus->pktgen_ptick >= bus->pktgen_print)) {
		bus->pktgen_ptick = 0;
		printf("%s: send attempts %d, rcvd %d, errors %d\n",
		       __FUNCTION__, bus->pktgen_sent, bus->pktgen_rcvd, bus->pktgen_fail);

		
		if (bus->pktgen_minlen == bus->pktgen_maxlen) {
			time_lapse = jiffies - bus->pktgen_prev_time;
			bus->pktgen_prev_time = jiffies;
			sent_pkts = bus->pktgen_sent - bus->pktgen_prev_sent;
			bus->pktgen_prev_sent = bus->pktgen_sent;
			rcvd_pkts = bus->pktgen_rcvd - bus->pktgen_prev_rcvd;
			bus->pktgen_prev_rcvd = bus->pktgen_rcvd;

			printf("%s: Tx Throughput %d kbps, Rx Throughput %d kbps\n",
			  __FUNCTION__,
			  (sent_pkts * bus->pktgen_len / jiffies_to_msecs(time_lapse)) * 8,
			  (rcvd_pkts * bus->pktgen_len  / jiffies_to_msecs(time_lapse)) * 8);
		}
	}

	
	if (bus->pktgen_mode == DHD_PKTGEN_RECV) {
		if (bus->pktgen_rcv_state == PKTGEN_RCV_IDLE) {
			bus->pktgen_rcv_state = PKTGEN_RCV_ONGOING;
			dhdsdio_sdtest_set(bus, bus->pktgen_total);
		}
		return;
	}

	
	for (pktcount = 0; pktcount < bus->pktgen_count; pktcount++) {
		
		if (bus->pktgen_total && (bus->pktgen_sent >= bus->pktgen_total)) {
			bus->pktgen_count = 0;
			break;
		}

		
		if (bus->pktgen_mode == DHD_PKTGEN_RXBURST) {
			len = SDPCM_TEST_PKT_CNT_FLD_LEN;
		} else {
			len = bus->pktgen_len;
		}
		if (!(pkt = PKTGET(osh, (len + SDPCM_HDRLEN + SDPCM_TEST_HDRLEN + DHD_SDALIGN),
		                   TRUE))) {;
			DHD_ERROR(("%s: PKTGET failed!\n", __FUNCTION__));
			break;
		}
		PKTALIGN(osh, pkt, (len + SDPCM_HDRLEN + SDPCM_TEST_HDRLEN), DHD_SDALIGN);
		data = (uint8*)PKTDATA(osh, pkt) + SDPCM_HDRLEN;

		
		switch (bus->pktgen_mode) {
		case DHD_PKTGEN_ECHO:
			*data++ = SDPCM_TEST_ECHOREQ;
			*data++ = (uint8)bus->pktgen_sent;
			break;

		case DHD_PKTGEN_SEND:
			*data++ = SDPCM_TEST_DISCARD;
			*data++ = (uint8)bus->pktgen_sent;
			break;

		case DHD_PKTGEN_RXBURST:
			*data++ = SDPCM_TEST_BURST;
			*data++ = (uint8)bus->pktgen_count; 
			break;

		default:
			DHD_ERROR(("Unrecognized pktgen mode %d\n", bus->pktgen_mode));
			PKTFREE(osh, pkt, TRUE);
			bus->pktgen_count = 0;
			return;
		}

		
		*data++ = (bus->pktgen_len >> 0);
		*data++ = (bus->pktgen_len >> 8);

		if (bus->pktgen_mode == DHD_PKTGEN_RXBURST) {
			*data++ = (uint8)(bus->pktgen_count >> 0);
			*data++ = (uint8)(bus->pktgen_count >> 8);
			*data++ = (uint8)(bus->pktgen_count >> 16);
			*data++ = (uint8)(bus->pktgen_count >> 24);
		} else {

			
			for (fillbyte = 0; fillbyte < len; fillbyte++)
				*data++ = SDPCM_TEST_FILL(fillbyte, (uint8)bus->pktgen_sent);
		}

#ifdef DHD_DEBUG
		if (DHD_BYTES_ON() && DHD_DATA_ON()) {
			data = (uint8*)PKTDATA(osh, pkt) + SDPCM_HDRLEN;
			prhex("dhdsdio_pktgen: Tx Data", data, PKTLEN(osh, pkt) - SDPCM_HDRLEN);
		}
#endif

		
		if (dhdsdio_txpkt(bus, pkt, SDPCM_TEST_CHANNEL, TRUE, FALSE)) {
			bus->pktgen_fail++;
			if (bus->pktgen_stop && bus->pktgen_stop == bus->pktgen_fail)
				bus->pktgen_count = 0;
		}
		bus->pktgen_sent++;

		
		if (++bus->pktgen_len > bus->pktgen_maxlen)
			bus->pktgen_len = (uint16)bus->pktgen_minlen;

		
		if (bus->pktgen_mode == DHD_PKTGEN_RXBURST)
			break;
	}
}

static void
dhdsdio_sdtest_set(dhd_bus_t *bus, uint count)
{
	void *pkt;
	uint8 *data;
	osl_t *osh = bus->dhd->osh;

	
	if (!(pkt = PKTGET(osh, SDPCM_HDRLEN + SDPCM_TEST_HDRLEN +
		SDPCM_TEST_PKT_CNT_FLD_LEN + DHD_SDALIGN, TRUE))) {
		DHD_ERROR(("%s: PKTGET failed!\n", __FUNCTION__));
		return;
	}
	PKTALIGN(osh, pkt, (SDPCM_HDRLEN + SDPCM_TEST_HDRLEN +
		SDPCM_TEST_PKT_CNT_FLD_LEN), DHD_SDALIGN);
	data = (uint8*)PKTDATA(osh, pkt) + SDPCM_HDRLEN;

	
	*data++ = SDPCM_TEST_SEND;
	*data++ = (count > 0)?TRUE:FALSE;
	*data++ = (bus->pktgen_maxlen >> 0);
	*data++ = (bus->pktgen_maxlen >> 8);
	*data++ = (uint8)(count >> 0);
	*data++ = (uint8)(count >> 8);
	*data++ = (uint8)(count >> 16);
	*data++ = (uint8)(count >> 24);

	
	if (dhdsdio_txpkt(bus, pkt, SDPCM_TEST_CHANNEL, TRUE, FALSE))
		bus->pktgen_fail++;
}


static void
dhdsdio_testrcv(dhd_bus_t *bus, void *pkt, uint seq)
{
	osl_t *osh = bus->dhd->osh;
	uint8 *data;
	uint pktlen;

	uint8 cmd;
	uint8 extra;
	uint16 len;
	uint16 offset;

	
	if ((pktlen = PKTLEN(osh, pkt)) < SDPCM_TEST_HDRLEN) {
		DHD_ERROR(("dhdsdio_restrcv: toss runt frame, pktlen %d\n", pktlen));
		PKTFREE(osh, pkt, FALSE);
		return;
	}

	
	data = PKTDATA(osh, pkt);
	cmd = *data++;
	extra = *data++;
	len = *data++; len += *data++ << 8;
	DHD_TRACE(("%s:cmd:%d, xtra:%d,len:%d\n", __FUNCTION__, cmd, extra, len));
	
	if (cmd == SDPCM_TEST_DISCARD || cmd == SDPCM_TEST_ECHOREQ || cmd == SDPCM_TEST_ECHORSP) {
		if (pktlen != len + SDPCM_TEST_HDRLEN) {
			DHD_ERROR(("dhdsdio_testrcv: frame length mismatch, pktlen %d seq %d"
			           " cmd %d extra %d len %d\n", pktlen, seq, cmd, extra, len));
			PKTFREE(osh, pkt, FALSE);
			return;
		}
	}

	
	switch (cmd) {
	case SDPCM_TEST_ECHOREQ:
		
		*(uint8 *)(PKTDATA(osh, pkt)) = SDPCM_TEST_ECHORSP;
		if (dhdsdio_txpkt(bus, pkt, SDPCM_TEST_CHANNEL, TRUE, FALSE) == 0) {
			bus->pktgen_sent++;
		} else {
			bus->pktgen_fail++;
			PKTFREE(osh, pkt, FALSE);
		}
		bus->pktgen_rcvd++;
		break;

	case SDPCM_TEST_ECHORSP:
		if (bus->ext_loop) {
			PKTFREE(osh, pkt, FALSE);
			bus->pktgen_rcvd++;
			break;
		}

		for (offset = 0; offset < len; offset++, data++) {
			if (*data != SDPCM_TEST_FILL(offset, extra)) {
				DHD_ERROR(("dhdsdio_testrcv: echo data mismatch: "
				           "offset %d (len %d) expect 0x%02x rcvd 0x%02x\n",
				           offset, len, SDPCM_TEST_FILL(offset, extra), *data));
				break;
			}
		}
		PKTFREE(osh, pkt, FALSE);
		bus->pktgen_rcvd++;
		break;

	case SDPCM_TEST_DISCARD:
		{
			int i = 0;
			uint8 *prn = data;
			uint8 testval = extra;
			for (i = 0; i < len; i++) {
				if (*prn != testval) {
					DHD_ERROR(("DIErr@Pkt#:%d,Ix:%d, expected:0x%x, got:0x%x\n",
						i, bus->pktgen_rcvd_rcvsession, testval, *prn));
					prn++; testval++;
				}
			}
		}
		PKTFREE(osh, pkt, FALSE);
		bus->pktgen_rcvd++;
		break;

	case SDPCM_TEST_BURST:
	case SDPCM_TEST_SEND:
	default:
		DHD_INFO(("dhdsdio_testrcv: unsupported or unknown command, pktlen %d seq %d"
		          " cmd %d extra %d len %d\n", pktlen, seq, cmd, extra, len));
		PKTFREE(osh, pkt, FALSE);
		break;
	}

	
	if (bus->pktgen_mode == DHD_PKTGEN_RECV) {
		if (bus->pktgen_rcv_state != PKTGEN_RCV_IDLE) {
			bus->pktgen_rcvd_rcvsession++;

			if (bus->pktgen_total &&
				(bus->pktgen_rcvd_rcvsession >= bus->pktgen_total)) {
			bus->pktgen_count = 0;
			DHD_ERROR(("Pktgen:rcv test complete!\n"));
			bus->pktgen_rcv_state = PKTGEN_RCV_IDLE;
			dhdsdio_sdtest_set(bus, FALSE);
				bus->pktgen_rcvd_rcvsession = 0;
			}
		}
	}
}
#endif 

extern void
dhd_disable_intr(dhd_pub_t *dhdp)
{
	dhd_bus_t *bus;
	bus = dhdp->bus;
	bcmsdh_intr_disable(bus->sdh);
}

extern bool
dhd_bus_watchdog(dhd_pub_t *dhdp)
{
	dhd_bus_t *bus;

	DHD_TIMER(("%s: Enter\n", __FUNCTION__));

	bus = dhdp->bus;

	if (bus->dhd->dongle_reset)
		return FALSE;

	
	if (!SLPAUTO_ENAB(bus) && bus->sleeping)
		return FALSE;

	if (dhdp->busstate == DHD_BUS_DOWN)
		return FALSE;

	
	if (!SLPAUTO_ENAB(bus) && (bus->poll && (++bus->polltick >= bus->pollrate))) {
		uint32 intstatus = 0;

		
		bus->polltick = 0;

		
		if (!bus->intr || (bus->intrcount == bus->lastintrs)) {

			if (!bus->dpc_sched) {
				uint8 devpend;
				devpend = bcmsdh_cfg_read(bus->sdh, SDIO_FUNC_0,
				                          SDIOD_CCCR_INTPEND, NULL);
				intstatus = devpend & (INTR_STATUS_FUNC1 | INTR_STATUS_FUNC2);
			}

			
			if (intstatus) {
				bus->pollcnt++;
				bus->ipend = TRUE;
				if (bus->intr) {
					bcmsdh_intr_disable(bus->sdh);
				}
				bus->dpc_sched = TRUE;
				dhd_sched_dpc(bus->dhd);

			}
		}

		
		bus->lastintrs = bus->intrcount;
	}

#ifdef DHD_DEBUG
	
	if (dhdp->busstate == DHD_BUS_DATA && dhd_console_ms != 0) {
		bus->console.count += dhd_watchdog_ms;
		if (bus->console.count >= dhd_console_ms) {
			bus->console.count -= dhd_console_ms;
			
			if (SLPAUTO_ENAB(bus))
				dhdsdio_bussleep(bus, FALSE);
			else
			dhdsdio_clkctl(bus, CLK_AVAIL, FALSE);
			if (dhdsdio_readconsole(bus) < 0)
				dhd_console_ms = 0;	
		}
	}
#endif 

#ifdef SDTEST
	
	if (bus->pktgen_count && (++bus->pktgen_tick >= bus->pktgen_freq)) {
		
		if (SLPAUTO_ENAB(bus))
			dhdsdio_bussleep(bus, FALSE);
		else
			dhdsdio_clkctl(bus, CLK_AVAIL, FALSE);
		bus->pktgen_tick = 0;
		dhdsdio_pktgen(bus);
	}
#endif

	
#ifdef DHD_USE_IDLECOUNT
	if (bus->activity)
		bus->activity = FALSE;
	else {
		bus->idlecount++;

		if (bus->idlecount >= bus->idletime) {
			DHD_TIMER(("%s: DHD Idle state!!\n", __FUNCTION__));
			if (SLPAUTO_ENAB(bus)) {
				if (dhdsdio_bussleep(bus, TRUE) != BCME_BUSY)
					dhd_os_wd_timer(bus->dhd, 0);
			} else
				dhdsdio_clkctl(bus, CLK_NONE, FALSE);

			bus->idlecount = 0;
		}
	}
#else
	if ((bus->idletime > 0) && (bus->clkstate == CLK_AVAIL)) {
		if (++bus->idlecount > bus->idletime) {
			bus->idlecount = 0;
			if (bus->activity) {
				bus->activity = FALSE;
				if (SLPAUTO_ENAB(bus)) {
					if (!bus->readframes)
						dhdsdio_bussleep(bus, TRUE);
					else
						bus->reqbussleep = TRUE;
				}
				else
					dhdsdio_clkctl(bus, CLK_NONE, FALSE);
			}
		}
	}
#endif 

	return bus->ipend;
}

#ifdef DHD_DEBUG
extern int
dhd_bus_console_in(dhd_pub_t *dhdp, uchar *msg, uint msglen)
{
	dhd_bus_t *bus = dhdp->bus;
	uint32 addr, val;
	int rv;
	void *pkt;

	
	if (bus->console_addr == 0)
		return BCME_UNSUPPORTED;

	
	dhd_os_sdlock(bus->dhd);

	
	if (bus->dhd->dongle_reset) {
		dhd_os_sdunlock(bus->dhd);
		return BCME_NOTREADY;
	}

	
	BUS_WAKE(bus);
	
	dhdsdio_clkctl(bus, CLK_AVAIL, FALSE);

	
	addr = bus->console_addr + OFFSETOF(hndrte_cons_t, cbuf_idx);
	val = htol32(0);
	if ((rv = dhdsdio_membytes(bus, TRUE, addr, (uint8 *)&val, sizeof(val))) < 0)
		goto done;

	
	addr = bus->console_addr + OFFSETOF(hndrte_cons_t, cbuf);
	if ((rv = dhdsdio_membytes(bus, TRUE, addr, (uint8 *)msg, msglen)) < 0)
		goto done;

	
	addr = bus->console_addr + OFFSETOF(hndrte_cons_t, vcons_in);
	val = htol32(msglen);
	if ((rv = dhdsdio_membytes(bus, TRUE, addr, (uint8 *)&val, sizeof(val))) < 0)
		goto done;

	if ((pkt = PKTGET(bus->dhd->osh, 4 + SDPCM_RESERVE, TRUE)) != NULL)
		dhdsdio_txpkt(bus, pkt, SDPCM_EVENT_CHANNEL, TRUE, FALSE);

done:
	if ((bus->idletime == DHD_IDLE_IMMEDIATE) && !bus->dpc_sched) {
		bus->activity = FALSE;
		dhdsdio_clkctl(bus, CLK_NONE, TRUE);
	}

	dhd_os_sdunlock(bus->dhd);

	return rv;
}
#endif 

#ifdef DHD_DEBUG
static void
dhd_dump_cis(uint fn, uint8 *cis)
{
	uint byte, tag, tdata;
	DHD_INFO(("Function %d CIS:\n", fn));

	for (tdata = byte = 0; byte < SBSDIO_CIS_SIZE_LIMIT; byte++) {
		if ((byte % 16) == 0)
			DHD_INFO(("    "));
		DHD_INFO(("%02x ", cis[byte]));
		if ((byte % 16) == 15)
			DHD_INFO(("\n"));
		if (!tdata--) {
			tag = cis[byte];
			if (tag == 0xff)
				break;
			else if (!tag)
				tdata = 0;
			else if ((byte + 1) < SBSDIO_CIS_SIZE_LIMIT)
				tdata = cis[byte + 1] + 1;
			else
				DHD_INFO(("]"));
		}
	}
	if ((byte % 16) != 15)
		DHD_INFO(("\n"));
}
#endif 

static bool
dhdsdio_chipmatch(uint16 chipid)
{
	if (chipid == BCM4325_CHIP_ID)
		return TRUE;
	if (chipid == BCM4329_CHIP_ID)
		return TRUE;
	if (chipid == BCM4315_CHIP_ID)
		return TRUE;
	if (chipid == BCM4319_CHIP_ID)
		return TRUE;
	if (chipid == BCM4336_CHIP_ID)
		return TRUE;
	if (chipid == BCM4330_CHIP_ID)
		return TRUE;
	if (chipid == BCM43237_CHIP_ID)
		return TRUE;
	if (chipid == BCM43362_CHIP_ID)
		return TRUE;
	if (chipid == BCM4314_CHIP_ID)
		return TRUE;
	if (chipid == BCM43242_CHIP_ID)
		return TRUE;
	if (chipid == BCM43341_CHIP_ID)
		return TRUE;
	if (chipid == BCM43143_CHIP_ID)
		return TRUE;
	if (chipid == BCM43342_CHIP_ID)
		return TRUE;
	if (chipid == BCM4334_CHIP_ID)
		return TRUE;
	if (chipid == BCM43239_CHIP_ID)
		return TRUE;
	if (chipid == BCM4324_CHIP_ID)
		return TRUE;
	if (chipid == BCM4335_CHIP_ID)
		return TRUE;
	if (chipid == BCM4350_CHIP_ID)
		return TRUE;
	return FALSE;
}

static void *
dhdsdio_probe(uint16 venid, uint16 devid, uint16 bus_no, uint16 slot,
	uint16 func, uint bustype, void *regsva, osl_t * osh, void *sdh)
{
	int ret;
	dhd_bus_t *bus;
#ifdef GET_CUSTOM_MAC_ENABLE
	struct ether_addr ea_addr;
#endif 

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 25))
	if (mutex_is_locked(&_dhd_sdio_mutex_lock_) == 0) {
		DHD_ERROR(("%s : no mutex held. set lock\n", __FUNCTION__));
	}
	else {
		DHD_ERROR(("%s : mutex is locked!. wait for unlocking\n", __FUNCTION__));
	}
	mutex_lock(&_dhd_sdio_mutex_lock_);
#endif 

	dhd_txbound = DHD_TXBOUND;
	dhd_rxbound = DHD_RXBOUND;
	dhd_alignctl = TRUE;
	sd1idle = TRUE;
	dhd_readahead = TRUE;
	retrydata = FALSE;
#ifndef REPEAT_READFRAME
	dhd_doflow = FALSE;
#else
	dhd_doflow = TRUE;
#endif
	dhd_dongle_memsize = 0;
	dhd_txminmax = DHD_TXMINMAX;

	forcealign = TRUE;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));
	DHD_INFO(("%s: venid 0x%04x devid 0x%04x\n", __FUNCTION__, venid, devid));

	
	ASSERT((uintptr)regsva == SI_ENUM_BASE);

	
	switch (venid) {
		case 0x0000:
		case VENDOR_BROADCOM:
			break;
		default:
			DHD_ERROR(("%s: unknown vendor: 0x%04x\n",
			           __FUNCTION__, venid));
			goto forcereturn;
	}

	
	switch (devid) {
		case BCM4325_D11DUAL_ID:		
		case BCM4325_D11G_ID:			
		case BCM4325_D11A_ID:			
			DHD_INFO(("%s: found 4325 Dongle\n", __FUNCTION__));
			break;
		case BCM4329_D11N_ID:		
		case BCM4329_D11N2G_ID:		
		case BCM4329_D11N5G_ID:		
		case 0x4329:
			DHD_INFO(("%s: found 4329 Dongle\n", __FUNCTION__));
			break;
		case BCM4315_D11DUAL_ID:		
		case BCM4315_D11G_ID:			
		case BCM4315_D11A_ID:			
			DHD_INFO(("%s: found 4315 Dongle\n", __FUNCTION__));
			break;
		case BCM4319_D11N_ID:			
		case BCM4319_D11N2G_ID:			
		case BCM4319_D11N5G_ID:			
			DHD_INFO(("%s: found 4319 Dongle\n", __FUNCTION__));
			break;
		case 0:
			DHD_INFO(("%s: allow device id 0, will check chip internals\n",
			          __FUNCTION__));
			break;

		default:
			DHD_ERROR(("%s: skipping 0x%04x/0x%04x, not a dongle\n",
			           __FUNCTION__, venid, devid));
			goto forcereturn;
	}

	if (osh == NULL) {
		
		if (!(osh = dhd_osl_attach(sdh, DHD_BUS))) {
			DHD_ERROR(("%s: osl_attach failed!\n", __FUNCTION__));
			goto forcereturn;
		}
	}

	
	if (!(bus = MALLOC(osh, sizeof(dhd_bus_t)))) {
		DHD_ERROR(("%s: MALLOC of dhd_bus_t failed\n", __FUNCTION__));
		goto fail;
	}
	bzero(bus, sizeof(dhd_bus_t));
	bus->sdh = sdh;
	bus->cl_devid = (uint16)devid;
	bus->bus = DHD_BUS;
	bus->tx_seq = SDPCM_SEQUENCE_WRAP - 1;
	bus->usebufpool = FALSE; 

	
	dhd_common_init(osh);

	
	if (!(dhdsdio_probe_attach(bus, osh, sdh, regsva, devid))) {
		DHD_ERROR(("%s: dhdsdio_probe_attach failed\n", __FUNCTION__));
		goto fail;
	}

	
	if (!(bus->dhd = dhd_attach(osh, bus, SDPCM_RESERVE))) {
		DHD_ERROR(("%s: dhd_attach failed\n", __FUNCTION__));
		goto fail;
	}

	
	if (!(dhdsdio_probe_malloc(bus, osh, sdh))) {
		DHD_ERROR(("%s: dhdsdio_probe_malloc failed\n", __FUNCTION__));
		goto fail;
	}

	if (!(dhdsdio_probe_init(bus, osh, sdh))) {
		DHD_ERROR(("%s: dhdsdio_probe_init failed\n", __FUNCTION__));
		goto fail;
	}

	if (bus->intr) {
		
		DHD_INTR(("%s: disable SDIO interrupts (not interested yet)\n", __FUNCTION__));
		bcmsdh_intr_disable(sdh);
		if ((ret = bcmsdh_intr_reg(sdh, dhdsdio_isr, bus)) != 0) {
			DHD_ERROR(("%s: FAILED: bcmsdh_intr_reg returned %d\n",
			           __FUNCTION__, ret));
			goto fail;
		}
		DHD_INTR(("%s: registered SDIO interrupt function ok\n", __FUNCTION__));
	} else {
		DHD_INFO(("%s: SDIO interrupt function is NOT registered due to polling mode\n",
		           __FUNCTION__));
	}

	DHD_INFO(("%s: completed!!\n", __FUNCTION__));

#ifdef GET_CUSTOM_MAC_ENABLE
	
	memset(&ea_addr, 0, sizeof(ea_addr));
	ret = dhd_custom_get_mac_address(ea_addr.octet);
	if (!ret) {
		memcpy(bus->dhd->mac.octet, (void *)&ea_addr, ETHER_ADDR_LEN);
	}
#endif 

	
	if (dhd_download_fw_on_driverload) {
		if ((ret = dhd_bus_start(bus->dhd)) != 0) {
			DHD_ERROR(("%s: dhd_bus_start failed\n", __FUNCTION__));
				goto fail;
		}
	}
	
	if (dhd_net_attach(bus->dhd, 0) != 0) {
		DHD_ERROR(("%s: Net attach failed!!\n", __FUNCTION__));
		goto fail;
	}

	

#if defined(CUSTOMER_HW4) && defined(BCMHOST_XTAL_PU_TIME_MOD)
	bcmsdh_reg_write(bus->sdh, 0x18000620, 2, 11);
	bcmsdh_reg_write(bus->sdh, 0x18000628, 4, 0x00F80001);
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 25))
	mutex_unlock(&_dhd_sdio_mutex_lock_);
	DHD_ERROR(("%s : the lock is released.\n", __FUNCTION__));
#endif 

	return bus;

fail:
	dhdsdio_release(bus, osh);

forcereturn:
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 25))
	mutex_unlock(&_dhd_sdio_mutex_lock_);
	DHD_ERROR(("%s : the lock is released.\n", __FUNCTION__));
#endif 

	return NULL;
}

#ifdef REGON_BP_HANG_FIX
static int dhd_sdio_backplane_reset(struct dhd_bus *bus)
{
	uint32 temp = 0;
	DHD_ERROR(("Resetting  the backplane to avoid failure in firmware download..\n"));

	temp = bcmsdh_reg_read(bus->sdh, 0x180021e0, 4);
	DHD_INFO(("SDIO Clk Control Reg = %x\n", temp));

	
	bcmsdh_reg_write(bus->sdh, 0x18000644, 4, 0x6000005);

	
	bcmsdh_reg_write(bus->sdh, 0x18000630, 4, 0xC8FFC8);

	
	bcmsdh_reg_write(bus->sdh, 0x180021e0, 4, 0x41);

	
	bcmsdh_reg_write(bus->sdh, 0x18004400, 4, 0xf92f1);
	
	bcmsdh_reg_write(bus->sdh, 0x18000650, 4, 0x3);
	bcmsdh_reg_write(bus->sdh, 0x18000654, 4, 0x0);

	
	bcmsdh_reg_write(bus->sdh, 0x18004400, 4, 0xf9af1);

	
	bcmsdh_reg_write(bus->sdh, 0x18004408, 4, 0x0);

	
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0xc0002000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x80008000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x1051f080);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x80008000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x1050f080);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x80008000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x1050f080);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x80008000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x1050f080);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000004);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x30a00000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000604);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x30a00000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00001604);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x30a00000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00001404);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x30a08c80);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00010001);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x14a00000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00011404);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x30a00000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00002000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x04a00000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00002000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0xf8000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00002000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x04a00000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00002000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0xf8000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00011604);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x30a00000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00010604);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x30a00000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00010004);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x30a00000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00010000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x14a00000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000004);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x30a00000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00010001);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x14a00000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00010004);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x30a00000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00010000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x30a00000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00010000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x14a00000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x30a00000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000008);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x04a00000);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0x00000008);
	bcmsdh_reg_write(bus->sdh, 0x1800440c, 4, 0xfc000000);
	

	
	bcmsdh_reg_write(bus->sdh, 0x18004400, 4, 0xf92f1);

	
	bcmsdh_reg_write(bus->sdh, 0x18000650, 4, 0x3);
	bcmsdh_reg_write(bus->sdh, 0x18000654, 4, 0x0);
	bcmsdh_reg_write(bus->sdh, 0x18000650, 4, 0x3);
	bcmsdh_reg_write(bus->sdh, 0x18000654, 4, 0x2);
	bcmsdh_reg_write(bus->sdh, 0x18000650, 4, 0x3);
	bcmsdh_reg_write(bus->sdh, 0x18000654, 4, 0x3);
	bcmsdh_reg_write(bus->sdh, 0x18000650, 4, 0x3);
	bcmsdh_reg_write(bus->sdh, 0x18000654, 4, 0x37);
	bcmsdh_reg_write(bus->sdh, 0x18000650, 4, 0x3);
	temp = bcmsdh_reg_read(bus->sdh, 0x18000654, 4);
	DHD_INFO(("0x18000654 = %x\n", temp));
	bcmsdh_reg_write(bus->sdh, 0x18000654, 4, 0x800037);
	OSL_DELAY(100000);
	
	bcmsdh_reg_write(bus->sdh, 0x18000644, 4, 0x0);
	bcmsdh_reg_write(bus->sdh, 0x18000630, 4, 0xC800C8);
	
	bcmsdh_reg_write(bus->sdh, 0x180021e0, 4, 0x40);
	OSL_DELAY(10000);
	return TRUE;
}

static int dhdsdio_sdio_hang_war(struct dhd_bus *bus)
{
	uint32 temp = 0, temp2 = 0, counter = 0, BT_pwr_up = 0, BT_ready = 0;
	
	bcmsdh_reg_write(bus->sdh, 0x18101408, 4, 0x3);
	bcmsdh_reg_write(bus->sdh, 0x18101800, 4, 0x0);
	bcmsdh_reg_write(bus->sdh, 0x18101408, 4, 0x1);
	
	bcmsdh_reg_write(bus->sdh, 0x180013D8, 2, 0xD1);
	bcmsdh_reg_write(bus->sdh, 0x180013DA, 2, 0x12);
	bcmsdh_reg_write(bus->sdh, 0x180013D8, 2, 0x2D0);
	
	temp = bcmsdh_reg_read(bus->sdh, 0x180013DA, 2);
	
	temp2 = bcmsdh_reg_read(bus->sdh, 0x1800002C, 4);
	(temp & 0xF0) ? (BT_pwr_up = 1):(BT_pwr_up = 0);
	(temp2 & (1<<17)) ? (BT_ready = 1):(BT_ready = 0);
	DHD_ERROR(("WARNING: Checking if BT is ready BT_pwr_up = %x"
		"BT_ready = %x \n", BT_pwr_up, BT_ready));
	while (BT_pwr_up && !BT_ready)
	{
		OSL_DELAY(1000);
		bcmsdh_reg_write(bus->sdh, 0x180013D8, 2, 0x2D0);
		temp = bcmsdh_reg_read(bus->sdh, 0x180013DA, 2);
		temp2 = bcmsdh_reg_read(bus->sdh, 0x1800002C, 4);
		(temp & 0xF0) ? (BT_pwr_up = 1):(BT_pwr_up = 0);
		(temp2 & (1<<17)) ? (BT_ready = 1):(BT_ready = 0);
		counter++;
		if (counter == 5000)
		{
			DHD_ERROR(("WARNING: Going ahead after 5 secs with"
					"risk of failure because BT ready is not yet set\n"));
			break;
		}
	}
	DHD_ERROR(("\nWARNING: WL Proceeding BT_pwr_up = %x BT_ready = %x"
			"\n", BT_pwr_up, BT_ready));
	counter = 0;
	OSL_DELAY(10000);
	DHD_TRACE(("%d: Read Value @ 0x18104808 = %x."
			"\n", __LINE__, bcmsdh_reg_read(bus->sdh, 0x18104808, 4)));
	DHD_TRACE(("%d: Read Value @ 0x1810480C = %x."
			"\n", __LINE__, bcmsdh_reg_read(bus->sdh, 0x1810480C, 4)));
	DHD_TRACE(("%d: Read Value @ 0x18106808 = %x."
			"\n", __LINE__, bcmsdh_reg_read(bus->sdh, 0x18106808, 4)));
	DHD_TRACE(("%d: Read Value @ 0x1810680C = %x."
			"\n", __LINE__, bcmsdh_reg_read(bus->sdh, 0x1810680C, 4)));
	DHD_TRACE(("%d: Read Value @ 0x18107808 = %x."
			"\n", __LINE__, bcmsdh_reg_read(bus->sdh, 0x18107808, 4)));
	DHD_TRACE(("%d: Read Value @ 0x1810780C = %x."
			"\n", __LINE__, bcmsdh_reg_read(bus->sdh, 0x1810780C, 4)));
	DHD_TRACE(("%d: Read Value @ 0x18108808 = %x."
			"\n", __LINE__, bcmsdh_reg_read(bus->sdh, 0x18108808, 4)));
	DHD_TRACE(("%d: Read Value @ 0x1810880C = %x."
			"\n", __LINE__, bcmsdh_reg_read(bus->sdh, 0x1810880C, 4)));
	DHD_TRACE(("%d: Read Value @ 0x18109808 = %x."
			"\n", __LINE__, bcmsdh_reg_read(bus->sdh, 0x18109808, 4)));
	DHD_TRACE(("%d: Read Value @ 0x1810980C = %x."
			"\n", __LINE__, bcmsdh_reg_read(bus->sdh, 0x1810980C, 4)));
	DHD_TRACE(("%d: Read Value @ 0x1810C808 = %x."
			"\n", __LINE__, bcmsdh_reg_read(bus->sdh, 0x1810c808, 4)));
	DHD_TRACE(("%d: Read Value @ 0x1810C80C = %x."
			"\n", __LINE__, bcmsdh_reg_read(bus->sdh, 0x1810c80C, 4)));
	counter = 0;
	while ((bcmsdh_reg_read(bus->sdh, 0x18104808, 4) == 5) ||
		(bcmsdh_reg_read(bus->sdh, 0x1810480C, 4) == 5) ||
		(bcmsdh_reg_read(bus->sdh, 0x18106808, 4) == 5) ||
		(bcmsdh_reg_read(bus->sdh, 0x1810680C, 4) == 5) ||
		(bcmsdh_reg_read(bus->sdh, 0x1810780C, 4) == 5) ||
		(bcmsdh_reg_read(bus->sdh, 0x1810780C, 4) == 5) ||
		(bcmsdh_reg_read(bus->sdh, 0x1810880C, 4) == 5) ||
		(bcmsdh_reg_read(bus->sdh, 0x1810880C, 4) == 5) ||
		(bcmsdh_reg_read(bus->sdh, 0x1810980C, 4) == 5) ||
		(bcmsdh_reg_read(bus->sdh, 0x1810980C, 4) == 5) ||
		(bcmsdh_reg_read(bus->sdh, 0x1810C80C, 4) == 5) ||
		(bcmsdh_reg_read(bus->sdh, 0x1810C80C, 4) == 5))
	{
		if (++counter > 10)
		{
			DHD_ERROR(("Unable to recover the backkplane corruption"
					"..Tried %d times.. Exiting\n", counter));
			break;
		}
		OSL_DELAY(10000);
		dhd_sdio_backplane_reset(bus);
		DHD_ERROR(("%d: Read Value @ 0x18104808 = %x."
				"\n", __LINE__, bcmsdh_reg_read(bus->sdh, 0x18104808, 4)));
		DHD_ERROR(("%d: Read Value @ 0x1810480C = %x."
				"\n", __LINE__, bcmsdh_reg_read(bus->sdh, 0x1810480C, 4)));
		DHD_ERROR(("%d: Read Value @ 0x18106808 = %x."
				"\n", __LINE__, bcmsdh_reg_read(bus->sdh, 0x18106808, 4)));
		DHD_ERROR(("%d: Read Value @ 0x1810680C = %x."
				"\n", __LINE__, bcmsdh_reg_read(bus->sdh, 0x1810680C, 4)));
		DHD_ERROR(("%d: Read Value @ 0x18107808 = %x."
				"\n", __LINE__, bcmsdh_reg_read(bus->sdh, 0x18107808, 4)));
		DHD_ERROR(("%d: Read Value @ 0x1810780C = %x."
				"\n", __LINE__, bcmsdh_reg_read(bus->sdh, 0x1810780C, 4)));
		DHD_ERROR(("%d: Read Value @ 0x18108808 = %x."
				"\n", __LINE__, bcmsdh_reg_read(bus->sdh, 0x18108808, 4)));
		DHD_ERROR(("%d: Read Value @ 0x1810880C = %x."
				"\n", __LINE__, bcmsdh_reg_read(bus->sdh, 0x1810880C, 4)));
		DHD_ERROR(("%d: Read Value @ 0x18109808 = %x."
				"\n", __LINE__, bcmsdh_reg_read(bus->sdh, 0x18109808, 4)));
		DHD_ERROR(("%d: Read Value @ 0x1810980C = %x."
				"\n", __LINE__, bcmsdh_reg_read(bus->sdh, 0x1810980C, 4)));
		DHD_ERROR(("%d: Read Value @ 0x1810C808 = %x."
				"\n", __LINE__, bcmsdh_reg_read(bus->sdh, 0x1810c808, 4)));
		DHD_ERROR(("%d: Read Value @ 0x1810C80C = %x."
				"\n", __LINE__, bcmsdh_reg_read(bus->sdh, 0x1810c80C, 4)));
	}
	
	DHD_ERROR(("Setting up AXI_OK\n"));
	bcmsdh_reg_write(bus->sdh, 0x18000658, 4, 0x3);
	temp = bcmsdh_reg_read(bus->sdh, 0x1800065c, 4);
	temp |= 0x80000000;
	bcmsdh_reg_write(bus->sdh, 0x1800065c, 4, temp);
	return TRUE;
}
#endif 
static bool
dhdsdio_probe_attach(struct dhd_bus *bus, osl_t *osh, void *sdh, void *regsva,
                     uint16 devid)
{
	int err = 0;
	uint8 clkctl = 0;

	bus->alp_only = TRUE;
	bus->sih = NULL;

	
	if (dhdsdio_set_siaddr_window(bus, SI_ENUM_BASE)) {
		DHD_ERROR(("%s: FAILED to return to SI_ENUM_BASE\n", __FUNCTION__));
	}

#ifdef DHD_DEBUG
	DHD_ERROR(("F1 signature read @0x18000000=0x%4x\n",
	       bcmsdh_reg_read(bus->sdh, SI_ENUM_BASE, 4)));

#endif 


	



	bcmsdh_cfg_write(sdh, SDIO_FUNC_1, SBSDIO_FUNC1_CHIPCLKCSR, DHD_INIT_CLKCTL1, &err);
	if (!err)
		clkctl = bcmsdh_cfg_read(sdh, SDIO_FUNC_1, SBSDIO_FUNC1_CHIPCLKCSR, &err);

	if (err || ((clkctl & ~SBSDIO_AVBITS) != DHD_INIT_CLKCTL1)) {
		DHD_ERROR(("dhdsdio_probe: ChipClkCSR access: err %d wrote 0x%02x read 0x%02x\n",
		           err, DHD_INIT_CLKCTL1, clkctl));
		goto fail;
	}

#ifdef DHD_DEBUG
	if (DHD_INFO_ON()) {
		uint fn, numfn;
		uint8 *cis[SDIOD_MAX_IOFUNCS];
		int err = 0;

		numfn = bcmsdh_query_iofnum(sdh);
		ASSERT(numfn <= SDIOD_MAX_IOFUNCS);

		
		SPINWAIT(((clkctl = bcmsdh_cfg_read(sdh, SDIO_FUNC_1,
		                                    SBSDIO_FUNC1_CHIPCLKCSR, NULL)),
		          !SBSDIO_ALPAV(clkctl)), PMU_MAX_TRANSITION_DLY);

		
		bcmsdh_cfg_write(sdh, SDIO_FUNC_1, SBSDIO_FUNC1_CHIPCLKCSR,
		                 DHD_INIT_CLKCTL2, &err);
		OSL_DELAY(65);

		for (fn = 0; fn <= numfn; fn++) {
			if (!(cis[fn] = MALLOC(osh, SBSDIO_CIS_SIZE_LIMIT))) {
				DHD_ERROR(("dhdsdio_probe: fn %d cis malloc failed\n", fn));
				break;
			}
			bzero(cis[fn], SBSDIO_CIS_SIZE_LIMIT);

			if ((err = bcmsdh_cis_read(sdh, fn, cis[fn], SBSDIO_CIS_SIZE_LIMIT))) {
				DHD_ERROR(("dhdsdio_probe: fn %d cis read err %d\n", fn, err));
				MFREE(osh, cis[fn], SBSDIO_CIS_SIZE_LIMIT);
				break;
			}
			dhd_dump_cis(fn, cis[fn]);
		}

		while (fn-- > 0) {
			ASSERT(cis[fn]);
			if(cis[fn])
				MFREE(osh, cis[fn], SBSDIO_CIS_SIZE_LIMIT);
		}

		if (err) {
			DHD_ERROR(("dhdsdio_probe: failure reading or parsing CIS\n"));
			goto fail;
		}
	}
#endif 

	
	if (!(bus->sih = si_attach((uint)devid, osh, regsva, DHD_BUS, sdh,
	                           &bus->vars, &bus->varsz))) {
		DHD_ERROR(("%s: si_attach failed!\n", __FUNCTION__));
		goto fail;
	}

#ifdef REGON_BP_HANG_FIX
	
	if (((uint16)bus->sih->chip == BCM4324_CHIP_ID) && (bus->sih->chiprev < 3))
			dhdsdio_sdio_hang_war(bus);
#endif 

	bcmsdh_chipinfo(sdh, bus->sih->chip, bus->sih->chiprev);

	if (!dhdsdio_chipmatch((uint16)bus->sih->chip)) {
		DHD_ERROR(("%s: unsupported chip: 0x%04x\n",
		           __FUNCTION__, bus->sih->chip));
		goto fail;
	}

	if (bus->sih->buscorerev >= 12)
		dhdsdio_clk_kso_init(bus);
	else
		bus->kso = TRUE;

	if (CST4330_CHIPMODE_SDIOD(bus->sih->chipst)) {
	}

	si_sdiod_drive_strength_init(bus->sih, osh, dhd_sdiod_drive_strength);


	
	if (!DHD_NOPMU(bus)) {
		if ((si_setcore(bus->sih, ARM7S_CORE_ID, 0)) ||
		    (si_setcore(bus->sih, ARMCM3_CORE_ID, 0)) ||
		    (si_setcore(bus->sih, ARMCR4_CORE_ID, 0))) {
			bus->armrev = si_corerev(bus->sih);
		} else {
			DHD_ERROR(("%s: failed to find ARM core!\n", __FUNCTION__));
			goto fail;
		}

		if (!si_setcore(bus->sih, ARMCR4_CORE_ID, 0)) {
			if (!(bus->orig_ramsize = si_socram_size(bus->sih))) {
				DHD_ERROR(("%s: failed to find SOCRAM memory!\n", __FUNCTION__));
				goto fail;
			}
		} else {
			
			if (!(bus->orig_ramsize = si_tcm_size(bus->sih))) {
				DHD_ERROR(("%s: failed to find CR4-TCM memory!\n", __FUNCTION__));
				goto fail;
			}
			
			switch ((uint16)bus->sih->chip) {
			case BCM4335_CHIP_ID:
				bus->dongle_ram_base = CR4_4335_RAM_BASE;
				break;
			case BCM4350_CHIP_ID:
				bus->dongle_ram_base = CR4_4350_RAM_BASE;
				break;
			case BCM4360_CHIP_ID:
				bus->dongle_ram_base = CR4_4360_RAM_BASE;
				break;
			default:
				bus->dongle_ram_base = 0;
				DHD_ERROR(("%s: WARNING: Using default ram base at 0x%x\n",
				           __FUNCTION__, bus->dongle_ram_base));
			}
		}
		bus->ramsize = bus->orig_ramsize;
		if (dhd_dongle_memsize)
			dhd_dongle_setmemsize(bus, dhd_dongle_memsize);

		DHD_ERROR(("DHD: dongle ram size is set to %d(orig %d) at 0x%x\n",
		           bus->ramsize, bus->orig_ramsize, bus->dongle_ram_base));

		bus->srmemsize = si_socram_srmem_size(bus->sih);
	}

	
	if (!(bus->regs = si_setcore(bus->sih, PCMCIA_CORE_ID, 0)) &&
	    !(bus->regs = si_setcore(bus->sih, SDIOD_CORE_ID, 0))) {
		DHD_ERROR(("%s: failed to find SDIODEV core!\n", __FUNCTION__));
		goto fail;
	}
	bus->sdpcmrev = si_corerev(bus->sih);

	
	OR_REG(osh, &bus->regs->corecontrol, CC_BPRESEN);
	bus->rxint_mode = SDIO_DEVICE_HMB_RXINT;

	if ((bus->sih->buscoretype == SDIOD_CORE_ID) && (bus->sdpcmrev >= 4) &&
		(bus->rxint_mode  == SDIO_DEVICE_RXDATAINT_MODE_1))
	{
		uint32 val;

		val = R_REG(osh, &bus->regs->corecontrol);
		val &= ~CC_XMTDATAAVAIL_MODE;
		val |= CC_XMTDATAAVAIL_CTRL;
		W_REG(osh, &bus->regs->corecontrol, val);
	}


	pktq_init(&bus->txq, (PRIOMASK + 1), QLEN);

	
	bus->rxhdr = (uint8 *)ROUNDUP((uintptr)&bus->hdrbuf[0], DHD_SDALIGN);

	
	bus->intr = (bool)dhd_intr;
	if ((bus->poll = (bool)dhd_poll))
		bus->pollrate = 1;

#ifdef BCMSDIOH_TXGLOM
	
	bus->glom_mode = bcmsdh_set_mode(bus->sdh, SDPCM_DEFGLOM_MODE);
	
	bus->glomsize = SDPCM_DEFGLOM_SIZE;
#endif

	return TRUE;

fail:
	if (bus->sih != NULL) {
		si_detach(bus->sih);
		local_sih = NULL;
		bus->sih = NULL;
	}
	return FALSE;
}

static bool
dhdsdio_probe_malloc(dhd_bus_t *bus, osl_t *osh, void *sdh)
{
	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	if (bus->dhd->maxctl) {
		bus->rxblen = ROUNDUP((bus->dhd->maxctl + SDPCM_HDRLEN), ALIGNMENT) + DHD_SDALIGN;
		if (!(bus->rxbuf = DHD_OS_PREALLOC(osh, DHD_PREALLOC_RXBUF, bus->rxblen))) {
			DHD_ERROR(("%s: MALLOC of %d-byte rxbuf failed\n",
			           __FUNCTION__, bus->rxblen));
			goto fail;
		}
	}
	
	if (!(bus->databuf = DHD_OS_PREALLOC(osh, DHD_PREALLOC_DATABUF, MAX_DATA_BUF))) {
		DHD_ERROR(("%s: MALLOC of %d-byte databuf failed\n",
			__FUNCTION__, MAX_DATA_BUF));
		
		if (!bus->rxblen)
			DHD_OS_PREFREE(osh, bus->rxbuf, bus->rxblen);
		goto fail;
	}

	
	if ((uintptr)bus->databuf % DHD_SDALIGN)
		bus->dataptr = bus->databuf + (DHD_SDALIGN - ((uintptr)bus->databuf % DHD_SDALIGN));
	else
		bus->dataptr = bus->databuf;

	return TRUE;

fail:
	return FALSE;
}

extern bool ap_fw_loaded;
static bool
dhdsdio_probe_init(dhd_bus_t *bus, osl_t *osh, void *sdh)
{
	int32 fnum;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

#ifdef SDTEST
	dhdsdio_pktgen_init(bus);
#endif 

	
	bcmsdh_cfg_write(sdh, SDIO_FUNC_0, SDIOD_CCCR_IOEN, SDIO_FUNC_ENABLE_1, NULL);

	bus->dhd->busstate = DHD_BUS_DOWN;
	bus->sleeping = FALSE;
	bus->rxflow = FALSE;
	bus->prev_rxlim_hit = 0;

	
	bcmsdh_cfg_write(sdh, SDIO_FUNC_1, SBSDIO_FUNC1_CHIPCLKCSR, 0, NULL);

	
	bus->clkstate = CLK_SDONLY;
	bus->idletime = (int32)dhd_idletime;
	bus->idleclock = DHD_IDLE_ACTIVE;


	if (strstr(fw_path, "_apsta") != NULL)  {
		bus->idletime = 50;
		ap_fw_loaded = TRUE;
	} else
		ap_fw_loaded = FALSE;


	
	if (bcmsdh_iovar_op(sdh, "sd_divisor", NULL, 0,
	                    &bus->sd_divisor, sizeof(int32), FALSE) != BCME_OK) {
		DHD_ERROR(("%s: fail on %s get\n", __FUNCTION__, "sd_divisor"));
		bus->sd_divisor = -1;
	} else {
		DHD_INFO(("%s: Initial value for %s is %d\n",
		          __FUNCTION__, "sd_divisor", bus->sd_divisor));
	}

	
	if (bcmsdh_iovar_op(sdh, "sd_mode", NULL, 0,
	                    &bus->sd_mode, sizeof(int32), FALSE) != BCME_OK) {
		DHD_ERROR(("%s: fail on %s get\n", __FUNCTION__, "sd_mode"));
		bus->sd_mode = -1;
	} else {
		DHD_INFO(("%s: Initial value for %s is %d\n",
		          __FUNCTION__, "sd_mode", bus->sd_mode));
	}

	
	fnum = 2;
	if (bcmsdh_iovar_op(sdh, "sd_blocksize", &fnum, sizeof(int32),
	                    &bus->blocksize, sizeof(int32), FALSE) != BCME_OK) {
		bus->blocksize = 0;
		DHD_ERROR(("%s: fail on %s get\n", __FUNCTION__, "sd_blocksize"));
	} else {
		DHD_INFO(("%s: Initial value for %s is %d\n",
		          __FUNCTION__, "sd_blocksize", bus->blocksize));

		if (bus->sih->chip == BCM4335_CHIP_ID)
			dhd_overflow_war(bus);
	}
	bus->roundup = MIN(max_roundup, bus->blocksize);

#if 0
	
	if (bcmsdh_iovar_op(sdh, "sd_rxchain", NULL, 0,
	                    &bus->sd_rxchain, sizeof(int32), FALSE) != BCME_OK) {
		bus->sd_rxchain = FALSE;
	} else {
		DHD_INFO(("%s: bus module (through bcmsdh API) %s chaining\n",
		          __FUNCTION__, (bus->sd_rxchain ? "supports" : "does not support")));
	}
#else
	bus->sd_rxchain = TRUE;
#endif
	bus->use_rxchain = (bool)bus->sd_rxchain;

	return TRUE;
}

bool
dhd_bus_download_firmware(struct dhd_bus *bus, osl_t *osh,
                          char *pfw_path, char *pnv_path)
{
	bool ret;
	bus->fw_path = pfw_path;
	bus->nv_path = pnv_path;

	ret = dhdsdio_download_firmware(bus, osh, bus->sdh);


	return ret;
}

static bool
dhdsdio_download_firmware(struct dhd_bus *bus, osl_t *osh, void *sdh)
{
	bool ret;

	DHD_OS_WAKE_LOCK(bus->dhd);

	
	dhdsdio_clkctl(bus, CLK_AVAIL, FALSE);

	ret = _dhdsdio_download_firmware(bus) == 0;

	dhdsdio_clkctl(bus, CLK_SDONLY, FALSE);

	DHD_OS_WAKE_UNLOCK(bus->dhd);
	return ret;
}

static void
dhdsdio_release(dhd_bus_t *bus, osl_t *osh)
{
	bool dongle_isolation = FALSE;
	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	if (bus) {
		ASSERT(osh);

		if (bus->dhd) {
			dongle_isolation = bus->dhd->dongle_isolation;
			dhd_detach(bus->dhd);
		}

		
		bcmsdh_intr_disable(bus->sdh);
		bcmsdh_intr_dereg(bus->sdh);

		if (bus->dhd) {
			dhdsdio_release_dongle(bus, osh, dongle_isolation, TRUE);
			dhd_free(bus->dhd);
			bus->dhd = NULL;
		}

		dhdsdio_release_malloc(bus, osh);

#ifdef DHD_DEBUG
		if (bus->console.buf != NULL)
			MFREE(osh, bus->console.buf, bus->console.bufsize);
#endif

		MFREE(osh, bus, sizeof(dhd_bus_t));
	}

	if (osh)
		dhd_osl_detach(osh);

	DHD_TRACE(("%s: Disconnected\n", __FUNCTION__));
}

static void
dhdsdio_release_malloc(dhd_bus_t *bus, osl_t *osh)
{
	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	if (bus->dhd && bus->dhd->dongle_reset)
		return;

	if (bus->rxbuf) {
#ifndef DHD_USE_STATIC_BUF
		MFREE(osh, bus->rxbuf, bus->rxblen);
#endif
		bus->rxctl = bus->rxbuf = NULL;
		bus->rxlen = 0;
	}

	if (bus->databuf) {
#ifndef DHD_USE_STATIC_BUF
		MFREE(osh, bus->databuf, MAX_DATA_BUF);
#endif
		bus->databuf = NULL;
	}

	if (bus->vars && bus->varsz) {
		MFREE(osh, bus->vars, bus->varsz);
		bus->vars = NULL;
	}

}


static void
dhdsdio_release_dongle(dhd_bus_t *bus, osl_t *osh, bool dongle_isolation, bool reset_flag)
{
	DHD_TRACE(("%s: Enter bus->dhd %p bus->dhd->dongle_reset %d \n", __FUNCTION__,
		bus->dhd, bus->dhd->dongle_reset));

	if ((bus->dhd && bus->dhd->dongle_reset) && reset_flag)
		return;

	if (bus->sih) {
#if !defined(BCMLXSDMMC)
		if (bus->dhd) {
			dhdsdio_clkctl(bus, CLK_AVAIL, FALSE);
		}
		if (KSO_ENAB(bus) && (dongle_isolation == FALSE))
			si_watchdog(bus->sih, 4);
		if (bus->dhd) {
			dhdsdio_clkctl(bus, CLK_NONE, FALSE);
		}
#endif 
		si_detach(bus->sih);
		local_sih = NULL;
		bus->sih = NULL;
		if (bus->vars && bus->varsz)
			MFREE(osh, bus->vars, bus->varsz);
		bus->vars = NULL;
	}

	DHD_TRACE(("%s: Disconnected\n", __FUNCTION__));
}

static void
dhdsdio_disconnect(void *ptr)
{
	dhd_bus_t *bus = (dhd_bus_t *)ptr;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 25))
	if (mutex_is_locked(&_dhd_sdio_mutex_lock_) == 0) {
		DHD_ERROR(("%s : no mutex held. set lock\n", __FUNCTION__));
	}
	else {
		DHD_ERROR(("%s : mutex is locked!. wait for unlocking\n", __FUNCTION__));
	}
	mutex_lock(&_dhd_sdio_mutex_lock_);
#endif 


	if (bus) {
		ASSERT(bus->dhd);
		dhdsdio_release(bus, bus->dhd->osh);
	}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 25))
	mutex_unlock(&_dhd_sdio_mutex_lock_);
	DHD_ERROR(("%s : the lock is released.\n", __FUNCTION__));
#endif 


	DHD_TRACE(("%s: Disconnected\n", __FUNCTION__));
}



static bcmsdh_driver_t dhd_sdio = {
	dhdsdio_probe,
	dhdsdio_disconnect
};

int
dhd_bus_register(void)
{
	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	return bcmsdh_register(&dhd_sdio);
}

void
dhd_bus_unregister(void)
{
	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	bcmsdh_unregister();
}

#if defined(BCMLXSDMMC)
int dhd_bus_reg_sdio_notify(void* semaphore)
{
	return bcmsdh_reg_sdio_notify(semaphore);
}

void dhd_bus_unreg_sdio_notify(void)
{
	bcmsdh_unreg_sdio_notify();
}
#endif 

#ifdef BCMEMBEDIMAGE
static int
dhdsdio_download_code_array(struct dhd_bus *bus)
{
	int bcmerror = -1;
	int offset = 0;
	unsigned char *ularray = NULL;

	DHD_INFO(("%s: download embedded firmware...\n", __FUNCTION__));

	
	while ((offset + MEMBLOCK) < sizeof(dlarray)) {
		
		if (si_setcore(bus->sih, ARMCR4_CORE_ID, 0)) {
			/* if address is 0, store the reset instruction to be written in 0 */

			if (offset == 0) {
				bus->resetinstr = *(((uint32*)dlarray));
				
				offset += bus->dongle_ram_base;
			}
		}

		bcmerror = dhdsdio_membytes(bus, TRUE, offset,
			(uint8 *) (dlarray + offset), MEMBLOCK);
		if (bcmerror) {
			DHD_ERROR(("%s: error %d on writing %d membytes at 0x%08x\n",
			        __FUNCTION__, bcmerror, MEMBLOCK, offset));
			goto err;
		}

		offset += MEMBLOCK;
	}

	if (offset < sizeof(dlarray)) {
		bcmerror = dhdsdio_membytes(bus, TRUE, offset,
			(uint8 *) (dlarray + offset), sizeof(dlarray) - offset);
		if (bcmerror) {
			DHD_ERROR(("%s: error %d on writing %d membytes at 0x%08x\n",
			        __FUNCTION__, bcmerror, sizeof(dlarray) - offset, offset));
			goto err;
		}
	}

#ifdef DHD_DEBUG
	
	{
		ularray = MALLOC(bus->dhd->osh, bus->ramsize);
		
		offset = 0;
		memset(ularray, 0xaa, bus->ramsize);
		while ((offset + MEMBLOCK) < sizeof(dlarray)) {
			bcmerror = dhdsdio_membytes(bus, FALSE, offset, ularray + offset, MEMBLOCK);
			if (bcmerror) {
				DHD_ERROR(("%s: error %d on reading %d membytes at 0x%08x\n",
					__FUNCTION__, bcmerror, MEMBLOCK, offset));
				goto err;
			}

			offset += MEMBLOCK;
		}

		if (offset < sizeof(dlarray)) {
			bcmerror = dhdsdio_membytes(bus, FALSE, offset,
				ularray + offset, sizeof(dlarray) - offset);
			if (bcmerror) {
				DHD_ERROR(("%s: error %d on reading %d membytes at 0x%08x\n",
					__FUNCTION__, bcmerror, sizeof(dlarray) - offset, offset));
				goto err;
			}
		}

		if (memcmp(dlarray, ularray, sizeof(dlarray))) {
			DHD_ERROR(("%s: Downloaded image is corrupted (%s, %s, %s).\n",
			           __FUNCTION__, dlimagename, dlimagever, dlimagedate));
			goto err;
		} else
			DHD_ERROR(("%s: Download, Upload and compare succeeded (%s, %s, %s).\n",
			           __FUNCTION__, dlimagename, dlimagever, dlimagedate));

	}
#endif 

err:
	if (ularray)
		MFREE(bus->dhd->osh, ularray, bus->ramsize);
	return bcmerror;
}
#endif 

static int
dhdsdio_download_code_file(struct dhd_bus *bus, char *pfw_path)
{
	int bcmerror = -1;
	int offset = 0;
	int len;
	void *image = NULL;
	uint8 *memblock = NULL, *memptr;
	static bool loading_mfg_flag = FALSE;

	DHD_INFO(("%s: download firmware %s\n", __FUNCTION__, pfw_path));

	image = dhd_os_open_image(pfw_path);
	if (image == NULL)
		goto err;

	memptr = memblock = MALLOC(bus->dhd->osh, MEMBLOCK + DHD_SDALIGN);
	if (memblock == NULL) {
		DHD_ERROR(("%s: Failed to allocate memory %d bytes\n", __FUNCTION__, MEMBLOCK));
		goto err;
	}
	if ((uint32)(uintptr)memblock % DHD_SDALIGN)
		memptr += (DHD_SDALIGN - ((uint32)(uintptr)memblock % DHD_SDALIGN));

	if (strstr(bus->fw_path, "mfg")) {
		printf("Enable polling mode to load MFG firmware\n");
		bus->poll = 1;
		bus->pollrate = 1;
		loading_mfg_flag = TRUE;
	} else if (loading_mfg_flag) {
		printf("Disable polling mode to load OS firmware\n");
		bus->poll = 0;
		bus->pollrate = 0;
		loading_mfg_flag = FALSE;
	}

	
	while ((len = dhd_os_get_image_block((char*)memptr, MEMBLOCK, image))) {
		if (len < 0) {
			DHD_ERROR(("%s: dhd_os_get_image_block failed (%d)\n", __FUNCTION__, len));
			bcmerror = BCME_ERROR;
			goto err;
		}
		
		if (si_setcore(bus->sih, ARMCR4_CORE_ID, 0)) {
			/* if address is 0, store the reset instruction to be written in 0 */

			if (offset == 0) {
				bus->resetinstr = *(((uint32*)memptr));
				
				offset += bus->dongle_ram_base;
			}
		}

		bcmerror = dhdsdio_membytes(bus, TRUE, offset, memptr, len);
		if (bcmerror) {
			DHD_ERROR(("%s: error %d on writing %d membytes at 0x%08x\n",
			        __FUNCTION__, bcmerror, MEMBLOCK, offset));
			goto err;
		}

		offset += MEMBLOCK;
	}

err:
	if (memblock)
		MFREE(bus->dhd->osh, memblock, MEMBLOCK + DHD_SDALIGN);

	if (image)
		dhd_os_close_image(image);

	return bcmerror;
}


void
dhd_bus_set_nvram_params(struct dhd_bus * bus, const char *nvram_params)
{
	bus->nvram_params = nvram_params;
}

#define WIFI_MAC_PARAM_STR     "macaddr="
#define WIFI_MAX_MAC_LEN       17 

#define NVS_LEN_OFFSET         0x0C
#define NVS_DATA_OFFSET                0x40

extern unsigned char *get_wifi_nvs_ram(void);

static uint
get_mac_from_wifi_nvs_ram(char* buf, unsigned int buf_len)
{
	unsigned char *nvs_ptr;
	unsigned char *mac_ptr;
	uint len = 0;

	if (!buf || !buf_len) {
		return 0;
	}

	nvs_ptr = get_wifi_nvs_ram();
	if (nvs_ptr) {
		nvs_ptr += NVS_DATA_OFFSET;
	}

	mac_ptr = strstr(nvs_ptr, WIFI_MAC_PARAM_STR);
	if (mac_ptr) {
		mac_ptr += strlen(WIFI_MAC_PARAM_STR);

		
		while (mac_ptr[0] == ' ') {
			mac_ptr++;
		}

		
		len = 0;
		while (mac_ptr[len] != '\r' && mac_ptr[len] != '\n' &&
				mac_ptr[len] != '\0') {
			len++;
		}

		if (len > buf_len) {
			len = buf_len;
		}
		memcpy(buf, mac_ptr, len);
	}

	return len;
}

static uint
modify_mac_attr(char* buf, unsigned buf_len, char *mac, unsigned int mac_len)
{
	unsigned char *mac_ptr;
	uint len;

	if (!buf || !mac) {
		return buf_len;
	}

	mac_ptr = strstr(buf, WIFI_MAC_PARAM_STR);
	if (mac_ptr) {
		mac_ptr += strlen(WIFI_MAC_PARAM_STR);

		
		len = 0;
		while (mac_ptr[len] != '\r' && mac_ptr[len] != '\n' &&
				mac_ptr[len] != '\0') {
			len++;
		}

		if (len != mac_len) {
			
			memmove(&mac_ptr[mac_len + 1], &mac_ptr[len + 1], buf_len - len);
			buf_len = buf_len - len + mac_len;
		}
		memcpy(mac_ptr, mac, mac_len);
	}

	return buf_len;
}
static int
dhdsdio_download_nvram(struct dhd_bus *bus)
{
	int bcmerror = -1;
	uint len;
	void * image = NULL;
	char * memblock = NULL;
	char *bufp;
	char *pnv_path;
	bool nvram_file_exists;

	char mac[WIFI_MAX_MAC_LEN];
	unsigned mac_len;
		
	pnv_path = bus->nv_path;

	nvram_file_exists = ((pnv_path != NULL) && (pnv_path[0] != '\0'));
	if (!nvram_file_exists && (bus->nvram_params == NULL))
		return (0);

	if (nvram_file_exists) {
		printf("%s: nvram_path=%s\n", __FUNCTION__, nv_path);
		image = dhd_os_open_image(pnv_path);
		if (image == NULL)
			goto err;
	}

	memblock = MALLOC(bus->dhd->osh, MAX_NVRAMBUF_SIZE);
	if (memblock == NULL) {
		DHD_ERROR(("%s: Failed to allocate memory %d bytes\n",
		           __FUNCTION__, MAX_NVRAMBUF_SIZE));
		goto err;
	}

	
	if (nvram_file_exists) {
		len = dhd_os_get_image_block(memblock, MAX_NVRAMBUF_SIZE, image);
		
		mac_len = get_mac_from_wifi_nvs_ram(mac, WIFI_MAX_MAC_LEN);
		if (mac_len > 0) {
			len = modify_mac_attr(memblock, len, mac, mac_len);
		}
		
	}
	else {
		len = strlen(bus->nvram_params);
		ASSERT(len <= MAX_NVRAMBUF_SIZE);
		memcpy(memblock, bus->nvram_params, len);
	}
	if (len > 0 && len < MAX_NVRAMBUF_SIZE) {
		bufp = (char *)memblock;
		bufp[len] = 0;
		len = process_nvram_vars(bufp, len);
		if (len % 4) {
			len += 4 - (len % 4);
		}
		bufp += len;
		*bufp++ = 0;
		if (len)
			bcmerror = dhdsdio_downloadvars(bus, memblock, len + 1);
		if (bcmerror) {
			DHD_ERROR(("%s: error downloading vars: %d\n",
			           __FUNCTION__, bcmerror));
		}
	}
	else {
		DHD_ERROR(("%s: error reading nvram file: %d\n",
		           __FUNCTION__, len));
		bcmerror = BCME_SDIO_ERROR;
	}

err:
	if (memblock)
		MFREE(bus->dhd->osh, memblock, MAX_NVRAMBUF_SIZE);

	if (image)
		dhd_os_close_image(image);

	return bcmerror;
}

extern int bcm_chip_is_4335a0;
extern int bcm_chip_is_4335;
#define BCM4335B0_STA_FW_PATH "/system/etc/firmware/fw_bcm4335_b0.bin"
#define BCM4335B0_APSTA_FW_PATH "/system/etc/firmware/fw_bcm4335_apsta_b0.bin"
#define BCM4335B0_P2P_FW_PATH "/system/etc/firmware/fw_bcm4335_p2p_b0.bin"
#define BCM4335B0_MFG_FW_PATH "/system/etc/firmware/bcm_mfg_b0.bin"

static int
_dhdsdio_download_firmware(struct dhd_bus *bus)
{
	int bcmerror = -1;

	bool embed = FALSE;	
	bool dlok = FALSE;	

	
	if ((bus->fw_path == NULL) || (bus->fw_path[0] == '\0')) {
#ifdef BCMEMBEDIMAGE
		embed = TRUE;
#else
		return 0;
#endif
	}

	if (bcm_chip_is_4335 && !bcm_chip_is_4335a0) {
		printf("chip is 4335 B0!\n");
		if (strstr(bus->fw_path, "_apsta") != NULL)
			strcpy(bus->fw_path, BCM4335B0_APSTA_FW_PATH);
		else if (strstr(bus->fw_path, "_p2p") != NULL)
			strcpy(bus->fw_path, BCM4335B0_P2P_FW_PATH);
		else if (strstr(bus->fw_path, "mfg") != NULL)
			strcpy(bus->fw_path, BCM4335B0_MFG_FW_PATH);
		else
			strcpy(bus->fw_path, BCM4335B0_STA_FW_PATH);
	} else
		printf("chip is 4335 A0!\n");
	DHD_DEFAULT(("firmware path = %s \n", bus->fw_path));
	
	if (dhdsdio_download_state(bus, TRUE)) {
		DHD_ERROR(("%s: error placing ARM core in reset\n", __FUNCTION__));
		goto err;
	}

	
	if ((bus->fw_path != NULL) && (bus->fw_path[0] != '\0')) {
		if (dhdsdio_download_code_file(bus, bus->fw_path)) {
			DHD_ERROR(("%s: dongle image file download failed\n", __FUNCTION__));
#ifdef BCMEMBEDIMAGE
			embed = TRUE;
#else
			goto err;
#endif
		}
		else {
			embed = FALSE;
			dlok = TRUE;
		}
	}

#ifdef BCMEMBEDIMAGE
	if (embed) {
		if (dhdsdio_download_code_array(bus)) {
			DHD_ERROR(("%s: dongle image array download failed\n", __FUNCTION__));
			goto err;
		}
		else {
			dlok = TRUE;
		}
	}
#else
	BCM_REFERENCE(embed);
#endif
	if (!dlok) {
		DHD_ERROR(("%s: dongle image download failed\n", __FUNCTION__));
		goto err;
	}

	
	
	

	
	if (dhdsdio_download_nvram(bus)) {
		DHD_ERROR(("%s: dongle nvram file download failed\n", __FUNCTION__));
		goto err;
	}

	
	if (dhdsdio_download_state(bus, FALSE)) {
		DHD_ERROR(("%s: error getting out of ARM core reset\n", __FUNCTION__));
		goto err;
	}

	bcmerror = 0;

err:
	return bcmerror;
}

static int
dhd_bcmsdh_recv_buf(dhd_bus_t *bus, uint32 addr, uint fn, uint flags, uint8 *buf, uint nbytes,
	void *pkt, bcmsdh_cmplt_fn_t complete, void *handle)
{
	int status;

	if (!KSO_ENAB(bus)) {
		DHD_ERROR(("%s: Device asleep\n", __FUNCTION__));
		return BCME_NODEVICE;
	}

	status = bcmsdh_recv_buf(bus->sdh, addr, fn, flags, buf, nbytes, pkt, complete, handle);

	return status;
}

static int
dhd_bcmsdh_send_buf(dhd_bus_t *bus, uint32 addr, uint fn, uint flags, uint8 *buf, uint nbytes,
	void *pkt, bcmsdh_cmplt_fn_t complete, void *handle)
{
	if (!KSO_ENAB(bus)) {
		DHD_ERROR(("%s: Device asleep\n", __FUNCTION__));
		return BCME_NODEVICE;
	}

	return (bcmsdh_send_buf(bus->sdh, addr, fn, flags, buf, nbytes, pkt, complete, handle));
}

#ifdef BCMSDIOH_TXGLOM
static void
dhd_bcmsdh_glom_post(dhd_bus_t *bus, uint8 *frame, void *pkt, uint len)
{
	bcmsdh_glom_post(bus->sdh, frame, pkt, len);
}

static void
dhd_bcmsdh_glom_clear(dhd_bus_t *bus)
{
	bcmsdh_glom_clear(bus->sdh);
}
#endif

uint
dhd_bus_chip(struct dhd_bus *bus)
{
	ASSERT(bus->sih != NULL);
	return bus->sih->chip;
}

void *
dhd_bus_pub(struct dhd_bus *bus)
{
	return bus->dhd;
}

void *
dhd_bus_txq(struct dhd_bus *bus)
{
	return &bus->txq;
}

uint
dhd_bus_hdrlen(struct dhd_bus *bus)
{
	return SDPCM_HDRLEN;
}

int
dhd_bus_devreset(dhd_pub_t *dhdp, uint8 flag)
{
	int bcmerror = 0;
	dhd_bus_t *bus;

	bus = dhdp->bus;

	if (flag == TRUE) {
		if (!bus->dhd->dongle_reset) {
			dhd_os_sdlock(dhdp);
			dhd_os_wd_timer(dhdp, 0);
#if !defined(IGNORE_ETH0_DOWN)
			
			dhd_txflowcontrol(bus->dhd, ALL_INTERFACES, ON);
#endif 
			
			
			dhd_bus_stop(bus, FALSE);

#if defined(OOB_INTR_ONLY) || defined(BCMSPI_ANDROID)
			
			bcmsdh_set_irq(FALSE);
#endif 

			
			dhdsdio_release_dongle(bus, bus->dhd->osh, TRUE, TRUE);

			bus->dhd->dongle_reset = TRUE;
			bus->dhd->up = FALSE;
#ifdef BCMSDIOH_TXGLOM
			dhd_txglom_enable(dhdp, FALSE);
#endif
			dhd_os_sdunlock(dhdp);

			DHD_TRACE(("%s:  WLAN OFF DONE\n", __FUNCTION__));
			
		} else
			bcmerror = BCME_SDIO_ERROR;
	} else {
		

		DHD_TRACE(("\n\n%s: == WLAN ON ==\n", __FUNCTION__));

		if (bus->dhd->dongle_reset) {
			
#ifdef DHDTHREAD
			dhd_os_sdlock(dhdp);
#endif 
			
			bcmsdh_reset(bus->sdh);

			
			if (dhdsdio_probe_attach(bus, bus->dhd->osh, bus->sdh,
			                        (uint32 *)SI_ENUM_BASE,
			                        bus->cl_devid)) {
				
				if (dhdsdio_probe_init(bus, bus->dhd->osh, bus->sdh) &&
					dhdsdio_download_firmware(bus, bus->dhd->osh, bus->sdh)) {

					
					bcmerror = dhd_bus_init((dhd_pub_t *) bus->dhd, FALSE);
					if (bcmerror == BCME_OK) {
#if defined(OOB_INTR_ONLY) || defined(BCMSPI_ANDROID)
						bcmsdh_set_irq(TRUE);
#ifndef BCMSPI_ANDROID
						dhd_enable_oob_intr(bus, TRUE);
#endif 
#endif 

						bus->dhd->dongle_reset = FALSE;
						bus->dhd->up = TRUE;

#if !defined(IGNORE_ETH0_DOWN)
						
						dhd_txflowcontrol(bus->dhd, ALL_INTERFACES, OFF);
#endif 
						dhd_os_wd_timer(dhdp, dhd_watchdog_ms);
#if 0 
						
						
						
						
#endif 
						DHD_TRACE(("%s: WLAN ON DONE\n", __FUNCTION__));
					} else {
						dhd_bus_stop(bus, FALSE);
						dhdsdio_release_dongle(bus, bus->dhd->osh,
							TRUE, FALSE);
					}
				} else
					bcmerror = BCME_SDIO_ERROR;
			} else
				bcmerror = BCME_SDIO_ERROR;

#ifdef DHDTHREAD
				dhd_os_sdunlock(dhdp);
#endif 
		} else {
			bcmerror = BCME_SDIO_ERROR;
			DHD_INFO(("%s called when dongle is not in reset\n",
				__FUNCTION__));
			DHD_INFO(("Will call dhd_bus_start instead\n"));
			sdioh_start(NULL, 1);
			if ((bcmerror = dhd_bus_start(dhdp)) != 0)
				DHD_ERROR(("%s: dhd_bus_start fail with %d\n",
					__FUNCTION__, bcmerror));
		}
	}
	return bcmerror;
}

uint dhd_bus_chip_id(dhd_pub_t *dhdp)
{
	dhd_bus_t *bus = dhdp->bus;

	return  bus->sih->chip;
}
int
dhd_bus_membytes(dhd_pub_t *dhdp, bool set, uint32 address, uint8 *data, uint size)
{
	dhd_bus_t *bus;

	bus = dhdp->bus;
	return dhdsdio_membytes(bus, set, address, data, size);
}
#if defined(CUSTOMER_HW4) && defined(SUPPORT_MULTIPLE_REVISION)
static int
concate_revision_bcm4334(dhd_bus_t *bus, char *path, int path_len)
{
#define	REV_ID_ADDR	0x1E008F90
#define BCM4334_B1_UNIQUE	0x30312E36

	uint chipver;
	uint32 unique_id;
	uint8 data[4];
	char chipver_tag[4] = "_b?";

	DHD_TRACE(("%s: BCM4334 Multiple Revision Check\n", __FUNCTION__));
	if (bus->sih->chip != BCM4334_CHIP_ID) {
		DHD_ERROR(("%s:Chip is not BCM4334\n", __FUNCTION__));
		return -1;
	}
	chipver = bus->sih->chiprev;
	if (chipver == 0x2) {
		dhdsdio_membytes(bus, FALSE, REV_ID_ADDR, data, 4);
		unique_id = load32_ua(data);
		if (unique_id == BCM4334_B1_UNIQUE)
			chipver = 0x01;
	}
	DHD_ERROR(("CHIP VER = [0x%x]\n", chipver));
	if (chipver == 1) {
		DHD_ERROR(("----- CHIP bcm4334_B0 -----\n"));
		strcpy(chipver_tag, "_b0");
	} else if (chipver == 2) {
		DHD_ERROR(("----- CHIP bcm4334_B1 -----\n"));
		strcpy(chipver_tag, "_b1");
	} else if (chipver == 3) {
		DHD_ERROR(("----- CHIP bcm4334_B2 -----\n"));
		strcpy(chipver_tag, "_b2");
	}
	else {
		DHD_ERROR(("----- Invalid chip version -----\n"));
		return -1;
	}
	strcat(path, chipver_tag);
#undef REV_ID_ADDR
#undef BCM4334_B1_UNIQUE
	return 0;
}

int
concate_revision(dhd_bus_t *bus, char *path, int path_len)
{

	if (!bus || !bus->sih) {
		DHD_ERROR(("%s:Bus is Invalid\n", __FUNCTION__));
		return -1;
	}
	if (bus->sih->chip == BCM4334_CHIP_ID) {
		return concate_revision_bcm4334(bus, path, path_len);
	}
	DHD_ERROR(("REVISION SPECIFIC feature is not required\n"));
	return -1;
}
#endif 
