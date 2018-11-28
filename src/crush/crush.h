#ifndef _CRUSH_CRUSH_H
#define _CRUSH_CRUSH_H

#ifdef __cplusplus
extern "C" {
#endif


#ifndef __KERNEL__
# include <assert.h>
# define BUG_ON(x) assert(!(x))
# include "include/inttypes.h"  /* just for int types */
#else
# include <linux/types.h>
#endif


/*** RULES ***/
enum {
	CRUSH_RULE_TAKE,
	CRUSH_RULE_CHOOSE_FIRSTN,
	CRUSH_RULE_CHOOSE_INDEP,
	CRUSH_RULE_EMIT
};

#define CRUSH_MAX_DEPTH 10
#define CRUSH_MAX_SET   10

struct crush_rule_step {
	__u32 op;
	__s32 arg1;
	__s32 arg2;
};

struct crush_rule {
	__u32 len;
	struct crush_rule_step steps[0];
};

#define crush_rule_size(len) (sizeof(struct crush_rule) + (len)*sizeof(struct crush_rule_step))



/*** BUCKETS ***/

enum {
	CRUSH_BUCKET_UNIFORM = 1,
	CRUSH_BUCKET_LIST = 2,
	CRUSH_BUCKET_TREE = 3,
	CRUSH_BUCKET_STRAW = 4
};

struct crush_bucket {
	__s32 id;        /* this'll be negative */
	__u16 type;
	__u16 bucket_type;
	__u32 weight;    /* 16-bit fixed point */
	__u32 size;      /* num items */
	__s32 *items;
};

struct crush_bucket_uniform {
	struct crush_bucket h;
	__u32 *primes;
	__u32 item_weight;  /* 16-bit fixed point */
};

struct crush_bucket_list {
	struct crush_bucket h;
	__u32 *item_weights;  /* 16-bit fixed point */
	__u32 *sum_weights;   /* 16-bit fixed point.  element i is sum of weights 0..i, inclusive */
};

struct crush_bucket_tree {
	struct crush_bucket h;  /* note: h.size is tree size, not number of actual items */
	__u32 *node_weights;
};

struct crush_bucket_straw {
	struct crush_bucket h;
	__u32 *straws;  /* 16-bit fixed point */
};



/*** CRUSH ***/

struct crush_map {
	struct crush_bucket **buckets;
	struct crush_rule **rules;
	
	/* parent pointers */
	__u32 *bucket_parents;
	__u32 *device_parents;
	
	/* offload
	 * size max_devices, values 0...0xffff
	 *        0 == normal
	 *  0x10000 == 100% offload (i.e. failed)
	 */
	__u32 *device_offload;   
	
	__u32 max_buckets;
	__u32 max_rules;
	__s32 max_devices;
};


/* common destructors */
extern void crush_calc_parents(struct crush_map *m);
extern void crush_destroy_bucket_uniform(struct crush_bucket_uniform *);
extern void crush_destroy_bucket_list(struct crush_bucket_list *);
extern void crush_destroy_bucket_tree(struct crush_bucket_tree *);
extern void crush_destroy_bucket_straw(struct crush_bucket_straw *);
extern void crush_destroy(struct crush_map *map);


#ifdef __cplusplus
}
#endif

#endif
