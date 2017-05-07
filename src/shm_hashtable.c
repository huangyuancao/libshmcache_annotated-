//shm_hashtable.c

#include <errno.h>
#include <pthread.h>
#include "logger.h"
#include "shared_func.h"
#include "sched_thread.h"
#include "shmopt.h"
#include "shm_lock.h"
#include "shm_object_pool.h"
#include "shm_striping_allocator.h"
#include "shm_hashtable.h"

int shm_ht_get_capacity(const int max_count)
{
    unsigned int *capacity;
    capacity = hash_get_prime_capacity(max_count);
    if (capacity == NULL) {
        return max_count;
    }
    return *capacity;
}

void shm_ht_init(struct shmcache_context *context, const int capacity)
{
    context->memory->hashtable.capacity = capacity;
    context->memory->hashtable.count = 0;
}

#define HT_GET_BUCKET_INDEX(context, key) \
    ((unsigned int)context->config.hash_func(key->data, key->length) % context->memory->hashtable.capacity)

#define HT_KEY_EQUALS(hentry, pkey) (hentry->key_len == pkey->length && memcmp(hentry->key, pkey->data, pkey->length) == 0)

#define HT_VALUE_EQUALS(hvalue, hv_len, pvalue) (hv_len == pvalue->length \
        && memcmp(hvalue, pvalue->data, pvalue->length) == 0)

int shm_ht_set(struct shmcache_context *context, const struct shmcache_key_info *key, const struct shmcache_value_info *value)
{
    int result;
    unsigned int index;
    int64_t old_offset;
    int64_t new_offset;
    struct shm_hash_entry *old_entry;
    struct shm_hash_entry *new_entry;
    struct shm_hash_entry *previous;
    char *hvalue;
    bool found;

    if (key->length > SHMCACHE_MAX_KEY_SIZE) {
		logError("file: "__FILE__", line: %d, "
                "invalid key length: %d exceeds %d", __LINE__,
                key->length, SHMCACHE_MAX_KEY_SIZE);
        return ENAMETOOLONG;
    }

    if (value->length > context->config.max_value_size) {
		logError("file: "__FILE__", line: %d, "
                "invalid value length: %d exceeds %d", __LINE__,
                value->length, context->config.max_value_size);
        return EINVAL;
    }

    if (context->memory->hashtable.count >= context->config.max_key_count) {
        //淘汰删除一些 过旧的key（按FIFO策略 删除entry空间）
        if ((result=shm_value_allocator_recycle(context, &context->memory->stats.memory.recycle.key, context->config.recycle_key_once)) != 0)
        {
            return result;
        }
    }

    if (!g_schedule_flag) {
        g_current_time = time(NULL);
    }

    //从 striping_allocator中分配一个可用的entry空间
    if ((new_entry=shm_value_allocator_alloc(context, key->length, value->length)) == NULL)
    {
       return result;
    }

    //从hashtable中 查下 是否已存在这个key
    previous = NULL;   //遍历过程中 记录上一个entry
    old_entry = NULL;
    found = false;
    index = HT_GET_BUCKET_INDEX(context, key);
    old_offset = context->memory->hashtable.buckets[index];
    while (old_offset > 0)
    {
        old_entry = shm_get_hentry_ptr(context, old_offset);
        if (HT_KEY_EQUALS(old_entry, key)) {
            found = true;
            break;
        }

        old_offset = old_entry->ht_next;
        previous = old_entry;
    }

    if (found) {   //如果找到了，则 修改new_entry->next，并释放old_entry 在striping_allocator中的空间
        bool recycled = false;
        new_entry->ht_next = old_entry->ht_next;
        //这里会 真实清空old_entry的数据吗？如果是的话，另一个读者在读的时候，会产生错误！
        //不会真实清空，会循环利用striping_allocator空间.
        //但会把entry->ht_next置为0。如果此时有一个读者在遍历链表，它的遍历过程会中断，会导致后面节点的数据没读到?
        shm_ht_free_entry(context, old_entry, old_offset, &recycled);
    } else {
        new_entry->ht_next = 0;   //加入 作为链表的最后一个结点
    }

    new_offset = shm_get_hentry_offset(new_entry);
    //copy key data
    memcpy(new_entry->key, key->data, key->length);
    new_entry->key_len = key->length;
    //copy value data
    hvalue = shm_get_value_ptr(context, new_entry);
    memcpy(hvalue, value->data, value->length);
    new_entry->value.length = value->length;
    new_entry->value.options = value->options;
    new_entry->expires = value->expires;

    //将entry加入到桶链表中
    if (previous != NULL) {  //add to tail
        previous->ht_next = new_offset;    //加入作为链表的 最后一个结点(这样在并发读的时候，不会影响读者遍历链表)
    } else {
        context->memory->hashtable.buckets[index] = new_offset;  //加入作为链表的第一个结点  这里应该用原子操作吧？？？
    }

    //修改统计数据
    context->memory->hashtable.count++;
    context->memory->usage.used.value += value->length;
    context->memory->usage.used.key += new_entry->key_len;
    shm_list_add_tail(context, new_offset);  //插入到context->list中

    return 0;
}

int shm_ht_get(struct shmcache_context *context,
        const struct shmcache_key_info *key,
        struct shmcache_value_info *value)
{
    unsigned int index;
    int64_t entry_offset;
    struct shm_hash_entry *entry;

    index = HT_GET_BUCKET_INDEX(context, key);
    entry_offset = context->memory->hashtable.buckets[index];
    while (entry_offset > 0)
    {
        entry = shm_get_hentry_ptr(context, entry_offset);
        if (HT_KEY_EQUALS(entry, key))  //如果是这个entry
        {
            value->data = shm_get_value_ptr(context, entry);
            value->length = entry->value.length;
            value->options = entry->value.options;
            value->expires = entry->expires;
            if (HT_ENTRY_IS_VALID(entry, get_current_time()))
            {
                return 0;
            }
            else
            {
                return ETIMEDOUT;
            }
        }

        entry_offset = entry->ht_next;
    }

    return ENOENT;
}

//释放hash entry在shm中的空间
void shm_ht_free_entry(struct shmcache_context *context,
        struct shm_hash_entry *entry, const int64_t entry_offset,
        bool *recycled)
{
    context->memory->hashtable.count--;
    context->memory->usage.used.value -= entry->value.length;
    context->memory->usage.used.key -= entry->key_len;
    shm_list_delete(context, entry_offset);
    shm_value_allocator_free(context, entry, recycled);
    entry->ht_next = 0;
}

//delete the key for internal usage
int shm_ht_delete_ex(struct shmcache_context *context, const struct shmcache_key_info *key, bool *recycled)
{
    int result;
    unsigned int index;
    int64_t entry_offset;
    struct shm_hash_entry *entry;
    struct shm_hash_entry *previous;

    previous = NULL;
    result = ENOENT;
    index = HT_GET_BUCKET_INDEX(context, key);
    entry_offset = context->memory->hashtable.buckets[index];
    while (entry_offset > 0)
    {
        entry = shm_get_hentry_ptr(context, entry_offset);
        //如果找到这个key对应的entry，则将它从 桶链表中删除.
        if (HT_KEY_EQUALS(entry, key))
        {
            if (previous != NULL)
            {
                previous->ht_next = entry->ht_next;
            }
            else
            {
                context->memory->hashtable.buckets[index] = entry->ht_next;
            }

            shm_ht_free_entry(context, entry, entry_offset, recycled);
            result = 0;
            break;
        }

        entry_offset = entry->ht_next;
        previous = entry;
    }

    return result;
}

int shm_ht_clear(struct shmcache_context *context)
{
    struct shm_striping_allocator *allocator;
    struct shm_striping_allocator *end;
    int64_t allocator_offset;
    int ht_count;

    context->memory->stats.hashtable.last_clear_time =
        context->memory->stats.last.calc_time = get_current_time();
    ht_count = context->memory->hashtable.count;
    memset(context->memory->hashtable.buckets, 0, sizeof(int64_t) *
            context->memory->hashtable.capacity);
    context->memory->hashtable.count = 0;
    shm_list_init(context);

    shm_object_pool_init_empty(&context->value_allocator.doing);
    shm_object_pool_init_empty(&context->value_allocator.done);
    end = context->value_allocator.allocators +
        context->memory->vm_info.striping.count.current;
    for (allocator=context->value_allocator.allocators; allocator<end; allocator++) {
        shm_striping_allocator_reset(allocator);
        allocator->in_which_pool = SHMCACHE_STRIPING_ALLOCATOR_POOL_DOING;
        allocator_offset = (char *)allocator - context->segments.hashtable.base;
        shm_object_pool_push(&context->value_allocator.doing, allocator_offset);
    }
    context->memory->usage.used.key = 0;
    context->memory->usage.used.value = 0;
    context->memory->usage.used.entry = 0;

    logInfo("file: "__FILE__", line: %d, pid: %d, "
            "clear hashtable, %d entries be cleared!",
            __LINE__, context->pid, ht_count);
    return ht_count;
}
