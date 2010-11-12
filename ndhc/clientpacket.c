/* clientpacket.c
 *
 * Packet generation and dispatching functions for the DHCP client.
 *
 * Russ Dill <Russ.Dill@asu.edu> July 2001
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
 
#include <string.h>
#include <sys/socket.h>
#include <features.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "dhcpd.h"
#include "packet.h"
#include "options.h"
#include "dhcpc.h"
#include "log.h"

/* Create a random xid */
unsigned long random_xid(void)
{
	static int initialized;
	if (!initialized) {
		int fd;
		unsigned long seed;

		fd = open("/dev/urandom", O_RDONLY);
		if (fd == -1 || read(fd, &seed, sizeof(seed)) < 0) {
			log_warning("Could not load seed from /dev/urandom: %s",
				    strerror(errno));
			seed = time(0);
		}
		if (fd != -1)
			close(fd);
		srand(seed);
		initialized++;
	}
	return rand();
}


/* initialize a packet with the proper defaults */
static void init_packet(struct dhcpMessage *packet, char type)
{
	struct vendor  {
		char vendor;
		char length;
		char str[sizeof("ndhc")];
	} vendor_id = { DHCP_VENDOR,  sizeof("ndhc") - 1, "ndhc"};
	
	init_header(packet, type);
	memcpy(packet->chaddr, client_config.arp, 6);
	add_option_string(packet->options, client_config.clientid);
	if (client_config.hostname)
		add_option_string(packet->options, client_config.hostname);
	add_option_string(packet->options, (unsigned char *)&vendor_id);
}


/* Add a paramater request list for stubborn DHCP servers. Pull the data
 * from the struct in options.c. Don't do bounds checking here because it
 * goes towards the head of the packet. */
static void add_requests(struct dhcpMessage *packet)
{
	int end = end_option(packet->options);
	int i, len = 0;

	packet->options[end + OPT_CODE] = DHCP_PARAM_REQ;
	for (i = 0; options[i].code; i++)
		if (options[i].flags & OPTION_REQ)
			packet->options[end + OPT_DATA + len++] = options[i].code;
	packet->options[end + OPT_LEN] = len;
	packet->options[end + OPT_DATA + len] = DHCP_END;

}


/* Broadcast a DHCP discover packet to the network, with an optionally
 * requested IP */
int send_discover(unsigned long xid, unsigned long requested)
{
	struct dhcpMessage packet;

	init_packet(&packet, DHCPDISCOVER);
	packet.xid = xid;
	if (requested)
		add_simple_option(packet.options, DHCP_REQUESTED_IP, requested);

	add_requests(&packet);
	log_line("Sending discover...");
	return raw_packet(&packet, INADDR_ANY, CLIENT_PORT, INADDR_BROADCAST, 
				SERVER_PORT, MAC_BCAST_ADDR, client_config.ifindex);
}


/* Broadcasts a DHCP request message */
int send_selecting(unsigned long xid, unsigned long server,
		unsigned long requested)
{
	struct dhcpMessage packet;
	struct in_addr addr;

	init_packet(&packet, DHCPREQUEST);
	packet.xid = xid;

	add_simple_option(packet.options, DHCP_REQUESTED_IP, requested);
	add_simple_option(packet.options, DHCP_SERVER_ID, server);
	
	add_requests(&packet);
	addr.s_addr = requested;
	log_line("Sending select for %s...", inet_ntoa(addr));
	return raw_packet(&packet, INADDR_ANY, CLIENT_PORT, INADDR_BROADCAST, 
				SERVER_PORT, MAC_BCAST_ADDR, client_config.ifindex);
}


/* Unicasts or broadcasts a DHCP renew message */
int send_renew(unsigned long xid, unsigned long server, unsigned long ciaddr)
{
	struct dhcpMessage packet;
	int ret = 0;

	init_packet(&packet, DHCPREQUEST);
	packet.xid = xid;
	packet.ciaddr = ciaddr;

	add_requests(&packet);
	log_line("Sending renew...");
	if (server) 
		ret = kernel_packet(&packet, ciaddr, CLIENT_PORT, server, SERVER_PORT);
	else 
		ret = raw_packet(&packet, INADDR_ANY, CLIENT_PORT, INADDR_BROADCAST,
				SERVER_PORT, MAC_BCAST_ADDR, client_config.ifindex);
	return ret;
}	


/* Unicasts a DHCP release message */
int send_release(unsigned long server, unsigned long ciaddr)
{
	struct dhcpMessage packet;

	init_packet(&packet, DHCPRELEASE);
	packet.xid = random_xid();
	packet.ciaddr = ciaddr;
	
	add_simple_option(packet.options, DHCP_REQUESTED_IP, ciaddr);
	add_simple_option(packet.options, DHCP_SERVER_ID, server);

	log_line("Sending release...");
	return kernel_packet(&packet, ciaddr, CLIENT_PORT, server, SERVER_PORT);
}


/* return -1 on errors that are fatal for the socket,
 * -2 for those that aren't */
int get_raw_packet(struct dhcpMessage *payload, int fd)
{
	struct udp_dhcp_packet packet;
	uint32_t source, dest;
	uint16_t check;

	ssize_t len = 0;
	const ssize_t wanted = sizeof(struct iphdr) + sizeof(struct udphdr);

	memset(&packet, 0, sizeof(struct udp_dhcp_packet));
	while (len < wanted) {
		ssize_t r = read(fd, &packet + len,
				 sizeof(struct udp_dhcp_packet) - len);
		if (r == 0)
			break;
		if (r == -1) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				log_line("EAGAIN or EWOULDBLOCK hit");
				break;
			}
			log_line("couldn't read on raw listening socket -- ignoring");
			usleep(500000); /* possible down interface, looping condition */
			return -1;
		}
		len += r;
	}

	if (len == 0) {
		usleep(50000);
		return -2;
	}

	log_line("len: %d wanted: %d", len, wanted);
	if (len < wanted) {
		log_line("Message too short to contain IP + UDP headers, ignoring");
		sleep(1);
		return -2;
	}
	
	if (len < ntohs(packet.ip.tot_len)) {
		log_line("Truncated packet");
		return -2;
	}
	
	/* ignore any extra garbage bytes */
	len = ntohs(packet.ip.tot_len);
	
	/* Make sure its the right packet for us, and that it passes
	 * sanity checks */
	if (packet.ip.protocol != IPPROTO_UDP) {
	    	log_line("IP header is not UDP");
		sleep(1);
	    	return -2;
	}
	if (packet.ip.version != IPVERSION) {
	    	log_line("IP version is not IPv4");
		sleep(1);
	    	return -2;
	}
	if (packet.ip.ihl != sizeof(packet.ip) >> 2) {
	    	log_line("IP header length incorrect");
		sleep(1);
	    	return -2;
	}
	if (packet.udp.dest != htons(CLIENT_PORT)) {
	    	log_line("UDP destination port incorrect");
		sleep(1);
	    	return -2;
	}
	if (len > (int)sizeof(struct udp_dhcp_packet)) {
	    	log_line("Data longer than that of a IP+UDP+DHCP message");
		sleep(1);
	    	return -2;
	}
	if (ntohs(packet.udp.len) != (short)(len - sizeof(packet.ip))) {
	    	log_line("UDP header length incorrect");
		sleep(1);
	    	return -2;
	}

	/* check IP checksum */
	check = packet.ip.check;
	packet.ip.check = 0;
	if (check != checksum(&(packet.ip), sizeof(packet.ip))) {
		log_line("bad IP header checksum, ignoring");
		return -1;
	}
	
	/* verify the UDP checksum by replacing the header with a psuedo header */
	source = packet.ip.saddr;
	dest = packet.ip.daddr;
	check = packet.udp.check;
	packet.udp.check = 0;
	memset(&packet.ip, 0, sizeof(packet.ip));

	packet.ip.protocol = IPPROTO_UDP;
	packet.ip.saddr = source;
	packet.ip.daddr = dest;
	packet.ip.tot_len = packet.udp.len; /* cheat on the psuedo-header */
	if (check && check != checksum(&packet, len)) {
		log_error("packet with bad UDP checksum received, ignoring");
		return -2;
	}
	
	memcpy(payload, &(packet.data),
			len - (sizeof(packet.ip) + sizeof(packet.udp)));
	
	if (ntohl(payload->cookie) != DHCP_MAGIC) {
		log_error("received bogus message (bad magic) -- ignoring");
		return -2;
	}
	log_line("oooooh!!! got some!");
	return len - (sizeof(packet.ip) + sizeof(packet.udp));
	
}

