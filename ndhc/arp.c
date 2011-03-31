/* arp.c - arp ping checking
 * Time-stamp: <2011-03-30 23:34:21 nk>
 *
 * Copyright 2010-2011 Nicholas J. Kain <njkain@gmail.com>
 *
 * Originally derived from busybox's udhcpc variant, which in turn was...
 * Mostly stolen from: dhcpcd - DHCP client daemon
 * by Yoichi Hariguchi <yoichi@fore.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/if_ether.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <errno.h>
#include "arp.h"
#include "dhcpmsg.h"
#include "packet.h"
#include "socket.h"
#include "sys.h"
#include "ifchange.h"
#include "log.h"
#include "strl.h"
#include "io.h"

static struct arpMsg arpreply;
static int arpreply_offset;
static struct dhcpMessage arp_dhcp_packet;

/* Returns fd of the arp socket, or -1 on failure. */
static int arpping(struct client_state_t *cs, uint32_t test_ip,
                   uint32_t from_ip, uint8_t *from_mac, const char *interface)
{
    int arpfd;
    int opt = 1;
    struct sockaddr addr;   /* for interface name */
    struct arpMsg arp;

    if (cs->arpFd != -1) {
        epoll_del(cs, cs->arpFd);
        close(cs->arpFd);
    }
    arpfd = socket(PF_PACKET, SOCK_PACKET, htons(ETH_P_ARP));
    if (arpfd == -1) {
        log_warning("arpping: failed to create socket: %s", strerror(errno));
        return -1;
    }

    if (setsockopt(arpfd, SOL_SOCKET, SO_BROADCAST,
                   &opt, sizeof opt) == -1) {
        log_warning("arpping: failed to set broadcast: %s", strerror(errno));
        close(arpfd);
        return -1;
    }

    set_sock_nonblock(arpfd);

    /* send arp request */
    memset(&arp, 0, sizeof arp);
    memset(arp.h_dest, 0xff, 6);                    /* MAC DA */
    memcpy(arp.h_source, from_mac, 6);              /* MAC SA */
    arp.h_proto = htons(ETH_P_ARP);                 /* protocol type (Ethernet) */
    arp.htype = htons(ARPHRD_ETHER);                /* hardware type */
    arp.ptype = htons(ETH_P_IP);                    /* protocol type (ARP message) */
    arp.hlen = 6;                                   /* hardware address length */
    arp.plen = 4;                                   /* protocol address length */
    arp.operation = htons(ARPOP_REQUEST);           /* ARP op code */
    memcpy(arp.sHaddr, from_mac, 6);                /* source hardware address */
    memcpy(arp.sInaddr, &from_ip, sizeof from_ip);  /* source IP address */
    /* tHaddr is zero-filled */                     /* target hardware address */
    memcpy(arp.tInaddr, &test_ip, sizeof test_ip);  /* target IP address */

    memset(&addr, 0, sizeof addr);
    strlcpy(addr.sa_data, interface, sizeof addr.sa_data);
    if (safe_sendto(arpfd, (const char *)&arp, sizeof arp,
                    0, &addr, sizeof addr) < 0) {
        log_error("arpping: sendto failed: %s", strerror(errno));
        close(arpfd);
        return -1;
    }
    return arpfd;
}

void arp_check(struct client_state_t *cs, struct dhcpMessage *packet)
{
    cs->arpPrevState = cs->dhcpState;
    cs->dhcpState = DS_ARP_CHECK;
    memcpy(&arp_dhcp_packet, packet, sizeof (struct dhcpMessage));
    cs->arpFd = arpping(cs, arp_dhcp_packet.yiaddr, 0, client_config.arp,
                        client_config.interface);
    epoll_add(cs, cs->arpFd);
    cs->timeout = 2000;
    memset(&arpreply, 0, sizeof arpreply);
    arpreply_offset = 0;
}

void arp_gw_check(struct client_state_t *cs)
{
    cs->arpPrevState = cs->dhcpState;
    cs->dhcpState = DS_ARP_GW_CHECK;
    memset(&arp_dhcp_packet, 0, sizeof (struct dhcpMessage));
    cs->arpFd = arpping(cs, cs->routerAddr, 0, client_config.arp,
                        client_config.interface);
    epoll_add(cs, cs->arpFd);
    cs->oldTimeout = cs->timeout;
    cs->timeout = 2000;
    memset(&arpreply, 0, sizeof arpreply);
    arpreply_offset = 0;
}

void arp_get_gw_hwaddr(struct client_state_t *cs)
{
    if (cs->dhcpState != DS_BOUND)
        log_warning("arp_get_gw_hwaddr: called when state != DS_BOUND");
    memset(&arp_dhcp_packet, 0, sizeof (struct dhcpMessage));
    cs->arpFd = arpping(cs, cs->routerAddr, 0, client_config.arp,
                        client_config.interface);
    epoll_add(cs, cs->arpFd);
    memset(&arpreply, 0, sizeof arpreply);
    arpreply_offset = 0;
}

static void arp_failed(struct client_state_t *cs)
{
    log_line("Offered address is in use: declining.");
    epoll_del(cs, cs->arpFd);
    close(cs->arpFd);
    cs->arpFd = -1;
    send_decline(cs->xid, cs->serverAddr, arp_dhcp_packet.yiaddr);

    if (cs->arpPrevState != DS_REQUESTING)
        ifchange(NULL, IFCHANGE_DECONFIG);
    cs->dhcpState = DS_INIT_SELECTING;
    cs->requestedIP = 0;
    cs->timeout = 0;
    cs->packetNum = 0;
    change_listen_mode(cs, LM_RAW);
}

void arp_gw_failed(struct client_state_t *cs)
{
    log_line("arp: gateway appears to have changed, getting new lease");
    epoll_del(cs, cs->arpFd);
    close(cs->arpFd);
    cs->arpFd = -1;

    // Same as packet.c: line 258
    ifchange(NULL, IFCHANGE_DECONFIG);
    cs->dhcpState = DS_INIT_SELECTING;
    cs->oldTimeout = 0;
    cs->timeout = 0;
    cs->requestedIP = 0;
    cs->packetNum = 0;
    change_listen_mode(cs, LM_RAW);
}

void arp_success(struct client_state_t *cs)
{
    struct in_addr temp_addr;

    epoll_del(cs, cs->arpFd);
    close(cs->arpFd);
    cs->arpFd = -1;

    /* enter bound state */
    cs->t1 = cs->lease >> 1;
    /* little fixed point for n * .875 */
    cs->t2 = (cs->lease * 0x7) >> 3;
    cs->timeout = cs->t1 * 1000;
    cs->leaseStartTime = curms();

    temp_addr.s_addr = arp_dhcp_packet.yiaddr;
    log_line("Lease of %s obtained, lease time %ld.",
             inet_ntoa(temp_addr), cs->lease);
    cs->requestedIP = arp_dhcp_packet.yiaddr;
    cs->dhcpState = DS_BOUND;
    ifchange(&arp_dhcp_packet,
             ((cs->arpPrevState == DS_RENEWING ||
               cs->arpPrevState == DS_REBINDING)
              ? IFCHANGE_RENEW : IFCHANGE_BOUND));
    change_listen_mode(cs, LM_NONE);
    if (client_config.quit_after_lease)
        exit(EXIT_SUCCESS);
    if (!client_config.foreground)
        background(cs);
}

void arp_gw_success(struct client_state_t *cs)
{
    log_line("arp: gateway seems unchanged");
    epoll_del(cs, cs->arpFd);
    close(cs->arpFd);
    cs->arpFd = -1;
    cs->timeout = cs->oldTimeout;
    cs->dhcpState = cs->arpPrevState;
}

typedef uint32_t aliased_uint32_t __attribute__((__may_alias__));
void handle_arp_response(struct client_state_t *cs)
{
    if (arpreply_offset < sizeof arpreply) {
        int r = safe_read(cs->arpFd, (char *)&arpreply + arpreply_offset,
                          sizeof arpreply - arpreply_offset);
        if (r < 0) {
            // Conservative responses: assume failure.
            if (cs->dhcpState == DS_ARP_CHECK)
                arp_failed(cs);
            else
                arp_gw_failed(cs);
            return;
        } else
            arpreply_offset += r;
    }

    if (arpreply_offset < ARP_MSG_SIZE) {
        log_warning("handle_arp_response: Received short ARP message.");
        return;
    }
    switch (cs->dhcpState) {
        case DS_ARP_CHECK:
            if (arpreply.operation == htons(ARPOP_REPLY)
                && !memcmp(arpreply.tHaddr, client_config.arp, 6)
                && *(aliased_uint32_t*)arpreply.sInaddr
                == arp_dhcp_packet.yiaddr)
            {
                // Check to see if we replied to our own ARP query.
                if (!memcmp(client_config.arp, arpreply.sHaddr, 6))
                    arp_success(cs);
                else
                    arp_failed(cs);
            } else {
                memset(&arpreply, 0, sizeof arpreply);
                arpreply_offset = 0;
            }
            break;
        case DS_ARP_GW_CHECK:
            if (arpreply.operation == htons(ARPOP_REPLY)
                && !memcmp(arpreply.tHaddr, client_config.arp, 6)
                && *(aliased_uint32_t*)arpreply.sInaddr == cs->routerAddr)
            {
                // Success only if the router/gw MAC matches stored value
                if (!memcmp(cs->routerArp, arpreply.sHaddr, 6))
                    arp_gw_success(cs);
                else
                    arp_gw_failed(cs);
            } else {
                memset(&arpreply, 0, sizeof arpreply);
                arpreply_offset = 0;
            }
            break;
        case DS_BOUND:
            if (arpreply.operation == htons(ARPOP_REPLY)
                && !memcmp(arpreply.tHaddr, client_config.arp, 6)
                && *(aliased_uint32_t*)arpreply.sInaddr == cs->routerAddr)
            {
                memcpy(cs->routerArp, arpreply.sHaddr, 6);
                epoll_del(cs, cs->arpFd);
                close(cs->arpFd);
                cs->arpFd = -1;
                log_line("gateway hardware address %02x:%02x:%02x:%02x:%02x:%02x",
                         cs->routerArp[0], cs->routerArp[1],
                         cs->routerArp[2], cs->routerArp[3],
                         cs->routerArp[4], cs->routerArp[5]);
            } else {
                log_line("still looking for gateway hardware address");
                memset(&arpreply, 0, sizeof arpreply);
                arpreply_offset = 0;
            }
            break;
        default:
            epoll_del(cs, cs->arpFd);
            close(cs->arpFd);
            cs->arpFd = -1;
            log_warning("handle_arp_response: called in invalid state 0x%02x",
                        cs->dhcpState);
            break;
    }
}
