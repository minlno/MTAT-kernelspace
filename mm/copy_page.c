/*
 * Parallel page copy routine.
 *
 * Referenced code:
 *   https://github.com/ysarch-lab/nimble_page_management_asplos_2019/tree/nimble_page_management_5_6_rc6
 */

#include <linux/highmem.h>
#include <linux/workqueue.h>
#include <linux/slab.h>

unsigned int limit_mt_num = 4;

/* ======================== multi-threaded copy page =========================== */

struct copy_item {
	char *to;
	char *from;
	unsigned long chunk_size;
};

struct copy_page_info {
	struct work_struct copy_page_work;
	unsigned long num_items;
	struct copy_item item_list[0];
};

static void copy_page_routine(char *vto, char *vfrom,
		unsigned long chunk_size)
{
	memcpy(vto, vfrom, chunk_size);
}

static void copy_page_work_queue_thread(struct work_struct *work)
{
	struct copy_page_info *my_work = (struct copy_page_info *) work;
	int i;

	for (i = 0; i < my_work->num_items; ++i)
		copy_page_routine(my_work->item_list[i].to,
				my_work->item_list[i].from, 
				my_work->item_list[i].chunk_size);
}

int copy_page_multithread(struct page *to, struct page *from, int nr_pages)
{
	unsigned int total_mt_num = limit_mt_num;
	int to_node = numa_node_id();
	int i;
	struct copy_page_info *work_items[40] = {0};
	char *vto, *vfrom;
	unsigned long chunk_size;
	const struct cpumask *per_node_cpumask = cpumask_of_node(to_node);
	int cpu_id_list[40] = {0};
	int cpu;
	int err = 0;

	total_mt_num = min_t(unsigned int, total_mt_num,
				cpumask_weight(per_node_cpumask));

	if (total_mt_num > 1)
		total_mt_num = (total_mt_num / 2) * 2;

	for (cpu = 0; cpu < total_mt_num; ++cpu) {
		work_items[cpu] = kzalloc(sizeof(struct copy_page_info)
				+ sizeof(struct copy_item), GFP_KERNEL);

		if (!work_items[cpu]) {
			err = -ENOMEM;
			goto free_work_items;
		}
	}

	i = 0;
	for_each_cpu(cpu, per_node_cpumask) {
		if (i >= total_mt_num)
			break;
		cpu_id_list[i] = cpu;
		++i;
	}

	vfrom = kmap(from);
	vto = kmap(to);
	chunk_size = PAGE_SIZE * nr_pages / total_mt_num;

	for (i = 0; i < total_mt_num; ++i) {
		INIT_WORK((struct work_struct *) work_items[i],
				copy_page_work_queue_thread);

		work_items[i]->num_items = 1;
		work_items[i]->item_list[0].to = vto + i * chunk_size;
		work_items[i]->item_list[0].from = vfrom + i * chunk_size;
		work_items[i]->item_list[0].chunk_size = chunk_size;

		queue_work_on(cpu_id_list[i],
				system_highpri_wq,
				(struct work_struct *) work_items[i]);
	}

	/* Wait until it finishes */
	for (i = 0; i < total_mt_num; ++i)
		flush_work((struct work_struct *) work_items[i]);

	kunmap(to);
	kunmap(from);

free_work_items:
	for (cpu = 0; cpu < total_mt_num; ++cpu)
		if (work_items[cpu])
			kfree(work_items[cpu]);

	return err;
}
