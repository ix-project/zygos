#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <ix/debug_desc.h>
#include <ix/lock.h>
// #include <ix/debug.h>

#if LOG

#if LOG_TO_STDOUT
static spinlock_t log_desc_lock;
#elif LOG_TO_SHMEM
static DEFINE_PERCPU(char *, ix_log[2]);
static DEFINE_PERCPU(int, ix_log_ofs[2]);
DEFINE_PERCPU(int, ix_log_context);
#define IX_LOG_SIZE (1<<26)
#endif

#include <ix/mbuf.h>
#include <stdarg.h>
#include <stdio.h>

#if LOG_DESC

static char *usys_names[] = {"USYS_UDP_RECV", "USYS_UDP_SENT", "USYS_TCP_CONNECTED", "USYS_TCP_KNOCK", "USYS_TCP_RECV", "USYS_TCP_SENT", "USYS_TCP_DEAD", "USYS_TIMER", "USYS_TCP_SENDV_RET"};
static int usys_params[] = {3,1,3,2,4,3,2,1,4};
static char *ksys_names[] = {"KSYS_UDP_SEND", "KSYS_UDP_SENDV", "KSYS_UDP_RECV_DONE", "KSYS_TCP_CONNECT", "KSYS_TCP_ACCEPT", "KSYS_TCP_REJECT", "KSYS_TCP_SEND", "KSYS_TCP_SENDV", "KSYS_TCP_RECV_DONE", "KSYS_TCP_CLOSE", "KSYS_NOP"};
static int ksys_params[] = {4,4,1,2,2,1,3,3,2,1,4};

#endif

DEFINE_PERCPU(int, poll_iteration);

static void myvprintf(const char *format, va_list ap)
{
#if LOG_TO_STDOUT
#if LOG_LOCK
	spin_lock(&log_lock);
#endif
	vprintf(format, ap);
#if LOG_LOCK
	spin_unlock(&log_lock);
#endif
#elif LOG_TO_SHMEM
	int ret;
	int ctx = percpu_get(ix_log_context);

	ret = vsprintf(percpu_get(ix_log)[ctx] + percpu_get(ix_log_ofs)[ctx], format, ap);
	percpu_get(ix_log_ofs)[ctx] += ret;

	if (percpu_get(ix_log_ofs)[ctx] > IX_LOG_SIZE - 4096)
		percpu_get(ix_log_ofs)[ctx] = 0;
#else
#error select LOG_TO_STDOUT or LOG_TO_SHMEM
#endif
}

static void myprintf(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	myvprintf(format, ap);
	va_end(ap);
}

#if LOG_DESC

void log_desc(const char *msg, int idx, bool usys, bool ret, const struct bsys_desc *desc)
{
	char **names = usys ? usys_names : ksys_names;
	int *count = usys ? usys_params : ksys_params;
	int c;

	// if (usys && desc->sysnr == USYS_FAKE)
		// return;

#if LOG_TO_STDOUT
	spin_lock(&log_desc_lock);
#endif
	if (ret)
		c = 4;
	else
		c = count[desc->sysnr];
	switch(c) {
	case 1:
		myprintf("%ld: %d: %d: %s: %s%s[%d](%s %lx) "            , rdtsc(), percpu_get(cpu_id), percpu_get(poll_iteration), msg, usys ? "usys" : "ksys", ret ? "_ret" : "", idx, names[desc->sysnr], desc->arga);
		break;
	case 2:
		myprintf("%ld: %d: %d: %s: %s%s[%d](%s %lx %lx) "        , rdtsc(), percpu_get(cpu_id), percpu_get(poll_iteration), msg, usys ? "usys" : "ksys", ret ? "_ret" : "", idx, names[desc->sysnr], desc->arga, desc->argb);
		break;
	case 3:
		myprintf("%ld: %d: %d: %s: %s%s[%d](%s %lx %lx %lx) "    , rdtsc(), percpu_get(cpu_id), percpu_get(poll_iteration), msg, usys ? "usys" : "ksys", ret ? "_ret" : "", idx, names[desc->sysnr], desc->arga, desc->argb, desc->argc);
		break;
	case 4:
		myprintf("%ld: %d: %d: %s: %s%s[%d](%s %lx %lx %lx %lx) ", rdtsc(), percpu_get(cpu_id), percpu_get(poll_iteration), msg, usys ? "usys" : "ksys", ret ? "_ret" : "", idx, names[desc->sysnr], desc->arga, desc->argb, desc->argc, desc->argd);
		break;
	}
	if (usys && !ret && desc->sysnr == USYS_TCP_KNOCK) {
		 struct ip_tuple *ip = (struct ip_tuple *) desc->argb;
		 myprintf("%x:%d %x:%d", ip->src_ip, ip->src_port, ip->dst_ip, ip->dst_port);
	} else if (usys && !ret && desc->sysnr == USYS_TCP_RECV) {
		 unsigned char *addr = iomap_to_mbuf(&percpu_get(mbuf_mempool), (void *) desc->argc);
		 myprintf("%lx %p %d %.*s", desc->argc, addr, desc->argd, desc->argd, addr);
		 // myprintf("%lx %p %d", desc->argc, addr, desc->argd);
		 // for (int i=0;i<desc->argd;i++)
			 // if (addr[i] >= 0x20 && addr[i] <= 0x7f)
				// myprintf("%c", addr[i]);
			 // else
				// myprintf("\\x%02x", addr[i]);
	} else if (!usys && !ret && desc->sysnr == KSYS_TCP_SENDV) {
		 struct sg_entry *ents = (struct sg_entry*) desc->argb;
		 myprintf("%p %d %.*s", ents[0].base, ents[0].len, ents[0].len, ents[0].base);
	}
	myprintf("\n");
#if LOG_TO_STDOUT
	spin_unlock(&log_desc_lock);
#endif
}

#endif

void log_desc_msg(const char *format, ...)
{
	va_list ap;

#if LOG_TO_STDOUT
	spin_lock(&log_desc_lock);
#endif
	myprintf("%ld: %d: %d: ", rdtsc(), percpu_get(cpu_id), percpu_get(poll_iteration));
	va_start(ap, format);
	myvprintf(format, ap);
	va_end(ap);
#if LOG_TO_STDOUT
	spin_unlock(&log_desc_lock);
#endif
}

#if LOG && LOG_TO_SHMEM
void open_shm(const char *name, void **buf, size_t size)
{
	char filename[64];
	int fd, ret;

	sprintf(filename, "/%s.%d", name, percpu_get(cpu_id));
	fd = shm_open(filename, O_RDWR | O_CREAT | O_TRUNC, 0660);
	assert(fd != -1);
	ret = ftruncate(fd, size);
	assert(!ret);
	*buf = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	assert(*buf != MAP_FAILED);
	bzero(*buf, size);
}
#endif

void debug_desc_init(void)
{
#if LOG && LOG_TO_SHMEM
	open_shm("ix-log", (void **) &percpu_get(ix_log)[0], IX_LOG_SIZE);
	open_shm("ix-log-int", (void **) &percpu_get(ix_log)[1], IX_LOG_SIZE);
#endif
}

#endif
