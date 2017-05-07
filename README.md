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

#### 1.存储模型
![segment][3]
![ring_queue][4]
![buckets][5]


#### 2.get(key)操作



#### 3.set(key,value)操作



#### 4.delete(key)操作



#### 5.如何保证一个写者多个读者同时进行时的无锁访问？


#### 6.死锁检测



#### 7.异常处理


#### 8.一些疑问
1. 当回收了有效的(未过期)键值对时,休眠 定时 以避免其他进程读到脏数据。  
这个特性没看到。

2. 有时候会出现写数据不完整(一个进程写一部分数据后崩溃或被kill掉)的情况，这个时候怎么处理？
直接把整个cache清除掉。因为是cache系统，不保证（承诺）数据持久化，所以万一出现这种bad case，只能把cache清空了。



附：数据结构图
![数据结构图][2]




[1]: https://github.com/happyfish100/libshmcache
[2]: data_structure.jpeg
[3]: segment.png 
[4]: ring_queue.png
[5]: buckets.png
