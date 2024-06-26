#include "tcp.h"

#include <sys/param.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include "net/pbuf.h"
#include "net/ipv4/ipv4.h"
#include "net/net.h"
#include "net/netif.h"

#include "irq.h"

#include <mios/service.h>
#include <mios/eventlog.h>
#include <mios/cli.h>

#define TCP_EVENT_CONNECT (1 << SOCKET_EVENT_PROTO)
/*
 * Based on these RFCs:
 *
 *  Transmission Control Protocol (TCP)
 *          https://datatracker.ietf.org/doc/html/rfc9293
 *
 *  Computing TCP's Retransmission Timer
 *          https://datatracker.ietf.org/doc/html/rfc6298
 *
 */


#define TCP_KEEPALIVE_INTERVAL 10000 // ms
#define TCP_TIMEOUT_INTERVAL   21000 // ms
#define TCP_TIMEOUT_HANDSHAKE   5000 // ms

#define TCP_STATE_CLOSED       0
#define TCP_STATE_LISTEN       1
#define TCP_STATE_SYN_SENT     2
#define TCP_STATE_SYN_RECEIVED 3
#define TCP_STATE_ESTABLISHED  4
#define TCP_STATE_FIN_WAIT1    5
#define TCP_STATE_FIN_WAIT2    6
#define TCP_STATE_CLOSING      7
#define TCP_STATE_TIME_WAIT    8
#define TCP_STATE_CLOSE_WAIT   9
#define TCP_STATE_LAST_ACK     10

LIST_HEAD(tcb_list, tcb);

static struct tcb_list tcbs;
static mutex_t tcbs_mutex = MUTEX_INITIALIZER("tcp");

typedef struct tcb {

  net_task_t tcb_task;

  LIST_ENTRY(tcb) tcb_link;

  socket_t tcb_sock;

  uint8_t tcb_state;
  uint8_t tcb_app_closed;

  struct {
    uint32_t nxt;
    uint32_t una;
    uint16_t wnd;
    uint16_t up;
    uint32_t wl1;
    uint32_t wl2;
  } tcb_snd;
  uint32_t tcb_iss;

  struct {
    uint32_t nxt;
    uint16_t wnd;
    uint16_t up;
    uint16_t mss;
  } tcb_rcv;

  uint32_t tcb_irs;

  uint32_t tcb_local_addr;
  uint32_t tcb_remote_addr;

  uint16_t tcb_remote_port;
  uint16_t tcb_local_port;

  struct pbuf_queue tcb_unaq;
  size_t tcb_unaq_buffers;

  timer_t tcb_rtx_timer;
  timer_t tcb_delayed_ack_timer;
  timer_t tcb_time_wait_timer;

  int tcb_rto;  // in ms
  int tcb_srtt;
  int tcb_rttvar;

  uint32_t tcb_app_pending;

  uint64_t tcb_tx_bytes;
  uint64_t tcb_rx_bytes;
  uint64_t tcb_rtx_bytes;

  uint64_t tcb_last_rx;
  uint32_t tcb_timo;

  const char *tcb_name;

} tcb_t;


#define TCP_PBUF_HEADROOM (16 + sizeof(ipv4_header_t) + sizeof(tcp_hdr_t))

static void tcp_close(tcb_t *tcb, const char *reason);

static const char *tcp_state_to_str(int state);


static void
tcp_set_state(tcb_t *tcb, int state, const char *reason)
{
  if(tcb->tcb_state == state)
    return;
#if 0
  evlog(LOG_DEBUG, "%s: %s->%s (%s)",
        tcb->tcb_name,
        tcp_state_to_str(tcb->tcb_state),
        tcp_state_to_str(state),
        reason);
#endif
  tcb->tcb_state = state;
}

static void
tcp_output(pbuf_t *pb, uint32_t local_addr, uint32_t remote_addr)
{
  nexthop_t *nh = ipv4_nexthop_resolve(remote_addr);
  if(nh == NULL) {
    pbuf_free(pb);
    return;
  }
  netif_t *ni = nh->nh_netif;

  if(local_addr == 0)
    local_addr = ni->ni_local_addr;

  tcp_hdr_t *th = pbuf_data(pb, 0);

#if 0
  if(th->flg == (TCP_F_ACK | TCP_F_PSH)) {
    static int x;
    x++;
    if(x == 10) {
      x = 0;
      printf("drop output\n");
      pbuf_free(pb);
      return;
    }
  }
#endif


  th->up = 0;
  th->cksum = 0;

  if(!(ni->ni_flags & NETIF_F_TX_TCP_CKSUM_OFFLOAD)) {
    th->cksum =
      ipv4_cksum_pbuf(ipv4_cksum_pseudo(local_addr, remote_addr,
                                        IPPROTO_TCP, pb->pb_pktlen),
                      pb, 0, pb->pb_pktlen);
  }

  pb = pbuf_prepend(pb, sizeof(ipv4_header_t), 0, 0);
  if(pb == NULL)
    return;

  ipv4_header_t *ip = pbuf_data(pb, 0);

  ip->ver_ihl = 0x45;
  ip->tos = 0;

  ip->total_length = htons(pb->pb_pktlen);
  ip->id = rand();
  ip->fragment_info = htons(IPV4_F_DF);
  ip->ttl = 255;
  ip->proto = IPPROTO_TCP;
  ip->src_addr = local_addr;
  ip->dst_addr = remote_addr;

  ip->cksum = 0;
  if(!(ni->ni_flags & NETIF_F_TX_IPV4_CKSUM_OFFLOAD)) {
    ip->cksum = ipv4_cksum_pbuf(0, pb, 0, sizeof(ipv4_header_t));
  }

  nh->nh_netif->ni_output(ni, nh, pb);

}

static void
tcp_output_tcb(tcb_t *tcb, pbuf_t *pb)
{
  if(pb->pb_flags & PBUF_SEQ) {
    // SYN or FIN packet, trim fake payload of 1 byte (representing seq)
    pbuf_trim(pb, 1);
  }

  tcp_hdr_t *th = pbuf_data(pb, 0);

  th->src_port = tcb->tcb_local_port;
  th->dst_port = tcb->tcb_remote_port;
  th->ack = htonl(tcb->tcb_rcv.nxt);
  th->wnd = htons(tcb->tcb_rcv.wnd);

  if(th->flg & TCP_F_SYN) {
    uint8_t *opts = pbuf_append(pb, 4);
    opts[0] = 2;
    opts[1] = 4;
    opts[2] = tcb->tcb_rcv.mss >> 8;
    opts[3] = tcb->tcb_rcv.mss;
    th->off = ((sizeof(tcp_hdr_t) >> 2) + 1) << 4;
  } else {
    th->off = (sizeof(tcp_hdr_t) >> 2) << 4;
  }

  tcp_output(pb, tcb->tcb_local_addr, tcb->tcb_remote_addr);
}


static void
tcp_send(tcb_t *tcb, pbuf_t *pb, int seg_len, uint8_t flag)
{
  timer_disarm(&tcb->tcb_delayed_ack_timer);

  if(seg_len) {
    if(STAILQ_FIRST(&tcb->tcb_unaq) == NULL) {
      net_timer_arm(&tcb->tcb_rtx_timer, clock_get() + tcb->tcb_rto * 1000);
    }

    pbuf_t *tx = pbuf_copy_pkt(pb, 0);
    while(pb) {
      pbuf_t *n = pb->pb_next;
      STAILQ_INSERT_TAIL(&tcb->tcb_unaq, pb, pb_link);
      tcb->tcb_unaq_buffers++;
      pb = n;
    }
    if(tx == NULL)
      return;
    pb = tx;
  }

  pb = pbuf_prepend(pb, sizeof(tcp_hdr_t), 0, 0);
  if(pb == NULL)
    return;
  tcp_hdr_t *th = pbuf_data(pb, 0);

  th->flg = flag;
  th->seq = htonl(tcb->tcb_snd.nxt);
  tcp_output_tcb(tcb, pb);
  tcb->tcb_snd.nxt += seg_len;
}


static void
arm_rtx(tcb_t *tcb, uint64_t now)
{
  int rtx_duration_ms;

  if(STAILQ_FIRST(&tcb->tcb_unaq)) {
    rtx_duration_ms = tcb->tcb_rto;
  } else {
    rtx_duration_ms = TCP_KEEPALIVE_INTERVAL;
  }
  net_timer_arm(&tcb->tcb_rtx_timer, now + rtx_duration_ms * 1000);
}


static void
tcp_rtx_cb(void *opaque, uint64_t now)
{
  tcb_t *tcb = opaque;

  if(now > tcb->tcb_last_rx + tcb->tcb_timo) {
    tcp_close(tcb, "TCP Timeout");
    return;
  }

  pbuf_t *q = STAILQ_FIRST(&tcb->tcb_unaq);
  pbuf_t *pb;

  if(q == NULL) {
    // Send keep-alive if ESTABLISHED

    pb = NULL;
    if(tcb->tcb_state == TCP_STATE_ESTABLISHED) {

      pb = pbuf_make(TCP_PBUF_HEADROOM, 0);
      if(pb == NULL)
        return;

      pb = pbuf_prepend(pb, sizeof(tcp_hdr_t), 0, 0);
      if(pb == NULL)
        return;
      tcp_hdr_t *th = pbuf_data(pb, 0);
      th->flg = TCP_F_ACK | TCP_F_PSH;

      th->seq = htonl(tcb->tcb_snd.una - 1);
    }

  } else {
    // Retransmit data

    pb = pbuf_copy_pkt(q, 0);
    if(pb != NULL) {
      pb = pbuf_prepend(pb, sizeof(tcp_hdr_t), 0, 0);
      if(pb != NULL) {
        tcp_hdr_t *th = pbuf_data(pb, 0);

        if(pb->pb_flags & PBUF_SEQ) {
          th->flg = *(const uint8_t *)pbuf_cdata(pb, sizeof(tcp_hdr_t));
        } else {
          th->flg = TCP_F_ACK | TCP_F_PSH;
          tcb->tcb_rtx_bytes += q->pb_pktlen;
        }
        th->seq = htonl(tcb->tcb_snd.una);
      }
    }
  }

  if(pb)
    tcp_output_tcb(tcb, pb);

  arm_rtx(tcb, now);
}


static void
tcp_ack(tcb_t *tcb, uint32_t count)
{
  if(count == 0)
    return;

  tcb->tcb_snd.una += count;

  // TBD: Is this really a correct implemenation of pbuf_drop() ?
  while(count) {
    pbuf_t *pb = STAILQ_FIRST(&tcb->tcb_unaq);

    size_t c = MIN(count, pb->pb_buflen);
    assert(pb != NULL);
    assert(pb->pb_flags & PBUF_SOP);
    pb->pb_buflen -= c;
    pb->pb_pktlen -= c;
    pb->pb_offset += c;
    count -= c;

    if(pb->pb_buflen == 0) {
      STAILQ_REMOVE_HEAD(&tcb->tcb_unaq, pb_link);
      tcb->tcb_unaq_buffers--;

      if(pb->pb_next != NULL) {
        if((pb->pb_flags & (PBUF_SOP | PBUF_EOP)) == PBUF_SOP) {
          // Propagate SOP-flag to next buffer for same packet
          pb->pb_next->pb_flags |= PBUF_SOP;
          pb->pb_next->pb_pktlen = pb->pb_pktlen;
        }
        pb->pb_next = NULL;
      }

      pbuf_free(pb);
    }
  }

  arm_rtx(tcb, tcb->tcb_last_rx);
}



static int
tcp_send_data(tcb_t *tcb)
{
  if(tcb->tcb_sock.app_opaque == NULL)
    return 0;

  // This is not precise enough
  if(tcb->tcb_snd.wnd < PBUF_DATA_SIZE)
    return 0;

  // Replace with some kind of global pbuf pressure
  if(tcb->tcb_unaq_buffers >= 10)
    return 0;

  pbuf_t *pb = tcb->tcb_sock.app->pull(tcb->tcb_sock.app_opaque);
  if(pb == NULL)
    return 0;

  tcp_send(tcb, pb, pb->pb_pktlen, TCP_F_ACK | TCP_F_PSH);
  tcb->tcb_tx_bytes += pb->pb_pktlen;
  return 1;
}


static void
tcp_send_flag(tcb_t *tcb, uint8_t flag, pbuf_t *pb)
{
  if(pb == NULL) {
    pb = pbuf_make(TCP_PBUF_HEADROOM, 0);
    if(pb == NULL) {
      return;
    }
  } else {
    // Recycle packet
    pbuf_reset(pb, TCP_PBUF_HEADROOM, 0);
  }

  int seg_len = 0;
  if(flag & (TCP_F_SYN | TCP_F_FIN)) {
    pb->pb_flags |= PBUF_SEQ;
    pb->pb_buflen = 1;
    pb->pb_pktlen = 1;
    *(uint8_t *)pbuf_data(pb, 0) = flag;
    seg_len = 1;
  }
  tcp_send(tcb, pb, seg_len, flag);
}




static pbuf_t *
tcp_send_data_or_ack(tcb_t *tcb, pbuf_t *pb)
{
  if(tcp_send_data(tcb)) {
    return pb;
  }
  tcp_send_flag(tcb, TCP_F_ACK, pb);
  return NULL;
}


static void
tcp_destroy(tcb_t *tcb)
{
  if(tcb->tcb_state != TCP_STATE_CLOSED)
    return;

  if(!tcb->tcb_app_closed)
    return;

  free(tcb);
}


static tcb_t *
tcb_find(uint32_t remote_addr, uint16_t remote_port, uint16_t local_port)
{
  tcb_t *tcb;
  LIST_FOREACH(tcb, &tcbs, tcb_link) {
    if(tcb->tcb_remote_addr == remote_addr &&
       tcb->tcb_remote_port == remote_port &&
       tcb->tcb_local_port == local_port) {
      return tcb;
    }
  }
  return NULL;
}

static void
tcp_do_connect(tcb_t *tcb)
{
  while(1) {
    uint16_t local_port = rand();
    if(local_port < 1024)
      continue;
    local_port = htons(local_port);
    if(tcb_find(tcb->tcb_remote_addr, tcb->tcb_remote_port, local_port))
      continue;
    tcb->tcb_local_port = local_port;
    break;
  }

  // A buffer has been preallocated for us in tcp_connect() and parked
  // on the unaq queue
  pbuf_t *pb = STAILQ_FIRST(&tcb->tcb_unaq);
  STAILQ_INIT(&tcb->tcb_unaq);

  // Set last_rx to now, otherwise we get an almost instant TCP timeout
  // in tcp_rtx_cb()
  tcb->tcb_last_rx = clock_get();

  tcp_send_flag(tcb, TCP_F_SYN, pb);
  tcp_set_state(tcb, TCP_STATE_SYN_SENT, "syn-sent");

  mutex_lock(&tcbs_mutex);
  LIST_INSERT_HEAD(&tcbs, tcb, tcb_link);
  mutex_unlock(&tcbs_mutex);
}


static void
tcp_task_cb(net_task_t *nt, uint32_t signals)
{
  tcb_t *tcb = (tcb_t *)nt;

  if(signals & TCP_EVENT_CONNECT) {
    tcp_do_connect(tcb);
  }

  if(signals & SOCKET_EVENT_PULL) {
    switch(tcb->tcb_state) {
    case TCP_STATE_ESTABLISHED:
    case TCP_STATE_CLOSE_WAIT:
      signals &= ~SOCKET_EVENT_PUSH;
      tcp_send_data(tcb);
      break;
    default:
      break;
    }
  }

  if(signals & SOCKET_EVENT_PUSH) {
    if(tcb->tcb_app_pending) {
      tcb->tcb_rx_bytes += tcb->tcb_app_pending;
      tcb->tcb_app_pending = 0;
      tcp_send_flag(tcb, TCP_F_ACK, NULL);
    }
  }

  if(signals & SOCKET_EVENT_CLOSE) {

    tcb->tcb_app_pending = 0;
    tcb->tcb_app_closed = 1;

    switch(tcb->tcb_state) {
    case TCP_STATE_ESTABLISHED:
    case TCP_STATE_SYN_RECEIVED:
      tcp_set_state(tcb, TCP_STATE_FIN_WAIT1, "service_close");
      tcp_send_flag(tcb, TCP_F_FIN | TCP_F_ACK, NULL);
      break;
    case TCP_STATE_CLOSE_WAIT:
      tcp_set_state(tcb, TCP_STATE_LAST_ACK, "service_close");
      tcp_send_flag(tcb, TCP_F_FIN | TCP_F_ACK, NULL);
      break;
    }

    tcp_destroy(tcb);
  }
}


static void
tcp_service_event_cb(void *opaque, uint32_t events)
{
  tcb_t *tcb = opaque;
  net_task_raise(&tcb->tcb_task, events);
}


static pbuf_t *
tcp_reply(struct netif *ni, struct pbuf *pb, uint32_t remote_addr,
          uint32_t seq, uint32_t ack, uint8_t flag, uint16_t wnd)
{
  tcp_hdr_t *th = pbuf_data(pb, 0);

  const uint16_t src_port = th->src_port;
  const uint16_t dst_port = th->dst_port;

  pbuf_reset(pb, pb->pb_offset, sizeof(tcp_hdr_t));

  th->src_port = dst_port;
  th->dst_port = src_port;
  th->seq = htonl(seq);
  th->ack = htonl(ack);
  th->flg = flag;
  th->wnd = htons(wnd);
  th->off = (sizeof(tcp_hdr_t) >> 2) << 4;

  tcp_output(pb, ni->ni_local_addr, remote_addr);
  return NULL;
}

struct pbuf *
tcp_reject(struct netif *ni, struct pbuf *pb, uint32_t remote_addr,
           uint16_t port, uint32_t ack, const char *reason)
{

  static int64_t last_log;
  int64_t now = clock_get();
  if(now > last_log + 250000) {
    last_log = now;
    evlog(LOG_NOTICE, "Connection from %Id to port %d rejected -- %s",
          remote_addr, port, reason);
  }
  return tcp_reply(ni, pb, remote_addr, 0, ack, TCP_F_ACK | TCP_F_RST, 0);
}


static void
tcp_close_app(tcb_t *tcb, const char *errmsg)
{
  if(tcb->tcb_sock.app_opaque) {
    tcb->tcb_sock.app->close(tcb->tcb_sock.app_opaque, errmsg);
    tcb->tcb_sock.app_opaque = NULL;
  }
}

static void
tcp_disarm_all_timers(tcb_t *tcb)
{
  timer_disarm(&tcb->tcb_rtx_timer);
  timer_disarm(&tcb->tcb_delayed_ack_timer);
  timer_disarm(&tcb->tcb_time_wait_timer);
}

static void
tcp_close(tcb_t *tcb, const char *reason)
{
  tcp_disarm_all_timers(tcb);

  int q = irq_forbid(IRQ_LEVEL_NET);
  pbuf_free_queue_irq_blocked(&tcb->tcb_unaq);
  irq_permit(q);

  mutex_lock(&tcbs_mutex);
  LIST_REMOVE(tcb, tcb_link);
  mutex_unlock(&tcbs_mutex);

  tcp_set_state(tcb, TCP_STATE_CLOSED, reason);

  tcp_close_app(tcb, reason);

  tcp_destroy(tcb);
}


static void
tcp_time_wait_cb(void *opaque, uint64_t now)
{
  tcb_t *tcb = opaque;
  tcp_close(tcb, "time-wait timeout");
}


static void
tcp_time_wait(tcb_t *tcb, const char *reason)
{
  tcp_set_state(tcb, TCP_STATE_TIME_WAIT, reason);
  tcp_disarm_all_timers(tcb);
  net_timer_arm(&tcb->tcb_time_wait_timer, clock_get() + 5000000);
}


static void
tcp_delayed_ack_cb(void *opaque, uint64_t now)
{
  tcb_t *tcb = opaque;
  tcp_send_flag(tcb, TCP_F_ACK, NULL);
}

static void
tcp_parse_options(tcb_t *tcb, const uint8_t *buf, size_t len)
{
  while(len > 0) {

    if(buf[0] == 0)
      return;
    if(buf[0] == 1) {
      buf++;
      len--;
      continue;
    }
    if(len < 2)
      return;
    int opt = buf[0];
    int optlen = buf[1];

    if(optlen > len)
      return;

    switch(opt) {
    case 2:
      tcb->tcb_sock.max_fragment_size = buf[3] | (buf[2] << 8);
      break;
    }
    buf += optlen;
    len -= optlen;
  }
}

static const socket_net_fn_t tcp_net_fn = {
  .event = tcp_service_event_cb,
};


static tcb_t *
tcb_create(const char *name)
{
  tcb_t *tcb = xalloc(sizeof(tcb_t), 0, MEM_MAY_FAIL);
  if(tcb == NULL)
    return NULL;

  memset(tcb, 0, sizeof(tcb_t));

  tcb->tcb_name = name;

  STAILQ_INIT(&tcb->tcb_unaq);

  tcb->tcb_task.nt_cb = tcp_task_cb;

  tcb->tcb_sock.preferred_offset = TCP_PBUF_HEADROOM;
  tcb->tcb_sock.net = &tcp_net_fn;
  tcb->tcb_sock.net_opaque = tcb;

  tcb->tcb_rto = 250;

  tcb->tcb_time_wait_timer.t_cb = tcp_time_wait_cb;
  tcb->tcb_time_wait_timer.t_opaque = tcb;
  tcb->tcb_time_wait_timer.t_name = "tcp";

  tcb->tcb_delayed_ack_timer.t_cb = tcp_delayed_ack_cb;
  tcb->tcb_delayed_ack_timer.t_opaque = tcb;
  tcb->tcb_delayed_ack_timer.t_name = "tcp";

  tcb->tcb_rtx_timer.t_cb = tcp_rtx_cb;
  tcb->tcb_rtx_timer.t_opaque = tcb;
  tcb->tcb_rtx_timer.t_name = "tcp";

  tcb->tcb_rcv.mss = 1460;
  tcb->tcb_rcv.wnd = tcb->tcb_rcv.mss;

  tcb->tcb_iss = rand();
  tcb->tcb_snd.nxt = tcb->tcb_iss;
  tcb->tcb_snd.una = tcb->tcb_iss;

  tcb->tcb_sock.max_fragment_size = 536;

  tcb->tcb_timo = TCP_TIMEOUT_HANDSHAKE * 1000;
  return tcb;
}




struct pbuf *
tcp_input_ipv4(struct netif *ni, struct pbuf *pb, int tcp_offset)
{
  const ipv4_header_t *ip = pbuf_data(pb, 0);
  if(ip->dst_addr != ni->ni_local_addr) {
    // XXX: counter
    return pb; // Not for us
  }

  uint32_t remote_addr = ip->src_addr;
  pb = pbuf_drop(pb, tcp_offset);
  if(pbuf_pullup(pb, sizeof(tcp_hdr_t))) {
    // XXX: counter
    return pb;
  }
  const tcp_hdr_t *th = pbuf_data(pb, 0);

  const uint32_t hdr_len = (th->off & 0xf0) >> 2;

  if(hdr_len > pb->pb_pktlen || hdr_len < sizeof(tcp_hdr_t)) {
    // XXX: counter
    return pb;
  }

  uint32_t seg_len = pb->pb_pktlen - hdr_len;

  if(th->flg & TCP_F_SYN)
    seg_len++;
  if(th->flg & TCP_F_FIN)
    seg_len++;

  const uint16_t local_port = th->dst_port;
  const uint16_t remote_port = th->src_port;

  tcb_t *tcb = tcb_find(remote_addr, remote_port, local_port);

  const uint32_t seq = ntohl(th->seq);
  const uint32_t ack = ntohl(th->ack);
  const uint16_t wnd = ntohs(th->wnd);
  const uint8_t flag = th->flg;

  if(tcb == NULL) {

    if(flag != TCP_F_SYN) {

      if(flag & TCP_F_RST)
        return pb;

      if(flag & TCP_F_ACK) {
        return tcp_reply(ni, pb, remote_addr, ack, 0, TCP_F_RST, 0);
      } else {
        return tcp_reply(ni, pb, remote_addr, 0, seq + seg_len,
                         TCP_F_RST | TCP_F_ACK, 0);
      }
    }

    if(local_port == 0)
      return pb;

    uint16_t local_port_ho = ntohs(local_port);

    const service_t *svc = service_find_by_ip_port(local_port_ho);

    if(svc->type != SERVICE_TYPE_STREAM)
      svc = NULL;

    if(svc == NULL) {
      return tcp_reject(ni, pb, remote_addr, local_port_ho, seq + 1,
                        "no service");
    }

    tcb_t *tcb = tcb_create(svc->name);
    if(tcb == NULL) {
      return tcp_reject(ni, pb, remote_addr, local_port_ho, seq + 1,
                        "no memory");
    }

    tcb->tcb_local_addr = ni->ni_local_addr;
    tcb->tcb_remote_addr = remote_addr;

    tcb->tcb_local_port = local_port;
    tcb->tcb_remote_port = remote_port;
    tcb->tcb_irs = seq;
    tcb->tcb_rcv.nxt = tcb->tcb_irs + 1;

    if(!pbuf_pullup(pb, hdr_len)) {
      tcp_parse_options(tcb,
                        pbuf_data(pb, sizeof(tcp_hdr_t)),
                        hdr_len - sizeof(tcp_hdr_t));
    }

    error_t err = svc->open(&tcb->tcb_sock);
    if(err) {
      free(tcb);
      return tcp_reject(ni, pb, remote_addr, local_port_ho, seq + 1,
                        error_to_string(err));
    }

    tcp_send_flag(tcb, TCP_F_SYN | TCP_F_ACK, pb);

    tcp_set_state(tcb, TCP_STATE_SYN_RECEIVED, "syn-recvd");
    mutex_lock(&tcbs_mutex);
    LIST_INSERT_HEAD(&tcbs, tcb, tcb_link);
    mutex_unlock(&tcbs_mutex);
    return NULL;
  }

  const int una_ack = ack - tcb->tcb_snd.una;
  const int ack_nxt = tcb->tcb_snd.nxt - ack;

  if(tcb->tcb_state == TCP_STATE_SYN_SENT) {

    int ack_acceptance = 0;

    // 3.10.7.3 SYN-SENT STATE

    // First check ACK bit
    if(flag & TCP_F_ACK) {
      int ack_iss = ack - tcb->tcb_iss;
      int ack_nxt = ack - tcb->tcb_snd.nxt;

      if(ack_iss <= 0 || ack_nxt > 0) {
        if(flag & TCP_F_RST) {
          return pb;
        }
        return tcp_reply(ni, pb, remote_addr, tcb->tcb_snd.nxt,
                         0, TCP_F_RST, 0);
      }

      ack_acceptance = una_ack > 0 && ack_nxt >= 0;
    }

    if(flag & TCP_F_RST) {
      if(ack_acceptance) {
        tcp_close(tcb, "Connection refused");
        return pb;
      }
      return pb;
    }

    if(flag & TCP_F_SYN) {

      tcb->tcb_last_rx = clock_get();

      tcb->tcb_rcv.nxt = seq + 1;
      tcb->tcb_irs = seq;

      if(flag & TCP_F_ACK) {
        tcp_ack(tcb, una_ack);
      }

      const int una_iss = tcb->tcb_snd.una - tcb->tcb_iss;

      tcb->tcb_snd.wnd = wnd;
      tcb->tcb_snd.wl1 = seq;
      tcb->tcb_snd.wl2 = ack;

      if(una_iss > 0) {
        tcb->tcb_timo = TCP_TIMEOUT_INTERVAL * 1000;
        tcp_set_state(tcb, TCP_STATE_ESTABLISHED, "ack-in-syn-recvd");
        return tcp_send_data_or_ack(tcb, pb);
      }

      tcp_send_flag(tcb, TCP_F_SYN | TCP_F_ACK, pb);
      tcp_set_state(tcb, TCP_STATE_SYN_RECEIVED, "syn-recvd");
      return NULL;
    }

    // Neither SYN or RST is set, drop segment
    return pb;
  }


  //
  // Step 1: Sequence acceptance check
  //

  int acceptance = 0;
  int nxt_seq = seq - tcb->tcb_rcv.nxt;
  if(seg_len == 0) {

    if(tcb->tcb_rcv.wnd == 0) {
      acceptance = nxt_seq == 0;
    } else {
      acceptance = (uint32_t)nxt_seq < tcb->tcb_rcv.nxt + tcb->tcb_rcv.wnd;
    }
  } else {

    if(tcb->tcb_rcv.wnd == 0) {

    } else {

#if 0
      acceptance = (uint32_t)nxt_seq < tcb->tcb_rcv.nxt + tcb->tcb_rcv.wnd;

      int seq_delta_end = nxt_seq + seg_len - 1;
      acceptance |=
        (uint32_t)seq_delta_end < tcb->tcb_rcv.nxt + tcb->tcb_rcv.wnd;
#endif

      acceptance = nxt_seq == 0;
    }
  }

  if(!acceptance) {
    if(flag & TCP_F_RST)
      return pb;
    return tcp_reply(ni, pb, remote_addr, tcb->tcb_snd.nxt,
                     tcb->tcb_rcv.nxt, TCP_F_ACK, tcb->tcb_rcv.wnd);
  }

  //
  // Step 2: RST
  //

  if(flag & TCP_F_RST) {
    tcp_close(tcb, "Got RST");
    return pb;
  }

  //
  // Step 3: Security
  //

  //
  // Step 4: SYN Processing
  //

  if(flag & TCP_F_SYN) {
    return tcp_reply(ni, pb, remote_addr, tcb->tcb_snd.nxt,
                     tcb->tcb_rcv.nxt, TCP_F_RST, 0);
  }

  //
  // Step 5: ACK Processing
  //

  if(!(flag & TCP_F_ACK)) {
    return pb;
  }

  tcb->tcb_last_rx = clock_get();

  int try_send_more = 0;

  switch(tcb->tcb_state) {
  case TCP_STATE_SYN_RECEIVED:
    if(una_ack >= 0 && ack_nxt <= 0) {
      tcb->tcb_timo = TCP_TIMEOUT_INTERVAL * 1000;
      tcp_set_state(tcb, TCP_STATE_ESTABLISHED, "ack-in-syn-recvd");
      tcb->tcb_snd.wnd = wnd;
      tcb->tcb_snd.wl1 = seq;
      tcb->tcb_snd.wl2 = ack;
    } else {
      return tcp_reply(ni, pb, remote_addr, tcb->tcb_snd.nxt,
                       tcb->tcb_rcv.nxt, TCP_F_RST, 0);
    }
    // FALLTHRU
  case TCP_STATE_ESTABLISHED:
  case TCP_STATE_FIN_WAIT1:
  case TCP_STATE_FIN_WAIT2:
  case TCP_STATE_CLOSE_WAIT:
  case TCP_STATE_CLOSING:

    if(una_ack >= 0 && ack_nxt >= 0) {

      if(una_ack)
        try_send_more = 1;

      tcp_ack(tcb, una_ack); // Handles una_ack=0 by doing nothing

      int wl1_seq = seq - tcb->tcb_snd.wl1;
      int wl2_ack = ack - tcb->tcb_snd.wl2;

      if(wl1_seq > 0 || (wl1_seq == 0 && wl2_ack >= 0)) {
        tcb->tcb_snd.wnd = wnd;
        tcb->tcb_snd.wl1 = seq;
        tcb->tcb_snd.wl2 = ack;
      }

    } else if(ack < 0) {
      // Old
    } else if(ack_nxt < 0) {
      // Too new
      return tcp_reply(ni, pb, remote_addr, tcb->tcb_snd.nxt,
                       tcb->tcb_rcv.nxt, TCP_F_ACK, tcb->tcb_rcv.wnd);
    }

    switch(tcb->tcb_state) {
    case TCP_STATE_FIN_WAIT1:
      if(ack_nxt == 0) {
        tcp_set_state(tcb, TCP_STATE_FIN_WAIT2, "ACK in FIN_WAIT1");
      }
      break;
    case TCP_STATE_FIN_WAIT2:
      break;
    case TCP_STATE_CLOSE_WAIT:
      break;
    case TCP_STATE_CLOSING:
      if(ack_nxt == 0) {
        tcp_set_state(tcb, TCP_STATE_FIN_WAIT2, "ACK in CLOSING");
        // arm timer
      }
    }
    break;

  case TCP_STATE_LAST_ACK:
    tcp_close(tcb, "LAST_ACK");
    break;
  case TCP_STATE_TIME_WAIT:
    tcp_set_state(tcb, TCP_STATE_TIME_WAIT, "ACK_IN_TIME_WAIT");
    break;
  }

  //
  // Step 7: URG bit (Ignored)
  //

  //
  // Step 8: Process payload
  //

  pb = pbuf_drop(pb, hdr_len);

  switch(tcb->tcb_state) {
  case TCP_STATE_ESTABLISHED:
  case TCP_STATE_FIN_WAIT1:
  case TCP_STATE_FIN_WAIT2:

    if(!pb->pb_pktlen)
      break;

    size_t bytes = 0;
    uint32_t events = 0;
    if(tcb->tcb_sock.app->push_partial) {

      bytes = tcb->tcb_sock.app->push_partial(tcb->tcb_sock.app_opaque, pb);

    } else if(tcb->tcb_sock.app->may_push(tcb->tcb_sock.app_opaque)) {

      bytes = pb->pb_pktlen;
      events = tcb->tcb_sock.app->push(tcb->tcb_sock.app_opaque, pb);

      pb = NULL;

      if(!events && bytes) {
        tcb->tcb_app_pending += bytes;
        tcb->tcb_rcv.nxt = seq + bytes;
        timer_disarm(&tcb->tcb_delayed_ack_timer);
        try_send_more = 0;
        break;
      }
    }

    if(bytes) {
      tcb->tcb_rcv.nxt = seq + bytes;
      tcb->tcb_rx_bytes += bytes;

      int piggybacked_ack_sent = tcp_send_data(tcb);

      if(!piggybacked_ack_sent) {
        if(!timer_disarm(&tcb->tcb_delayed_ack_timer)) {
          // Already waited once, don't wait more, send now
          tcp_send_flag(tcb, TCP_F_ACK, pb);
          pb = NULL;
        } else {
          net_timer_arm(&tcb->tcb_delayed_ack_timer,
                        tcb->tcb_last_rx + 20000);
        }
      } else {
        try_send_more = 0;
      }
    } else {

      if(tcp_send_data_or_ack(tcb, pb)) {
        try_send_more = 0;
      } else {
        pb = NULL;
      }
    }
    break;

  default:
    break;
  }

  if(flag & TCP_F_FIN) {

    try_send_more = 0;
    tcp_close_app(tcb, "Connection closed by peer");

    switch(tcb->tcb_state) {
    case TCP_STATE_CLOSED:
    case TCP_STATE_LISTEN:
    case TCP_STATE_SYN_SENT:
      return pb;
    default:
      break;
    }

    tcb->tcb_rcv.nxt++;
    if(tcb->tcb_app_pending == 0) {
      tcp_send_flag(tcb, TCP_F_ACK, pb);
      pb = NULL;
    }

    switch(tcb->tcb_state) {
    case TCP_STATE_SYN_RECEIVED:
    case TCP_STATE_ESTABLISHED:
      tcp_set_state(tcb, TCP_STATE_CLOSE_WAIT, "FIN in ESTABLISHED");
      break;
    case TCP_STATE_FIN_WAIT1:
      if(ack == tcb->tcb_snd.nxt) {
        tcp_time_wait(tcb, "FIN in FIN_WAIT1");
      } else {
        tcp_set_state(tcb, TCP_STATE_CLOSING, "FIN in FIN_WAIT1");
      }
      break;
    case TCP_STATE_FIN_WAIT2:
      tcp_time_wait(tcb, "FIN in FIN_WAIT2");
      break;
    case TCP_STATE_CLOSE_WAIT:
    case TCP_STATE_CLOSING:
    case TCP_STATE_LAST_ACK:
      break;
    }
  }

  if(try_send_more) {
    tcp_send_data(tcb);
  }

  return pb;
}


struct socket *
tcp_create_socket(const char *name)
{
  tcb_t *tcb = tcb_create(name);
  if(tcb == NULL)
    return NULL;
  return &tcb->tcb_sock;
}


void
tcp_connect(struct socket *sk, uint32_t dst_addr, uint16_t dst_port)
{
  tcb_t *tcb = sk->net_opaque;
  tcb->tcb_remote_addr = dst_addr;
  tcb->tcb_remote_port = htons(dst_port);

  // Allocate a buffer for the SYN packet here where we might sleep
  pbuf_t *pb = pbuf_make(TCP_PBUF_HEADROOM, 1);
  STAILQ_INSERT_TAIL(&tcb->tcb_unaq, pb, pb_link);

  net_task_raise(&tcb->tcb_task, TCP_EVENT_CONNECT);
}


static const char *tcp_statenames =
  "CLOSED\0"
  "LISTEN\0"
  "SYN_SENT\0"
  "SYN_RECEIVED\0"
  "ESTABLISHED\0"
  "FIN_WAIT1\0"
  "FIN_WAIT2\0"
  "CLOSING\0"
  "TIME_WAIT\0"
  "CLOSE_WAIT\0"
  "LAST_ACK\0";


static const char *
tcp_state_to_str(int state)
{
  return strtbl(tcp_statenames, state);
}


static error_t
cmd_tcp(cli_t *cli, int argc, char **argv)
{
  tcb_t *tcb;

  mutex_lock(&tcbs_mutex);

  LIST_FOREACH(tcb, &tcbs, tcb_link) {
    cli_printf(cli, "%s\n\tLocal: %Id:%-5d\tRemote: %Id:%-5d\n",
               tcb->tcb_name,
               tcb->tcb_local_addr,
               ntohs(tcb->tcb_local_port),
               tcb->tcb_remote_addr,
               ntohs(tcb->tcb_remote_port));
    cli_printf(cli, "\t%s  TX:%lld RX:%lld  ReTX:%lld\n",
               tcp_state_to_str(tcb->tcb_state),
               tcb->tcb_tx_bytes,
               tcb->tcb_rx_bytes,
               tcb->tcb_rtx_bytes);
    cli_printf(cli, "\tRTO: %d ms\n", tcb->tcb_rto);
    cli_printf(cli, "\tUnacked: %d\n",
               tcb->tcb_snd.nxt  - tcb->tcb_snd.una);
  }

  mutex_unlock(&tcbs_mutex);
  return 0;
}

CLI_CMD_DEF("tcp", cmd_tcp);

