#include "source_size_policy.h"
#include <assert.h>
#include <stdio.h>
int main(void){
    unsigned small=0;
    assert(desktop_source_should_resize(0,1024,&small));
    assert(desktop_source_should_resize(4096,4097,&small));
    for(unsigned i=0;i<100;++i)assert(!desktop_source_should_resize(4096,1,&small));
    for(unsigned i=0;i<59;++i)assert(!desktop_source_should_resize(16384,4096,&small));
    assert(desktop_source_should_resize(16384,4096,&small) && !small);
    for(unsigned i=0;i<40;++i)assert(!desktop_source_should_resize(16384,4096,&small));
    assert(!desktop_source_should_resize(16384,4097,&small) && !small);
    assert(!desktop_source_should_resize(16384,0,&small));
    puts("DESKTOP_SOURCE_SIZE_POLICY_PASS");return 0;
}
