/* Publish one profile symlink without removing its old destination first.
 * Caller holds the profile mutation lock. This is not a whole-profile
 * transaction: directory fsync failure is reported even after publication.
 */
#ifndef BROOK_PROFILE_LINK_H
#define BROOK_PROFILE_LINK_H
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 1: replaced, 0: already correct, -1: failed (possibly after rename). */
static int profile_link_replace(const char *target,const char *destination) {
    size_t length=strlen(target);
    char *existing=malloc(length+1),*directory=strdup(destination),*temporary=NULL;
    int fd=-1,result=-1,created=0,saved=0;
    static unsigned long sequence;
    if(!existing || !directory) {errno=ENOMEM;goto done;}
    ssize_t n=readlink(destination,existing,length+1);
    if(n>=0 && (size_t)n==length && !memcmp(existing,target,length)) {result=0;goto done;}
    char *slash=strrchr(directory,'/');
    if(slash) {if(slash==directory)slash[1]=0;else *slash=0;}
    else strcpy(directory,".");
    fd=open(directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(fd<0)goto done;
    size_t capacity=strlen(directory)+80;
    temporary=malloc(capacity);
    if(!temporary) {errno=ENOMEM;goto done;}
    /* symlink is exclusive; never unlink a name left by another writer/crash.
     * Fixed-size basename also permits a NAME_MAX-length destination. */
    for(unsigned attempt=0;attempt<128;++attempt) {
        snprintf(temporary,capacity,"%s/.brook-link.%ld.%lu",directory,(long)getpid(),++sequence);
        if(symlink(target,temporary)==0) {created=1;break;}
        if(errno!=EEXIST)goto done;
    }
    if(!created) {errno=EEXIST;goto done;}
    if(rename(temporary,destination)<0)goto done;
    created=0;
    if(fsync(fd)<0)goto done;
    result=1;
done:
    saved=errno;
    if(created)unlink(temporary);
    if(fd>=0)close(fd);
    free(temporary);free(directory);free(existing);errno=saved;
    return result;
}
#endif
