/*
 * Copyright 2013-16 Board of Trustees of Stanford University
 * Copyright 2013-16 Ecole Polytechnique Federale Lausanne (EPFL)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * tcp_api.c - plumbing between the TCP and userspace
 */

#include <assert.h>
#include <ix/stddef.h>
#include <ix/errno.h>
#include <ix/syscall.h>
#include <ix/log.h>
#include <ix/uaccess.h>
#include <ix/ethdev.h>
#include <ix/kstats.h>
#include <ix/cfg.h>
#include <ix/config.h>
#include <ix/stats.h>
#include <ix/queue.h>
#include <dune.h>
#include <ix/apic.h>

#include <lwip/tcp.h>

int ip_send_one(struct eth_fg *cur_fg, struct ip_addr *dst_addr, struct mbuf *pkt, size_t len);

#define MAX_PCBS	(512*1024)
#define DEFAULT_PORT 8000

/* FIXME: this should be probably per queue */
static DEFINE_PERCPU(struct tcp_pcb_listen[CFG_MAX_PORTS], listen_ports);

static DEFINE_PERCPU(uint16_t, local_port);
/* FIXME: this should be more adaptive to various configurations */
#define PORTS_PER_CPU (65536 / 32)

#if CONFIG_PRINT_CONNECTION_COUNT

static DEFINE_PERCPU(int, open_connections);
static DEFINE_PERCPU(struct timer, print_conn_timer);

#endif

#if CONFIG_RUN_TCP_STACK_IPI

static DEFINE_PERCPU(long, last_ipi_time);

#define IPI_TIMEOUT (4 * cycles_per_us)

#define RUN_TCP_STACK_IPI_VECTOR 0xf2

#endif

#define PCB_FLAG_READY 1
#define PCB_FLAG_CLOSED 2

/*
 * FIXME: LWIP and IX have different lifetime rules so we have to maintain
 * a seperate pcb. Otherwise, we'd be plagued by use-after-free problems.
 */
struct tcpapi_pcb {
	unsigned long alive; /* FIXME: this overlaps with mempool_hdr so
			      * we can tell if this pcb is allocated or not. */
	struct tcp_pcb *pcb;
	unsigned long cookie;
	struct ip_tuple *id;
	hid_t handle;
	struct pbuf *recvd;
	struct pbuf *recvd_tail;
	int queue;
	bool accepted;
	int sent_len;
	int len_xmited;
	struct queue pbuf_for_usys;
	struct queue_node ready_queue;
	int active_usys_count;
	char uevents;
	char flags;
	struct {
		uint64_t sysnr;
		long err;
	} lasterr;
};

#define PCB_UEVENT_KNOCK 1
#define PCB_UEVENT_CONNECTED 2

static struct mempool_datastore pcb_datastore;
static struct mempool_datastore id_datastore;

static DEFINE_PERCPU(struct mempool, pcb_mempool __attribute__((aligned(64))));
static DEFINE_PERCPU(struct mempool, id_mempool __attribute__((aligned(64))));

struct pcb_ready_queue {
	struct queue queue;
	spinlock_t lock;
};

static DEFINE_PERCPU(struct pcb_ready_queue, pcb_ready_queue);
static DEFINE_PERCPU(struct drand48_data, drand48_data);

static void remove_fdir_filter(struct ip_tuple *id);

static inline int handle_to_fg_id(hid_t handle)
{
	return (handle >> 48) & 0xffff;
}

static struct tcpapi_pcb *__handle_to_tcpapi(hid_t handle)
{
	struct mempool *p;
	unsigned long idx = (handle & 0xffffffffffff);
	p = &percpu_get(pcb_mempool);
	return (struct tcpapi_pcb *) mempool_idx_to_ptr(p, idx);
}

/**
 * handle_to_tcpapi - converts a handle to a PCB
 * @handle: the input handle
 *
 * Return a PCB, or NULL if the handle is invalid.
 */
static inline struct tcpapi_pcb *handle_to_tcpapi(hid_t handle, struct eth_fg **new_cur_fg)
{
	struct tcpapi_pcb *api;
	int fg = handle_to_fg_id(handle);

	if (unlikely(fg >= ETH_MAX_TOTAL_FG + NCPU))
		return NULL;

	*new_cur_fg = fgs[fg];
	eth_fg_set_current(fgs[fg]);

	api = __handle_to_tcpapi(handle);
	MEMPOOL_SANITY_ACCESS(api);

	/* check if the handle is actually allocated */
	if (unlikely(api->alive > 1))
		return NULL;

	return api;
}

/**
 * tcpapi_to_handle - converts a PCB to a handle
 * @pcb: the PCB.
 *
 * Returns a handle.
 */
static inline hid_t tcpapi_to_handle(struct eth_fg *cur_fg, struct tcpapi_pcb *pcb)
{
	struct mempool *p = &percpu_get(pcb_mempool);
	MEMPOOL_SANITY_ACCESS(pcb);
	hid_t hid = mempool_ptr_to_idx(p, pcb) | ((uintptr_t)(cur_fg->fg_id) << 48);
	return hid;
}

static void pcb_ready_enqueue(struct tcpapi_pcb *api)
{
	assert(fgs[handle_to_fg_id(api->handle)]->cur_cpu == percpu_get(cpu_id));

	if (api->active_usys_count) {
		api->flags |= PCB_FLAG_READY;
		return;
	}

	queue_push_back(&percpu_get(pcb_ready_queue).queue, &api->ready_queue);
}

static void __tcp_gen_usys(struct tcpapi_pcb *api)
{
	struct mbuf *pkt;
	struct pbuf *p, *pbufs;
	void *id;

	assert(!api->flags);
	assert(!api->active_usys_count);

	log_debug("%lx: __tcp_gen_usys(%lx)\n", api, api->handle);

	if (api->uevents & PCB_UEVENT_KNOCK) {
		id = mempool_pagemem_to_iomap(&percpu_get(id_mempool), api->id);
		log_debug("%lx: usys_tcp_knock(%lx, %lx)\n", api, api->handle, id);
		usys_tcp_knock(api->handle, id);
		api->active_usys_count++;
	}

	if (api->uevents & PCB_UEVENT_CONNECTED) {
		log_debug("%lx: usys_tcp_connected(%lx, %lx, %d)\n", api, api->handle, api->cookie, RET_OK);
		usys_tcp_connected(api->handle, api->cookie, RET_OK);
		api->active_usys_count++;
	}

	api->uevents = 0;

	if (api->len_xmited) {
		log_debug("%lx: usys_tcp_sendv_ret(%lx, %lx, %d)\n", api, api->handle, api->cookie, api->len_xmited);
		usys_tcp_sendv_ret(api->handle, api->cookie, api->len_xmited);
		api->len_xmited = 0;
		api->active_usys_count++;
	}

	if (api->sent_len) {
		log_debug("%lx: usys_tcp_sent(%lx, %lx, %d)\n", api, api->handle, api->cookie, api->sent_len);
		usys_tcp_sent(api->handle, api->cookie, api->sent_len);
		api->sent_len = 0;
		api->active_usys_count++;
	}

	queue_for_each_entry(pbufs, &api->pbuf_for_usys, pbuf_for_usys) {
		p = pbufs;
		/* Walk through the full receive chain */
		do {
			pkt = p->mbuf;
			pkt->len = p->len; /* repurpose len for recv_done */
			log_debug("%lx: usys_tcp_recv(%lx, %lx, %lx, %d)\n", api, api->handle, api->cookie, mbuf_to_iomap(pkt, p->payload), p->len);
			usys_tcp_recv(api->handle, api->cookie, mbuf_to_iomap(pkt, p->payload), p->len);
			api->active_usys_count++;
			p = p->next;
		} while (p);
	}

	queue_clear(&api->pbuf_for_usys);

	if (!api->alive) {
		log_debug("%lx: usys_tcp_dead(%lx, %lx)\n", api, api->handle, api->cookie);
		usys_tcp_dead(api->handle, api->cookie);
		api->active_usys_count++;
	}

	if (api->lasterr.sysnr) {
		usys_ksys_ret(api->lasterr.sysnr, api->lasterr.err, api->cookie);
		api->lasterr.sysnr = 0;
		api->lasterr.err = 0;
		api->active_usys_count++;
	}
}

static bool ksys_is_tcp(const struct bsys_desc *desc)
{
	switch (desc->sysnr) {
	case KSYS_TCP_CONNECT:
	case KSYS_TCP_ACCEPT:
	case KSYS_TCP_REJECT:
	case KSYS_TCP_SEND:
	case KSYS_TCP_SENDV:
	case KSYS_TCP_RECV_DONE:
	case KSYS_TCP_CLOSE:
		return true;
	}

	return false;
}

static bool usys_is_tcp(const struct bsys_desc *desc)
{
	switch (desc->sysnr) {
	case USYS_TCP_CONNECTED:
	case USYS_TCP_KNOCK:
	case USYS_TCP_RECV:
	case USYS_TCP_SENT:
	case USYS_TCP_DEAD:
	case USYS_TCP_SENDV_RET:
		return true;
	}
	return false;
}

static int bsys_tcp_home_id(const struct bsys_desc *desc)
{
   return fgs[handle_to_fg_id(desc->arga)]->cur_cpu;
}

void tcp_route_ksys(struct bsys_desc __user *d, unsigned int nr)
{
	int i, home;
	struct locked_bsys_arr *remote;

	for (i = 0; i < nr; i++) {
		if (!ksys_is_tcp(&d[i]))
			continue;

		home = bsys_tcp_home_id(&d[i]);
		if (home == percpu_get(cpu_id))
			continue;

		log_debug("ksys route to remote %d %lx %lx %lx %lx\n", d[i].sysnr, d[i].arga, d[i].argb, d[i].argc, d[i].argd);

		remote = &percpu_get_remote(ksys_remote, home);
		spin_lock(&remote->lock);
		assert(remote->len < LOCKED_BSYS_MAX_LEN);
		remote->descs[remote->len++] = d[i];
		d[i].sysnr = KSYS_NOP;
		spin_unlock(&remote->lock);
	}
}

static void __tcp_finish_usys(void *_api)
{
	struct tcpapi_pcb *api = (struct tcpapi_pcb *) _api;

	bsys_dispatch_remote();

	spin_lock(&percpu_get(pcb_ready_queue).lock);
	api->active_usys_count--;
	if (!api->active_usys_count && api->flags & PCB_FLAG_CLOSED) {
		mempool_free(&percpu_get(pcb_mempool), api);
	} else if (!api->active_usys_count && api->flags & PCB_FLAG_READY) {
		api->flags &= ~PCB_FLAG_READY;
		pcb_ready_enqueue(api);
	}
	spin_unlock(&percpu_get(pcb_ready_queue).lock);
}

void tcp_finish_usys(void)
{
	int i, home, ret;
	struct tcpapi_pcb *api;
	struct bsys_desc *descs = percpu_get(usys_arr)->descs;
#if CONFIG_RUN_TCP_STACK_IPI
	long now, last;
#endif

	for (i = 0; i < percpu_get(usys_arr)->len; i++) {
		if (!usys_is_tcp(&descs[i]))
			continue;

		api = __handle_to_tcpapi(descs[i].arga);

		home = bsys_tcp_home_id(&descs[i]);
		if (home == percpu_get(cpu_id)) {
			__tcp_finish_usys(api);
		} else {
			ret = cpu_run_on_one(__tcp_finish_usys, api, home);
			assert(!ret);
#if CONFIG_RUN_TCP_STACK_IPI
			/* Send an IPI in case the home core is in userspace */
			now = rdtsc();
			last = percpu_get_remote(last_ipi_time, home);
			if (!last || now - last >= IPI_TIMEOUT) {
				percpu_get_remote(last_ipi_time, home) = now;
				apic_send_ipi(home, RUN_TCP_STACK_IPI_VECTOR);
			}
#endif
		}
	}
}

void tcp_generate_usys(void)
{
	struct pcb_ready_queue *queue;
	struct queue_node *n;
	struct tcpapi_pcb *api;

	queue = &percpu_get(pcb_ready_queue);

	spin_lock(&queue->lock);
	n = queue_pop_front(&queue->queue);
	spin_unlock(&queue->lock);

	if (n) {
		api = container_of(n, struct tcpapi_pcb, ready_queue);
		__tcp_gen_usys(api);
	}
}

#if CONFIG_RUN_TCP_STACK_IPI

static void run_tcp_stack_ipi_handler(struct dune_tf *tf)
{
	char fxsave[512];

	if (percpu_get(in_kernel))
		goto out;

	asm volatile("fxsave %0" : "=m" (fxsave));

	/* Needed so that we process remote ksys */
	cpu_do_bookkeeping();

	eth_process_poll();

	eth_process_recv();

	eth_process_send();

	asm volatile("fxrstor %0" : "=m" (fxsave));

out:
	apic_eoi();
	percpu_get(last_ipi_time) = 0;
}

#endif

#if CONFIG_RUN_TCP_STACK_IPI

static void tcp_steal_ipi_send(void)
{
	int count = 0, cpu_id, i;
	long rnd, last, now;
	struct eth_rx_queue *rxq;
	unsigned char cpus[NCPU];

	now = rdtsc();
	for (i = 0; i < CFG.num_cpus; i++) {
		if (percpu_get_remote(in_kernel, CFG.cpu[i]))
			continue;
		last = percpu_get_remote(last_ipi_time, CFG.cpu[i]);
		if (last && now - last < IPI_TIMEOUT)
			continue;
		rxq = percpu_get_remote(eth_rxqs[0], CFG.cpu[i]);
		if (rxq->ready(rxq))
			cpus[count++] = CFG.cpu[i];
	}

	if (count) {
		lrand48_r(&percpu_get(drand48_data), &rnd);
		cpu_id = cpus[rnd % count];

		percpu_get_remote(last_ipi_time, cpu_id) = now;
		apic_send_ipi(cpu_id, RUN_TCP_STACK_IPI_VECTOR);
	}
}

#endif

void tcp_steal_idle_wait(uint64_t usecs)
{
	int count, cpu_id, i, ok = 0;
	long rnd;
	unsigned char cpus[NCPU];
	unsigned long deadline;
	struct eth_rx_queue *rxq;
	struct pcb_ready_queue *remote_queue;
	struct tcpapi_pcb *api;
	struct queue_node *n;
#if CONFIG_STATS
	int events_before, events_after;
#endif

	deadline = rdtsc() + usecs * cycles_per_us;
	do {
		if (percpu_get(ksys_remote).len)
			return;

		for (i = 0; i < percpu_get(eth_num_queues); i++) {
			rxq = percpu_get(eth_rxqs[i]);
			if(rxq->ready(rxq))
				return;
		}

		count = 0;
		for (i = 0; i < CFG.num_cpus; i++) {
			if (percpu_get_remote(in_kernel, CFG.cpu[i]))
				continue;

			remote_queue = &percpu_get_remote(pcb_ready_queue, CFG.cpu[i]);
			if (queue_front(&remote_queue->queue))
				cpus[count++] = CFG.cpu[i];
		}

		if (count) {
			lrand48_r(&percpu_get(drand48_data), &rnd);
			cpu_id = cpus[rnd % count];

			log_debug("steal attempt from %d\n", cpu_id);
			remote_queue = &percpu_get_remote(pcb_ready_queue, cpu_id);
			if (spin_try_lock(&remote_queue->lock)) {
				n = queue_front(&remote_queue->queue);
				api = container_of(n, struct tcpapi_pcb, ready_queue);
				log_debug("steal from %d %lx\n", cpu_id, api);
				if (n) {
					assert(!api->flags);
					log_debug("steal success from %d %lx\n", cpu_id, api);
					queue_pop_front(&remote_queue->queue);
#if CONFIG_STATS
					events_before = percpu_get(usys_arr)->len;
#endif
					__tcp_gen_usys(api);
#if CONFIG_STATS
					events_after = percpu_get(usys_arr)->len;
#endif
					ok = 1;
				}
				spin_unlock(&remote_queue->lock);
			}

			if (ok) {
#if CONFIG_STATS
				stats_counter_steals(events_after - events_before);
#endif
				return;
			}
		} else {
#if CONFIG_RUN_TCP_STACK_IPI
			tcp_steal_ipi_send();
#endif
		}
		cpu_relax();
	} while (rdtsc() < deadline);
}

static void recv_a_pbuf(struct tcpapi_pcb *api, struct pbuf *p)
{
	MEMPOOL_SANITY_LINK(api, p);

	spin_lock(&percpu_get(pcb_ready_queue).lock);
	queue_push_back(&api->pbuf_for_usys, &p->pbuf_for_usys);
	assert(api->pbuf_for_usys.tail == &p->pbuf_for_usys);
	pcb_ready_enqueue(api);
	spin_unlock(&percpu_get(pcb_ready_queue).lock);
}

void bsys_tcp_accept(hid_t handle, unsigned long cookie)
{
	/*
	 * FIXME: this function is sort of a placeholder since we have no
	 * choice but to have already accepted the connection under LWIP's
	 * synchronous API.
	 */

	struct eth_fg *cur_fg;
	struct tcpapi_pcb *api = handle_to_tcpapi(handle, &cur_fg);
	struct pbuf *tmp;

	KSTATS_VECTOR(bsys_tcp_accept);

	log_debug("tcpapi: bsys_tcp_accept() - handle %lx, cookie %lx\n",
		  handle, cookie);

	if (unlikely(!api)) {
		log_debug("tcpapi: invalid handle\n");
		usys_ksys_ret(KSYS_TCP_ACCEPT, -RET_BADH, 0);
		return;
	}

	if (api->id) {
		mempool_free(&percpu_get(id_mempool), api->id);
		api->id = NULL;
	}

	api->cookie = cookie;
	api->accepted = true;

	tmp = api->recvd;
	while (tmp) {
		recv_a_pbuf(api, tmp);
		tmp = tmp->tcp_api_next;
	}
}

void bsys_tcp_reject(hid_t handle)
{
	/*
	 * FIXME: LWIP's synchronous handling of accepts
	 * makes supporting this call impossible.
	 */

	KSTATS_VECTOR(bsys_tcp_reject);

	panic("tcpapi: bsys_tcp_reject() is not implemented\n");
}

void bsys_tcp_send(hid_t handle, void *addr, size_t len)
{
	KSTATS_VECTOR(bsys_tcp_send);

	log_debug("tcpapi: bsys_tcp_send() - addr %p, len %lx\n",
		  addr, len);

	panic("tcpapi: bsys_tcp_send() is not implemented\n");
}

void bsys_tcp_sendv(hid_t handle, struct sg_entry __user *ents,
		       unsigned int nrents)
{
	struct eth_fg *cur_fg;
	struct tcpapi_pcb *api = handle_to_tcpapi(handle, &cur_fg);
	int i;
	size_t len_xmited = 0;

	KSTATS_VECTOR(bsys_tcp_sendv);

	log_debug("tcpapi: bsys_tcp_sendv() - handle %lx, ents %p, nrents %ld\n",
		  handle, ents, nrents);

	if (unlikely(!api)) {
		log_debug("tcpapi: invalid handle\n");
		usys_ksys_ret(KSYS_TCP_SENDV, -RET_BADH, 0);
		return;
	}

	if (unlikely(!api->alive)) {
		api->lasterr.sysnr = KSYS_TCP_SENDV;
		api->lasterr.err = -RET_CLOSED;
		pcb_ready_enqueue(api);
		return;
	}

	if (unlikely(!uaccess_okay(ents, nrents * sizeof(struct sg_entry)))) {
		api->lasterr.sysnr = KSYS_TCP_SENDV;
		api->lasterr.err = -RET_FAULT;
		pcb_ready_enqueue(api);
		return;
	}

	nrents = min(nrents, MAX_SG_ENTRIES);
	for (i = 0; i < nrents; i++) {
		err_t err;
		void *base = (void *) uaccess_peekq((uint64_t *) &ents[i].base);
		size_t len = uaccess_peekq(&ents[i].len);
		bool buf_full = len > min(api->pcb->snd_buf, 0xFFFF);

		if (unlikely(!uaccess_okay(base, len)))
			break;

		/*
		 * FIXME: hacks to deal with LWIP's send buffering
		 * design when handling large send requests. LWIP
		 * buffers send data but in IX we don't want any
		 * buffering in the kernel at all. Thus, the real
		 * limit here should be the TCP cwd. Unfortunately
		 * tcp_out.c needs to be completely rewritten to
		 * support this.
		 */
		if (buf_full)
			len = min(api->pcb->snd_buf, 0xFFFF);
		if (!len)
			break;

		/*
		 * FIXME: Unfortunately LWIP's TX path is compeletely
		 * broken in terms of zero-copy. It's also somewhat
		 * broken in terms of large write requests. Here's a
		 * hacky placeholder until we can rewrite this path.
		 */
		err = tcp_write(api->pcb, base, len, 0);
		if (err != ERR_OK)
			break;

		len_xmited += len;
		if (buf_full)
			break;
	}

	if (len_xmited) {
		tcp_output(cur_fg, api->pcb);
		spin_lock(&percpu_get(pcb_ready_queue).lock);
		api->len_xmited += len_xmited;
		pcb_ready_enqueue(api);
		spin_unlock(&percpu_get(pcb_ready_queue).lock);
	}
}

void bsys_tcp_recv_done(hid_t handle, size_t len)
{
	struct eth_fg *cur_fg;
	struct tcpapi_pcb *api = handle_to_tcpapi(handle, &cur_fg);
	struct pbuf *recvd, *next;

	KSTATS_VECTOR(bsys_tcp_recv_done);

	log_debug("tcpapi: bsys_tcp_recv_done - handle %lx, len %ld\n",
		  handle, len);

	if (unlikely(!api)) {
		log_debug("tcpapi: invalid handle\n");
		usys_ksys_ret(KSYS_TCP_RECV_DONE, -RET_BADH, 0);
		return;
	}

	recvd = api->recvd;

	if (api->pcb)
		tcp_recved(cur_fg, api->pcb, len);
	while (recvd) {
		if (len < recvd->len)
			break;

		len -= recvd->len;
		next = recvd->tcp_api_next;
		pbuf_free(recvd);
		recvd = next;
	}

	api->recvd = recvd;
}

void bsys_tcp_close(hid_t handle)
{
	struct eth_fg *cur_fg;
	struct tcpapi_pcb *api = handle_to_tcpapi(handle, &cur_fg);
	struct pbuf *recvd, *next;

	KSTATS_VECTOR(bsys_tcp_close);

	log_debug("tcpapi: bsys_tcp_close - handle %lx\n", handle);

	if (unlikely(!api)) {
		log_debug("tcpapi: invalid handle\n");
		usys_ksys_ret(KSYS_TCP_CLOSE, -RET_BADH, 0);
		return;
	}

	if (api->pcb) {
		tcp_close_with_reset(cur_fg, api->pcb);
	}

	recvd = api->recvd;
	while (recvd) {
		next = recvd->tcp_api_next;
		pbuf_free(recvd);
		recvd = next;
	}

	if (api->id) {
		remove_fdir_filter(api->id);
		mempool_free(&percpu_get(id_mempool), api->id);
	}

	if (api->active_usys_count)
		api->flags |= PCB_FLAG_CLOSED;
	else
		mempool_free(&percpu_get(pcb_mempool), api);
}

#if CONFIG_PRINT_CONNECTION_COUNT

static void __print_conn(struct timer *t, struct eth_fg *cur_fg)
{
	log_info("open connections = %d\n", percpu_get(open_connections));
}

static void print_conn(int change)
{
	percpu_get(open_connections) += change;
	timer_mod(&percpu_get(print_conn_timer), NULL, 1 * ONE_SECOND);
}

#endif

static void mark_dead(struct tcpapi_pcb *api, unsigned long cookie)
{
#if CONFIG_PRINT_CONNECTION_COUNT
	print_conn(-1);
#endif

	if (!api) {
		usys_tcp_dead(0, cookie);
		return;
	}

	if (api->id)
		remove_fdir_filter(api->id);

	spin_lock(&percpu_get(pcb_ready_queue).lock);
	api->alive = false;
	pcb_ready_enqueue(api);
	spin_unlock(&percpu_get(pcb_ready_queue).lock);
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
	struct tcpapi_pcb *api;

	log_debug("tcpapi: on_recv - arg %p, pcb %p, pbuf %p, err %d\n",
		  arg, pcb, p, err);

	api = (struct tcpapi_pcb *) arg;

	/* FIXME: It's not really clear what to do with "err" */

	/* Was the connection closed? */
	if (!p) {
		mark_dead(api, api->cookie);
		return ERR_OK;
	}

	if (!api->recvd) {
		api->recvd = p;
		api->recvd_tail = p;
	} else {
		api->recvd_tail->tcp_api_next = p;
		api->recvd_tail = p;
	}
	p->tcp_api_next = NULL;

	/*
	 * FIXME: This is a pretty annoying hack. LWIP accepts connections
	 * synchronously while we have to wait for the app to accept the
	 * connection. As a result, we have no choice but to assume the
	 * connection will be accepted. Thus, we may start receiving data
	 * packets before the app has allocated a recieve context and set
	 * the appropriate cookie value. For now we wait for the app to
	 * accept the connection before we allow receive events to be
	 * sent. Clearly, the receive path needs to be rewritten.
	 */
	if (!api->accepted)
		goto done;

	recv_a_pbuf(api, p);

done:
	return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
	struct tcpapi_pcb *api;
	unsigned long cookie;

	log_debug("tcpapi: on_err - arg %p err %d\n", arg, err);

	/* Because we use the LWIP_EVENT_API, LWIP can invoke on_err before we
	 * invoke tcp_arg, thus arg will be NULL. This happens, e.g., if we
	 * receive a RST after sending a SYN+ACK. */
	if (!arg)
		return;

	api = (struct tcpapi_pcb *) arg;
	cookie = api->cookie;

	if (err == ERR_ABRT || err == ERR_RST || err == ERR_CLSD) {
		mark_dead(api, cookie);
		api->pcb = NULL;
	}
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
	struct tcpapi_pcb *api;

	log_debug("tcpapi: on_sent - arg %p, pcb %p, len %hd\n",
		  arg, pcb, len);

	api = (struct tcpapi_pcb *) arg;
	spin_lock(&percpu_get(pcb_ready_queue).lock);
	api->sent_len += len;
	pcb_ready_enqueue(api);
	spin_unlock(&percpu_get(pcb_ready_queue).lock);

	return ERR_OK;
}

static err_t on_accept(struct eth_fg *cur_fg, void *arg, struct tcp_pcb *pcb, err_t err)
{
	struct tcpapi_pcb *api;
	struct ip_tuple *id;
	hid_t handle;


	log_debug("tcpapi: on_accept - arg %p, pcb %p, err %d\n",
		  arg, pcb, err);

	api = mempool_alloc(&percpu_get(pcb_mempool));
	if (unlikely(!api))
		return ERR_MEM;
	id = mempool_alloc(&percpu_get(id_mempool));
	if (unlikely(!id)) {
		mempool_free(&percpu_get(pcb_mempool), api);
		return ERR_MEM;
	}

	api->pcb = pcb;
	api->alive = true;
	api->cookie = 0;
	api->recvd = NULL;
	api->recvd_tail = NULL;
	api->accepted = false;
	api->sent_len = 0;
	api->len_xmited = 0;
	init_queue(&api->pbuf_for_usys);
	init_queue_node(&api->ready_queue);
	api->active_usys_count = 0;
	api->uevents = 0;
	api->flags = 0;

	tcp_nagle_disable(pcb);
	tcp_arg(pcb, api);

#if  LWIP_CALLBACK_API
	tcp_recv(pcb, on_recv);
	tcp_err(pcb, on_err);
	tcp_sent(pcb, on_sent);
#endif

	id->src_ip = ntoh32(pcb->remote_ip.addr);
	id->dst_ip = CFG.host_addr.addr;
	id->src_port = pcb->remote_port;
	id->dst_port = pcb->local_port;
	api->id = id;
	handle = tcpapi_to_handle(cur_fg, api);
	api->handle = handle;

#if CONFIG_PRINT_CONNECTION_COUNT
	print_conn(1);
#endif

	spin_lock(&percpu_get(pcb_ready_queue).lock);
	api->uevents |= PCB_UEVENT_KNOCK;
	pcb_ready_enqueue(api);
	spin_unlock(&percpu_get(pcb_ready_queue).lock);

	return ERR_OK;
}

static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
	struct tcpapi_pcb *api = (struct tcpapi_pcb *) arg;

	if (err != ERR_OK) {
		log_err("tcpapi: connection failed, ret %d\n", err);
		/* FIXME: free memory and mark handle dead */
		usys_tcp_connected(api->handle, api->cookie, RET_CONNREFUSED);
		return err;
	}

	spin_lock(&percpu_get(pcb_ready_queue).lock);
	api->uevents |= PCB_UEVENT_CONNECTED;
	pcb_ready_enqueue(api);
	spin_unlock(&percpu_get(pcb_ready_queue).lock);

	return ERR_OK;
}



/**
 * lwip_tcp_event -- "callback from the LWIP library
 */

err_t
lwip_tcp_event(struct eth_fg *cur_fg, void *arg, struct tcp_pcb *pcb,
	       enum lwip_event event,
	       struct pbuf *p,
	       u16_t size,
	       err_t err)
{
	switch (event) {
	case LWIP_EVENT_ACCEPT:
		return on_accept(cur_fg, arg, pcb, err);
		break;
	case LWIP_EVENT_SENT:
		return on_sent(arg, pcb, size);
		break;
	case LWIP_EVENT_RECV:
		return on_recv(arg, pcb, p, err);
		break;
	case LWIP_EVENT_CONNECTED:
		return on_connected(arg, pcb, err);
		break;
	case LWIP_EVENT_ERR:
		on_err(arg, err);
		return 0;
		break;

	case LWIP_EVENT_POLL:
		return ERR_OK;
	default:
		assert(0);
	}
	return ERR_OK;

}

/* FIXME: we should maintain a bitmap to hold the available TCP ports */

/* FIXME:
   -- this is totally broken with flow-group migration.  The match should be based on a matching fgid for that device
   -- for multi-device bonds, need to also figure out (and reverse) the L3+L4 bond that is in place.
   -- performance will be an issue as well with 1/128 probability of success (from 1/16).

   -- short version: need to fix this by using flow director for all outbound connections.  (EdB 2014-11-17)
*/

static uint32_t compute_toeplitz_hash(const uint8_t *key, uint32_t src_addr, uint32_t dst_addr, uint16_t src_port, uint16_t dst_port)
{
	int i, j;
	uint8_t input[12];
	uint32_t result = 0;
	uint32_t key_part = htonl(((uint32_t *)key)[0]);

	memcpy(&input[0], &src_addr, 4);
	memcpy(&input[4], &dst_addr, 4);
	memcpy(&input[8], &src_port, 2);
	memcpy(&input[10], &dst_port, 2);

	for (i = 0; i < 12; i++) {
		for (j = 128; j; j >>= 1) {
			if (input[i] & j)
				result ^= key_part;
			key_part <<= 1;
			if (key[i + 4] & j)
				key_part |= 1;
		}
	}

	return result;
}

static void remove_fdir_filter(struct ip_tuple *id)
{
	struct rte_fdir_filter fdir_ftr;
	struct ix_rte_eth_dev *dev;

	fdir_ftr.iptype = RTE_FDIR_IPTYPE_IPV4;
	fdir_ftr.l4type = RTE_FDIR_L4TYPE_TCP;
	fdir_ftr.ip_src.ipv4_addr = id->dst_ip;
	fdir_ftr.ip_dst.ipv4_addr = id->src_ip;
	fdir_ftr.port_src = id->dst_port;
	fdir_ftr.port_dst = id->src_port;
	dev = percpu_get(eth_rxqs[0])->dev;
	dev->dev_ops->fdir_remove_perfect_filter(dev, &fdir_ftr, 0);
}

static struct eth_fg *get_port_with_fdir(struct ip_tuple *id)
{
	int ret;
	struct rte_fdir_filter fdir_ftr;
	struct ix_rte_eth_dev *dev;
	struct eth_rx_queue *queue;

	fdir_ftr.iptype = RTE_FDIR_IPTYPE_IPV4;
	fdir_ftr.l4type = RTE_FDIR_L4TYPE_TCP;
	fdir_ftr.ip_src.ipv4_addr = id->dst_ip;
	fdir_ftr.ip_dst.ipv4_addr = id->src_ip;
	fdir_ftr.port_src = id->dst_port;
	fdir_ftr.port_dst = id->src_port;

	queue = percpu_get(eth_rxqs[0]);
	dev = queue->dev;

	ret = dev->dev_ops->fdir_add_perfect_filter(dev, &fdir_ftr, 0, queue->queue_idx, 0);
	if (ret < 0)
		return NULL;

	eth_fg_set_current(outbound_fg());
	return outbound_fg();
}

struct eth_fg *get_local_port_and_set_queue(struct ip_tuple *id)
{
	int ret;
	uint32_t hash;
	uint32_t fg_idx;
	struct eth_fg *fg;
	struct ix_rte_eth_dev *dev;
	struct ix_rte_eth_rss_conf rss_conf;

	if (eth_dev_count > 1)
		panic("tcp_connect not implemented for bonded interfaces\n");

	if (!percpu_get(local_port))
		percpu_get(local_port) = percpu_get(cpu_id) * PORTS_PER_CPU;

	percpu_get(local_port)++;
	id->src_port = percpu_get(local_port);

	fg = get_port_with_fdir(id);
	if (fg)
		return fg;

	dev = percpu_get(eth_rxqs[0])->dev;
	ret = dev->dev_ops->rss_hash_conf_get(dev, &rss_conf);
	if (ret < 0)
		return NULL;

	while (1) {
		if (percpu_get(local_port) >= (percpu_get(cpu_id) + 1) * PORTS_PER_CPU)
			percpu_get(local_port) = percpu_get(cpu_id) * PORTS_PER_CPU + 1;
		hash = compute_toeplitz_hash(rss_conf.rss_key, htonl(id->dst_ip), htonl(id->src_ip), htons(id->dst_port), htons(id->src_port));
		fg_idx = hash & (dev->data->nb_rx_fgs - 1);
		if (percpu_get(eth_rxqs[0])->dev->data->rx_fgs[fg_idx].cur_cpu == percpu_get(cpu_id)) {
			//set_current_queue(percpu_get(eth_rxqs)[0]);

			// this will fail with eth_dev_count >1
			assert(&percpu_get(eth_rxqs[0])->dev->data->rx_fgs[fg_idx] == fgs[fg_idx]);
			eth_fg_set_current(&percpu_get(eth_rxqs[0])->dev->data->rx_fgs[fg_idx]);

			return fgs[fg_idx];
		}
		percpu_get(local_port)++;
		id->src_port = percpu_get(local_port);
	}

	return 0;
}

void bsys_tcp_connect(struct ip_tuple __user *id, unsigned long cookie)
{
	err_t err;
	struct ip_tuple tmp;
	struct ip_addr addr;
	struct tcp_pcb *pcb;
	struct tcpapi_pcb *api;

	KSTATS_VECTOR(bsys_tcp_connect);

	log_debug("tcpapi: bsys_tcp_connect() - id %p, cookie %lx\n",
		  id, cookie);

	if (unlikely(copy_from_user(id, &tmp, sizeof(struct ip_tuple)))) {
		usys_ksys_ret(KSYS_TCP_CONNECT, -RET_FAULT, 0);
		return;
	}

	tmp.src_ip = CFG.host_addr.addr;

	struct eth_fg *cur_fg = get_local_port_and_set_queue(&tmp);
	if (unlikely(!cur_fg)) {
		usys_ksys_ret(KSYS_TCP_CONNECT, -RET_FAULT, 0);
		return;
	}


	pcb = tcp_new(cur_fg);
	if (unlikely(!pcb))
		goto pcb_fail;
	tcp_nagle_disable(pcb);

	api = mempool_alloc(&percpu_get(pcb_mempool));
	if (unlikely(!api)) {
		goto connect_fail;
	}

	api->pcb = pcb;
	api->alive = true;
	api->cookie = cookie;
	api->recvd = NULL;
	api->recvd_tail = NULL;
	api->accepted = true;
	api->sent_len = 0;
	api->len_xmited = 0;
	init_queue(&api->pbuf_for_usys);
	init_queue_node(&api->ready_queue);
	api->active_usys_count = 0;
	api->uevents = 0;
	api->flags = 0;

	tcp_arg(pcb, api);

	api->handle = tcpapi_to_handle(cur_fg, api);

#if  LWIP_CALLBACK_API
	tcp_recv(pcb, on_recv);
	tcp_err(pcb, on_err);
	tcp_sent(pcb, on_sent);
#endif

	addr.addr = hton32(tmp.src_ip);

	err = tcp_bind(cur_fg, pcb, &addr, tmp.src_port);
	if (unlikely(err != ERR_OK))
		goto connect_fail;

	addr.addr = hton32(tmp.dst_ip);

	err = tcp_connect(cur_fg, pcb, &addr, tmp.dst_port, on_connected);
	if (unlikely(err != ERR_OK))
		goto connect_fail;

	usys_ksys_ret(KSYS_TCP_CONNECT, api->handle, api->cookie);
	return;

connect_fail:
	tcp_abort(cur_fg, pcb);
pcb_fail:

	usys_ksys_ret(KSYS_TCP_CONNECT, -RET_NOMEM, 0);
}



/* derived from ip_output_hinted; a mess because of conflicts between LWIP and IX */
extern int arp_lookup_mac(struct ip_addr *addr, struct eth_addr *mac);

int tcp_output_packet(struct eth_fg *cur_fg, struct tcp_pcb *pcb, struct pbuf *p)
{
	int ret;
	struct mbuf *pkt;
	struct eth_hdr *ethhdr;
	struct ip_hdr *iphdr;
	unsigned char *payload;
	struct pbuf *curp;
	struct ip_addr dst_addr;

	pkt = mbuf_alloc_local();
	if (unlikely(!pkt))
		return -ENOMEM;

	ethhdr = mbuf_mtod(pkt, struct eth_hdr *);
	iphdr = mbuf_nextd(ethhdr, struct ip_hdr *);
	payload = mbuf_nextd(iphdr, unsigned char *);

	dst_addr.addr = ntoh32(pcb->remote_ip.addr);

	/* setup IP hdr */
	IPH_VHL_SET(iphdr, 4, sizeof(struct ip_hdr) / 4);
	//iphdr->header_len = sizeof(struct ip_hdr) / 4;
	//iphdr->version = 4;
	iphdr->_len = hton16(sizeof(struct ip_hdr) + p->tot_len);
	iphdr->_id = 0;
	iphdr->_offset = 0;
	iphdr->_proto = IP_PROTO_TCP;
	iphdr->_chksum = 0;
	iphdr->_tos = pcb->tos;
	iphdr->_ttl = pcb->ttl;
	iphdr->src.addr = pcb->local_ip.addr;
	iphdr->dest.addr = pcb->remote_ip.addr;

	for (curp = p; curp; curp = curp->next) {
		memcpy(payload, curp->payload, curp->len);
		payload += curp->len;
	}

	/* Offload IP and TCP tx checksums */
	pkt->ol_flags = PKT_TX_IP_CKSUM;
	pkt->ol_flags |= PKT_TX_TCP_CKSUM;

	ret = ip_send_one(cur_fg, &dst_addr, pkt, sizeof(struct eth_hdr) +
			  sizeof(struct ip_hdr) + p->tot_len);
	if (unlikely(ret)) {
		mbuf_free(pkt);
		return -EIO;
	}

	return 0;
}


int tcp_api_init(void)
{
	int ret;
	ret = mempool_create_datastore(&pcb_datastore, MAX_PCBS,
				       sizeof(struct tcpapi_pcb), 0, MEMPOOL_DEFAULT_CHUNKSIZE, "pcb");
	if (ret)
		return ret;

	ret = mempool_create_datastore(&id_datastore, MAX_PCBS,
				       sizeof(struct ip_tuple), 1, MEMPOOL_DEFAULT_CHUNKSIZE, "ip");
	if (ret)
		return ret;

	ret = mempool_pagemem_map_to_user(&id_datastore);
	return ret;
}


int tcp_api_init_cpu(void)
{
	int ret;
	ret = mempool_create(&percpu_get(pcb_mempool), &pcb_datastore, MEMPOOL_SANITY_PERCPU, percpu_get(cpu_id));
	if (ret)
		return ret;

	ret = mempool_create(&percpu_get(id_mempool), &id_datastore, MEMPOOL_SANITY_PERCPU, percpu_get(cpu_id));
	if (ret)
		return ret;

	if (CFG.num_ports == 0) {
		ret = tcp_listen_with_backlog(&percpu_get(listen_ports[0]), TCP_DEFAULT_LISTEN_BACKLOG, IP_ADDR_ANY, DEFAULT_PORT);
		if (ret)
			return ret;
	} else {
		int i;
		for (i = 0; i < CFG.num_ports; i++) {
			ret = tcp_listen_with_backlog(&percpu_get(listen_ports[i]), TCP_DEFAULT_LISTEN_BACKLOG, IP_ADDR_ANY, CFG.ports[i]);
			if (ret)
				return ret;
		}
	}

//	percpu_get(port8000).accept = on_accept;

#if CONFIG_PRINT_CONNECTION_COUNT
	timer_init_entry(&percpu_get(print_conn_timer), __print_conn);
#endif

	srand48_r(rdtsc(), &percpu_get(drand48_data));

#if CONFIG_RUN_TCP_STACK_IPI
	dune_register_intr_handler(RUN_TCP_STACK_IPI_VECTOR, run_tcp_stack_ipi_handler);
	dune_control_guest_ints(true);
#endif

	return 0;
}

int tcp_api_init_fg(void)
{

	return 0;
}


