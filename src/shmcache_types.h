//shmcache_types.h

#ifndef _SHMCACHE_TYPES_H
#define _SHMCACHE_TYPES_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/shm.h>
#include "hash.h"
#include "common_define.h"

#define SHMCACHE_MAJOR_VERSION  1
#define SHMCACHE_MINOR_VERSION  0
#define SHMCACHE_PATCH_VERSION  2

#define SHMCACHE_MAX_KEY_SIZE  64

#define SHMCACHE_STATUS_INIT   0
#define SHMCACHE_STATUS_NORMAL 0x12345678

#define SHMCACHE_TYPE_SHM    1
#define SHMCACHE_TYPE_MMAP   2

#define SHMCACHE_NEVER_EXPIRED  0

#define SHMCACHE_SERIALIZER_STRING    0   //string type
#define SHMCACHE_SERIALIZER_INTEGER   1   //integer type
#define SHMCACHE_SERIALIZER_NONE      0x100
#define SHMCACHE_SERIALIZER_IGBINARY  0x200
#define SHMCACHE_SERIALIZER_MSGPACK   0x400
#define SHMCACHE_SERIALIZER_PHP       0x800

#define SHMCACHE_STRIPING_ALLOCATOR_POOL_DOING  0
#define SHMCACHE_STRIPING_ALLOCATOR_POOL_DONE   1

struct shmcache_config {
    char filename[MAX_PATH_SIZE];
    int64_t min_memory;
    int64_t max_memory;
    int64_t segment_size;
    int max_key_count;
    int max_value_size;
    int type;  //shm or mmap

    int recycle_key_once;  //recycle key number once when reach max keys

    struct {
        /* avg. key TTL threshold for recycling memory
         * unit: second
         * <= 0 for never recycle memory until reach memory limit (max_memory)
         */
        int avg_key_ttl;

        /* when the remain memory <= this parameter, discard it.
         * put this allocator in the doing queue to the done queue
         */
        int discard_memory_size;

        /* when a allocator in the doing queue allocate fail times > this parameter,
         * means it is almost full, put it to the done queue
         */
        int max_fail_times;

        /* sleep time to avoid other processes read dirty data when recycle
         * more than one valid (in TTL / not expired) KV entries.
         * 0 for never sleep
         */
        int sleep_us_when_recycle_valid_entries;
    } va_policy;   //value allocator policy

    struct {
        int trylock_interval_us;
        int detect_deadlock_interval_ms;
    } lock_policy;

    HashFunc hash_func;
};

struct shm_segment_striping_pair {
    short segment;  //shm segment index
    short striping; //shm striping index
};

struct shm_value {
    int length;     //value length
    int options;    //options for application
};

struct shm_list {
    int64_t prev;   //上一个结点的entry offset
    int64_t next;   //下一个结点的entry offset
};

union shm_hentry_offset {
    int64_t offset;
    struct {
        int index :16;
        int64_t offset :48;
    } segment;
};

//存储顺序: sizeof(struct shm_hash_entry) + MEM_ALIGN(entry->key_len) + value.length
struct shm_hash_entry {
    struct shm_list list;  //for recycle, must be first

    int key_len;
    time_t expires;
    struct shm_value value;

    struct {
        int size;       //alloc size
        struct shm_segment_striping_pair index;   //此key/value的 segment index和striping index
        int64_t offset; //value segment offset  此key/value对　在shm segment块的偏移量
    } memory;

    int64_t ht_next;  //for hashtable   //此桶链表的 下一个entry节点在 shm中的 segment index, 偏移量
    char key[0];　　　 //存放 key 内容，长度为key_len
};

struct shm_ring_queue {
    int capacity;
    int head;  //for pop   分配空闲的striping allocator对象
    int tail;  //for push
};

struct shm_object_pool_info {
    struct {
        int64_t base_offset;
        int element_size;
    } object;
    struct shm_ring_queue queue;
};

struct shm_hashtable {
    struct shm_list head; //for recycle
    int capacity;   //允许的key的最大个数
    int count;      //当前存储的key的个数
    int64_t buckets[0]; //entry offset     bucket index -> bucket entry offset
};

//存储 一个striping_allocator对象的参数信息
struct shm_striping_allocator {
    time_t last_alloc_time;  //record the timestamp of fist allocate
    int fail_times;   //allocate fail times
    short in_which_pool;  //in doing or done
    struct shm_segment_striping_pair index;
    struct {
        int total;
        int used;   //已使用的大小
    } size;

    struct {
        int64_t base;
        int64_t free;   //空闲空间的 偏移量
        int64_t end;
    } offset;
};

struct shm_value_allocator {
    struct shm_object_pool_info doing;
    struct shm_object_pool_info done;
};

struct shm_value_size_info {
    int64_t size;
    struct {
        int current;
        int max;
    } count;
};

struct shm_value_memory_info {
    struct shm_value_size_info segment;     //当前已分配的shm segment个数，最大个数
    struct shm_value_size_info striping;    //当前已分配的 striping_allocator 个数，最大个数
};

struct shm_lock {
    pid_t pid;
    pthread_mutex_t mutex;
};

struct shm_counter {
    volatile int64_t total;
    volatile int64_t success;
};

struct shm_recycle_stats {
    int64_t total;   //total count
    int64_t success; //succes count
    int64_t force;  //force recycle count (clear valid entries)
    int64_t last_recycle_time;
};

struct shm_stats {
    struct {
        struct shm_counter set;
        struct shm_counter get;
        struct shm_counter del;
        struct shm_counter incr;
        int64_t last_clear_time;
    } hashtable;

    struct {
        struct {
            int64_t total;
            int64_t valid;
        } clear_ht_entry;  //clear for recycle

        struct {
            struct shm_recycle_stats key;
            struct shm_recycle_stats value_striping;
        } recycle;
    } memory;

    struct {
        volatile int64_t total;
        volatile int64_t retry;
        volatile int64_t detect_deadlock;
        volatile int64_t unlock_deadlock;
        int64_t last_detect_deadlock_time;
        int64_t last_unlock_deadlock_time;
    } lock;

    //for calculate hit ratio
    struct {
        struct shm_counter get;
        int64_t calc_time;
    } last;

    int64_t init_time;   //init unix timestamp
};

struct shm_memory_usage {
    int64_t alloced;   //已分配 shm 大小
    struct {
        int64_t common; //first segment include hashtable
        int64_t entry;  //whole entry include key and value
        int64_t key;
        int64_t value;
    } used;
};

// 存储 共享内存 hash表的所有信息及地址
struct shm_memory_info {
    int size;           //sizeof(struct shm_memory_info)
    int status;
    time_t init_time;    //init unix timestamp
    int max_key_count;   //配置项：最多的key/value对　个数
    struct shm_lock lock;    //posix mutex
    struct shm_value_memory_info vm_info;  //value memory info
    struct shm_value_allocator value_allocator;
    struct shm_stats stats;
    struct shm_memory_usage usage;
    struct shm_hashtable hashtable;   //must be last
};

struct shmcache_key_info {
    char *data;
    int length;
};

struct shmcache_value_info {
    char *data;
    int length;
    int options;    //options for application
    time_t expires; //expire time
};

struct shmcache_segment_info {
    int proj_id;   //用来生成 shm key的
    key_t key;     //shm key
    int64_t size;  //memory size
    char *base;    //共享内存的首地址
};

struct shmcache_object_pool_context {
    struct shm_object_pool_info *obj_pool_info;
    int64_t *offsets;  //object offset array  存储ring queue中所有结点(即striping_allocator对象)的shm offset：ring queue中的index -> segment offset
    int index;   //for iterator  用来遍历当前可用的striping_allocator对象
};

struct shmcache_value_allocator_context {
    struct shmcache_object_pool_context doing; //doing queue  存储当前可分配的striping_allocator对象   //它的结构体中的参数　都是context->memory->value_allocator中的地址，实际是shm空间中地址　
    struct shmcache_object_pool_context done;  //done queue　 存储当前已分配满的striping_allocator对象　//它的结构体中的参数　都是context->memory->value_allocator中的地址，实际是shm空间中地址　
    struct shm_striping_allocator *allocators; //base address 是一个数组，存储所有的striping_allocator对象的参数信息 　//它指向的内存地址是 shm空间中的地址,  即context->segments.hashtable.base中的空间
};

struct shmcache_list {
    struct {
        struct shm_list *ptr;
        int64_t offset;  //链表最后一个结点的entry offset
    } head;   //head.ptr->next: 链表的第一个结点
};

struct shmcache_context {
    pid_t pid;
    int lock_fd;    //for file lock　　用配置的文件 做　文件锁
    int detect_deadlock_clocks;
    struct shmcache_config config;
    struct shm_memory_info *memory;   //存储hash表的元信息    (memory指向的地址是segments->hashtable->base, 是shm空间)

    //bzh: 这里 hashtable在栈上分配、items在堆上分配的，会不会有问题？当有另一个写者在操作时，会不会错乱？？？
    struct {
        struct shmcache_segment_info hashtable;   //保存当前使用的shm segment空间的参数
        struct {
            int count;   //当前item的个数, 当前已分配的shm segment个数
            struct shmcache_segment_info *items;   //(指针items指向的内存由malloc分配，大小: segment最大个数*sizeof(shmcache_segment_info))　　存储所有已分配的shm segment的参数
        } values;
    } segments;

    struct shmcache_value_allocator_context value_allocator;   //存储所有的striping_allocator对象的 相关信息（在分配hash entry时，会用到）
    struct shmcache_list list;   //for value recycle  //将所有已经已存储的key/value entry的offset都 保存到这个链表上
    bool create_segment;  //if check segment size
};

struct shmcache_stats {
    struct shm_stats shm;
    struct {
        int64_t segment_size;  //segment memory size
        int capacity;
        int count;  //key count
    } hashtable;
    int max_key_count;

    struct {
        int64_t max;
        int64_t used;
        struct shm_memory_usage usage;
    } memory;

    struct {
        double ratio;
        double get_qps;
        int seconds;
    } hit;
};

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
}
#endif

#endif

