/* Real SDL backend acceptance: no synthetic SDL_PushEvent, no host input API.
 * Select a relative QEMU mouse, send 10*(200,-100), left-down, another ten,
 * left-up then Escape. The owning controller verifies exit and lineage too. */
#include <SDL.h>
#include <inttypes.h>
#include <stdio.h>
int main(void) {
    if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS)<0) {fprintf(stderr,"SDL_Init: %s\n",SDL_GetError());return 1;}
    SDL_Window *window=SDL_CreateWindow("Brook relative pointer control",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,640,480,0);
    if(!window) {fprintf(stderr,"SDL_CreateWindow: %s\n",SDL_GetError());SDL_Quit();return 1;}
    SDL_Surface *surface=SDL_GetWindowSurface(window);
    if(!surface || SDL_FillRect(surface,NULL,SDL_MapRGB(surface->format,35,75,95))<0 || SDL_UpdateWindowSurface(window)<0) {
        fprintf(stderr,"SDL surface: %s\n",SDL_GetError());SDL_DestroyWindow(window);SDL_Quit();return 1;
    }
    if(SDL_SetRelativeMouseMode(SDL_TRUE)<0) {fprintf(stderr,"relative mode: %s\n",SDL_GetError());SDL_DestroyWindow(window);SDL_Quit();return 1;}
    printf("{\"phase\":\"ready\",\"backend\":\"%s\",\"relative_mode\":%s}\n",SDL_GetCurrentVideoDriver(),SDL_GetRelativeMouseMode()?"true":"false");fflush(stdout);
    int64_t dx=0,dy=0,down_x=0,down_y=0;unsigned downs=0,ups=0,motions=0;int stop=0,exit_code=1;
    Uint64 start=SDL_GetTicks64();
    while(!stop && SDL_GetTicks64()-start<90000) {
        SDL_Event event;
        while(SDL_PollEvent(&event)) {
            if(event.type==SDL_MOUSEMOTION) {dx+=event.motion.xrel;dy+=event.motion.yrel;++motions;}
            if(event.type==SDL_MOUSEBUTTONDOWN && event.button.button==SDL_BUTTON_LEFT) {++downs;down_x=dx;down_y=dy;}
            if(event.type==SDL_MOUSEBUTTONUP && event.button.button==SDL_BUTTON_LEFT)++ups;
            if(event.type==SDL_KEYDOWN && event.key.keysym.sym==SDLK_r) {
                dx=dy=down_x=down_y=0;downs=ups=motions=0;
                puts("{\"phase\":\"armed\"}");fflush(stdout);
            }
            if(event.type==SDL_KEYDOWN && event.key.keysym.sym==SDLK_ESCAPE)stop=1;
            if(event.type==SDL_QUIT)stop=1;
        }
        SDL_Delay(5);
    }
    SDL_SetRelativeMouseMode(SDL_FALSE);
    if(stop && dx==4000 && dy==-2000 && downs==1 && ups==1 && down_x==2000 && down_y==-1000)exit_code=0;
    printf("{\"phase\":\"complete\",\"delta\":[%" PRId64 ",%" PRId64 "],\"down_position\":[%" PRId64 ",%" PRId64 "],\"downs\":%u,\"ups\":%u,\"motions\":%u,\"exit\":%d}\n",dx,dy,down_x,down_y,downs,ups,motions,exit_code);fflush(stdout);
    SDL_DestroyWindow(window);SDL_Quit();return exit_code;
}
