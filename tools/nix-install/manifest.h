/* Stream profile metadata to a sibling, flush, then atomically publish it.
 * Caller serializes the entire profile mutation, including symlink changes.
 * A failure before rename preserves the original file byte-for-byte. A failure
 * flushing the directory after rename reports failure, but publication occurred.
 */
#ifndef BROOK_NIX_MANIFEST_H
#define BROOK_NIX_MANIFEST_H
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static int manifest_update(const char *path, const char *name,
                           const char *version, const char *store) {
    /* store==NULL means remove. Names are single TSV fields, never paths. */
    if (!name || !*name || strpbrk(name,"\t\r\n") ||
        (store && (!version || strpbrk(version,"\t\r\n") ||
                   !*store || strpbrk(store,"/\t\r\n")))) {
        errno=EINVAL;return -1;
    }
    size_t length=strlen(path);
    char *temporary=malloc(length+16), *directory=strdup(path);
    if (!temporary || !directory) {free(temporary);free(directory);errno=ENOMEM;return -1;}
    snprintf(temporary,length+16,"%s.tmp.XXXXXX",path);
    char *slash=strrchr(directory,'/');
    if (slash) { if (slash==directory) slash[1]=0; else *slash=0; }
    else strcpy(directory,".");
    FILE *input=NULL,*output=NULL;int fd=-1,dir=-1,result=-1,saved=0,found=0,created=0;
    dir=open(directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if (dir<0) goto done;
    fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) {if(errno!=ENOENT || !store)goto done;}
    else {
        struct stat st;
        if (fstat(fd,&st)<0)goto done;
        if (!S_ISREG(st.st_mode)) {errno=EINVAL;goto done;}
        input=fdopen(fd,"r");if(!input)goto done;fd=-1;
    }
    fd=mkstemp(temporary);if(fd<0)goto done;created=1;
    if (fchmod(fd,0644)<0)goto done;
    output=fdopen(fd,"w");if(!output)goto done;fd=-1;
    char line[16384];
    while (input && fgets(line,sizeof(line),input)) {
        size_t n=strlen(line);
        /* Refuse oversized/torn/malformed rows rather than silently discard
         * them or join an append to an unterminated final record. */
        if(n==1 && line[0]=='\n') {if(fwrite(line,1,n,output)!=n)goto done;continue;}
        char *tab=strchr(line,'\t'), *second=tab?strchr(tab+1,'\t'):NULL;
        if (!n || line[n-1]!='\n' || !tab || tab==line || !second || second==line+n-2) {
            errno=EINVAL;goto done;
        }
        int match=(size_t)(tab-line)==strlen(name) && !strncasecmp(line,name,(size_t)(tab-line));
        if (match) {found=1;continue;}
        if (fwrite(line,1,n,output)!=n)goto done;
    }
    if (input && ferror(input))goto done;
    if (!store && !found) {errno=ENOENT;goto done;}
    if (store && fprintf(output,"%s\t%s\t%s\n",name,version,store)<0)goto done;
    if (fflush(output)!=0 || fsync(fileno(output))<0)goto done;
    if (fclose(output)!=0) {output=NULL;goto done;}output=NULL;
    if (rename(temporary,path)<0)goto done;
    if (fsync(dir)<0)goto done;
    result=0;
done:
    saved=errno;
    if(input)fclose(input);
    if(output)fclose(output);
    if(fd>=0)close(fd);
    if(dir>=0)close(dir);
    /* Only a successful mkstemp creates this unpredictable sibling. */
    if(created)unlink(temporary);
    free(temporary);free(directory);errno=saved;return result;
}
#endif
