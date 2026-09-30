// Host implementation lives in test_vita_sound_worker.cpp; game builds use SDL2.
#pragma once
#include <cstddef>
#define SDLCALL
struct SDL_Thread;
struct SDL_mutex;
struct SDL_cond;
SDL_mutex *SDL_CreateMutex();
void SDL_DestroyMutex(SDL_mutex *);
int SDL_LockMutex(SDL_mutex *);
int SDL_UnlockMutex(SDL_mutex *);
SDL_cond *SDL_CreateCond();
void SDL_DestroyCond(SDL_cond *);
int SDL_CondWait(SDL_cond *, SDL_mutex *);
int SDL_CondSignal(SDL_cond *);
SDL_Thread *SDL_CreateThreadWithStackSize(int (*)(void *), const char *, size_t, void *);
void SDL_WaitThread(SDL_Thread *, int *);
