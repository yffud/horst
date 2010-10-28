/* horst - olsr scanning tool
 *
 * Copyright (C) 2005-2010 Bruno Randolf (br1@einfach.org)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <net/if_arp.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

#include "prism_header.h"
#include "ieee80211_radiotap.h"
#include "ieee80211.h"
#include "ieee80211_util.h"
#include "olsr_header.h"
#include "batman_header.h"

#include "protocol_parser.h"
#include "main.h"
#include "util.h"


static int parse_prism_header(unsigned char** buf, int len, struct packet_info* current_packet);
static int parse_radiotap_header(unsigned char** buf, int len, struct packet_info* current_packet);
static int parse_80211_header(unsigned char** buf, int len, struct packet_info* current_packet);
static int parse_llc(unsigned char** buf, int len, struct packet_info* current_packet);
static int parse_ip_header(unsigned char** buf, int len, struct packet_info* current_packet);
static int parse_udp_header(unsigned char** buf, int len, struct packet_info* current_packet);
static int parse_olsr_packet(unsigned char** buf, int len, struct packet_info* current_packet);
static int parse_batman_packet(unsigned char** buf, int len, struct packet_info* current_packet);


/* return 1 if we parsed enough = min ieee header */
int
parse_packet(unsigned char* buf, int len, struct packet_info* current_packet)
{
	if (conf.arphrd == ARPHRD_IEEE80211_PRISM) {
		len = parse_prism_header(&buf, len, current_packet);
		if (len <= 0)
			return 0;
	}
	else if (conf.arphrd == ARPHRD_IEEE80211_RADIOTAP) {
		len = parse_radiotap_header(&buf, len, current_packet);
		if (len <= 0)
			return 0;
	}

	if (conf.arphrd == ARPHRD_IEEE80211 ||
	    conf.arphrd == ARPHRD_IEEE80211_PRISM ||
	    conf.arphrd == ARPHRD_IEEE80211_RADIOTAP) {
		DEBUG("before parse 80211 len: %d\n", len);
		len = parse_80211_header(&buf, len, current_packet);
		if (len < 0) /* couldnt parse */
			return 0;
		else if (len == 0)
			return 1;
	}

	len = parse_llc(&buf, len, current_packet);
	if (len <= 0)
		return 1;

	len = parse_ip_header(&buf, len, current_packet);
	if (len <= 0)
		return 1;

	len = parse_udp_header(&buf, len, current_packet);
	if (len <= 0)
		return 1;

	return 1;
}


static int
parse_prism_header(unsigned char** buf, int len, struct packet_info* current_packet)
{
	wlan_ng_prism2_header* ph;

	DEBUG("PRISM2 HEADER\n");

	if (len < sizeof(wlan_ng_prism2_header))
		return -1;

	ph = (wlan_ng_prism2_header*)*buf;

	/*
	 * different drivers report S/N and rssi values differently
	 * let's make sure here that SNR is always positive, so we
	 * don't have do handle special cases later
	*/
	if (((int)ph->noise.data) < 0) {
		/* new madwifi */
		current_packet->signal = ph->signal.data;
		current_packet->noise = ph->noise.data;
		current_packet->snr = ph->rssi.data;
	}
	else if (((int)ph->rssi.data) < 0) {
		/* broadcom hack */
		current_packet->signal = ph->rssi.data;
		current_packet->noise = -95;
		current_packet->snr = 95 + ph->rssi.data;
	}
	else {
		/* assume hostap */
		current_packet->signal = ph->signal.data;
		current_packet->noise = ph->noise.data;
		current_packet->snr = ph->signal.data - ph->noise.data; //XXX rssi?
	}

	current_packet->rate = ph->rate.data;

	/* just in case...*/
	if (current_packet->snr < 0)
		current_packet->snr = -current_packet->snr;
	if (current_packet->snr > 99)
		current_packet->snr = 99;
	if (current_packet->rate == 0 || current_packet->rate > 108) {
		/* assume min rate, guess mode from channel */
		DEBUG("*** fixing wrong rate\n");
		if (ph->channel.data > 14)
			current_packet->rate = 12; /* 6 * 2 */
		else
			current_packet->rate = 2; /* 1 * 2 */
	}

	/* guess phy mode */
	if (ph->channel.data > 14)
		current_packet->phy_flags |= PHY_FLAG_A;
	else
		current_packet->phy_flags |= PHY_FLAG_G;
	/* always assume shortpre */
	current_packet->phy_flags |= PHY_FLAG_SHORTPRE;

	DEBUG("devname: %s\n", ph->devname);
	DEBUG("signal: %d -> %d\n", ph->signal.data, current_packet->signal);
	DEBUG("noise: %d -> %d\n", ph->noise.data, current_packet->noise);
	DEBUG("rate: %d\n", ph->rate.data);
	DEBUG("rssi: %d\n", ph->rssi.data);
	DEBUG("*** SNR %d\n", current_packet->snr);

	*buf = *buf + sizeof(wlan_ng_prism2_header);
	return len - sizeof(wlan_ng_prism2_header);
}


static int
parse_radiotap_header(unsigned char** buf, int len, struct packet_info* current_packet)
{
	struct ieee80211_radiotap_header* rh;
	__le32 present; /* the present bitmap */
	unsigned char* b; /* current byte */
	int i;
	u16 rt_len, x;

	DEBUG("RADIOTAP HEADER\n");

	DEBUG("len: %d\n", len);

	if (len < sizeof(struct ieee80211_radiotap_header))
		return -1;

	rh = (struct ieee80211_radiotap_header*)*buf;
	b = *buf + sizeof(struct ieee80211_radiotap_header);
	present = le32toh(rh->it_present);
	rt_len = le16toh(rh->it_len);
	DEBUG("radiotap header len: %d\n", rt_len);
	DEBUG("%08x\n", present);

	/* check for header extension - ignore for now, just advance current position */
	while (present & 0x80000000  && b - *buf < rt_len) {
		DEBUG("extension\n");
		b = b + 4;
		present = le32toh(*(__le32*)b);
	}
	present = le32toh(rh->it_present); // in case it moved

	/* radiotap bitmap has 32 bit, but we are only interrested until
	 * bit 12 (IEEE80211_RADIOTAP_DB_ANTSIGNAL) => i<13 */
	for (i = 0; i < 13 && b - *buf < rt_len; i++) {
		if ((present >> i) & 1) {
			DEBUG("1");
			switch (i) {
				/* just ignore the following (advance position only) */
				case IEEE80211_RADIOTAP_TSFT:
					DEBUG("[+8]");
					b = b + 8;
					break;
				case IEEE80211_RADIOTAP_DBM_TX_POWER:
				case IEEE80211_RADIOTAP_ANTENNA:
				case IEEE80211_RADIOTAP_RTS_RETRIES:
				case IEEE80211_RADIOTAP_DATA_RETRIES:
					DEBUG("[+1]");
					b++;
					break;
				case IEEE80211_RADIOTAP_EXT:
					DEBUG("[+4]");
					b = b + 4;
					break;
				case IEEE80211_RADIOTAP_FHSS:
				case IEEE80211_RADIOTAP_LOCK_QUALITY:
				case IEEE80211_RADIOTAP_TX_ATTENUATION:
				case IEEE80211_RADIOTAP_RX_FLAGS:
				case IEEE80211_RADIOTAP_TX_FLAGS:
				case IEEE80211_RADIOTAP_DB_TX_ATTENUATION:
					DEBUG("[+2]");
					b = b + 2;
					break;
				/* we are only interrested in these: */
				case IEEE80211_RADIOTAP_RATE:
					DEBUG("[rate %0x]", *b);
					current_packet->rate = (*b);
					b++;
					break;
				case IEEE80211_RADIOTAP_DBM_ANTSIGNAL:
					DEBUG("[sig %0x]", *b);
					current_packet->signal = *(char*)b;
					b++;
					break;
				case IEEE80211_RADIOTAP_DBM_ANTNOISE:
					DEBUG("[noi %0x]", *b);
					current_packet->noise = *(char*)b;
					b++;
					break;
				case IEEE80211_RADIOTAP_DB_ANTSIGNAL:
					DEBUG("[snr %0x]", *b);
					current_packet->snr = *b;
					b++;
					break;
				case IEEE80211_RADIOTAP_FLAGS:
					/* short preamble */
					DEBUG("[flags %0x", *b);
					if (*b & IEEE80211_RADIOTAP_F_SHORTPRE) {
						current_packet->phy_flags |= PHY_FLAG_SHORTPRE;
						DEBUG(" shortpre");
					}
					DEBUG("]");
					b++;
					break;
				case IEEE80211_RADIOTAP_CHANNEL:
					/* channel & channel type */
					current_packet->phy_freq = le16toh(*(u_int16_t*)b);
					DEBUG("[chan %d ", current_packet->phy_freq);
					b = b + 2;
					x = le16toh(*(u_int16_t*)b);
					if (x & IEEE80211_CHAN_A) {
						current_packet->phy_flags |= PHY_FLAG_A;
						DEBUG("A]");
					}
					else if (x & IEEE80211_CHAN_G) {
						current_packet->phy_flags |= PHY_FLAG_G;
						DEBUG("G]");
					}
					else if (x & IEEE80211_CHAN_B) {
						current_packet->phy_flags |= PHY_FLAG_B;
						DEBUG("B]");
					}
					b = b + 2;
					break;
			}
		}
		else {
			DEBUG("0");
		}
	}
	DEBUG("\n");

	if (!(present & (1 << IEEE80211_RADIOTAP_DB_ANTSIGNAL))) {
		/* no SNR in radiotap, try to calculate */
		if (present & (1 << IEEE80211_RADIOTAP_DBM_ANTSIGNAL) &&
		    present & (1 << IEEE80211_RADIOTAP_DBM_ANTNOISE))
			current_packet->snr = current_packet->signal - current_packet->noise;
		/* HACK: here we just assume noise to be -95dBm */
		else if (!(present & (1 << IEEE80211_RADIOTAP_DBM_ANTNOISE)))
			current_packet->snr = current_packet->signal + 95;
	}

	/* sanitize */
	if (current_packet->snr > 99)
		current_packet->snr = 99;
	if (current_packet->rate == 0 || current_packet->rate > 108) {
		/* assume min rate for mode */
		DEBUG("*** fixing wrong rate\n");
		if (current_packet->phy_flags & PHY_FLAG_A)
			current_packet->rate = 12; /* 6 * 2 */
		else if (current_packet->phy_flags & PHY_FLAG_B)
			current_packet->rate = 2; /* 1 * 2 */
		else if (current_packet->phy_flags & PHY_FLAG_G)
			current_packet->rate = 12; /* 6 * 2 */
		else
			current_packet->rate = 2;
	}

	DEBUG("\nrate: %d\n", current_packet->rate);
	DEBUG("signal: %d\n", current_packet->signal);
	DEBUG("noise: %d\n", current_packet->noise);
	DEBUG("snr: %d\n", current_packet->snr);

	*buf = *buf + rt_len;
	return len - rt_len;
}


static int
parse_80211_header(unsigned char** buf, int len, struct packet_info* current_packet)
{
	struct ieee80211_hdr* wh;
	struct ieee80211_mgmt* whm;
	int hdrlen;
	u8* sa = NULL;
	u8* da = NULL;
	u8* bssid = NULL;
	u16 fc, cap_i;

	if (len < 2) /* not even enough space for fc */
		return -1;

	wh = (struct ieee80211_hdr*)*buf;
	fc = le16toh(wh->frame_control);
	hdrlen = ieee80211_get_hdrlen(fc);

	DEBUG("len %d hdrlen %d\n", len, hdrlen);

	if (len < hdrlen)
		return -1;

	current_packet->len = len;
	current_packet->wlan_type = (fc & (IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE));

	DEBUG("wlan_type %x - type %x - stype %x\n", fc, fc & IEEE80211_FCTL_FTYPE, fc & IEEE80211_FCTL_STYPE );

	DEBUG("%s\n", get_packet_type_name(fc));

	bssid = ieee80211_get_bssid(wh, len);

	switch (current_packet->wlan_type & IEEE80211_FCTL_FTYPE) {
	case IEEE80211_FTYPE_DATA:
		current_packet->pkt_types = PKT_TYPE_DATA;
		switch (current_packet->wlan_type & IEEE80211_FCTL_STYPE) {
		case IEEE80211_STYPE_NULLFUNC:
			current_packet->pkt_types |= PKT_TYPE_NULL;
			break;
		case IEEE80211_STYPE_QOS_DATA:
			/* TODO: ouch, should properly define a qos header */
			current_packet->wlan_qos_class = wh->addr4[0] & 0x7;
			DEBUG("***QDATA %x\n", current_packet->wlan_qos_class);
			break;
		}
		current_packet->wlan_nav = le16toh(wh->duration_id);
		DEBUG("DATA NAV %d\n", current_packet->wlan_nav);
		sa = ieee80211_get_SA(wh);
		da = ieee80211_get_DA(wh);
		/* AP, STA or IBSS */
		if ((fc & IEEE80211_FCTL_FROMDS) == 0 &&
		    (fc & IEEE80211_FCTL_TODS) == 0)
			current_packet->wlan_mode = WLAN_MODE_IBSS;
		else if (fc & IEEE80211_FCTL_FROMDS)
			current_packet->wlan_mode = WLAN_MODE_AP;
		else if (fc & IEEE80211_FCTL_TODS)
			current_packet->wlan_mode = WLAN_MODE_STA;
		/* WEP */
		if (fc & IEEE80211_FCTL_PROTECTED)
			current_packet->wlan_wep = 1;

		if (fc & IEEE80211_FCTL_RETRY)
			current_packet->wlan_retry =1;

		break;

	case IEEE80211_FTYPE_CTL:
		current_packet->pkt_types = PKT_TYPE_CTRL;
		switch (current_packet->wlan_type & IEEE80211_FCTL_STYPE) {
		case IEEE80211_STYPE_RTS:
			current_packet->pkt_types |= PKT_TYPE_RTS;
			current_packet->wlan_nav = le16toh(wh->duration_id);
			DEBUG("RTS NAV %d\n", current_packet->wlan_nav);
			sa = wh->addr2;
			da = wh->addr1;
			break;

		case IEEE80211_STYPE_CTS:
			current_packet->pkt_types |= PKT_TYPE_CTS;
			current_packet->wlan_nav = le16toh(wh->duration_id);
			DEBUG("CTS NAV %d\n", current_packet->wlan_nav);
			da = wh->addr1;
			break;

		case IEEE80211_STYPE_ACK:
			current_packet->pkt_types |= PKT_TYPE_ACK;
			current_packet->wlan_nav = le16toh(wh->duration_id);
			DEBUG("ACK NAV %d\n", current_packet->wlan_nav);
			da = wh->addr1;
			break;

		case IEEE80211_STYPE_PSPOLL:
			sa = wh->addr2;
			break;

		case IEEE80211_STYPE_CFEND:
			da = wh->addr1;
			sa = wh->addr2;
			break;

		case IEEE80211_STYPE_CFENDACK:
			/* dont know, dont care */
			break;
		}
		break;

	case IEEE80211_FTYPE_MGMT:
		current_packet->pkt_types = PKT_TYPE_MGMT;
		whm = (struct ieee80211_mgmt*)*buf;
		sa = whm->sa;
		da = whm->da;

		switch (current_packet->wlan_type & IEEE80211_FCTL_STYPE) {
		case IEEE80211_STYPE_BEACON:
			current_packet->pkt_types |= PKT_TYPE_BEACON;
			current_packet->wlan_tsf = le64toh(whm->u.beacon.timestamp);
			ieee802_11_parse_elems(whm->u.beacon.variable,
				len - sizeof(struct ieee80211_mgmt) - 4 /* FCS */, current_packet);
			DEBUG("ESSID %s \n", current_packet->wlan_essid );
			DEBUG("CHAN %d \n", current_packet->wlan_channel );
			cap_i = le16toh(whm->u.beacon.capab_info);
			if (cap_i & WLAN_CAPABILITY_IBSS)
				current_packet->wlan_mode = WLAN_MODE_IBSS;
			else if (cap_i & WLAN_CAPABILITY_ESS)
				current_packet->wlan_mode = WLAN_MODE_AP;
			if (cap_i & WLAN_CAPABILITY_PRIVACY)
				current_packet->wlan_wep = 1;
			break;

		case IEEE80211_STYPE_PROBE_RESP:
			current_packet->pkt_types |= PKT_TYPE_PROBE;
			current_packet->wlan_tsf = le64toh(whm->u.beacon.timestamp);
			ieee802_11_parse_elems(whm->u.beacon.variable,
				len - sizeof(struct ieee80211_mgmt) - 4 /* FCS */, current_packet);
			DEBUG("ESSID %s \n", current_packet->wlan_essid );
			DEBUG("CHAN %d \n", current_packet->wlan_channel );
			cap_i = le16toh(whm->u.beacon.capab_info);
			if (cap_i & WLAN_CAPABILITY_IBSS)
				current_packet->wlan_mode = WLAN_MODE_IBSS;
			else if (cap_i & WLAN_CAPABILITY_ESS)
				current_packet->wlan_mode = WLAN_MODE_AP;
			if (cap_i & WLAN_CAPABILITY_PRIVACY)
				current_packet->wlan_wep = 1;
			break;

		case IEEE80211_STYPE_PROBE_REQ:
			current_packet->pkt_types |= PKT_TYPE_PROBE;
			ieee802_11_parse_elems(whm->u.probe_req.variable,
				len - 24 - 4 /* FCS */,
				current_packet);
			current_packet->wlan_mode |= WLAN_MODE_PROBE;
			break;

		case IEEE80211_STYPE_ASSOC_REQ:
		case IEEE80211_STYPE_ASSOC_RESP:
		case IEEE80211_STYPE_REASSOC_REQ:
		case IEEE80211_STYPE_REASSOC_RESP:
		case IEEE80211_STYPE_DISASSOC:
			current_packet->pkt_types |= PKT_TYPE_ASSOC;
			break;

		case IEEE80211_STYPE_AUTH:
		case IEEE80211_STYPE_DEAUTH:
			current_packet->pkt_types |= PKT_TYPE_AUTH;
			break;
		}
		break;
	}

	if (sa != NULL) {
		memcpy(current_packet->wlan_src, sa, MAC_LEN);
		DEBUG("SA    %s\n", ether_sprintf(sa));
	}
	if (da != NULL) {
		memcpy(current_packet->wlan_dst, da, MAC_LEN);
		DEBUG("DA    %s\n", ether_sprintf(da));
	}
	if (bssid!=NULL) {
		memcpy(current_packet->wlan_bssid, bssid, MAC_LEN);
		DEBUG("BSSID %s\n", ether_sprintf(bssid));
	}

	/* only data frames contain more info, otherwise stop parsing */
	if (IEEE80211_IS_DATA(current_packet->wlan_type) &&
	    current_packet->wlan_wep != 1) {
		*buf = *buf + hdrlen;
		return len - hdrlen;
	}
	return 0;
}


static int
parse_llc(unsigned char ** buf, int len, struct packet_info* current_packet)
{
	DEBUG("* parse LLC\n");

	if (len < 6)
		return -1;

	/* check type in LLC header */
	*buf = *buf + 6;
	if (**buf != 0x08)
		return -1;
	(*buf)++;
	if (**buf == 0x06) { /* ARP */
		current_packet->pkt_types |= PKT_TYPE_ARP;
		return 0;
	}
	if (**buf != 0x00)  /* not IP */
		return -1;
	(*buf)++;

	DEBUG("* parse LLC left %d\n", len - 8);

	return len - 8;
}


static int
parse_ip_header(unsigned char** buf, int len, struct packet_info* current_packet)
{
	struct iphdr* ih;

	DEBUG("* parse IP\n");

	if (len < sizeof(struct iphdr))
		return -1;

	ih = (struct iphdr*)*buf;

	DEBUG("*** IP SRC: %s\n", ip_sprintf(ih->saddr));
	DEBUG("*** IP DST: %s\n", ip_sprintf(ih->daddr));
	current_packet->ip_src = ih->saddr;
	current_packet->ip_dst = ih->daddr;
	current_packet->pkt_types |= PKT_TYPE_IP;

	DEBUG("IP proto: %d\n", ih->protocol);
	switch (ih->protocol) {
	case IPPROTO_UDP: current_packet->pkt_types |= PKT_TYPE_UDP; break;
	/* all others set the type and return. no more parsing */
	case IPPROTO_ICMP: current_packet->pkt_types |= PKT_TYPE_ICMP; return 0;
	case IPPROTO_TCP: current_packet->pkt_types |= PKT_TYPE_TCP; return 0;
	}


	*buf = *buf + ih->ihl * 4;
	return len - ih->ihl * 4;
}


static int
parse_udp_header(unsigned char** buf, int len, struct packet_info* current_packet)
{
	struct udphdr* uh;

	if (len < sizeof(struct udphdr))
		return -1;

	uh = (struct udphdr*)*buf;

	DEBUG("UPD dest port: %d\n", ntohs(uh->dest));

	*buf = *buf + 8;
	len = len - 8;

	if (ntohs(uh->dest) == 698) /* OLSR */
		return parse_olsr_packet(buf, len, current_packet);

	if (ntohs(uh->dest) == BAT_PORT) /* batman */
		return parse_batman_packet(buf, len, current_packet);

	return 0;
}


static int
parse_olsr_packet(unsigned char** buf, int len, struct packet_info* current_packet)
{
	struct olsr* oh;
	int number, i, msgtype;

	if (len < sizeof(struct olsr))
		return -1;

	oh = (struct olsr*)*buf;

	// TODO: more than one olsr messages can be in one packet
	msgtype = oh->olsr_msg[0].olsr_msgtype;

	DEBUG("OLSR msgtype: %d\n*** ", msgtype);

	current_packet->pkt_types |= PKT_TYPE_OLSR;
	current_packet->olsr_type = msgtype;

	if (msgtype == LQ_HELLO_MESSAGE || msgtype == LQ_TC_MESSAGE )
		current_packet->pkt_types |= PKT_TYPE_OLSR_LQ;

	if (msgtype == HELLO_MESSAGE) {
		number = (ntohs(oh->olsr_msg[0].olsr_msgsize) - 12) / sizeof(struct hellomsg);
		DEBUG("HELLO %d\n", number);
		current_packet->olsr_neigh = number;
	}

	if (msgtype == LQ_HELLO_MESSAGE) {
		number = (ntohs(oh->olsr_msg[0].olsr_msgsize) - 16) / 12;
		DEBUG("LQ_HELLO %d (%d)\n", number, (ntohs(oh->olsr_msg[0].olsr_msgsize) - 16));
		current_packet->olsr_neigh = number;
	}
#if 0
/*	XXX: tc messages are relayed. so we would have to find the originating node (IP)
	and store the information there. skip for now */

	if (msgtype == TC_MESSAGE) {
		number = (ntohs(oh->olsr_msg[0].olsr_msgsize)-12) / sizeof(struct tcmsg);
		DEBUG("TC %d\n", number);
		current_packet->olsr_tc = number;
	}

	if (msgtype == LQ_TC_MESSAGE) {
		number = (ntohs(oh->olsr_msg[0].olsr_msgsize)-16) / 8;
		DEBUG("LQ_TC %d (%d)\n", number, (ntohs(oh->olsr_msg[0].olsr_msgsize)-16));
		current_packet->olsr_tc = number;
	}
#endif
	if (msgtype == HNA_MESSAGE) {
		/* same here, but we assume that nodes which relay a HNA with a default gateway
		know how to contact the gw, so have a indirect connection to a GW themselves */
		struct hnapair* hna;
		number = (ntohs(oh->olsr_msg[0].olsr_msgsize) - 12) / sizeof(struct hnapair);
		DEBUG("HNA NUM: %d (%d) [%d]\n", number, ntohs(oh->olsr_msg[0].olsr_msgsize),
			(int)sizeof(struct hnapair) );
		for (i = 0; i < number; i++) {
			hna = &(oh->olsr_msg[0].message.hna.hna_net[i]);
			DEBUG("HNA %s", ip_sprintf(hna->addr));
			DEBUG("/%s\n", ip_sprintf(hna->netmask));
			if (hna->addr == 0 && hna->netmask == 0)
				current_packet->pkt_types |= PKT_TYPE_OLSR_GW;
		}
	}
	/* done for good */
	return 0;
}


static int
parse_batman_packet(unsigned char** buf, int len, struct packet_info* current_packet)
{
	current_packet->pkt_types |= PKT_TYPE_BATMAN;

	return 0;
}
