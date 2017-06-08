#pragma once

void tcp_route_ksys(struct bsys_desc __user *d, unsigned int nr);
void tcp_finish_usys(void);
void tcp_generate_usys(void);
void tcp_steal_idle_wait(uint64_t usecs);
