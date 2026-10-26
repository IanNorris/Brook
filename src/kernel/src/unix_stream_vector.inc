// Included after iovec and the AF_UNIX transport helpers. The caller pins the
// descriptor. Stage only one ring-sized prefix, before taking transport locks:
// validating a shrinking multi-MiB tail on every short write is quadratic.
static int64_t UnixStreamWriteVector(FdEntry* entry,uint64_t address,uint64_t count)
{
    if(count>1024)return -EINVAL;
    if(!count)return 0;
    struct Stage {
        iovec* vectors=nullptr;
        char* bytes=nullptr;
        ~Stage(){if(bytes)kfree(bytes);if(vectors)kfree(vectors);}
    } stage;
    stage.vectors=static_cast<iovec*>(kmalloc(count*sizeof(iovec)));
    if(!stage.vectors)return -ENOMEM;
    if(!CopyFromUser(stage.vectors,address,count*sizeof(iovec)))return -EFAULT;
    uint64_t length=0;
    for(uint64_t i=0;i<count;++i) {
        if(stage.vectors[i].iov_len>0x7fffffffffffffffULL-length)return -EINVAL;
        length+=stage.vectors[i].iov_len;
    }
    if(!length)return 0;
    const uint64_t capacity=length<PIPE_BUF_UNIX_SIZE?length:PIPE_BUF_UNIX_SIZE-1;
    stage.bytes=static_cast<char*>(kmalloc(capacity));
    if(!stage.bytes)return -ENOMEM;
    uint64_t copied=0;
    for(uint64_t i=0;i<count && copied<capacity;++i) {
        uint64_t part=stage.vectors[i].iov_len;
        if(part>capacity-copied)part=capacity-copied;
        if(part && !CopyFromUser(stage.bytes+copied,stage.vectors[i].iov_base,part))
            return -EFAULT;
        copied+=part;
    }
    return UnixSocketWriteSome(static_cast<UnixSocketData*>(entry->handle),
        (entry->statusFlags&0x800)!=0,stage.bytes,copied);
}
