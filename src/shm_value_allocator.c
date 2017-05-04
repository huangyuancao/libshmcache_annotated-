//shm_value_allocator.c

#include <errno.h>
#include "sched_thread.h"
#include "shared_func.h"
#include "shm_object_pool.h"
#include "shm_striping_allocator.h"
#include "shm_list.h"
#include "shmopt.h"
#include "shm_hashtable.h"
#include "shm_value_allocator.h"

//从striping_allocator对象空间 中 分配一个entry空间
static struct shm_hash_entry *shm_value_striping_alloc(
        struct shmcache_context *context,
        struct shm_striping_allocator *allocator, const int size)
{
    int64_t offset;
    char *base;
    struct shm_hash_entry *entry;
    offset = shm_striping_allocator_alloc(allocator, size);
    if (offset < 0) {
        return NULL;
    }

    base = shmopt_get_value_segment(context, allocator->index.segment);
    if (base == NULL) {
        return NULL;
    }

    entry = (struct shm_hash_entry *)(base + offset);
    entry->memory.offset = offset;
    entry->memory.index = allocator->index;
    entry->memory.size = size;
    return entry;
}

//从现有的striping_allocator对象的空间中，分配一个entry空间
//返回NULL，表示分配失败
static struct shm_hash_entry *shm_value_allocator_do_alloc(struct shmcache_context *context, const int size)
{
    int64_t allocator_offset;
    int64_t removed_offset;
    struct shm_striping_allocator *allocator;
    struct shm_hash_entry *entry;

    //获取 第一个可用的striping_allocator对象的offset
    allocator_offset = shm_object_pool_first(&context->value_allocator.doing);
    while (allocator_offset > 0)
    {
        allocator = (struct shm_striping_allocator *)(context->segments.hashtable.base + allocator_offset);
        //从striping_allocator 中获取一个 entry
        if ((entry=shm_value_striping_alloc(context, allocator, size)) != NULL)
        {
            context->memory->usage.used.entry += size;
            return entry;
        }

        if ((shm_striping_allocator_free_size(allocator) <= context->config.va_policy.discard_memory_size) ||
                (allocator->fail_times > context->config.va_policy.max_fail_times))
        {
            //这个 striping_allocator 已分配满了，将它的索引从 context->value_allocator.doing 中删除，再保存到 context->value_allocator.done中。
            removed_offset = shm_object_pool_remove(&context->value_allocator.doing);
            if (removed_offset == allocator_offset)
            {
                allocator->in_which_pool = SHMCACHE_STRIPING_ALLOCATOR_POOL_DONE;
                shm_object_pool_push(&context->value_allocator.done, allocator_offset);
            }
            else
            {
                logCrit("file: "__FILE__", line: %d, "
                        "shm_object_pool_remove fail, "
                        "offset: %"PRId64" != expect: %"PRId64, __LINE__,
                        removed_offset, allocator_offset);
            }
        }

        //获取 下一个可用的striping_allocator对象的offset
        allocator_offset = shm_object_pool_next(&context->value_allocator.doing);
    }

    return NULL;
}


static int shm_value_allocator_do_recycle(struct shmcache_context *context, struct shm_striping_allocator *allocator)
{
    int64_t allocator_offset;
    if (allocator->in_which_pool == SHMCACHE_STRIPING_ALLOCATOR_POOL_DONE)
    {
        allocator_offset = (char *)allocator - context->segments.hashtable.base;
        if (shm_object_pool_remove_by(&context->value_allocator.done, allocator_offset) >= 0)
        {
            allocator->in_which_pool = SHMCACHE_STRIPING_ALLOCATOR_POOL_DOING;
            shm_object_pool_push(&context->value_allocator.doing, allocator_offset);
        }
        else
        {
            logCrit("file: "__FILE__", line: %d, "
                    "shm_object_pool_remove_by fail, "
                    "index: %d, offset: %"PRId64, __LINE__,
                    allocator->index.striping, allocator_offset);
            return EFAULT;
        }
    }
    return 0;
}

//function: recycle oldest hashtable entries
//recycle_keys_once: recycle key number once when reach max keys
//                   <= 0 means recycle one memory striping
//                   default: 0  当达到最多个数的key时，要回收的key数量
int shm_value_allocator_recycle(struct shmcache_context *context, struct shm_recycle_stats *recycle_stats, const int recycle_keys_once)
{
    int64_t entry_offset;
    int64_t start_time;
    struct shm_hash_entry *entry;
    struct shmcache_key_info key;
    int result;
    int index;
    int clear_count;
    int valid_count;
    bool valid;
    bool recycled;

    result = ENOMEM;
    clear_count = valid_count = 0;
    start_time = get_current_time_us();
    g_current_time = start_time / 1000000;
    recycled = false;        //是否 释放了一个striping_allocator对象的空间

    //bzh: 为什么要按FIFO策略来淘汰key entry？
    //我猜想是因为 这里的任务是释放一个striping_allocator对象的空间，而一个 striping_allocator对象的空间是由 连续的几个key entry来瓜分的，所以需要释放连续的key entry.
    while ((entry_offset=shm_list_first(context)) > 0)    //从头到尾 遍历链表中的结点
    {
        entry = shm_get_hentry_ptr(context, entry_offset);
        index = entry->memory.index.striping;
        key.data = entry->key;
        key.length = entry->key_len;
        valid = HT_ENTRY_IS_VALID(entry, g_current_time);
        if (shm_ht_delete_ex(context, &key, &recycled) != 0)  //删除这个key对应的entry空间
        {
            logError("file: "__FILE__", line: %d, "
                    "shm_ht_delete fail, index: %d, "
                    "entry offset: %"PRId64", "
                    "key: %.*s, key length: %d", __LINE__,
                    index, entry_offset, entry->key_len,
                    entry->key, entry->key_len);

            shm_ht_free_entry(context, entry, entry_offset, &recycled);
        }

        clear_count++;
        if (valid)
        {
            valid_count++;
        }

        if (recycle_keys_once > 0)   //释放了recycle_keys_once个key entry空间，完成任务
        {
            if (clear_count >= recycle_keys_once)
            {
                result = 0;
                break;
            }
        }
        else if (recycled)   //此时 释放了一个striping_allocator对象的空间，完成任务
        {
            logInfo("file: "__FILE__", line: %d, "
                    "recycle #%d striping memory, "
                    "clear total entries: %d, "
                    "clear valid entries: %d, "
                    "time used: %"PRId64" us", __LINE__,
                    index, clear_count, valid_count,
                    get_current_time_us() - start_time);
            result = 0;
            break;
        }
    }

    //修改一些统计数据//////////////////////////////////////////////////
    context->memory->stats.memory.clear_ht_entry.total += clear_count;
    if (valid_count > 0)
    {
        context->memory->stats.memory.clear_ht_entry.valid += valid_count;
    }

    recycle_stats->last_recycle_time = g_current_time;
    recycle_stats->total++;
    if (result == 0)
    {
        recycle_stats->success++;
        if (valid_count > 0)
        {
            recycle_stats->force++;
            if (context->config.va_policy.
                    sleep_us_when_recycle_valid_entries > 0)
            {
                usleep(context->config.va_policy.sleep_us_when_recycle_valid_entries);
            }
        }
    }
    else
    {
        logError("file: "__FILE__", line: %d, "
                "unable to recycle memory, "
                "clear total entries: %d, "
                "cleared valid entries: %d, "
                "time used: %"PRId64" us", __LINE__,
                __LINE__, clear_count, valid_count,
                get_current_time_us() - start_time);
    }
    /////////////////////////////////////////////////////////////////
    return result;
}

struct shm_hash_entry *shm_value_allocator_alloc(struct shmcache_context *context, const int key_len, const int value_len)
{
    int result;
    int size;
    bool recycle;  //是否需要 回收一个striping_allocator对象空间
    int64_t allocator_offset;
    struct shm_striping_allocator *allocator;
    struct shm_hash_entry *entry;

    size = sizeof(struct shm_hash_entry) + MEM_ALIGN(key_len) + MEM_ALIGN(value_len);
    if ((entry=shm_value_allocator_do_alloc(context, size)) != NULL) {
        return entry;
    }

    //此时shm_value_allocator_do_alloc()返回NULL，说明此时 没有可用的striping_allocator对象。
    //需要 淘汰一些key entry => 回收一个striping_allocator对象，或者，向OS申请一块新的shm segment空间.
    if (context->memory->vm_info.segment.count.current >= context->memory->vm_info.segment.count.max)
    {
        recycle = true;
    }
    else
    {
        allocator_offset = shm_object_pool_first(&context->value_allocator.done);
        if (allocator_offset > 0) {
            allocator = (struct shm_striping_allocator *)(context->segments.hashtable.base + allocator_offset);
            recycle = (context->config.va_policy.avg_key_ttl > 0 && g_current_time - allocator->last_alloc_time >= context->config.va_policy.avg_key_ttl);
        } else {
            recycle = false;
        }
    }

    if (recycle) {
        result = shm_value_allocator_recycle(context, &context->memory->stats.memory.recycle.value_striping, -1);
    } else {
        result = shmopt_create_value_segment(context);      //分配一个shm segment
    }
    if (result == 0) {
        entry  = shm_value_allocator_do_alloc(context, size);
    }
    if (entry == NULL) {
        logError("file: "__FILE__", line: %d, "
                "malloc %d bytes from shm fail", __LINE__, size);
    }
    return entry;
}

int shm_value_allocator_free(struct shmcache_context *context, struct shm_hash_entry *entry, bool *recycled)
{
    struct shm_striping_allocator *allocator;
    int64_t used;

    allocator = context->value_allocator.allocators + entry->memory.index.striping;  //获取entry对应的 striping_allocator对象
    used = shm_striping_allocator_free(allocator, entry->memory.size);
    context->memory->usage.used.entry -= entry->memory.size;
    if (used <= 0) {   //若此 striping_allocator已使用的空间为0，则将它 归还到context->value_allocator.doing
        if (used < 0) {
            logError("file: "__FILE__", line: %d, "
                    "striping used memory: %"PRId64" < 0, "
                    "segment: %d, striping: %d, offset: %"PRId64", size: %d",
                    __LINE__, used, entry->memory.index.segment,
                    entry->memory.index.striping, entry->memory.offset,
                    entry->memory.size);
        }
        *recycled = true;
        shm_striping_allocator_reset(allocator);
        return shm_value_allocator_do_recycle(context, allocator);
    }

    return 0;
}
