# libshmcache_annotated

这个rep是我在阅读[libshmcache][1]时的一份代码注释。

`libshmcache`的特性：
* 支持多个进程访问本机的共享内存。
* 使用Hashtable，支持快速的set, get, delete。
* 支持一个写者多个读者的场景下无锁访问。
* 多个写者的场景下，用`pthread_mutex`加锁。
* 当写时，如果当前的共享内存空间已满、且已经达到最大值，则会按FIFO策略来淘汰hash entry。
* 以一次次地申请shm segment的方式来分配共享内存，按需向OS申请shm segment。
* key的长度限制在64字节以内。
* 多个写者操作时，如果一个写者加锁后崩溃，会导致`pthread_mutex`一直处于已锁住的状态。`libshmcache`支持在这种场景下的死锁检测和恢复。


### 阅读笔记

#### 存储模型

<img src="segment.png"  width="60%" height="60%" alt="还在路上，稍等..."/>

程序按segment（默认8M）为单位向OS申请共享内存。初始化时，会分配几个segment，然后将每个segment切分成多个striping_allocator空间（默认1M），由striping_allocator来分配单个的hash entry空间。  
hash entry由`struct shm_hash_entry`表示，主要存储hash entry在striping_allocator中的偏移量（准确的说，是基于segment首地址的偏移量）。  
所有striping_allocator对象由`shmcache_value_allocator_context`来管理，它由2个ring queue(`doing`, `done`)分别保存所有空闲的striping_allocator和已分配满的striping_allocator。如下图所示。  

<img src="ring_queue.png"  width="50%" height="50%" alt="还在路上，稍等..."/>

segment是按需分配的。在插入时，若当前striping_allocator分配满了，会从`doing` ring queue中取一个空闲的striping_allocator，然后再分配hash entry空间。若所有striping_allocator都分配满了，则向OS申请一块新的setment。    
Hashtable的实现是正规的开链法。如下图所示，hash桶的个数是`context->memory->hashtable->capacity`，由配置`max_key_count`指定。  

<img src="buckets.png" width="50%" height="50%" alt="还在路上，稍等..."/>

#### get(key)操作



#### set(key,value)操作



#### delete(key)操作



#### 如何保证一个写者多个读者同时进行时的无锁访问？


#### 死锁检测



#### 异常处理


#### 一些疑问
1. 有时候会出现写数据不完整(一个进程写一部分数据后崩溃或被kill掉)的情况，这个时候怎么处理？
直接把整个cache清除掉。因为是cache系统，不保证（承诺）数据持久化，所以万一出现这种bad case，只能把cache清空了。

附：[数据结构图][2]

[1]: https://github.com/happyfish100/libshmcache
[2]: https://github.com/baozh/libshmcache_annotated/blob/master/data_structure.jpeg
[3]: segment.png 
[4]: ring_queue.png
[5]: buckets.png
