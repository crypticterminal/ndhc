#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "state.h"
#include "ifchange.h"
#include "arp.h"
#include "options.h"
#include "log.h"
#include "sys.h"
#include "random.h"

static void selecting_packet(struct client_state_t *cs, struct dhcpmsg *packet,
                             uint8_t *message);
static void an_packet(struct client_state_t *cs, struct dhcpmsg *packet,
                      uint8_t *message);
static void selecting_timeout(struct client_state_t *cs);
static void requesting_timeout(struct client_state_t *cs);
static void bound_timeout(struct client_state_t *cs);
static void renewing_timeout(struct client_state_t *cs);
static void rebinding_timeout(struct client_state_t *cs);
static void released_timeout(struct client_state_t *cs);
static void collision_check_timeout(struct client_state_t *cs);
static void bound_gw_check_timeout(struct client_state_t *cs);
static void xmit_release(struct client_state_t *cs);
static void print_release(struct client_state_t *cs);
static void frenew(struct client_state_t *cs);

typedef struct {
    void (*packet_fn)(struct client_state_t *cs, struct dhcpmsg *packet,
                      uint8_t *message);
    void (*timeout_fn)(struct client_state_t *cs);
    void (*force_renew_fn)(struct client_state_t *cs);
    void (*force_release_fn)(struct client_state_t *cs);
} dhcp_state_t;

dhcp_state_t dhcp_states[] = {
    { selecting_packet, selecting_timeout, 0, print_release}, // SELECTING
    { an_packet, requesting_timeout, 0, print_release},       // REQUESTING
    { 0, bound_timeout, frenew, xmit_release},                // BOUND
    { an_packet, renewing_timeout, 0, xmit_release},          // RENEWING
    { an_packet, rebinding_timeout, 0, xmit_release},         // REBINDING
    { 0, bound_gw_check_timeout, 0, xmit_release},            // BOUND_GW_CHECK
    { 0, collision_check_timeout, 0, xmit_release},          // COLLISION_CHECK
    { 0, released_timeout, frenew, 0},                       // RELEASED
    { 0, 0, 0, 0},                                           // NUM_STATES
};

static unsigned int num_dhcp_requests;

static int delay_timeout(int numpackets)
{
    int to = 64;
    char tot[] = { 4, 8, 16, 32, 64 };
    if (numpackets < sizeof tot)
        to = tot[numpackets];
    return to * 1000 + rand() % 1000;
}

void reinit_selecting(struct client_state_t *cs, int timeout)
{
    ifchange_deconfig();
    arp_close_fd(cs);
    cs->dhcpState = DS_SELECTING;
    cs->timeout = timeout;
    cs->clientAddr = 0;
    num_dhcp_requests = 0;
    arp_reset_send_stats();
    set_listen_raw(cs);
}

static void set_released(struct client_state_t *cs)
{
    ifchange_deconfig();
    arp_close_fd(cs);
    cs->dhcpState = DS_RELEASED;
    cs->timeout = -1;
    cs->clientAddr = 0;
    num_dhcp_requests = 0;
    arp_reset_send_stats();
    set_listen_none(cs);
}

// Triggered after a DHCP lease request packet has been sent and no reply has
// been received within the response wait time.  If we've not exceeded the
// maximum number of request retransmits, then send another packet and wait
// again.  Otherwise, return to the DHCP initialization state.
static void requesting_timeout(struct client_state_t *cs)
{
    if (num_dhcp_requests < 5) {
        send_selecting(cs);
        cs->timeout = delay_timeout(num_dhcp_requests);
        num_dhcp_requests++;
    } else
        reinit_selecting(cs, 0);
}

// Triggered when the lease has been held for a significant fraction of its
// total time, and it is time to renew the lease so that it is not lost.
static void bound_timeout(struct client_state_t *cs)
{
    arp_retransmit(cs);

    if (curms() < cs->leaseStartTime + cs->renewTime * 1000)
        return;
    cs->dhcpState = DS_RENEWING;
    set_listen_cooked(cs);
    log_line("Entering renew state.");
    renewing_timeout(cs);
}

static void lease_timedout(struct client_state_t *cs)
{
    log_line("Lease lost, entering init state.");
    reinit_selecting(cs, 0);
}

// Triggered when a DHCP renew request has been sent and no reply has been
// received within the response wait time.  This function is also directly
// called by bound_timeout() when it is time to renew a lease before it
// expires.  Check to see if the lease is still valid, and if it is, send
// a unicast DHCP renew packet.  If it is not, then change to the REBINDING
// state to send broadcast queries.
static void renewing_timeout(struct client_state_t *cs)
{
    long long ct = curms();
    long long rbt = cs->leaseStartTime + cs->rebindTime * 1000;
    if (ct < rbt) {
        long long wt = (rbt - ct) / 2;
        if (wt >= 30000)
            send_renew(cs);
        else
            wt = rbt - ct;
        cs->timeout = wt;
        return;
    }
    long long elt = cs->leaseStartTime + cs->lease * 1000;
    if (ct < elt) {
        cs->dhcpState = DS_REBINDING;
        cs->timeout = elt - ct / 2;
        log_line("Entering rebinding state.");
    } else
        lease_timedout(cs);
}

// Triggered when a DHCP rebind request has been sent and no reply has been
// received within the response wait time.  Check to see if the lease is still
// valid, and if it is, send a broadcast DHCP renew packet.  If it is not, then
// change to the SELECTING state to get a new lease.
static void rebinding_timeout(struct client_state_t *cs)
{
    long long ct = curms();
    long long elt = cs->leaseStartTime + cs->lease * 1000;
    if (ct < elt) {
        long long wt = (elt - ct) / 2;
        if (wt >= 30000)
            send_rebind(cs);
        else
            wt = elt - ct;
        cs->timeout = wt;
    } else
        lease_timedout(cs);
}

static void released_timeout(struct client_state_t *cs)
{
    cs->timeout = -1;
}

static void collision_check_timeout(struct client_state_t *cs)
{
    arp_retransmit(cs);
}

static void bound_gw_check_timeout(struct client_state_t *cs)
{
    arp_retransmit(cs);
}

// Can transition to DS_BOUND or DS_SELECTING.
static void an_packet(struct client_state_t *cs, struct dhcpmsg *packet,
                      uint8_t *message)
{
    if (*message == DHCPACK) {
        ssize_t optlen;
        uint8_t *temp = get_option_data(packet, DHCP_LEASE_TIME, &optlen);
        cs->leaseStartTime = curms();
        if (!temp) {
            log_line("No lease time received, assuming 1h.");
            cs->lease = 60 * 60;
        } else {
            memcpy(&cs->lease, temp, 4);
            cs->lease = ntohl(cs->lease);
            cs->lease &= 0x7fffffff;
            if (cs->lease < 60) {
                log_warning("Server sent lease of <1m.  Forcing lease to 1m.");
                cs->lease = 60;
            }
        }
        // Always use RFC2131 'default' values.  It's not worth validating
        // the remote server values, if they even exist, for sanity.
        cs->renewTime = cs->lease >> 1;
        cs->rebindTime = (cs->lease * 0x7) >> 3; // * 0.875

        // Only check if we are either in the REQUESTING state, or if we
        // have received a lease with a different IP than what we had before.
        if (cs->dhcpState == DS_REQUESTING ||
            memcmp(&packet->yiaddr, &cs->clientAddr, 4)) {
            if (arp_check(cs, packet) == -1) {
                log_warning("arp_check failed to make arp socket, retrying lease");
                reinit_selecting(cs, 3000);
            }
        }

    } else if (*message == DHCPNAK) {
        log_line("Received DHCP NAK.");
        reinit_selecting(cs, 3000);
    }
}

static void selecting_packet(struct client_state_t *cs, struct dhcpmsg *packet,
                             uint8_t *message)
{
    if (*message == DHCPOFFER) {
        uint8_t *temp = NULL;
        ssize_t optlen;
        if ((temp = get_option_data(packet, DHCP_SERVER_ID, &optlen))) {
            memcpy(&cs->serverAddr, temp, 4);
            cs->xid = packet->xid;
            cs->clientAddr = packet->yiaddr;
            cs->dhcpState = DS_REQUESTING;
            cs->timeout = 0;
            num_dhcp_requests = 0;
        } else {
            log_line("No server ID in message");
        }
    }
}

// Triggered after a DHCP discover packet has been sent and no reply has
// been received within the response wait time.  If we've not exceeded the
// maximum number of discover retransmits, then send another packet and wait
// again.  Otherwise, background or fail.
static void selecting_timeout(struct client_state_t *cs)
{
    if (cs->init && num_dhcp_requests >= 2) {
        if (client_config.background_if_no_lease) {
            log_line("No lease, going to background.");
            cs->init = 0;
            background(cs);
        } else if (client_config.abort_if_no_lease) {
            log_line("No lease, failing.");
            exit(EXIT_FAILURE);
        }
    }
    if (num_dhcp_requests == 0)
        cs->xid = libc_random_u32();
    send_discover(cs);
    cs->timeout = delay_timeout(num_dhcp_requests);
    num_dhcp_requests++;
}

static void xmit_release(struct client_state_t *cs)
{
    log_line("Unicasting a release of %s to %s.",
             inet_ntoa((struct in_addr){.s_addr=cs->clientAddr}),
             inet_ntoa((struct in_addr){.s_addr=cs->serverAddr}));
    send_release(cs);
    print_release(cs);
}

static void print_release(struct client_state_t *cs)
{
    log_line("Entering released state.");
    set_released(cs);
}

static void frenew(struct client_state_t *cs)
{
    if (cs->dhcpState == DS_BOUND) {
        log_line("Forcing a DHCP renew...");
        cs->dhcpState = DS_RENEWING;
        set_listen_cooked(cs);
        send_renew(cs);
    } else if (cs->dhcpState == DS_RELEASED)
        reinit_selecting(cs, 0);
}

void ifup_action(struct client_state_t *cs)
{
    // If we have a lease, check to see if our gateway is still valid via ARP.
    // If it fails, state -> SELECTING.
    if (cs->dhcpState == DS_BOUND || cs->dhcpState == DS_RENEWING ||
        cs->dhcpState == DS_REBINDING) {
        if (arp_gw_check(cs) == -1)
            log_warning("nl: arp_gw_check could not make arp socket, assuming lease is still OK");
        else
            log_line("nl: interface back, revalidating lease");
        // If we don't have a lease, state -> SELECTING.
    } else if (cs->dhcpState != DS_SELECTING) {
        log_line("nl: %s back, querying for new lease", client_config.interface);
        reinit_selecting(cs, 0);
    }
}

void ifdown_action(struct client_state_t *cs)
{
    log_line("Interface shut down.  Going to sleep.");
    set_released(cs);
}

void ifnocarrier_action(struct client_state_t *cs)
{
    log_line("Interface carrier down.");
}

void packet_action(struct client_state_t *cs, struct dhcpmsg *packet,
                   uint8_t *message)
{
    if (dhcp_states[cs->dhcpState].packet_fn)
        dhcp_states[cs->dhcpState].packet_fn(cs, packet, message);
}

void timeout_action(struct client_state_t *cs)
{
    if (dhcp_states[cs->dhcpState].timeout_fn)
        dhcp_states[cs->dhcpState].timeout_fn(cs);
}

void force_renew_action(struct client_state_t *cs)
{
    if (dhcp_states[cs->dhcpState].force_renew_fn)
        dhcp_states[cs->dhcpState].force_renew_fn(cs);
}

void force_release_action(struct client_state_t *cs)
{
    if (dhcp_states[cs->dhcpState].force_release_fn)
        dhcp_states[cs->dhcpState].force_release_fn(cs);
}

