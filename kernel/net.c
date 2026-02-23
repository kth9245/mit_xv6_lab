#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"

// xv6's ethernet and IP addresses
static uint8 local_mac[ETHADDR_LEN] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static uint32 local_ip = MAKE_IP_ADDR(10, 0, 2, 15);

// qemu host's ethernet address.
static uint8 host_mac[ETHADDR_LEN] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };

static struct spinlock netlock;

#define UDPQMAX 16
#define NBOUND 64

struct uptq {
  int src_ip;
  short src_port;
  char *buf;
  int len;
};

struct bound {
  int port;
  struct uptq q[UDPQMAX];
  int qhead;
  int qtail;
};

static struct bound bound[NBOUND];

static int
q_empty(struct bound *b)
{
  return b->qhead == b->qtail;
}

static int
q_full(struct bound *b)
{
  return ((b->qtail + 1) % UDPQMAX) == b->qhead;
}

static int
q_push(struct bound *b, int src_ip, short src_port, char *payload, int len)
{
  if(q_full(b))
    return -1;

  char *copy = kalloc();
  if(copy == 0)
    return -1;

  if(len > PGSIZE) len = PGSIZE;
  memmove(copy, payload, len);

  int t = b->qtail;
  if(b->q[t].buf){
    kfree(b->q[t].buf);
    b->q[t].buf = 0;
  }

  b->q[t].src_ip = src_ip;
  b->q[t].src_port = src_port;
  b->q[t].buf = copy;
  b->q[t].len = len;

  b->qtail = (b->qtail + 1) % UDPQMAX;
  return 0;
}

static int
q_pop(struct bound *b, int *src_ip, short *src_port, char **buf, int *len)
{
  if(q_empty(b))
    return -1;

  int h = b->qhead;

  *src_ip = b->q[h].src_ip;
  *src_port = b->q[h].src_port;
  *buf = b->q[h].buf;
  *len = b->q[h].len;

  b->q[h].src_ip = 0;
  b->q[h].src_port = 0;
  b->q[h].buf = 0;
  b->q[h].len = 0;

  b->qhead = (b->qhead + 1) % UDPQMAX;
  return 0;
}

int
find_bound(int port)
{
  for(int i = 0; i < NBOUND; i++){
    if(bound[i].port == port)
      return i;
  }
  return -1;
}

void
netinit(void)
{
  initlock(&netlock, "netlock");
}


//
// bind(int port)
// prepare to receive UDP packets address to the port,
// i.e. allocate any queues &c needed.
//
uint64
sys_bind(void)
{
  //
  // Your code here.
  //
  int port;
  argint(0, &port);
  acquire(&netlock);
  if (find_bound(port) >= 0){
    release(&netlock);
    return -1;
  }
  
  for (int i = 0; i < NBOUND; i++){
    if (bound[i].port == 0){
      bound[i].port = port;
      bound[i].qhead = 0;
      bound[i].qtail = 0;
      for (int j = 0; j < UDPQMAX; j++){
        bound[i].q[j].src_ip = 0;
        bound[i].q[j].src_port = 0;
        bound[i].q[j].buf = 0;
        bound[i].q[j].len = 0;
      }
      release(&netlock);
      return 0;
    }
  }
  return -1;
}

//
// unbind(int port)
// release any resources previously created by bind(port);
// from now on UDP packets addressed to port should be dropped.
//
uint64
sys_unbind(void)
{
  //
  // Optional: Your code here.
  //

  return 0;
}

//
// recv(int dport, int *src, short *sport, char *buf, int maxlen)
// if there's a received UDP packet already queued that was
// addressed to dport, then return it.
// otherwise wait for such a packet.
//
// sets *src to the IP source address.
// sets *sport to the UDP source port.
// copies up to maxlen bytes of UDP payload to buf.
// returns the number of bytes copied,
// and -1 if there was an error.
//
// dport, *src, and *sport are host byte order.
// bind(dport) must previously have been called.
//
uint64
sys_recv(void)
{
  //
  // Your code here.
  //
  struct proc *p = myproc();
  int dport;
  uint64 srcaddr;
  uint64 sportaddr;
  uint64 bufaddr;
  int maxlen;

  argint(0, &dport);
  argaddr(1, &srcaddr);
  argaddr(2, &sportaddr);
  argaddr(3, &bufaddr);
  argint(4, &maxlen);

  acquire(&netlock);

  for(;;){
    int idx = find_bound(dport);
    if(idx < 0){
      release(&netlock);
      return -1;
    }
    struct bound *b = &bound[idx];
    int src_ip;
    short src_port;
    char *kbuf;
    int klen;

    if(q_pop(b, &src_ip, &src_port, &kbuf, &klen) == 0){
      int n = klen;
      if(n > maxlen) n = maxlen;
      if(n < 0) n = 0;

      if(copyout(p->pagetable, bufaddr, kbuf, n) < 0){
        if(kbuf) kfree(kbuf);
        release(&netlock);
        return -1;
      }
      if(copyout(p->pagetable, srcaddr, (char*)&src_ip, sizeof(src_ip)) < 0){
        if(kbuf) kfree(kbuf);
        release(&netlock);
        return -1;
      }
      if(copyout(p->pagetable, sportaddr, (char*)&src_port, sizeof(src_port)) < 0){
        if(kbuf) kfree(kbuf);
        release(&netlock);
        return -1;
      }
      if(kbuf) kfree(kbuf);
      release(&netlock);
      return n;
    }

    sleep(b, &netlock);
  }
}

// This code is lifted from FreeBSD's ping.c, and is copyright by the Regents
// of the University of California.
static unsigned short
in_cksum(const unsigned char *addr, int len)
{
  int nleft = len;
  const unsigned short *w = (const unsigned short *)addr;
  unsigned int sum = 0;
  unsigned short answer = 0;

  /*
   * Our algorithm is simple, using a 32 bit accumulator (sum), we add
   * sequential 16 bit words to it, and at the end, fold back all the
   * carry bits from the top 16 bits into the lower 16 bits.
   */
  while (nleft > 1)  {
    sum += *w++;
    nleft -= 2;
  }

  /* mop up an odd byte, if necessary */
  if (nleft == 1) {
    *(unsigned char *)(&answer) = *(const unsigned char *)w;
    sum += answer;
  }

  /* add back carry outs from top 16 bits to low 16 bits */
  sum = (sum & 0xffff) + (sum >> 16);
  sum += (sum >> 16);
  /* guaranteed now that the lower 16 bits of sum are correct */

  answer = ~sum; /* truncate to 16 bits */
  return answer;
}

//
// send(int sport, int dst, int dport, char *buf, int len)
//
uint64
sys_send(void)
{
  struct proc *p = myproc();
  int sport;
  int dst;
  int dport;
  uint64 bufaddr;
  int len;

  argint(0, &sport);
  argint(1, &dst);
  argint(2, &dport);
  argaddr(3, &bufaddr);
  argint(4, &len);

  int total = len + sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp);
  if(total > PGSIZE)
    return -1;

  char *buf = kalloc();
  if(buf == 0){
    printf("sys_send: kalloc failed\n");
    return -1;
  }
  memset(buf, 0, PGSIZE);

  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, host_mac, ETHADDR_LEN);
  memmove(eth->shost, local_mac, ETHADDR_LEN);
  eth->type = htons(ETHTYPE_IP);

  struct ip *ip = (struct ip *)(eth + 1);
  ip->ip_vhl = 0x45; // version 4, header length 4*5
  ip->ip_tos = 0;
  ip->ip_len = htons(sizeof(struct ip) + sizeof(struct udp) + len);
  ip->ip_id = 0;
  ip->ip_off = 0;
  ip->ip_ttl = 100;
  ip->ip_p = IPPROTO_UDP;
  ip->ip_src = htonl(local_ip);
  ip->ip_dst = htonl(dst);
  ip->ip_sum = in_cksum((unsigned char *)ip, sizeof(*ip));

  struct udp *udp = (struct udp *)(ip + 1);
  udp->sport = htons(sport);
  udp->dport = htons(dport);
  udp->ulen = htons(len + sizeof(struct udp));

  char *payload = (char *)(udp + 1);
  if(copyin(p->pagetable, payload, bufaddr, len) < 0){
    kfree(buf);
    printf("send: copyin failed\n");
    return -1;
  }

  e1000_transmit(buf, total);

  return 0;
}

void
ip_rx(char *buf, int len)
{
  // don't delete this printf; make grade depends on it.
  static int seen_ip = 0;
  if(seen_ip == 0)
    printf("ip_rx: received an IP packet\n");
  seen_ip = 1;

  //
  // Your code here.
  //
  if(len < (int)(sizeof(struct eth) + sizeof(struct ip))){
    kfree(buf);
    return;
  }

  struct eth *eth = (struct eth *)buf;
  struct ip *ip = (struct ip *)(eth + 1);

  int ihl = (ip->ip_vhl & 0x0f) * 4;
  if(ihl < (int)sizeof(struct ip)){
    kfree(buf);
    return;
  }

  int iplen = ntohs(ip->ip_len);
  int have = len - (int)sizeof(struct eth);
  if(iplen > have || iplen < ihl){
    kfree(buf);
    return;
  }

  if(ip->ip_p != IPPROTO_UDP){
    kfree(buf);
    return;
  }

  if(iplen < ihl + (int)sizeof(struct udp)){
    kfree(buf);
    return;
  }

  struct udp *udp = (struct udp *)((char*)ip + ihl);
  int ulen = ntohs(udp->ulen);
  if(ulen < (int)sizeof(struct udp)){
    kfree(buf);
    return;
  }
  int udp_avail = iplen - ihl;
  if(ulen > udp_avail){
    kfree(buf);
    return;
  }

  int dport = ntohs(udp->dport);
  int sport = ntohs(udp->sport);
  int payload_len = ulen - (int)sizeof(struct udp);
  char *payload = (char *)(udp + 1);

  int src_ip = ntohl(ip->ip_src);
  short src_port = (short)sport;

  acquire(&netlock);

  int idx = find_bound(dport);
  if(idx >= 0){
    struct bound *b = &bound[idx];
    if(q_push(b, src_ip, src_port, payload, payload_len) == 0){
      wakeup(b);
    }
  }
  release(&netlock);
  kfree(buf);
}

//
// send an ARP reply packet to tell qemu to map
// xv6's ip address to its ethernet address.
// this is the bare minimum needed to persuade
// qemu to send IP packets to xv6; the real ARP
// protocol is more complex.
//
void
arp_rx(char *inbuf)
{
  static int seen_arp = 0;

  if(seen_arp){
    kfree(inbuf);
    return;
  }
  printf("arp_rx: received an ARP packet\n");
  seen_arp = 1;

  struct eth *ineth = (struct eth *) inbuf;
  struct arp *inarp = (struct arp *) (ineth + 1);

  char *buf = kalloc();
  if(buf == 0)
    panic("send_arp_reply");
  
  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, ineth->shost, ETHADDR_LEN); // ethernet destination = query source
  memmove(eth->shost, local_mac, ETHADDR_LEN); // ethernet source = xv6's ethernet address
  eth->type = htons(ETHTYPE_ARP);

  struct arp *arp = (struct arp *)(eth + 1);
  arp->hrd = htons(ARP_HRD_ETHER);
  arp->pro = htons(ETHTYPE_IP);
  arp->hln = ETHADDR_LEN;
  arp->pln = sizeof(uint32);
  arp->op = htons(ARP_OP_REPLY);

  memmove(arp->sha, local_mac, ETHADDR_LEN);
  arp->sip = htonl(local_ip);
  memmove(arp->tha, ineth->shost, ETHADDR_LEN);
  arp->tip = inarp->sip;

  e1000_transmit(buf, sizeof(*eth) + sizeof(*arp));

  kfree(inbuf);
}

void
net_rx(char *buf, int len)
{
  struct eth *eth = (struct eth *) buf;

  if(len >= sizeof(struct eth) + sizeof(struct arp) &&
     ntohs(eth->type) == ETHTYPE_ARP){
    arp_rx(buf);
  } else if(len >= sizeof(struct eth) + sizeof(struct ip) &&
     ntohs(eth->type) == ETHTYPE_IP){
    ip_rx(buf, len);
  } else {
    kfree(buf);
  }
}
