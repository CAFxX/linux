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
#include <linux/ratelimit.h>
#include <linux/printk.h>

#define PNOOP_QUEUES (8+1+1)
#define PNOOP_QUEUE_BE 8
#define PNOOP_QUEUE_IDLE 9

struct pnoop_data {
	struct list_head queues[PNOOP_QUEUES];
	uint64_t enq[PNOOP_QUEUES];
	uint64_t deq[PNOOP_QUEUES];
};

static int
pnoop_queueid_for_request(struct request_queue *q, struct request *rq)
{
	unsigned long rq_ioprio = req_get_ioprio(rq);

	switch (IOPRIO_PRIO_CLASS(rq_ioprio)) {
	case IOPRIO_CLASS_RT:
		return clamp(IOPRIO_PRIO_DATA(rq_ioprio), 0UL, 7UL);
	case IOPRIO_CLASS_BE:
	default:
		return PNOOP_QUEUE_BE;
	case IOPRIO_CLASS_IDLE:
		return PNOOP_QUEUE_IDLE;
	}
}

static struct list_head *
pnoop_queue_for_request(struct request_queue *q, struct request *rq)
{
	struct pnoop_data *nd = q->elevator->elevator_data;

	return &nd->queues[pnoop_queueid_for_request(q, rq)];
}

static void pnoop_merged_requests(struct request_queue *q, struct request *rq,
				 struct request *next)
{
	struct pnoop_data *nd = q->elevator->elevator_data;

	nd->deq[pnoop_queueid_for_request(q, next)]++;
	list_del_init(&next->queuelist);
}

static int pnoop_dispatch(struct request_queue *q, int force)
{
	struct pnoop_data *nd = q->elevator->elevator_data;
	struct request *rq = NULL;
	int i;

	for (i=0; i<PNOOP_QUEUES && !rq; i++)
		rq = list_first_entry_or_null(&nd->queues[i], struct request,
						queuelist);

	if (rq) {
		list_del_init(&rq->queuelist);
		elv_dispatch_sort(q, rq);
		nd->deq[i-1]++;
	}
	
	#define __PNF(x) x "[%llu/%llu] "
	#define __PNS(x) nd->enq[x], nd->enq[x] - nd->deq[x] 
	#undef DEFAULT_RATELIMIT_INTERVAL
	#undef DEFAULT_RATELIMIT_BURST
	#define DEFAULT_RATELIMIT_INTERVAL (1*HZ)
	#define DEFAULT_RATELIMIT_BURST 1
	printk_ratelimited("pnoop: "
			__PNF("RT0") __PNF("RT1") __PNF("RT2") __PNF("RT3") 
			__PNF("RT4") __PNF("RT5") __PNF("RT6") __PNF("RT7") 
			__PNF("BE") __PNF("IDLE"), 
			__PNS(0), __PNS(1), __PNS(2), __PNS(3),
			__PNS(4), __PNS(5), __PNS(6), __PNS(7),
			__PNS(8), __PNS(9));
				
	return rq ? 1 : 0;
}

static void pnoop_add_request(struct request_queue *q, struct request *rq)
{
	struct pnoop_data *nd = q->elevator->elevator_data;

	list_add_tail(&rq->queuelist, pnoop_queue_for_request(q, rq));
	nd->enq[pnoop_queueid_for_request(q, rq)]++;
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

	for (i=0; i<PNOOP_QUEUES; i++)
		INIT_LIST_HEAD(&nd->queues[i]);
	for (i=0; i<PNOOP_QUEUES; i++)
		nd->enq[i] = nd->deq[i] = 0;
	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);
	return 0;
}

static void pnoop_exit_queue(struct elevator_queue *e)
{
	struct pnoop_data *nd = e->elevator_data;
	int i;
	
	for (i=0; i<PNOOP_QUEUES; i++)
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
