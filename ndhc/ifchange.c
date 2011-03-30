/* ifchange.c
 *
 * Functions to call the interface change daemon
 *
 * Russ Dill <Russ.Dill@asu.edu> July 2001
 * Nicholas J. Kain <njkain at gmail dot com> 2004-2010
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
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <errno.h>

#include "options.h"
#include "config.h"
#include "packet.h"
#include "options.h"
#include "log.h"
#include "io.h"
#include "ifchange.h"

static int snprintip(char *dest, size_t size, unsigned char *ip)
{
    if (!dest)
        return -1;
    return snprintf(dest, size, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
}

static int sprintip(char *dest, size_t size, char *pre, unsigned char *ip)
{
    if (!dest)
        return -1;
    return snprintf(dest, size, "%s%d.%d.%d.%d",
					pre, ip[0], ip[1], ip[2], ip[3]);
}

/* Fill buf with the ifchd command text of option 'option'. */
/* Returns 0 if successful, -1 if nothing was filled in. */
static int fill_options(char *buf, unsigned char *option, ssize_t optlen,
                        struct dhcp_option *type_p, unsigned int maxlen)
{
    char *obuf = buf;
    enum option_type type = type_p->type;
    ssize_t typelen = option_length(type);
    ssize_t rem = optlen;
    uint8_t code = type_p->code;

    if (!option)
        return -1;

    if (type == OPTION_STRING) {
        buf += snprintf(buf, maxlen, "%s=", type_p->name);
        if (maxlen < rem + 1)
            return -1;
        memcpy(buf, option, rem);
        buf[rem] = '\0';
        return 0;
    }

    // Length and type checking.
    if (optlen != typelen) {
        if (option_valid_list(code)) {
            if ((optlen % typelen)) {
                log_warning("Bad data received - option list size mismatch: code=0x%02x proplen=0x%02x optlen=0x%02x",
                            code, typelen, optlen);
                return -1;
            }
        } else {
            log_warning("Bad data received - option size mismatch: code=0x%02x proplen=0x%02x optlen=0x%02x",
                        code, typelen, optlen);
            return -1;
        }
    }

    buf += snprintf(buf, maxlen, "%s=", type_p->name);

    for(;;) {
        switch (type) {
            case OPTION_IP: /* Works regardless of host byte order. */
                buf += sprintip(buf, maxlen - (buf - obuf), "", option);
                break;
            case OPTION_U8:
                buf += snprintf(buf, maxlen - (buf - obuf), "%u ", *option);
                break;
            case OPTION_U16: {
                uint16_t val_u16;
                memcpy(&val_u16, option, 2);
                buf += snprintf(buf, maxlen - (buf - obuf), "%u ",
                                ntohs(val_u16));
                break;
            }
            case OPTION_S16: {
                int16_t val_s16;
                memcpy(&val_s16, option, 2);
                buf += snprintf(buf, maxlen - (buf - obuf), "%d ",
                                ntohs(val_s16));
                break;
            }
            case OPTION_U32: {
                uint32_t val_u32;
                memcpy(&val_u32, option, 4);
                buf += snprintf(buf, maxlen - (buf - obuf), "%u ",
                                ntohl(val_u32));
                break;
            }
            case OPTION_S32: {
                int32_t val_s32;
                memcpy(&val_s32, option, 4);
                buf += snprintf(buf, maxlen - (buf - obuf), "%d ",
                                ntohl(val_s32));
                break;
            }
            default:
                return 0;
        }
        option += typelen;
        rem -= typelen;
        if (rem <= 0)
            break;
        *(buf++) = ':';
    }
    return 0;
}

static int open_ifch(void) {
    int sockfd, ret;
    struct sockaddr_un address = {
        .sun_family = AF_UNIX,
        .sun_path = "ifchange"
    };

    sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    ret = connect(sockfd, (struct sockaddr *)&address, sizeof(address));

    if (ret == -1) {
        log_error("unable to connect to ifchd!");
        exit(EXIT_FAILURE);
    }

    return sockfd;
}

static void sockwrite(int fd, const char *buf, size_t count)
{
    if (safe_write(fd, buf, count) == -1)
        log_error("sockwrite: write failed: %s", strerror(errno));
    else
        log_line("sent to ifchd: %s", buf);
}

static void deconfig_if(void)
{
    int sockfd;
    char buf[256];

    sockfd = open_ifch();

    snprintf(buf, sizeof buf, "interface:%s:", client_config.interface);
    sockwrite(sockfd, buf, strlen(buf));

    snprintf(buf, sizeof buf, "ip:0.0.0.0:");
    sockwrite(sockfd, buf, strlen(buf));

    close(sockfd);
}

static void translate_option(int sockfd, struct dhcpMessage *packet,
                             unsigned char code)
{
    char buf[256], buf2[256];
    unsigned char *p;
    int i;
    struct dhcp_option *opt = NULL;
    ssize_t optlen;

    if (!packet)
        return;

    for (i = 0; options[i].code; ++i) {
        if (options[i].code == code) {
            opt = &options[i];
            break;
        }
    }
    if (!opt)
        return;

    memset(buf, '\0', sizeof(buf));
    memset(buf2, '\0', sizeof(buf2));

    p = get_option(packet, code, &optlen);
    if (fill_options(buf2, p, optlen, opt, sizeof buf2 - 1) == -1)
        return;
    snprintf(buf, sizeof buf, "%s:", buf2);
    for (i = 0; i < 256; i++) {
        if (buf[i] == '\0')
            break;
        if (buf[i] == '=') {
            buf[i] = ':';
            break;
        }
    }
    sockwrite(sockfd, buf, strlen(buf));
}

static void bound_if(struct dhcpMessage *packet)
{
    int sockfd;
    char buf[256];
    char ip[32];

    if (!packet)
        return;

    sockfd = open_ifch();

    snprintf(buf, sizeof buf, "interface:%s:", client_config.interface);
    sockwrite(sockfd, buf, strlen(buf));

    snprintip(ip, sizeof ip, (unsigned char *) &packet->yiaddr);
    snprintf(buf, sizeof buf, "ip:%s:", ip);
    sockwrite(sockfd, buf, strlen(buf));

    translate_option(sockfd, packet, DHCP_SUBNET);
    translate_option(sockfd, packet, DHCP_ROUTER);
    translate_option(sockfd, packet, DHCP_DNS_SERVER);
    translate_option(sockfd, packet, DHCP_HOST_NAME);
    translate_option(sockfd, packet, DHCP_DOMAIN_NAME);
    translate_option(sockfd, packet, DHCP_MTU);
    translate_option(sockfd, packet, DHCP_BROADCAST);
    translate_option(sockfd, packet, DHCP_WINS_SERVER);

    close(sockfd);
}

void ifchange(struct dhcpMessage *packet, int mode)
{
    switch (mode) {
        case IFCHANGE_DECONFIG:
            deconfig_if();
            break;
        case IFCHANGE_BOUND:
            bound_if(packet);
            break;
        case IFCHANGE_RENEW:
            bound_if(packet);
            break;
        case IFCHANGE_NAK:
            deconfig_if();
            break;
        default:
            log_error("invalid ifchange mode: %d", mode);
            break;
    }
}

