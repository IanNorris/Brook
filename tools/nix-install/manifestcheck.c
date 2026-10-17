/* Run against a private directory on /data and /nix; no real profile mutation. */
#define _GNU_SOURCE
#include "manifest.h"
#include <limits.h>
static int failures;
#define CHECK(test) do {if(!(test)){printf("MANIFESTCHECK FAIL line=%d errno=%d: %s\n",__LINE__,errno,#test);++failures;}}while(0)
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    char directory[PATH_MAX-32],path[PATH_MAX],target[PATH_MAX];
    if(snprintf(directory,sizeof(directory),"%s/manifestcheck-%ld",argv[1],(long)getpid())>=(int)sizeof(directory)-32)return 2;
    if(mkdir(directory,0700)<0){perror("private fixture");return 1;}
    snprintf(path,sizeof(path),"%s/manifest.tsv",directory);
    snprintf(target,sizeof(target),"%s/target",directory);
    FILE *f=fopen(path,"w");CHECK(f!=NULL);if(!f)return 1;
    for(unsigned i=0;i<1000;++i)fprintf(f,"package-%u\t1\thash-%u\n",i,i);
    CHECK(fclose(f)==0);
    CHECK(manifest_update(path,"package-500",NULL,NULL)==0);
    f=fopen(path,"r");CHECK(f!=NULL);if(!f)return 1;
    char line[128],expected[128];unsigned count=0;
    for(unsigned i=0;i<1000;++i) {
        if(i==500)continue;
        snprintf(expected,sizeof(expected),"package-%u\t1\thash-%u\n",i,i);
        CHECK(fgets(line,sizeof(line),f)!=NULL && !strcmp(line,expected));++count;
    }
    CHECK(fgets(line,sizeof(line),f)==NULL && !ferror(f));fclose(f);
    printf("MANIFESTCHECK bulk-preserved=%u\n",count);
    CHECK(manifest_update(path,"viewer","2","hash-viewer")==0);
    CHECK(manifest_update(path,"VIEWER",NULL,NULL)==0);
    CHECK(manifest_update(path,"absent",NULL,NULL)<0 && errno==ENOENT);
    f=fopen(path,"w");CHECK(f!=NULL);if(!f)return 1;
    fputs("torn\t1\thash-torn",f);fclose(f);
    CHECK(manifest_update(path,"viewer","2","hash-viewer")<0 && errno==EINVAL);
    f=fopen(path,"r");CHECK(f!=NULL);if(!f)return 1;
    CHECK(fgets(line,sizeof(line),f)!=NULL && !strcmp(line,"torn\t1\thash-torn"));fclose(f);
    CHECK(rename(path,target)==0);CHECK(symlink("target",path)==0);
    CHECK(manifest_update(path,"viewer","2","hash-viewer")<0);
    struct stat st;CHECK(lstat(path,&st)==0 && S_ISLNK(st.st_mode));
    CHECK(unlink(path)==0);CHECK(unlink(target)==0);CHECK(rmdir(directory)==0);
    printf("MANIFESTCHECK %s uid=%ld mount=%s failures=%d\n",failures?"FAIL":"PASS",(long)geteuid(),argv[1],failures);fflush(stdout);
    return failures?1:0;
}
