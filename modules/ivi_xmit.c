/*************************************************************************
 *
 * ivi_xmit.c :
 *
 * MAP-T/MAP-E Packet Transmission Kernel Module
 *
 * Copyright (C) 2013 CERNET Network Center
 * All rights reserved.
 * 
 * Design and coding: 
 *   Xing Li <xing@cernet.edu.cn> 
 *	 Congxiao Bao <congxiao@cernet.edu.cn>
 *   Guoliang Han <bupthgl@gmail.com>
 * 	 Yuncheng Zhu <haoyu@cernet.edu.cn>
 * 	 Wentao Shang <wentaoshang@gmail.com>
 * 	
 * 
 * Contributions:
 *
 * This file is part of MAP-T/MAP-E Kernel Module.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * You should have received a copy of the GNU General Public License 
 * along with MAP-T/MAP-E Kernel Module. If not, see 
 * <http://www.gnu.org/licenses/>.
 *
 * For more versions, please send an email to <bupthgl@gmail.com> to
 * obtain an password to access the svn server.
 *
 * LIC: GPLv3
 *
 ************************************************************************/

#include "ivi_xmit.h"


static inline int link_local_addr(const struct in6_addr *addr) {
	return ((addr->s6_addr32[0] & htonl(0xffc00000)) == htonl(0xfe800000));
}

static inline int mc_v6_addr(const struct in6_addr *addr) {
	return (addr->s6_addr[0] == 0xff);
}

static inline int addr_in_v4network(const unsigned int *addr) {
	return ((ntohl(*addr) & v4mask) == (v4address & v4mask));
}

u8 ivi_mode = 0;  // working mode for IVI translation

/*
 * Local parameter cache for fast path local address translation in hgw mode
 */

// IPv4 address of v4dev
__be32 v4address = 0x01010101;  // "1.1.1.1" in host byte order

__be32 v4mask = 0xffffff00;  // "/24"

// NAT public address and mask for v4 network
__be32 v4publicaddr = 0x03030303;  // "3.3.3.3" in host byte order

__be32 v4publicmask = 0xffffff00;  // "/24"

// v6 prefix where v4 network or public address is mapped into.
__u8 v6prefix[16] = { 0x20, 0x01, 0x0d, 0xa8, 0x01, 0x23, 0x04, 0x56 };  // "2001:da8:123:456::" in network byte order

__be32 v6prefixlen = 64;  // "/64" prefix length

u8 hgw_fmt = ADDR_FMT_MAPT;  // default address format is MAP-T

u8 hgw_transport = 0;  // header manipulation manner

u16 mss_limit = 1440;  // max mss supported


#define ADDR_DIR_SRC 0
#define ADDR_DIR_DST 1

static int ipaddr_4to6(unsigned int *v4addr, u16 port, u8 _dir, struct in6_addr *v6addr, u8 *transpt) {
	int prefixlen, plen4, ealen;
	u32 eabits;  //FIXME: we assume 'ealen' won't be larger than 32 although max length of eabits is 48
	u32 addr, mask;
	u16 ratio, adjacent, offset, suffix;
	u8 fmt, remainder, i, o;

	addr = ntohl(*v4addr);
	eabits = 0;
	ealen = 0;
	ratio = adjacent = offset = suffix = fmt = 0;
	remainder= 0;

	memset(v6addr, 0, sizeof(struct in6_addr));

	if (_dir == ADDR_DIR_DST) {		
		if (ivi_rule_lookup(addr, v6addr, &plen4, &prefixlen, &ratio, &adjacent, &fmt, transpt) != 0) {
			printk(KERN_DEBUG "ipaddr_4to6: failed to map v4 addr " NIP4_FMT "\n", NIP4(addr));
			return -1;
		}
		
		// when *transpt is set to MAP_E, an /128 IPv6 destination address is used in encapsulation header.
		if (*transpt == MAP_E) 
			return 0;
		
  		remainder = prefixlen - ((prefixlen >> 3) << 3); // in case IPv6 prefix isn't on a BYTE boundary
		prefixlen = prefixlen >> 3; // counted in bytes
		
		if (fmt != ADDR_FMT_NONE && ratio && adjacent) { // FMR matching rule found
			// Create EA bits for MAP format
			ratio = fls(ratio) - 1; // Length of PSID
			adjacent = fls(adjacent) - 1; // Length of M
					
			mask = ntohl(inet_make_mask(32 - ratio));
			offset = (port >> adjacent) & ~mask & 0xffff;
			
			mask = ntohl(inet_make_mask(plen4));
			eabits = (addr & ~mask) << plen4;
			ealen = 32 - plen4 + ratio;  // IPv4 suffix length + length of PSID			
			eabits += offset << (32 - ealen);
			
			if (fmt == ADDR_FMT_MAPT || fmt == ADDR_FMT_MAPX_CPE)
				suffix = offset; // left-padded
		}
		
	} else if (_dir == ADDR_DIR_SRC) {
		// Fast path for local address translation in hgw mode, use global parameters
		prefixlen = v6prefixlen >> 3;
		remainder = v6prefixlen - (prefixlen << 3);
		fmt = hgw_fmt;
		
		if (hgw_fmt == ADDR_FMT_MAPX_CPE && prefixlen != 8) {
#ifdef IVI_DEBUG_RULE
			printk(KERN_DEBUG "ipaddr_4to6: MAP-X CPE prefix must be /64.\n");
#endif
			return -1;
		}
		
		// If prefix length isn't on a BYTE boundary, we have to copy (prefixlen + 1) bytes
		if(remainder) 
			memcpy(v6addr, v6prefix, prefixlen + 1);
		else
			memcpy(v6addr, v6prefix, prefixlen);
			
		ratio = hgw_ratio;
		offset = hgw_offset;
		suffix = hgw_suffix;
		
		if (ivi_mode == IVI_MODE_HGW_NAT44)
			mask = v4publicmask;
		else
			mask = v4mask;
		
		// Create EA bits for MAP format
		ealen = ffs(mask) - 1;  // Length of IPv4 subnet ID
		eabits = (addr & ~mask) << (32 - ealen);
		ealen += fls(ratio) - 1;  // Length of PSID + Length of IPv4 subnet ID
		eabits += offset << (32 - ealen);
				
	}

	if (fmt == ADDR_FMT_MAPT) {
		if ((prefixlen << 3) + ealen <= 64) {
  			o = 24 + remainder; // initial offset for the first uncompleted byte
  			for (i = prefixlen; i < 8; i++) { // eabits outside /64 are cutted
 				v6addr->s6_addr[i] += (unsigned char)((eabits >> o) & 0xff);
 				o -= 8;
  			}
  			v6addr->s6_addr[8] = 0x00;
			v6addr->s6_addr[9] = (unsigned char)(addr >> 24);
			v6addr->s6_addr[10] = (unsigned char)((addr >> 16) & 0xff);
			v6addr->s6_addr[11] = (unsigned char)((addr >> 8) & 0xff);
			v6addr->s6_addr[12] = (unsigned char)(addr & 0xff);
			v6addr->s6_addr[13] = (suffix >> 8) & 0xff;
			v6addr->s6_addr[14] = suffix & 0xff;
		} else {
#ifdef IVI_DEBUG_RULE
			printk(KERN_DEBUG "ipaddr_4to6: cannot map v4 addr " NIP4_FMT \
			                  " because 'prefixlen + ealen' exceed 64\n", NIP4(addr));
#endif
			return -1;
		}
	} else if (fmt == ADDR_FMT_MAPX_CPE) {
		// this format has no eabits
		v6addr->s6_addr[9] = (unsigned char)(addr >> 24);
		v6addr->s6_addr[10] = (unsigned char)((addr >> 16) & 0xff);
		v6addr->s6_addr[11] = (unsigned char)((addr >> 8) & 0xff);
		v6addr->s6_addr[12] = (unsigned char)(addr & 0xff);
		v6addr->s6_addr[13] = (suffix >> 8) & 0xff;
		v6addr->s6_addr[14] = suffix & 0xff;
	} else {
		// DMR translation: just copy the addr
		v6addr->s6_addr[9] = (unsigned char)(addr >> 24);
		v6addr->s6_addr[10] = (unsigned char)((addr >> 16) & 0xff);
		v6addr->s6_addr[11] = (unsigned char)((addr >> 8) & 0xff);
		v6addr->s6_addr[12] = (unsigned char)(addr & 0xff);
	}

	return 0;
}

static int ipaddr_6to4(struct in6_addr *v6addr, u8 _dir, unsigned int *v4addr, u16 *ratio, u16 *adjacent, u16 *offset) {
	u32 addr;
	int prefixlen;
	u8 fmt;
	int retval;
	u32 prefix4, mask;
	int plen4;

	addr = prefix4 = mask = 0;
	fmt = 0;
	plen4 = 0;
	retval = 0;
	
	if ((ratio == NULL) || (adjacent == NULL) ||(offset == NULL)) {
		return -1;
	}

	// Do not translate ipv6 link local address.
	if (link_local_addr(v6addr)) {
#ifdef IVI_DEBUG_RULE
		printk(KERN_DEBUG "ipaddr_6to4: ignore link local address.\n");
#endif
		return -1;
	}
	
	addr |= ((unsigned int)v6addr->s6_addr[9]) << 24;
	addr |= ((unsigned int)v6addr->s6_addr[10]) << 16;
	addr |= ((unsigned int)v6addr->s6_addr[11]) << 8;
	addr |= ((unsigned int)v6addr->s6_addr[12]);
	*v4addr = htonl(addr);

	if (_dir == ADDR_DIR_DST) {		
		// Do not translate native IPv6 address
		if (ivi_mode == IVI_MODE_HGW && ((addr & v4mask) != (v4address & v4mask))) {
			//printk(KERN_DEBUG "ipaddr6to4: destination address not translated\n");
			return -1;
		} 
		else if (ivi_mode == IVI_MODE_HGW_NAT44 && ((addr & v4publicmask) != (v4publicaddr & v4publicmask))) {
			//printk(KERN_DEBUG "ipaddr6to4: destination address not translated\n");
			return -1;
		}
		
		fmt = hgw_fmt;
		*ratio = hgw_ratio;
		*adjacent = hgw_adjacent;
		*offset = hgw_offset;
	}

	else if (_dir == ADDR_DIR_SRC) {
		if (ivi_rule6_lookup(v6addr, &prefixlen, &prefix4, &plen4, ratio, adjacent, &fmt) != 0) {
			// Solve the problem of "MAP-T packet's src address doesn't have a matching rule in MAP-E opposite end"
			*ratio = 1;
			*adjacent = 1;
			fmt = ADDR_FMT_NONE;
			retval = 1;
		}

		/* offset is obtained from Interface Identifier */	
		if (fmt == ADDR_FMT_MAPT)
			*offset = (v6addr->s6_addr[13] << 8) + v6addr->s6_addr[14];
		else if (fmt == ADDR_FMT_NONE)
			*offset = 0;
	}

	return retval;
}

int ivi_v4v6_xmit(struct sk_buff *skb) {
	struct sk_buff *newskb;
	struct ethhdr *eth4, *eth6;
	struct iphdr *ip4h;
	struct ipv6hdr *ip6h;
	struct tcphdr *tcph;
	struct udphdr *udph;
	struct icmphdr *icmph;
	struct icmp6hdr *icmp6h;
	__u8 *payload;
	unsigned int hlen, plen;
	u16 newp, s_port, d_port;
	u8 transport;
	char flag_udp_nullcheck;
	
	eth4 = eth_hdr(skb);
	if (unlikely(eth4->h_proto != __constant_ntohs(ETH_P_IP))) {
		// This should not happen since we are hooked on PF_INET.
#ifdef IVI_DEBUG
		printk(KERN_ERR "ivi_v4v6_xmit: non-IPv4 packet type %x received on IPv4 hook.\n", ntohs(eth4->h_proto));
#endif
		return -EINVAL;  // Just accept.
	}

	ip4h = ip_hdr(skb);
	
	// By pass multicast packet
	if (ipv4_is_multicast(ip4h->daddr) || ipv4_is_lbcast(ip4h->daddr) || ipv4_is_loopback(ip4h->daddr)) {
#ifdef IVI_DEBUG
		printk(KERN_DEBUG "ivi_v4v6_xmit: by pass ipv4 multicast/broadcast/loopback dest address.\n");
#endif
		return -EINVAL;  // Just accept.
	}

	// Do not translate ipv4 packets (hair pin) that are toward v4network.
	if (addr_in_v4network(&(ip4h->daddr))) {
#ifdef IVI_DEBUG
		printk(KERN_DEBUG "ivi_v4v6_xmit: IPv4 packet toward the v4 network bypassed in HGW mode.\n");
#endif
		return -EINVAL;  // Just accept.
	}

	if (ip4h->ttl <= 1) {
		return -EINVAL;  // Just accept.
	}
	
	plen = ntohs(ip4h->tot_len) - (ip4h->ihl * 4);
	payload = (__u8 *)(ip4h) + (ip4h->ihl << 2);
	s_port = d_port = newp = 0;
	transport = 0;
	flag_udp_nullcheck = 0;

	switch (ip4h->protocol) {
		case IPPROTO_TCP:
			tcph = (struct tcphdr *)payload;
			
			if (tcph->syn && (tcph->doff > 5)) {
				__u16 *option = (__u16*)tcph;
				if (option[10] == htons(0x0204)) {
					if (ntohs(option[11]) > mss_limit) {
						csum_replace2(&tcph->check, option[11], htons(mss_limit));
						option[11] = htons(mss_limit);
					}
				}
			}
			
			if (ivi_mode == IVI_MODE_HGW && ntohs(tcph->source) < 1024) {
				newp = ntohs(tcph->source);
			}
			
			else if (get_outflow_tcp_map_port(ntohl(ip4h->saddr), ntohs(tcph->source), ntohl(ip4h->daddr), \
				ntohs(tcph->dest), hgw_ratio, hgw_adjacent, hgw_offset, tcph, plen, &newp) == -1) {
#ifdef IVI_DEBUG
				printk(KERN_ERR "ivi_v4v6_xmit: fail to perform nat44 mapping for " NIP4_FMT \
				                ":%d (TCP).\n", NIP4(ip4h->saddr), ntohs(tcph->source));
#endif
				return 0; // silently drop
					
			}
			
			if (ivi_mode == IVI_MODE_HGW_NAT44) {
				csum_replace4(&tcph->check, ip4h->saddr, htonl(v4publicaddr));
				csum_replace4(&ip4h->check, ip4h->saddr, htonl(v4publicaddr));
				ip4h->saddr = htonl(v4publicaddr);
			}
			csum_replace2(&tcph->check, tcph->source, htons(newp));
			tcph->source = htons(newp);
			s_port = ntohs(tcph->source);
			d_port = ntohs(tcph->dest);

			break;

		case IPPROTO_UDP:
			udph = (struct udphdr *)payload;
			if (udph->check == 0) 
				flag_udp_nullcheck = 1;
			
			if (ivi_mode == IVI_MODE_HGW && ntohs(udph->source) < 1024) {
				newp = ntohs(udph->source);
			}
			
			else if (get_outflow_map_port(&udp_list, ntohl(ip4h->saddr), ntohs(udph->source), \
				ntohl(ip4h->daddr), hgw_ratio, hgw_adjacent, hgw_offset, &newp) == -1) {
#ifdef IVI_DEBUG
				printk(KERN_ERR "ivi_v4v6_xmit: fail to perform nat44 mapping for " NIP4_FMT \
				                ":%d (UDP).\n", NIP4(ip4h->saddr), ntohs(udph->source));
#endif
				return 0; // silently drop
				
			} 
			
			if (ivi_mode == IVI_MODE_HGW_NAT44) {
				if (!flag_udp_nullcheck) {
					csum_replace4(&udph->check, ip4h->saddr, htonl(v4publicaddr));
				}
				csum_replace4(&ip4h->check, ip4h->saddr, htonl(v4publicaddr));
				ip4h->saddr = htonl(v4publicaddr);
			}
			if (!flag_udp_nullcheck) {
				csum_replace2(&udph->check, udph->source, htons(newp));
			}
			udph->source = htons(newp);
			s_port = ntohs(udph->source);
			d_port = ntohs(udph->dest);

			break;

		case IPPROTO_ICMP:
			icmph = (struct icmphdr *)payload;

			if (icmph->type == ICMP_ECHO) {
				if (get_outflow_map_port(&icmp_list, ntohl(ip4h->saddr), ntohs(icmph->un.echo.id), \
					ntohl(ip4h->daddr), hgw_ratio, hgw_adjacent, hgw_offset, &newp) == -1) {
#ifdef IVI_DEBUG
					printk(KERN_ERR "ivi_v4v6_xmit: fail to perform nat44 mapping for " NIP4_FMT \
					                ":%d (ICMP).\n", NIP4(ip4h->saddr), ntohs(icmph->un.echo.id));
#endif
					return 0; // silently drop
						
				} else {
					if (ivi_mode == IVI_MODE_HGW_NAT44) {
						csum_replace4(&ip4h->check, ip4h->saddr, htonl(v4publicaddr));
						ip4h->saddr = htonl(v4publicaddr);
					}
					csum_replace2(&icmph->checksum, icmph->un.echo.id, htons(newp));
					icmph->un.echo.id = htons(newp);
				}
				s_port = d_port = ntohs(icmph->un.echo.id);
							
			} else if (icmph->type == ICMP_ECHOREPLY) {
				if (ivi_mode == IVI_MODE_HGW_NAT44) { 
#ifdef IVI_DEBUG
					printk(KERN_ERR "ivi_v4v6_xmit: we currently doesn't send ECHO-REPLY " \
					                "when CPE is working in NAT44 mode\n");
#endif
					return 0; // silently drop
				}
				s_port = d_port = ntohs(icmph->un.echo.id);
				
			} else {
				printk(KERN_ERR "ivi_v4v6_xmit: unsupported ICMP type in NAT44. Drop packet now.\n");
				return 0;
			}

			break;

#ifdef IVI_DEBUG
		default:
			printk(KERN_ERR "ivi_v4v6_xmit: unsupported protocol %d in IPv4 packet.\n", ip4h->protocol);
#endif
	}

	hlen = sizeof(struct ipv6hdr);
	if (!(newskb = dev_alloc_skb(2 + ETH_HLEN + hlen + htons(ip4h->tot_len)))) {
		// Allocation size is enough for both E and T;
		// Even in ICMP translation case, it's enough for two IP headers' translation. 
		printk(KERN_ERR "ivi_v4v6_xmit: failed to allocate new socket buffer.\n");
		return 0;  // Drop packet on low memory
	}
	skb_reserve(newskb, 2);  // Align IP header on 16 byte boundary (ETH_LEN + 2)

	eth6 = (struct ethhdr *)skb_put(newskb, ETH_HLEN);
	// Keep mac unchanged
	memcpy(eth6, eth4, 12);
	eth6->h_proto  = __constant_ntohs(ETH_P_IPV6);

	ip6h = (struct ipv6hdr *)skb_put(newskb, hlen);

	if (ipaddr_4to6(&(ip4h->daddr), d_port, ADDR_DIR_DST, &(ip6h->daddr), &transport) != 0) {
		kfree_skb(newskb);
		return -EINVAL;
	}
	
	if (ipaddr_4to6(&(ip4h->saddr), s_port, ADDR_DIR_SRC, &(ip6h->saddr), NULL) != 0) {
		kfree_skb(newskb);
		return -EINVAL;
	}

	*(__u32 *)ip6h = __constant_htonl(0x60000000);
	
	if (transport == MAP_E) {
		// Encapsulation
		ip6h->payload_len = ip4h->tot_len;
		plen = ntohs(ip4h->tot_len);
		ip6h->nexthdr = IPPROTO_IPIP;
		ip6h->hop_limit = 64 + 1; // we have to put translated IPv6 packet into the protocol stack again
		payload = (__u8 *)skb_put(newskb, plen);
		skb_copy_bits(skb, 0, payload, plen);
	} 
	
	else {
		// Translation
		ip6h->hop_limit = ip4h->ttl;
		ip6h->payload_len = htons(plen);
		ip6h->nexthdr = ip4h->protocol;  /* Need to be xlated for ICMP protocol */

		payload = (__u8 *)skb_put(newskb, plen);
		switch (ip6h->nexthdr) {
			case IPPROTO_TCP:
				skb_copy_bits(skb, ip4h->ihl * 4, payload, plen);
				tcph = (struct tcphdr *)payload;
				tcph->check = 0;
				tcph->check = csum_ipv6_magic(&(ip6h->saddr), &(ip6h->daddr), plen, IPPROTO_TCP, \
				                                csum_partial(payload, plen, 0));
				break;

			case IPPROTO_UDP:
				skb_copy_bits(skb, ip4h->ihl * 4, payload, plen);
				udph = (struct udphdr *)payload;
				udph->check = 0;
				udph->check = csum_ipv6_magic(&(ip6h->saddr), &(ip6h->daddr), plen, IPPROTO_UDP, \
				                                csum_partial(payload, plen, 0));
				break;

			case IPPROTO_ICMP: 
				ip6h->nexthdr = IPPROTO_ICMPV6;
				skb_copy_bits(skb, ip4h->ihl * 4, payload, 4); // ICMPv6 header length
				icmp6h = (struct icmp6hdr *)payload;
				
				if (icmp6h->icmp6_type == ICMP_ECHO || icmp6h->icmp6_type == ICMP_ECHOREPLY) {
					skb_copy_bits(skb, ip4h->ihl * 4 + 4, payload + 4, plen - 4);
					if (icmp6h->icmp6_type == ICMP_ECHO)
						icmp6h->icmp6_type = ICMPV6_ECHO_REQUEST;
					else
						icmp6h->icmp6_type = ICMPV6_ECHO_REPLY;
					
					icmp6h->icmp6_cksum = 0;
					icmp6h->icmp6_cksum = csum_ipv6_magic(&(ip6h->saddr), &(ip6h->daddr), plen, \
					                            IPPROTO_ICMPV6, csum_partial(payload, plen, 0));
					
				} else {
					//printk(KERN_ERR "ivi_v4v6_xmit: unsupported ICMP type in xlate. Drop packet.\n");
					kfree_skb(newskb);
					return 0;
				}
				break;

			default:
				kfree_skb(newskb);
				return 0;
		}
	}

	// Prepare to re-enter the protocol stack
	newskb->protocol = eth_type_trans(newskb, skb->dev);
	newskb->ip_summed = CHECKSUM_NONE;

	netif_rx(newskb);
	return 0;
}


static inline bool port_in_range(u16 _port, u16 _ratio, u16 _adjacent, u16 _offset)
{
	if (_ratio == 1)
		return true;
	else {
		// (_port / _adjacent) % _ratio
		u16 temp;
		_ratio = fls(_ratio) - 1;
		_adjacent = fls(_adjacent) - 1;
		temp = (_port >> _adjacent);
		return (temp - ((temp >> _ratio) << _ratio) == _offset);
	}
}

int ivi_v6v4_xmit(struct sk_buff *skb) {
	struct sk_buff *newskb;
	struct ethhdr *eth6, *eth4;
	struct iphdr *ip4h, *icmp_ip4h;
	struct ipv6hdr *ip6h, *icmp_ip6h;
	struct tcphdr *tcph, *icmp_tcph;
	struct udphdr *udph, *icmp_udph;
	struct icmphdr *icmph, *icmp_icmp4h;
	struct frag_hdr *fragh;
	__u8 *payload;
	int hlen, plen;
	__u8 poffset;
	__u8 flag4;
	__u16 off4;
	__be32 oldaddr;
	__be16 oldp;
	u16 s_ratio, s_adj, s_offset, d_ratio, d_adj, d_offset;
	u8 next_hdr, *ext_hdr;
	u32 tempaddr;
	
	fragh = NULL;
		
	eth6 = eth_hdr(skb);
	ip6h = ipv6_hdr(skb);
	hlen = sizeof(struct iphdr);
	plen = ntohs(ip6h->payload_len);
	poffset = sizeof(struct ipv6hdr);  // Payload Offset
	
	// This should not happen since we are hooked on PF_INET6.
	if (unlikely(eth6->h_proto != __constant_ntohs(ETH_P_IPV6))) {
		return -EINVAL;  // Just accept.
	}
	
	// By pass ipv6 multicast packet (for ND)
	if (mc_v6_addr(&(ip6h->daddr))) {
		return -EINVAL;
	}

	// leave the generation of ICMP packet to the protocol stack
	if (ip6h->hop_limit <= 1) {
		return -EINVAL;
	}
	
	// process extension headers
	next_hdr = ip6h->nexthdr;
	ext_hdr = (__u8 *)ip6h + sizeof(struct ipv6hdr);
	while (next_hdr != IPPROTO_IPIP &&
	       next_hdr != IPPROTO_TCP && 
	       next_hdr != IPPROTO_UDP && 
	       next_hdr != IPPROTO_ICMPV6) {
	       
		if (next_hdr == IPPROTO_FRAGMENT) {
			printk(KERN_INFO "FRAGMENT header: frag_off is %x, identification is %x\n", \
			                  ntohs(*((u16 *)ext_hdr + 1)), ntohl(*((u32 *)ext_hdr + 1)));
			fragh = (struct frag_hdr *)ext_hdr;
			plen -= sizeof(struct frag_hdr);
			poffset += sizeof(struct frag_hdr);
			next_hdr = fragh->nexthdr;
			ext_hdr += sizeof(struct frag_hdr);
		}
		else if (next_hdr == IPPROTO_AH) {
			printk(KERN_INFO "AH header, length is %d\n", *(ext_hdr + 1));
			plen -= (*(ext_hdr + 1) << 2) + 8;
			poffset += (*(ext_hdr + 1) << 2) + 8;
			next_hdr = *ext_hdr;
			ext_hdr += (*(ext_hdr + 1) << 2) + 8;
		}
		else {
			printk(KERN_INFO "other header type is %d, length is %d\n", next_hdr, *(ext_hdr + 1));
			plen -= (*(ext_hdr + 1) << 3) + 8;
			poffset += (*(ext_hdr + 1) << 3) + 8;
			next_hdr = *ext_hdr;
			ext_hdr += (*(ext_hdr + 1) << 3) + 8;
		}
	}
	
	if (!(newskb = dev_alloc_skb(2 + ETH_HLEN + max(hlen + plen, 184) + 20))) {
		printk(KERN_ERR "ivi_v6v4_xmit: failed to allocate new socket buffer.\n");
		return 0;  // Drop packet on low memory
	}
	skb_reserve(newskb, 2);  // Align IP header on 16 byte boundary (ETH_LEN + 2)

	eth4 = (struct ethhdr *)skb_put(newskb, ETH_HLEN);
	memcpy(eth4, eth6, 12); // Keep mac unchanged
	eth4->h_proto  = __constant_ntohs(ETH_P_IP);
	
	if (next_hdr == IPPROTO_IPIP) { // Decapsulation
		payload = (__u8 *)skb_put(newskb, plen);
		skb_copy_bits(skb, poffset, payload, plen);
		ip4h = (struct iphdr *)payload;
		payload += ip4h->ihl << 2;
		
		switch (ip4h->protocol) {
			case IPPROTO_TCP:
				tcph = (struct tcphdr *)payload;

				if (!port_in_range(ntohs(tcph->dest), hgw_ratio, hgw_adjacent, hgw_offset)) {
					//printk(KERN_INFO "ivi_v6v4_xmit: TCP dest port %d is not in range (r=%d, m=%d, o=%d)."
					//                 "Drop packet.\n", ntohs(tcph->dest), hgw_ratio, hgw_adjacent, hgw_offset);
					kfree_skb(newskb);
					return 0;
				}
				
				if (ivi_mode == IVI_MODE_HGW && ntohs(tcph->dest) < 1024) {
					oldaddr = ntohl(ip4h->daddr);
					oldp = ntohs(tcph->dest);
				}
				
				else if (get_inflow_tcp_map_port(ntohs(tcph->dest), ntohl(ip4h->saddr), ntohs(tcph->source), \
				                            tcph, plen, &oldaddr, &oldp) == -1) {
					//printk(KERN_ERR "ivi_v6v4_xmit: fail to perform nat44 mapping for %d (TCP).\n",
					//	               ntohs(tcph->dest));
					kfree_skb(newskb);
					return 0;
				}
						
				csum_replace4(&tcph->check, ip4h->daddr, htonl(oldaddr));
				csum_replace4(&ip4h->check, ip4h->daddr, htonl(oldaddr));
				ip4h->daddr = htonl(oldaddr);
						
				csum_replace2(&tcph->check, tcph->dest, htons(oldp));
				tcph->dest = htons(oldp);

				if (tcph->syn && (tcph->doff > 5)) {
					__u16 *option = (__u16*)tcph;
					if (option[10] == htons(0x0204)) {
						if (ntohs(option[11]) > mss_limit) {
							csum_replace2(&tcph->check, option[11], htons(mss_limit));
							option[11] = htons(mss_limit);
						}
					}
				}

				break;

			case IPPROTO_UDP:
				udph = (struct udphdr *)payload;

				if (!port_in_range(ntohs(udph->dest), hgw_ratio, hgw_adjacent, hgw_offset)) {
					//printk(KERN_INFO "ivi_v6v4_xmit: UDP dest port %d is not in range (r=%d, m=%d, o=%d)."
					//                 "Drop packet.\n", ntohs(udph->dest), hgw_ratio, hgw_adjacent, hgw_offset);
					kfree_skb(newskb);
					return 0;
				}
				
				if (ivi_mode == IVI_MODE_HGW && ntohs(udph->dest) < 1024) {
					oldaddr = ntohl(ip4h->daddr);
					oldp = ntohs(udph->dest);
				}
				
				else if (get_inflow_map_port(&udp_list,  ntohs(udph->dest), ntohl(ip4h->saddr), \
				                        &oldaddr, &oldp) == -1) {
					//printk(KERN_ERR "ivi_v6v4_xmit: fail to perform nat44 mapping for %d (UDP).\n",
					//                 ntohs(udph->dest));	
					kfree_skb(newskb);
					return 0;
				}
				
				// If checksum of UDP inside IPv4 packet is 0, we MUST NOT update the checksum value.
				if (udph->check != 0) {
					csum_replace4(&udph->check, ip4h->daddr, htonl(oldaddr));
					csum_replace2(&udph->check, udph->dest, htons(oldp));
				}						
				
				csum_replace4(&ip4h->check, ip4h->daddr, htonl(oldaddr));
				ip4h->daddr = htonl(oldaddr);
				udph->dest = htons(oldp);

				break;

			case IPPROTO_ICMP:
				icmph = (struct icmphdr *)payload;
				if (icmph->type == ICMP_ECHOREPLY) {
					if (get_inflow_map_port(&icmp_list, ntohs(icmph->un.echo.id), ntohl(ip4h->saddr), \
					                        &oldaddr, &oldp) == -1) {
					    tempaddr = ntohl(ip4h->saddr);
						printk(KERN_ERR "ivi_v6v4_xmit: fail to perform nat44 mapping for ( " NIP4_FMT \
						                ", %d) (ICMP).\n", NIP4(tempaddr), ntohs(icmph->un.echo.id));
						kfree_skb(newskb);
						return 0;
					} else {
						csum_replace4(&ip4h->check, ip4h->daddr, htonl(oldaddr));
						ip4h->daddr = htonl(oldaddr);
							
						csum_replace2(&icmph->checksum, icmph->un.echo.id, htons(oldp));
						icmph->un.echo.id = htons(oldp);
					}
				}
				else if (icmph->type == ICMP_ECHO) {
					if (ivi_mode == IVI_MODE_HGW_NAT44) { 
#ifdef IVI_DEBUG
						printk(KERN_INFO "ivi_v6v4_xmit: you can't ping private address when CPE is working in NAT44 mode\n");
#endif
						return 0; // silently drop
					}
				} 
				else if (icmph->type == ICMP_TIME_EXCEEDED) {
					icmp_ip4h = (struct iphdr *)((__u8 *)icmph + 8);
					if (icmp_ip4h->protocol == IPPROTO_ICMP) {
						icmp_icmp4h = (struct icmphdr *)((__u8 *)icmp_ip4h + (icmp_ip4h->ihl << 2));
						if (icmp_icmp4h->type == ICMP_ECHO) {
							if (get_inflow_map_port(&icmp_list, ntohs(icmp_icmp4h->un.echo.id), \
							                   ntohl(icmp_ip4h->daddr), &oldaddr, &oldp) == -1) {
								printk(KERN_ERR "ivi_v6v4_xmit: fail to perform nat44 mapping for %d (ICMP) "\
								                "in IP packet.\n", ntohs(icmph->un.echo.id));
								kfree_skb(newskb);
								return 0;
							} else {
								csum_replace4(&icmp_ip4h->check, icmp_ip4h->saddr, htonl(oldaddr));
								icmp_ip4h->saddr = htonl(oldaddr);					
								csum_replace2(&icmp_icmp4h->checksum, icmp_icmp4h->un.echo.id, htons(oldp));
								icmp_icmp4h->un.echo.id = htons(oldp);
							}
						}
					}
				}
				break;

			default:
				kfree_skb(newskb);
				return 0;
		}
	} 
	
	else { // Translation
		ip4h = (struct iphdr *)skb_put(newskb, hlen);
		if (ipaddr_6to4(&(ip6h->saddr), ADDR_DIR_SRC, &(ip4h->saddr), &s_ratio, &s_adj, &s_offset) < 0) {
			kfree_skb(newskb);
			return -EINVAL;  // Just accept.
		}
		if (ipaddr_6to4(&(ip6h->daddr), ADDR_DIR_DST, &(ip4h->daddr), &d_ratio, &d_adj, &d_offset) < 0) {
			kfree_skb(newskb);
			return -EINVAL;  // Just accept.
		}

		*(__u16 *)ip4h = __constant_htons(0x4500);
		ip4h->tot_len = htons(hlen + plen);
		
		if (fragh) { // Containing Fragment Header
			ip4h->id = htons(ntohl(fragh->identification) & 0xffff);
			flag4 = ntohs(fragh->frag_off) & 0x0001; // DF=0, MF is copied without change
			off4 = (ntohs(fragh->frag_off)>>3) & 0x1fff;
			ip4h->frag_off = htons((flag4 << 13) + off4);
			ip4h->ttl = ip6h->hop_limit;
			ip4h->protocol = next_hdr; // ICMPv6 is translated below
		} 
		else {
			ip4h->id = 0;
			ip4h->frag_off = htons(0x4000); // DF=1
			ip4h->ttl = ip6h->hop_limit;
			ip4h->protocol = next_hdr; // ICMPv6 is translated below
		}
		
		payload = (__u8 *)skb_put(newskb, plen);
		switch (next_hdr) {
			case IPPROTO_TCP:
				skb_copy_bits(skb, poffset, payload, plen);
				tcph = (struct tcphdr *)payload;

				if (!port_in_range(ntohs(tcph->dest), hgw_ratio, hgw_adjacent, hgw_offset)) {
					//printk(KERN_INFO "ivi_v6v4_xmit: TCP dest port %d is not in range (r=%d, m=%d, o=%d). "
					//                 "Drop packet.\n", ntohs(tcph->dest), hgw_ratio, hgw_adjacent, hgw_offset);
					kfree_skb(newskb);
					return 0;
				}
				
				if (ivi_mode == IVI_MODE_HGW && ntohs(tcph->dest) < 1024) {
					oldaddr = ntohl(ip4h->daddr);
					oldp = ntohs(tcph->dest);
				}
				
				else if (get_inflow_tcp_map_port(ntohs(tcph->dest), ntohl(ip4h->saddr), ntohs(tcph->source), \
				                            tcph, plen, &oldaddr, &oldp) == -1) {
					//printk(KERN_ERR "ivi_v6v4_xmit: fail to perform nat44 mapping for %d (TCP).\n", 
					//                 ntohs(tcph->dest));                 
					kfree_skb(newskb);
					return 0;
				} 
					
				ip4h->daddr = htonl(oldaddr);
				tcph->dest = htons(oldp);

				if (tcph->syn && (tcph->doff > 5)) {
					__u16 *option = (__u16*)tcph;
					if (option[10] == htons(0x0204)) {
						if (ntohs(option[11]) > mss_limit) {
							option[11] = htons(mss_limit);
						}
					}
				}

				tcph->check = 0;
				tcph->check = csum_tcpudp_magic(ip4h->saddr, ip4h->daddr, plen, IPPROTO_TCP, \
				                                csum_partial(payload, plen, 0));
				break;

			case IPPROTO_UDP:
				skb_copy_bits(skb, poffset, payload, plen);
				udph = (struct udphdr *)payload;

				if (!port_in_range(ntohs(udph->dest), hgw_ratio, hgw_adjacent, hgw_offset)) {
					//printk(KERN_INFO "ivi_v6v4_xmit: UDP dest port %d is not in range (r=%d, m=%d, o=%d)." 
					//	                " Drop packet.\n", ntohs(udph->dest), hgw_ratio, hgw_adjacent, hgw_offset);
					kfree_skb(newskb);
					return 0;
				}
					
				if (ivi_mode == IVI_MODE_HGW && ntohs(udph->dest) < 1024) {
					oldaddr = ntohl(ip4h->daddr);
					oldp = ntohs(udph->dest);
				}
					
				else if (get_inflow_map_port(&udp_list, ntohs(udph->dest), ntohl(ip4h->saddr), \
				                        &oldaddr, &oldp) == -1) {
					//printk(KERN_ERR "ivi_v6v4_xmit: fail to perform nat44 mapping for %d (UDP).\n", ntohs(udph->dest));
					kfree_skb(newskb);
					return 0;
				}

				ip4h->daddr = htonl(oldaddr);
				udph->dest = htons(oldp);

				udph->check = 0;
				udph->check = csum_tcpudp_magic(ip4h->saddr, ip4h->daddr, plen, IPPROTO_UDP, \
				                                csum_partial(payload, plen, 0));
				break;

			case IPPROTO_ICMPV6:  // indicating ICMPv4 packet			
				ip4h->protocol = IPPROTO_ICMP;
				skb_copy_bits(skb, poffset, payload, 8);
				icmph = (struct icmphdr *)payload;
				
				if (icmph->type == ICMPV6_ECHO_REQUEST || icmph->type == ICMPV6_ECHO_REPLY) {
					skb_copy_bits(skb, poffset + 8, payload + 8, plen - 8);
					icmph->type = (icmph->type == ICMPV6_ECHO_REQUEST) ? ICMP_ECHO : ICMP_ECHOREPLY;

					if (icmph->type == ICMP_ECHOREPLY) {
						if (get_inflow_map_port(&icmp_list, ntohs(icmph->un.echo.id), ntohl(ip4h->saddr),\
						                        &oldaddr, &oldp) == -1) {
							//printk(KERN_INFO "ivi_v6v4_xmit: fail to perform nat44 mapping for %d (ICMP).\n", 
							//                ntohs(icmph->un.echo.id));
						} else {
							ip4h->daddr = htonl(oldaddr);
							icmph->un.echo.id = htons(oldp);
						}
					}

					icmph->checksum = 0;
					icmph->checksum = ip_compute_csum(icmph, plen);
					
				} else {
					if (icmph->type == ICMPV6_TIME_EXCEED) {
						icmph->type = ICMP_TIME_EXCEEDED;
					}
					else if (icmph->type == ICMPV6_DEST_UNREACH) {
						icmph->type = ICMP_DEST_UNREACH;
						if (icmph->code == ICMPV6_NOROUTE)
							icmph->code = ICMP_HOST_UNREACH;
						else if (icmph->code == ICMPV6_PORT_UNREACH)
							icmph->code = ICMP_PORT_UNREACH;
					}
					else {
						//printk(KERN_ERR "ivi_v6v4_xmit: unsupported ICMP type. Drop Packet now.\n");
						return 0;
					}
					
					icmph->checksum = 0;
					memset((__u8 *)icmph + 4, 0, 4);

					// translation of ipv6 header embeded in icmpv6
					icmp_ip4h = (struct iphdr *)((__u8 *)icmph + 8); 
					icmp_ip6h = (struct ipv6hdr *)((__u8 *)ip6h + poffset + sizeof(struct icmp6hdr));
					*(__u16 *)icmp_ip4h = __constant_htons(0x4500);
					
					icmp_ip4h->id = 0;
					icmp_ip4h->frag_off = htons(0x4000);
					icmp_ip4h->ttl = icmp_ip6h->hop_limit;		
					icmp_ip4h->protocol = icmp_ip6h->nexthdr;				
					icmp_ip4h->check = 0;
					ipaddr_6to4(&(icmp_ip6h->saddr), ADDR_DIR_SRC, &(icmp_ip4h->saddr), &s_ratio, &s_adj, &s_offset);
					ipaddr_6to4(&(icmp_ip6h->daddr), ADDR_DIR_DST, &(icmp_ip4h->daddr), &d_ratio, &d_adj, &d_offset);
					payload = (__u8 *)icmp_ip4h + sizeof(struct iphdr);
					
					ip4h->tot_len = htons(ntohs(ip4h->tot_len)-20);
					icmp_ip4h->tot_len = htons(sizeof(struct iphdr) + ntohs(icmp_ip6h->payload_len));
					skb_copy_bits(skb, poffset + sizeof(struct icmp6hdr) + sizeof(struct ipv6hdr), payload,\
						              ntohs(icmp_ip6h->payload_len));

					switch (icmp_ip4h->protocol) {
						case IPPROTO_TCP:
							icmp_tcph = (struct tcphdr *)((__u8 *)icmp_ip4h + 20);
							oldaddr = oldp = 0;
							get_inflow_tcp_map_port(ntohs(icmp_tcph->source), ntohl(icmp_ip4h->daddr), 
							    ntohs(icmp_tcph->dest), icmp_tcph, ntohs(icmp_ip4h->tot_len) - 20,&oldaddr, &oldp);
							    
							if (oldaddr == 0 && oldp == 0) // Many ICMP packets have an uncomplete inside TCP structure:
							                               // return value is -1 alone cannot imply a fail lookup. 
								printk(KERN_ERR "ivi_v6v4_xmit: tcp-in-icmp reverse lookup failure.\n");
								
							else {
								icmp_ip4h->saddr = ip4h->daddr = htonl(oldaddr);
								icmp_tcph->source = htons(oldp);
							}
							icmp_tcph->check = 0;
							icmp_tcph->check = csum_tcpudp_magic(icmp_ip4h->saddr, icmp_ip4h->daddr, \
							                       ntohs(icmp_ip4h->tot_len) - 20, IPPROTO_TCP, \
							                       csum_partial(payload, ntohs(icmp_ip4h->tot_len-20), 0));
							break;
						case IPPROTO_UDP:
							icmp_udph = (struct udphdr *)((__u8 *)icmp_ip4h + 20);
							if (get_inflow_map_port(&udp_list, ntohs(icmp_udph->source), ntohl(icmp_ip4h->daddr), \
							                        &oldaddr, &oldp) == -1) {
								printk(KERN_ERR "ivi_v6v4_xmit: udp-in-icmp reverse lookup failure.\n");
								
							} else {
								icmp_ip4h->saddr = ip4h->daddr = htonl(oldaddr);
								icmp_udph->source = htons(oldp);
							}
							icmp_udph->len = htons(ntohs(icmp_ip4h->tot_len) - 20);
							icmp_udph->check = 0;
							icmp_udph->check = csum_tcpudp_magic(icmp_ip4h->saddr, icmp_ip4h->daddr, \
							                       ntohs(icmp_ip4h->tot_len) - 20, IPPROTO_UDP, \
							                       csum_partial(payload, ntohs(icmp_ip4h->tot_len)-20, 0));
							break;
						case IPPROTO_ICMPV6:
							icmp_ip4h->protocol = IPPROTO_ICMP;
							icmp_icmp4h = (struct icmphdr *)((__u8 *)icmp_ip4h + 20);
							if (icmp_icmp4h->type == ICMPV6_ECHO_REQUEST || icmp_icmp4h->type == ICMPV6_ECHO_REPLY) {
								icmp_icmp4h->type=(icmp_icmp4h->type==ICMPV6_ECHO_REQUEST)?ICMP_ECHO:ICMPV6_ECHO_REPLY;
								if (get_inflow_map_port(&icmp_list, ntohs(icmp_icmp4h->un.echo.id), \
								                        ntohl(icmp_ip4h->daddr), &oldaddr, &oldp) == -1)
									printk(KERN_ERR "ivi_v6v4_xmit: echo-in-icmp reverse lookup failure.\n");
								else {
									icmp_ip4h->saddr = ip4h->daddr = htonl(oldaddr);
									icmp_icmp4h->un.echo.id = htons(oldp);
								}
								icmp_icmp4h->checksum = 0;
								icmp_icmp4h->checksum = ip_compute_csum(icmp_icmp4h, ntohs(icmp_ip4h->tot_len)-20);
							}
							break;
						default:
							break;
					}
					
					icmp_ip4h->check = ip_fast_csum((__u8 *)icmp_ip4h, icmp_ip4h->ihl);
					icmph->checksum = ip_compute_csum(icmph, ntohs(icmp_ip4h->tot_len) + 8);		
				}
				break;

			default:
				kfree_skb(newskb);
				return 0;
		}
		ip4h->check = 0;
		ip4h->check = ip_fast_csum((__u8 *)ip4h, ip4h->ihl);
	}

	// Prepare to re-enter the protocol stack
	newskb->protocol = eth_type_trans(newskb, skb->dev);
	newskb->ip_summed = CHECKSUM_NONE;
 
	netif_rx(newskb);
	return 0;
}
