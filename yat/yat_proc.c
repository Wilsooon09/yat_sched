/*
 * yat_proc.c -- Implementation of the /proc/yat directory tree.
 */

#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/seq_file.h>

#include <yat/debug_trace.h>
#include <yat/yat.h>
#include <yat/yat_proc.h>

#include <yat/clustered.h>

/* in yat/yat.c */
extern atomic_t rt_task_count;

static struct proc_dir_entry *yat_dir = NULL,
	*curr_file = NULL,
	*stat_file = NULL,
	*plugs_dir = NULL,
#ifdef CONFIG_RELEASE_MASTER
	*release_master_file = NULL,
#endif
	*plugs_file = NULL,
	*domains_dir = NULL,
	*cpus_dir = NULL;


/* in yat/sync.c */
int count_tasks_waiting_for_release(void);

static int yat_stats_proc_show(struct seq_file *m, void *v)
{
        seq_printf(m,
		   "real-time tasks   = %d\n"
		   "ready for release = %d\n",
		   atomic_read(&rt_task_count),
		   count_tasks_waiting_for_release());
	return 0;
}

static int yat_stats_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, yat_stats_proc_show, PDE_DATA(inode));
}

static const struct file_operations yat_stats_proc_fops = {
	.open		= yat_stats_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};


static int yat_loaded_proc_show(struct seq_file *m, void *v)
{
	print_sched_plugins(m);
	return 0;
}

static int yat_loaded_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, yat_loaded_proc_show, PDE_DATA(inode));
}

static const struct file_operations yat_loaded_proc_fops = {
	.open		= yat_loaded_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};




/* in yat/yat.c */
int switch_sched_plugin(struct sched_plugin*);

static ssize_t yat_active_proc_write(struct file *file,
					const char __user *buffer, size_t count,
					loff_t *ppos)
{
	char name[65];
	struct sched_plugin* found;
	ssize_t ret = -EINVAL;
	int err;


	ret = copy_and_chomp(name, sizeof(name), buffer, count);
	if (ret < 0)
		return ret;

	found = find_sched_plugin(name);

	if (found) {
		err = switch_sched_plugin(found);
		if (err) {
			printk(KERN_INFO "Could not switch plugin: %d\n", err);
			ret = err;
		}
	} else {
		printk(KERN_INFO "Plugin '%s' is unknown.\n", name);
		ret = -ESRCH;
	}

	return ret;
}

static int yat_active_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%s\n", yat->plugin_name);
	return 0;
}

static int yat_active_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, yat_active_proc_show, PDE_DATA(inode));
}

static const struct file_operations yat_active_proc_fops = {
	.open		= yat_active_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
	.write		= yat_active_proc_write,
};


#ifdef CONFIG_RELEASE_MASTER
static ssize_t yat_release_master_proc_write(
	struct file *file,
	const char __user *buffer, size_t count,
	loff_t *ppos)
{
	int cpu, err, online = 0;
	char msg[64];
	ssize_t len;

	len = copy_and_chomp(msg, sizeof(msg), buffer, count);

	if (len < 0)
		return len;

	if (strcmp(msg, "NO_CPU") == 0)
		atomic_set(&release_master_cpu, NO_CPU);
	else {
		err = sscanf(msg, "%d", &cpu);
		if (err == 1 && cpu >= 0 && (online = cpu_online(cpu))) {
			atomic_set(&release_master_cpu, cpu);
		} else {
			TRACE("invalid release master: '%s' "
			      "(err:%d cpu:%d online:%d)\n",
			      msg, err, cpu, online);
			len = -EINVAL;
		}
	}
	return len;
}

static int yat_release_master_proc_show(struct seq_file *m, void *v)
{
	int master;
	master = atomic_read(&release_master_cpu);
	if (master == NO_CPU)
		seq_printf(m, "NO_CPU\n");
	else
		seq_printf(m, "%d\n", master);
	return 0;
}

static int yat_release_master_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, yat_release_master_proc_show, PDE_DATA(inode));
}

static const struct file_operations yat_release_master_proc_fops = {
	.open		= yat_release_master_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
	.write		= yat_release_master_proc_write,
};
#endif

int __init init_yat_proc(void)
{
	yat_dir = proc_mkdir("yat", NULL);
	if (!yat_dir) {
		printk(KERN_ERR "Could not allocate YAT^RT procfs entry.\n");
		return -ENOMEM;
	}

	curr_file = proc_create("active_plugin", 0644, yat_dir,
				&yat_active_proc_fops);

	if (!curr_file) {
		printk(KERN_ERR "Could not allocate active_plugin "
		       "procfs entry.\n");
		return -ENOMEM;
	}

#ifdef CONFIG_RELEASE_MASTER
	release_master_file = proc_create("release_master", 0644, yat_dir,
					  &yat_release_master_proc_fops);
	if (!release_master_file) {
		printk(KERN_ERR "Could not allocate release_master "
		       "procfs entry.\n");
		return -ENOMEM;
	}
#endif

	stat_file = proc_create("stats", 0444, yat_dir, &yat_stats_proc_fops);

	plugs_dir = proc_mkdir("plugins", yat_dir);
	if (!plugs_dir){
		printk(KERN_ERR "Could not allocate plugins directory "
				"procfs entry.\n");
		return -ENOMEM;
	}

	plugs_file = proc_create("loaded", 0444, plugs_dir,
				 &yat_loaded_proc_fops);

	domains_dir = proc_mkdir("domains", yat_dir);
	if (!domains_dir) {
		printk(KERN_ERR "Could not allocate domains directory "
				"procfs entry.\n");
		return -ENOMEM;
	}

	cpus_dir = proc_mkdir("cpus", yat_dir);
	if (!cpus_dir) {
		printk(KERN_ERR "Could not allocate cpus directory "
				"procfs entry.\n");
		return -ENOMEM;
	}

	return 0;
}

void exit_yat_proc(void)
{
	if (cpus_dir || domains_dir) {
		deactivate_domain_proc();
		if (cpus_dir)
			remove_proc_entry("cpus", yat_dir);
		if (domains_dir)
			remove_proc_entry("domains", yat_dir);
	}
	if (plugs_file)
		remove_proc_entry("loaded", plugs_dir);
	if (plugs_dir)
		remove_proc_entry("plugins", yat_dir);
	if (stat_file)
		remove_proc_entry("stats", yat_dir);
	if (curr_file)
		remove_proc_entry("active_plugin", yat_dir);
#ifdef CONFIG_RELEASE_MASTER
	if (release_master_file)
		remove_proc_entry("release_master", yat_dir);
#endif
	if (yat_dir)
		remove_proc_entry("yat", NULL);
}

long make_plugin_proc_dir(struct sched_plugin* plugin,
		struct proc_dir_entry** pde_in)
{
	struct proc_dir_entry *pde_new = NULL;
	long rv;

	if (!plugin || !plugin->plugin_name){
		printk(KERN_ERR "Invalid plugin struct passed to %s.\n",
				__func__);
		rv = -EINVAL;
		goto out_no_pde;
	}

	if (!plugs_dir){
		printk(KERN_ERR "Could not make plugin sub-directory, because "
				"/proc/yat/plugins does not exist.\n");
		rv = -ENOENT;
		goto out_no_pde;
	}

	pde_new = proc_mkdir(plugin->plugin_name, plugs_dir);
	if (!pde_new){
		printk(KERN_ERR "Could not make plugin sub-directory: "
				"out of memory?.\n");
		rv = -ENOMEM;
		goto out_no_pde;
	}

	rv = 0;
	*pde_in = pde_new;
	goto out_ok;

out_no_pde:
	*pde_in = NULL;
out_ok:
	return rv;
}

void remove_plugin_proc_dir(struct sched_plugin* plugin)
{
	if (!plugin || !plugin->plugin_name){
		printk(KERN_ERR "Invalid plugin struct passed to %s.\n",
				__func__);
		return;
	}
	remove_proc_entry(plugin->plugin_name, plugs_dir);
}



/* misc. I/O helper functions */

int copy_and_chomp(char *kbuf, unsigned long ksize,
		   __user const char* ubuf, unsigned long ulength)
{
	/* caller must provide buffer space */
	BUG_ON(!ksize);

	ksize--; /* leave space for null byte */

	if (ksize > ulength)
		ksize = ulength;

	if(copy_from_user(kbuf, ubuf, ksize))
		return -EFAULT;

	kbuf[ksize] = '\0';

	/* chomp kbuf */
	if (ksize > 0 && kbuf[ksize - 1] == '\n')
		kbuf[ksize - 1] = '\0';

	return ksize;
}

/* helper functions for clustered plugins */
static const char* cache_level_names[] = {
	"ALL",
	"L1",
	"L2",
	"L3",
};

int parse_cache_level(const char *cache_name, enum cache_level *level)
{
	int err = -EINVAL;
	int i;
	/* do a quick and dirty comparison to find the cluster size */
	for (i = GLOBAL_CLUSTER; i <= L3_CLUSTER; i++)
		if (!strcmp(cache_name, cache_level_names[i])) {
			*level = (enum cache_level) i;
			err = 0;
			break;
		}
	return err;
}

const char* cache_level_name(enum cache_level level)
{
	int idx = level;

	if (idx >= GLOBAL_CLUSTER && idx <= L3_CLUSTER)
		return cache_level_names[idx];
	else
		return "INVALID";
}




/* proc file interface to configure the cluster size */

static ssize_t yat_cluster_proc_write(struct file *file,
					const char __user *buffer, size_t count,
					loff_t *ppos)
{
	enum cache_level *level = (enum cache_level *) PDE_DATA(file_inode(file));
	ssize_t len;
	char cache_name[8];

	len = copy_and_chomp(cache_name, sizeof(cache_name), buffer, count);

	if (len > 0 && parse_cache_level(cache_name, level)) {
		printk(KERN_INFO "Cluster '%s' is unknown.\n", cache_name);
		len = -EINVAL;
	}

	return len;
}

static int yat_cluster_proc_show(struct seq_file *m, void *v)
{
	enum cache_level *level = (enum cache_level *)  m->private;

	seq_printf(m, "%s\n", cache_level_name(*level));
	return 0;
}

static int yat_cluster_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, yat_cluster_proc_show, PDE_DATA(inode));
}

static const struct file_operations yat_cluster_proc_fops = {
	.open		= yat_cluster_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
	.write		= yat_cluster_proc_write,
};

struct proc_dir_entry* create_cluster_file(struct proc_dir_entry* parent,
					   enum cache_level* level)
{
	struct proc_dir_entry* cluster_file;


	cluster_file = proc_create_data("cluster", 0644, parent,
					&yat_cluster_proc_fops,
					(void *) level);
	if (!cluster_file) {
		printk(KERN_ERR
		       "Could not cluster procfs entry.\n");
	}
	return cluster_file;
}

static struct domain_proc_info* active_mapping = NULL;

static int yat_mapping_proc_show(struct seq_file *m, void *v)
{
	struct cd_mapping *mapping = (struct cd_mapping*) m->private;

	if(!mapping)
		return 0;

	seq_printf(m, "%*pb\n", cpumask_pr_args(mapping->mask));
	return 0;
}

static int yat_mapping_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, yat_mapping_proc_show, PDE_DATA(inode));
}

static const struct file_operations yat_domain_proc_fops = {
	.open		= yat_mapping_proc_open,
	.read		= seq_read,
	.llseek 	= seq_lseek,
	.release 	= single_release,
};

long activate_domain_proc(struct domain_proc_info* map)
{
	int i;
	char name[8];

	if (!map)
		return -EINVAL;
	if (cpus_dir == NULL || domains_dir == NULL)
		return -EINVAL;

	if (active_mapping)
		deactivate_domain_proc();

	active_mapping = map;

	for (i = 0; i < map->num_cpus; ++i) {
		struct cd_mapping* m = &map->cpu_to_domains[i];
		snprintf(name, sizeof(name), "%d", m->id);
		m->proc_file = proc_create_data(name, 0444, cpus_dir,
			&yat_domain_proc_fops, (void*)m);
	}

	for (i = 0; i < map->num_domains; ++i) {
		struct cd_mapping* m = &map->domain_to_cpus[i];
		snprintf(name, sizeof(name), "%d", m->id);
		m->proc_file = proc_create_data(name, 0444, domains_dir,
			&yat_domain_proc_fops, (void*)m);
	}

	return 0;
}

long deactivate_domain_proc()
{
	int i;
	char name[65];

	struct domain_proc_info* map = active_mapping;

	if (!map)
		return -EINVAL;

	for (i = 0; i < map->num_cpus; ++i) {
		struct cd_mapping* m = &map->cpu_to_domains[i];
		snprintf(name, sizeof(name), "%d", m->id);
		remove_proc_entry(name, cpus_dir);
		m->proc_file = NULL;
	}
	for (i = 0; i < map->num_domains; ++i) {
		struct cd_mapping* m = &map->domain_to_cpus[i];
		snprintf(name, sizeof(name), "%d", m->id);
		remove_proc_entry(name, domains_dir);
		m->proc_file = NULL;
	}

	active_mapping = NULL;

	return 0;
}

long init_domain_proc_info(struct domain_proc_info* m,
				int num_cpus, int num_domains)
{
	int i;
	int num_alloced_cpu_masks = 0;
	int num_alloced_domain_masks = 0;

	m->cpu_to_domains =
		kmalloc(sizeof(*(m->cpu_to_domains))*num_cpus,
			GFP_ATOMIC);
	if(!m->cpu_to_domains)
		goto failure;

	m->domain_to_cpus =
		kmalloc(sizeof(*(m->domain_to_cpus))*num_domains,
			GFP_ATOMIC);
	if(!m->domain_to_cpus)
		goto failure;

	for(i = 0; i < num_cpus; ++i) {
		if(!zalloc_cpumask_var(&m->cpu_to_domains[i].mask, GFP_ATOMIC))
			goto failure;
		++num_alloced_cpu_masks;
	}
	for(i = 0; i < num_domains; ++i) {
		if(!zalloc_cpumask_var(&m->domain_to_cpus[i].mask, GFP_ATOMIC))
			goto failure;
		++num_alloced_domain_masks;
	}

	return 0;

failure:
	for(i = 0; i < num_alloced_cpu_masks; ++i)
		free_cpumask_var(m->cpu_to_domains[i].mask);
	for(i = 0; i < num_alloced_domain_masks; ++i)
		free_cpumask_var(m->domain_to_cpus[i].mask);
	if(m->cpu_to_domains)
		kfree(m->cpu_to_domains);
	if(m->domain_to_cpus)
		kfree(m->domain_to_cpus);
	return -ENOMEM;
}

void destroy_domain_proc_info(struct domain_proc_info* m)
{
	int i;
	for(i = 0; i < m->num_cpus; ++i)
		free_cpumask_var(m->cpu_to_domains[i].mask);
	for(i = 0; i < m->num_domains; ++i)
		free_cpumask_var(m->domain_to_cpus[i].mask);
	kfree(m->cpu_to_domains);
	kfree(m->domain_to_cpus);
	memset(m, 0, sizeof(*m));
}
