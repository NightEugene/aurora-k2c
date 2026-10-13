#include "android_compat.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>

static void *(*prepare)(int);
static void *(*for_thread)(void);
static int (*poll_once)(int, int *, int *, void **);
static void (*post)(void *, void (*)(int64_t, void *), void *);
static int frames;
static int64_t last_frame;
static void frame(int64_t timestamp, void *looper)
{
    assert(timestamp > last_frame);
    last_frame = timestamp;
    if (++frames < 3) post(looper, frame, looper);
}
static int fd_callback(int fd, int events, void *data)
{
    char c;
    assert(events & 1);
    assert(read(fd, &c, 1) == 1 && c == 'x');
    ++*(int *)data;
    return 0;
}
static void *other_thread(void *parent)
{
    assert(for_thread() == NULL);
    assert(prepare(0) != parent);
    assert(for_thread() == prepare(0));
    return NULL;
}
int main(void)
{
    assert(k2c_android_fallback_symbol("ALooper_prepare") == NULL);
    k2c_android_fallback_enable();
    prepare = k2c_android_fallback_symbol("ALooper_prepare");
    for_thread = k2c_android_fallback_symbol("ALooper_forThread");
    poll_once = k2c_android_fallback_symbol("ALooper_pollOnce");
    post = k2c_android_fallback_symbol("AChoreographer_postFrameCallback64");
    void *l = prepare(1);
    assert(l && l == for_thread());
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, other_thread, l));
    pthread_join(worker, NULL);
    post(l, frame, l);
    for (int i = 0; i < 20 && frames < 3; ++i) poll_once(50, NULL, NULL, NULL);
    assert(frames == 3);
    int (*add)(void *, int, int, int, int (*)(int, int, void *), void *) =
        k2c_android_fallback_symbol("ALooper_addFd");
    int (*remove_fd)(void *, int) = k2c_android_fallback_symbol("ALooper_removeFd");
    int pipe_fds[2], callbacks = 0;
    assert(!pipe(pipe_fds));
    assert(add(l, pipe_fds[0], 42, 1, fd_callback, &callbacks) == 1);
    assert(write(pipe_fds[1], "x", 1) == 1);
    assert(poll_once(100, NULL, NULL, NULL) == -2);
    assert(callbacks == 1 && remove_fd(l, pipe_fds[0]) == 0);
    assert(add(l, pipe_fds[0], 42, 1, NULL, &callbacks) == 1);
    assert(write(pipe_fds[1], "x", 1) == 1);
    int fd, events; void *data;
    assert(poll_once(100, &fd, &events, &data) == 42);
    assert(fd == pipe_fds[0] && (events & 1) && data == &callbacks);
    assert(remove_fd(l, fd) == 1);
    close(pipe_fds[0]); close(pipe_fds[1]);
    poll_once(0, NULL, NULL, NULL); /* consume wake from removeFd */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    assert(poll_once(20, NULL, NULL, NULL) == -4);
    clock_gettime(CLOCK_MONOTONIC, &end);
    assert((end.tv_sec - start.tv_sec) * 1000000000LL + end.tv_nsec - start.tv_nsec >= 15000000);
    puts("Android fallback: thread isolation, frame rescheduling, FD callbacks, identifiers and timeout passed");
}
