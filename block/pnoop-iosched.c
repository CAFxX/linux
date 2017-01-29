/*
 * elevator pnoop
 */
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/ioprio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/kernel.h>

#define PNOOP_NUM_QUEUES_RT 8
#define PNOOP_NUM_QUEUES_BE 7
#define PNOOP_NUM_QUEUES_IDLE 1
#define PNOOP_NUM_QUEUES (PNOOP_NUM_QUEUES_RT+PNOOP_NUM_QUEUES_BE+PNOOP_NUM_QUEUES_IDLE)
#define PNOOP_QUEUE_BE PNOOP_NUM_QUEUES_RT
#define PNOOP_QUEUE_IDLE PNOOP_QUEUE_BE+PNOOP_NUM_QUEUES_BE

struct pnoop_data {
	struct list_head queues[PNOOP_NUM_QUEUES];
	unsigned state;
};

static unsigned
pnoop_rand(struct pnoop_data *nd) {
	return (nd->state = nd->state * 33797 + 1) >> 8;
}

static int
pnoop_stochastic_prio_be(struct pnoop_data *nd, unsigned prio) {
	unsigned mask = ( 2 << clamp(prio, 0UL, 7UL) ) - 1;
	for (i=0; i<PNOOP_NUM_QUEUES_BE-1; i++)
		if (pnoop_rand(nd) & mask == 0)
			return i;
	return PNOOP_NUM_QUEUES_BE-1;
}

static struct list_head *
pnoop_queue_for_request(struct request_queue *q, struct request *rq)
{
	struct pnoop_data *nd = q->elevator->elevator_data;
	unsigned short rq_class = IOPRIO_PRIO_CLASS(req_get_ioprio(rq));
	unsigned short rq_prio = IOPRIO_PRIO_DATA(req_get_ioprio(rq));

	switch (rq_class) {
	case IOPRIO_CLASS_RT:
		return &nd->queues[PNOOP_QUEUE_RT+clamp(rq_prio, 0, PNOOP_NUM_QUEUES_RT-1)];
	case IOPRIO_CLASS_BE:
		return &nd->queues[PNOOP_QUEUE_BE+pnoop_stochastic_prio_be(nd, rq_prio)];
	default:
		return &nd->queues[PNOOP_QUEUE_BE+pnoop_stochastic_prio_be(nd, 4)];
	case IOPRIO_CLASS_IDLE:
		return &nd->queues[PNOOP_QUEUE_IDLE];
	}
}

static void pnoop_merged_requests(struct request_queue *q, struct request *rq,
				 struct request *next)
{
	list_del_init(&next->queuelist);
}

static int pnoop_dispatch(struct request_queue *q, int force)
{
	struct pnoop_data *nd = q->elevator->elevator_data;
	struct request *rq = NULL;
	int i;

	for (i=0; i<PNOOP_NUM_QUEUES && !rq; i++)
		rq = list_first_entry_or_null(&nd->queues[i], struct request, 
						queuelist);

	if (rq) {
		list_del_init(&rq->queuelist);
		elv_dispatch_sort(q, rq);
		return 1;
	}
	return 0;
}

static void pnoop_add_request(struct request_queue *q, struct request *rq)
{
	list_add_tail(&rq->queuelist, pnoop_queue_for_request(q, rq));
}

static struct request *
pnoop_former_request(struct request_queue *q, struct request *rq)
{
	if (rq->queuelist.prev == pnoop_queue_for_request(q, rq))
		return NULL;
	return list_prev_entry(rq, queuelist);
}

static struct request *
pnoop_latter_request(struct request_queue *q, struct request *rq)
{
	if (rq->queuelist.next == pnoop_queue_for_request(q, rq))
		return NULL;
	return list_next_entry(rq, queuelist);
}

static int pnoop_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct pnoop_data *nd;
	struct elevator_queue *eq;
	int i;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	nd = kmalloc_node(sizeof(*nd), GFP_KERNEL, q->node);
	if (!nd) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}
	eq->elevator_data = nd;

	for (i=0; i<PNOOP_NUM_QUEUES; i++)
		INIT_LIST_HEAD(&nd->queues[i]);
	nd->state = 0;

	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);
	return 0;
}

static void pnoop_exit_queue(struct elevator_queue *e)
{
	struct pnoop_data *nd = e->elevator_data;
	int i;
	
	for (i=0; i<PNOOP_NUM_QUEUES; i++)
		BUG_ON(!list_empty(&nd->queues[i]));

	kfree(nd);
}

static struct elevator_type elevator_pnoop = {
	.ops = {
		.elevator_merge_req_fn		= pnoop_merged_requests,
		.elevator_dispatch_fn		= pnoop_dispatch,
		.elevator_add_req_fn		= pnoop_add_request,
		.elevator_former_req_fn		= pnoop_former_request,
		.elevator_latter_req_fn		= pnoop_latter_request,
		.elevator_init_fn		= pnoop_init_queue,
		.elevator_exit_fn		= pnoop_exit_queue,
	},
	.elevator_name = "pnoop",
	.elevator_owner = THIS_MODULE,
};

static int __init pnoop_init(void)
{
	return elv_register(&elevator_pnoop);
}

static void __exit pnoop_exit(void)
{
	elv_unregister(&elevator_pnoop);
}

module_init(pnoop_init);
module_exit(pnoop_exit);


MODULE_AUTHOR("Carlo Alberto Ferraris");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Prio-no-op IO scheduler");
