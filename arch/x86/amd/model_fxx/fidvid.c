#include <mainboard.h>
#include <types.h>
#include <lib.h>
#include <console.h>
#include <globalvars.h>
#include <device/device.h>
#include <device/pci.h>
#include <string.h>
#include <msr.h>
#include <io.h>
#include <cpu.h>
#include <amd/k8/k8.h>
#include <mc146818rtc.h>
#include <spd.h>
#include <lapic.h>

#define K8_SET_FIDVID_DEBUG 1

#define K8_SET_FIDVID_ONE_BY_ONE 1

#define K8_SET_FIDVID_STORE_AP_APICID_AT_FIRST 1

#ifndef SB_VFSMAF
#define SB_VFSMAF 1
#endif

#define FX_SUPPORT 1

void enable_fid_change(void)
{
	u32 dword;
	unsigned nodes;
	int i;

	nodes = ((pci_conf1_read_config32(PCI_BDF(0, 0x18, 0), NODEID) >> 4) & 7) + 1;

#warning document these settings!
	for (i = 0; i < nodes; i++) {
		dword = pci_conf1_read_config32(PCI_BDF(0, 0x18+i, 3), 0xd8);
		dword &= 0x8ff00000;
		dword |= (2 << 28) | (0x02710);
		pci_conf1_write_config32(PCI_BDF(0, 0x18+i, 3), 0xd8, dword);

		dword = 0x04e2a707;
		pci_conf1_write_config32(PCI_BDF(0, 0x18+i, 3), 0xd4, dword);

		/* disable the DRAM interface at first, it will be enabled
		 * by raminit again */
		dword = pci_conf1_read_config32(PCI_BDF(0, 0x18+i, 2), 0x94);
		dword |= (1 << 14);
		pci_conf1_write_config32(PCI_BDF(0, 0x18+i, 2), 0x94, dword);

		dword = 0x23070700; /* enable FID/VID change */
//		dword = 0x00070000; /* enable FID/VID change */
		pci_conf1_write_config32(PCI_BDF(0, 0x18+i, 3), 0x80, dword);

		dword = 0x00132113;
		pci_conf1_write_config32(PCI_BDF(0, 0x18+i, 3), 0x84, dword);

	}
}

#if K8_SET_FIDVID_ONE_BY_ONE == 0
static unsigned set_fidvid_without_init(unsigned fidvid)
{
	struct msr msr;
	u32 vid;
	u32 fid;

	fid = (fidvid >> 8) & 0x3f;
	vid = (fidvid >> 16) & 0x3f;

	/* set new FID/VID */
	msr.hi = 1;
	msr.lo = (vid<<8) | fid;
	wrmsr(FIDVID_CTL, msr);
	return fidvid;
}
#endif

static unsigned set_fidvid(unsigned apicid, unsigned fidvid, int showmessage)
{
	/* for (cur, new) there is one <1600MHz x8 to find out next_fid */
	static const u8 next_fid_a[] = {
	/*  x4  x5  x6  x7  x8  x9 x10 x11 x12 x13 x14 x15 */
/* x4 */    0,  9,  9,  8,  9,  9,  9,  9,  9,  9,  9,  9,
/* x5 */    9,  0, 11, 11,  9,  9, 10, 11, 11, 11, 11, 11,
/* x6 */   11, 11,  0, 13, 11, 11, 11, 11, 12, 13, 13, 13,
/* x7 */   13, 13, 13,  0, 13, 13, 13, 13, 13, 13, 14, 15,
/* x8 */    4,  9,  9,  9,  0,  9,  9,  9,  9,  9,  9,  9,
/* x9 */    4,  5, 10, 10,  8,  0,  0,  0,  0,  0,  0,  0,
/*x10 */    9,  5, 11, 11,  9,  0,  0,  0,  0,  0,  0,  0,
/*x11 */   10,  5,  6, 12, 10,  0,  0,  0,  0,  0,  0,  0,
/*x12 */   11, 11,  6, 13, 11,  0,  0,  0,  0,  0,  0,  0,
/*x13 */   12, 12,  6,  7, 12,  0,  0,  0,  0,  0,  0,  0,
/*x14 */   13, 13, 13,  7, 13,  0,  0,  0,  0,  0,  0,  0,
/*x15 */   14, 14, 14,  7, 14,  0,  0,  0,  0,  0,  0,  0,
/* 0:x4, 2:x5....BASE=4, MIN=4, MAX=25, INC=2 result = (xX-BASE)*INC */
	};

	struct msr msr;
	u32 vid;
	u32 fid;
	u32 vid_max;
	u32 fid_max;
	u32 vid_cur;
	u32 fid_cur;
	unsigned int apicidx;

	int steps;
	int loop;

	apicidx = lapicid();

	if (apicid != apicidx) {
		printk(BIOS_ERR, 
			"wrong apicid, we want change %x, but it is %x\n", apicid, apicidx);
		return fidvid;
	}

	fid = (fidvid >> 8) & 0x3f;
	vid = (fidvid >> 16) & 0x3f;

	msr = rdmsr(FIDVID_STATUS);

	vid_cur = msr.hi & 0x3f;
	fid_cur = msr.lo & 0x3f;

	if ((vid_cur==vid) && (fid_cur==fid))
		return fidvid;

	vid_max = (msr.hi>>(48-32)) & 0x3f;
	fid_max = ((msr.lo>>16) & 0x3f); /* max fid */
#if FX_SUPPORT
	if (fid_max>=((25-4)*2)) { /* FX max fid is 5G */
		fid_max = ((msr.lo >> 8) & 0x3f) + 5 * 2; /* max FID is min fid + 1G */
		if (fid_max >= ((25-4) * 2)) {
			fid_max = (10-4) * 2; /* hard set to 2G */
		}
	}
#endif

	/* set vid to max */
	msr.hi = 1;
	msr.lo = (vid_max << 8) | (fid_cur);
#if SB_VFSMAF == 1
	msr.lo |= (1<<16); /* init changes */
#endif
	wrmsr(FIDVID_CTL, msr);
#if SB_VFSMAF == 0
	ldtstop_sb();
#endif


	for (loop=0;loop<100000;loop++){
		msr = rdmsr(FIDVID_STATUS);
		if (!(msr.lo & (1<<31)))
			break;
	}
	vid_cur = msr.hi & 0x3f;

	steps = 8; //??
	while ((fid_cur != fid) && (steps-- > 0)) {
		u32 fid_temp;
		if ((fid_cur > (8-4)*2) && (fid> (8-4)*2)) {
			if (fid_cur<fid) {
				fid_temp = fid_cur + 2;
				} else {
					fid_temp = fid_cur - 2;
				}
		} else {
			/* there is one < 8, So we need to lookup the table to
			 * find the fid_cur */
			int temp;
			temp = next_fid_a[(fid_cur/2)*12+(fid/2)];
			if (temp <= 0) break;
			fid_temp = (temp-4) * 2;
		}
		if (fid_temp > fid_max)
			break;

		fid_cur = fid_temp;

		/* set target fid */
		msr.hi = (100000/5);
		msr.lo = (vid_cur << 8) | fid_cur;
#if SB_VFSMAF == 1
		msr.lo |= (1 << 16); /* init changes */
#endif
		wrmsr(FIDVID_CTL, msr);
#if SB_VFSMAF == 0
		ldtstop_sb();
#endif

#if K8_SET_FIDVID_DEBUG == 1
		if (showmessage) {
			printk(BIOS_DEBUG, "\tapicid in set_fidvid = %02x\n", apicid);
			printk(BIOS_DEBUG, "ctrl msr fid, vid %08x:%08x\n", msr.hi, msr.lo);
		}
#endif
		for (loop = 0; loop < 100000; loop++){
			msr = rdmsr(FIDVID_STATUS);
			if (!(msr.lo & (1 << 31)))
				break;
		}
		fid_cur = msr.lo & 0x3f;

#if K8_SET_FIDVID_DEBUG == 1
		if (showmessage)	{
			 printk(BIOS_DEBUG, "status msr fid, vid %08x:%08x\n", 
				msr.hi, msr.lo);
		}
#endif
	}

	/* set vid to final */
	msr.hi = 1;
	msr.lo = (vid << 8) | (fid_cur);
#if SB_VFSMAF == 1
	msr.lo |= (1 << 16); // init changes
#endif
	wrmsr(FIDVID_CTL, msr);
#if SB_VFSMAF == 0
	ldtstop_sb();
#endif

	for (loop = 0; loop < 100000; loop++){
		msr = rdmsr(FIDVID_STATUS);
		if (!(msr.lo & (1 << 31)))
			break;
	}
	vid_cur = msr.hi & 0x3f;

	fidvid = (vid_cur << 16) | (fid_cur << 8);

	if (showmessage) {
		if (vid!=vid_cur) {
			printk(BIOS_ERR, "set vid failed for apicid =%08x\n", apicidx);
		}
		if (fid!=fid_cur) {
			printk(BIOS_ERR, "set fid failed for apicid =%08x\n",apicidx);
		}
	}

	return fidvid;

}

void init_fidvid_ap(unsigned bsp_apicid, unsigned apicid)
{
	u32 send;
	u32 readback = 0;
	unsigned int timeout = 1;
	struct msr msr;
	u32 vid_cur;
	u32 fid_cur;
	u32 fid_max;
	int loop;

	msr =	rdmsr(FIDVID_STATUS);
	fid_max = ((msr.lo >> 16) & 0x3f); /* max fid */
#if FX_SUPPORT
	if (fid_max >= ((25-4) * 2)) { /* FX max fid is 5G */
		fid_max = ((msr.lo>>8) & 0x3f) + 5*2; /* maxFID = minFID + 1G */
		if (fid_max >= ((25-4) * 2)) {
			fid_max = (10-4) * 2; // hard set to 2G
		}
	}
#endif
	send = fid_max<<8;

	send |= ((msr.hi >> (48-32)) & 0x3f) << 16; /* max vid */
	send |= (apicid << 24); /* ap apicid */

#if K8_SET_FIDVID_ONE_BY_ONE == 1
	vid_cur = msr.hi & 0x3f;
	fid_cur = msr.lo & 0x3f;

	/* set to current */ 
	msr.hi = 1;
	msr.lo = (vid_cur << 8) | (fid_cur);
	wrmsr(FIDVID_CTL, msr);
#endif

	timeout = wait_cpu_state(bsp_apicid, 1);
	if (timeout) {
		printk(BIOS_ERR, 
			"fidvid_ap_stage1: time out while reading from BSP on %08x\n", 
			apicid);
	}
	/* send signal to BSP about this AP max fid and vid */
	/* AP at state 1 that sent our fid and vid */
	lapic_write(LAPIC_MSG_REG, send | 1);

//	wait_cpu_state(bsp_apicid, 2); /* don't need we can use apicid directly */
	loop = 1000000;
	while (--loop > 0) {
		/* remote read BSP signal that include vid/fid that need to set */
		if (lapic_remote_read(bsp_apicid, LAPIC_MSG_REG, &readback)!=0)
			continue;
		if (((readback >> 24) & 0xff) == apicid)
			break; /* it is this cpu turn */
	}

	if (loop > 0) {
	#if K8_SET_FIDVID_ONE_BY_ONE == 1
			readback = set_fidvid(apicid, readback & 0xffff00, 1); // this AP
	#else
		readback = set_fidvid_without_init(readback & 0xffff00); // this AP
	#endif
		/* send signal to BSP that this AP fid/vid is set */
		/* allow to change state2 is together with apicid */
		/* AP at state that We set the requested fid/vid */
		send = (apicid<<24) | (readback & 0x00ffff00); 
	} else {
		printk(BIOS_ERR, 
			"fidvid_ap_stage2: time out while reading from BSP on %08x\n", 
			apicid);
	}

		lapic_write(LAPIC_MSG_REG, send | 2);

	timeout = wait_cpu_state(bsp_apicid, 3);
	if (timeout) {
		printk(BIOS_ERR, 
			"fidvid_ap_stage3: time out while reading from BSP on %08x\n", 
			apicid);
	}
}

static unsigned int calc_common_fidvid(unsigned int fidvid, unsigned int fidvidx)
{
	/* FIXME: need to check the change path to verify if it is reachable 
	 * when common fid is small than 1.6G */
	if ((fidvid & 0xff00) <= (fidvidx & 0xff00)) {
		return fidvid;
	}
	else {
		return fidvidx;
	}
}

struct fidvid_st {
	unsigned common_fidvid;
};

static void init_fidvid_bsp_stage1(unsigned int ap_apicid, void *gp )
{
	unsigned int readback = 0;
	unsigned int timeout = 1;

	struct fidvid_st *fvp = gp;
	int loop;

	printk(BIOS_DEBUG, "state 1: ap_apicid=%08x\n", ap_apicid);

	loop = 1000000;
	while (--loop > 0) {
		if (lapic_remote_read(ap_apicid, LAPIC_MSG_REG, &readback)!=0)
			continue;
		if ((readback & 0xff) == 1) {
			timeout = 0;
			break; /* target ap is in stage 1 */
		}
	}
	if (timeout) {
		printk(BIOS_ERR, 
			"fidvid_bsp_stage1: time out while reading from ap %08x\n", 
			ap_apicid);
		return;
	}

	printk(BIOS_DEBUG, "\treadback=%x\n", readback);

	fvp->common_fidvid = calc_common_fidvid(fvp->common_fidvid, readback & 0xffff00);

	printk(BIOS_DEBUG, "\tcommon_fidvid=%08x\n", fvp->common_fidvid);
}

static void init_fidvid_bsp_stage2(unsigned ap_apicid, void *gp)
{
	unsigned int readback = 0;
	unsigned int timeout = 1;

	struct fidvid_st *fvp = gp;
	int loop;

	printk(BIOS_DEBUG, "state 2: ap_apicid=%08x\n", ap_apicid);

	/* all set to state2 */
	lapic_write(LAPIC_MSG_REG, fvp->common_fidvid | (ap_apicid<<24) | 2);

	loop = 1000000;
	while (--loop > 0) {
		if (lapic_remote_read(ap_apicid, LAPIC_MSG_REG, &readback)!=0)
			continue;
		if ((readback & 0xff) == 2) {
			timeout = 0;
			break; /* target ap is stage 2, it's FID has beed set */
		}
	}

	if (timeout) {
		printk(BIOS_ERR, 
			"fidvid_bsp_stage2: time out while reading from ap %08x\n", 
			ap_apicid);
		return;
	}

	printk(BIOS_DEBUG, "\treadback=%08x", readback);
}

#if K8_SET_FIDVID_STORE_AP_APICID_AT_FIRST == 1
struct ap_apicid_st {
	unsigned num;
	unsigned apicid[16]; /* 8 way dual core need 16 */
	/* FIXME: 32 node quad core, may need 128 */
};

static void store_ap_apicid(unsigned ap_apicid, void *gp)
{
	struct ap_apicid_st *p = gp;

	p->apicid[p->num++] = ap_apicid;

}
#endif

void init_fidvid_bsp(unsigned bsp_apicid)
{
	u32 vid_max;
	u32 fid_max;

	struct fidvid_st fv;

#if K8_SET_FIDVID_STORE_AP_APICID_AT_FIRST == 1
	struct ap_apicid_st ap_apicidx;
	unsigned int i;
#endif

	struct msr msr;
	msr =	rdmsr(FIDVID_STATUS);
	fid_max = ((msr.lo >> 16) & 0x3f); /* max fid */
	printk(BIOS_DEBUG, "init_fidvid_bsp: fid_max is 0x%x\n", fid_max);
#if FX_SUPPORT == 1
	if (fid_max >= ((25-4) * 2)) { /* FX max fid is 5G */
		fid_max = ((msr.lo >> 8) & 0x3f) + 5*2; /* maxFID = minFID + 1G */
		if (fid_max >= ((25-4) * 2)) {
			fid_max = (10-4) * 2; /* hard set to 2G */
		}
	}
#endif
	vid_max = ((msr.hi>>(48-32)) & 0x3f); //max vid
	fv.common_fidvid = (fid_max << 8)|(vid_max << 16);


	/* for all APs (We know the APIC ID of all APs even the APIC ID is lifted)
	 * remote read from AP about max fid/vid */

	/* let all ap trains to state 1 */
	lapic_write(LAPIC_MSG_REG,  (bsp_apicid << 24) | 1);

	/* calculate the common max fid/vid that could be used for 
	 * all APs and BSP */
#if K8_SET_FIDVID_STORE_AP_APICID_AT_FIRST == 1
	ap_apicidx.num = 0;

	for_each_ap(bsp_apicid, K8_SET_FIDVID_CORE0_ONLY, store_ap_apicid, &ap_apicidx);

	for (i = 0; i < ap_apicidx.num; i++) {
		init_fidvid_bsp_stage1(ap_apicidx.apicid[i], &fv);
	}
#else
	for_each_ap(bsp_apicid, K8_SET_FIDVID_CORE0_ONLY, init_fidvid_bsp_stage1, &fv);
#endif


#if 0
	unsigned fid, vid;
	/* Can we use max only? So we can only set fid in one around,
	 * otherwise we need to set that to max after raminit */
	/* set fid vid to DQS training required */
	fid = (fv.common_fidvid >> 8)  & 0x3f;
	vid = (fv.common_fidvid >> 16) & 0x3f;

	if (fid > (10-4) * 2) {
		fid = (10-4) * 2; // x10
	}

	if (vid >= 0x1f) {
		vid += 4; /* unit is 12.5mV */
	} else {
		vid += 2; /* unit is 25mV */
	}

	fv.common_fidvid = (fid<<8) | (vid<<16);

	printk(BIOS_DEBUG, "common_fidvid=%08x\n", fv.common_fidvid);

#endif

#if K8_SET_FIDVID_ONE_BY_ONE == 1
	/* set BSP fid and vid */
	printk(BIOS_DEBUG,"bsp apicid=%08x\n", bsp_apicid);
	fv.common_fidvid = set_fidvid(bsp_apicid, fv.common_fidvid, 1);
	printk(BIOS_DEBUG,"common_fidvid=%08x\n", fv.common_fidvid);
#endif

	/* For all APs ( We know the APIC ID of all AP even the APIC ID is lifted)
	 *  send signal to the AP it could change it's fid/vid */
	/* remote read signal from AP that AP is done */

	fv.common_fidvid &= 0xffff00;

	/* set state 2 allow is in init_fidvid_bsp_stage2 */
#if K8_SET_FIDVID_STORE_AP_APICID_AT_FIRST == 1
	for (i = 0; i < ap_apicidx.num; i++) {
		init_fidvid_bsp_stage2(ap_apicidx.apicid[i], &fv);
	}
#else
	for_each_ap(bsp_apicid, K8_SET_FIDVID_CORE0_ONLY, init_fidvid_bsp_stage2, &fv);
#endif

#if K8_SET_FIDVID_ONE_BY_ONE == 0
	/* set BSP fid and vid */
	printk(BIOS_DEBUG, "bsp apicid=%08x\n", bsp_apicid);
	fv.common_fidvid = set_fidvid(bsp_apicid, fv.common_fidvid, 1);
	printk(BIOS_DEBUG, "common_fidvid=%08x\n", fv.common_fidvid);

#endif

	/* clear the state */
	lapic_write(LAPIC_MSG_REG, fv.common_fidvid | (bsp_apicid << 24) | 3);

	/* here wait a while, so last ap could read pack, and stop it, don't
	 * call init_timer too early or just don't use init_timer */

}
