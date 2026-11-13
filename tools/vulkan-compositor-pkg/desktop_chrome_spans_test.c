#include "desktop_chrome_spans.h"
#include <assert.h>
#include <stdio.h>
int main(void){
    struct BrookDesktopLayer chrome={.window=3,.incarnation=9,.width=12,.height=10,.clientX=-4,.clientY=7};
    struct BrookDesktopLayer client={.window=3,.incarnation=9};
    for(int left=-3;left<=15;++left)for(int top=-3;top<=13;++top)
    for(unsigned w=1;w<=14;w+=3)for(unsigned h=1;h<=12;h+=3){
        client.clientX=chrome.clientX+left;client.clientY=chrome.clientY+top;client.clientWidth=w;client.clientHeight=h;
        struct DesktopChromeSpan spans[4];unsigned n=desktop_chrome_spans(&client,&chrome,spans);assert(n<=4);
        for(unsigned y=0;y<chrome.height;++y)for(unsigned x=0;x<chrome.width;++x){
            unsigned coverage=0;for(unsigned i=0;i<n;++i){struct DesktopChromeSpan s=spans[i];
                assert(s.width && s.height && s.x+s.width<=chrome.width && s.y+s.height<=chrome.height);
                coverage+=(x>=s.x && x<s.x+s.width && y>=s.y && y<s.y+s.height);}
            int interior=(int)x>=left && (int)y>=top && (int)x<left+(int)w && (int)y<top+(int)h;
            assert(coverage==(unsigned)!interior);
        }
    }
    client.incarnation=10;struct DesktopChromeSpan spans[4];assert(!desktop_chrome_spans(&client,&chrome,spans));
    {
        uint32_t pixels[14*10],copy[12*10];for(unsigned i=0;i<14*10;++i)pixels[i]=i*17;
        struct DesktopChromeSpan s={3,2,7,5};
        assert(!desktop_chrome_copy_span(pixels,14,12,10,&s,copy,34));
        assert(desktop_chrome_copy_span(pixels,14,12,10,&s,copy,35));
        for(unsigned y=0;y<5;++y)for(unsigned x=0;x<7;++x)assert(copy[y*7+x]==pixels[(y+2)*14+x+3]);
        s.width=10;assert(!desktop_chrome_copy_span(pixels,14,12,10,&s,copy,120));
    }
    puts("DESKTOP_CHROME_SPANS_PASS");return 0;
}
