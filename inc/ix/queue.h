#pragma once

struct queue_node {
	struct queue_node *next;
};

struct queue {
	struct queue_node *head;
	struct queue_node *tail;
};

static inline void init_queue(struct queue *q)
{
	q->head = NULL;
	q->tail = NULL;
}

static inline void init_queue_node(struct queue_node *n)
{
	n->next = NULL;
}

static inline bool queue_contains(const struct queue *q, const struct queue_node *n)
{
	return n->next || q->tail == n;
}

static inline void queue_push_back(struct queue *q, struct queue_node *n)
{
	if (queue_contains(q, n))
		return;

	if (!q->head) {
		q->head = n;
		q->tail = n;
	} else {
		q->tail->next = n;
		q->tail = n;
	}
}

static inline struct queue_node *queue_front(const struct queue *q)
{
	return q->head;
}

static inline struct queue_node *queue_pop_front(struct queue *q)
{
	struct queue_node *n = queue_front(q);
	if (!n)
		return NULL;

	q->head = q->head->next;
	n->next = NULL;
	if (!q->head)
		q->tail = NULL;
	return n;
}

static inline void queue_clear(struct queue *q)
{
	while (queue_pop_front(q))
		;
}

#define queue_for_each_entry(pos, queue, member)                             \
        for (pos = container_of((queue)->head, typeof(*pos), member);        \
             &pos->member;                                                   \
             pos = container_of((pos)->member.next, typeof(*(pos)), member))
