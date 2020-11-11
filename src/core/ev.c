/*
* Copyright (c) 2020 Calvin Rose
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to
* deal in the Software without restriction, including without limitation the
* rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
* sell copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*/

#ifndef JANET_AMALG
#include "features.h"
#include <janet.h>
#include "util.h"
#include "gc.h"
#include "state.h"
#include "fiber.h"
#endif

#ifdef JANET_EV

/* Includes */

#ifdef JANET_WINDOWS

#include <windows.h>
#include <math.h>

#else

#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <fcntl.h>

#ifdef JANET_BSD
#include <netinet/in.h>
#endif

#ifdef JANET_EV_EPOLL
#include <sys/epoll.h>
#include <sys/timerfd.h>
#endif

#endif

/* General queue */

/* Ring buffer for storing a list of fibers */
typedef struct {
    int32_t capacity;
    int32_t head;
    int32_t tail;
    void *data;
} JanetQueue;

#define JANET_MAX_Q_CAPACITY 0x7FFFFFF

static void janet_q_init(JanetQueue *q) {
    q->data = NULL;
    q->head = 0;
    q->tail = 0;
    q->capacity = 0;
}

static void janet_q_deinit(JanetQueue *q) {
    free(q->data);
}

static int32_t janet_q_count(JanetQueue *q) {
    return (q->head > q->tail)
           ? (q->tail + q->capacity - q->head)
           : (q->tail - q->head);
}

static int janet_q_push(JanetQueue *q, void *item, size_t itemsize) {
    int32_t count = janet_q_count(q);
    /* Resize if needed */
    if (count + 1 >= q->capacity) {
        if (count + 1 >= JANET_MAX_Q_CAPACITY) return 1;
        int32_t newcap = (count + 2) * 2;
        if (newcap > JANET_MAX_Q_CAPACITY) newcap = JANET_MAX_Q_CAPACITY;
        q->data = realloc(q->data, itemsize * newcap);
        if (NULL == q->data) {
            JANET_OUT_OF_MEMORY;
        }
        if (q->head > q->tail) {
            /* Two segments, fix 2nd seg. */
            int32_t newhead = q->head + (newcap - q->capacity);
            size_t seg1 = (size_t)(q->capacity - q->head);
            if (seg1 > 0) {
                memmove((char *) q->data + (newhead * itemsize),
                        (char *) q->data + (q->head * itemsize),
                        seg1 * itemsize);
            }
            q->head = newhead;
        }
        q->capacity = newcap;
    }
    memcpy((char *) q->data + itemsize * q->tail, item, itemsize);
    q->tail = q->tail + 1 < q->capacity ? q->tail + 1 : 0;
    return 0;
}

static int janet_q_pop(JanetQueue *q, void *out, size_t itemsize) {
    if (q->head == q->tail) return 1;
    memcpy(out, (char *) q->data + itemsize * q->head, itemsize);
    q->head = q->head + 1 < q->capacity ? q->head + 1 : 0;
    return 0;
}

/* New fibers to spawn or resume */
typedef struct JanetTask JanetTask;
struct JanetTask {
    JanetFiber *fiber;
    Janet value;
    JanetSignal sig;
};

/* Min priority queue of timestamps for timeouts. */
typedef int64_t JanetTimestamp;
typedef struct JanetTimeout JanetTimeout;
struct JanetTimeout {
    JanetTimestamp when;
    JanetFiber *fiber;
    uint32_t sched_id;
    int is_error;
};

/* Forward declaration */
static void janet_unlisten(JanetListenerState *state);

/* Global data */
JANET_THREAD_LOCAL size_t janet_vm_active_listeners = 0;
JANET_THREAD_LOCAL size_t janet_vm_tq_count = 0;
JANET_THREAD_LOCAL size_t janet_vm_tq_capacity = 0;
JANET_THREAD_LOCAL JanetQueue janet_vm_spawn;
JANET_THREAD_LOCAL JanetTimeout *janet_vm_tq = NULL;
JANET_THREAD_LOCAL JanetRNG janet_vm_ev_rng;

/* Get current timestamp (millisecond precision) */
static JanetTimestamp ts_now(void);

/* Get current timestamp + an interval (millisecond precision) */
static JanetTimestamp ts_delta(JanetTimestamp ts, double delta) {
    ts += (int64_t)round(delta * 1000);
    return ts;
}

/* Look at the next timeout value without
 * removing it. */
static int peek_timeout(JanetTimeout *out) {
    if (janet_vm_tq_count == 0) return 0;
    *out = janet_vm_tq[0];
    return 1;
}

/* Remove the next timeout from the priority queue */
static void pop_timeout(size_t index) {
    if (janet_vm_tq_count <= index) return;
    janet_vm_tq[index] = janet_vm_tq[--janet_vm_tq_count];
    for (;;) {
        size_t left = (index << 1) + 1;
        size_t right = left + 1;
        size_t smallest = index;
        if (left < janet_vm_tq_count &&
                (janet_vm_tq[left].when < janet_vm_tq[smallest].when))
            smallest = left;
        if (right < janet_vm_tq_count &&
                (janet_vm_tq[right].when < janet_vm_tq[smallest].when))
            smallest = right;
        if (smallest == index) return;
        JanetTimeout temp = janet_vm_tq[index];
        janet_vm_tq[index] = janet_vm_tq[smallest];
        janet_vm_tq[smallest] = temp;
        index = smallest;
    }
}

/* Add a timeout to the timeout min heap */
static void add_timeout(JanetTimeout to) {
    size_t oldcount = janet_vm_tq_count;
    size_t newcount = oldcount + 1;
    if (newcount > janet_vm_tq_capacity) {
        size_t newcap = 2 * newcount;
        JanetTimeout *tq = realloc(janet_vm_tq, newcap * sizeof(JanetTimeout));
        if (NULL == tq) {
            JANET_OUT_OF_MEMORY;
        }
        janet_vm_tq = tq;
        janet_vm_tq_capacity = newcap;
    }
    /* Append */
    janet_vm_tq_count = (int32_t) newcount;
    janet_vm_tq[oldcount] = to;
    /* Heapify */
    size_t index = oldcount;
    while (index > 0) {
        size_t parent = (index - 1) >> 1;
        if (janet_vm_tq[parent].when <= janet_vm_tq[index].when) break;
        /* Swap */
        JanetTimeout tmp = janet_vm_tq[index];
        janet_vm_tq[index] = janet_vm_tq[parent];
        janet_vm_tq[parent] = tmp;
        /* Next */
        index = parent;
    }
}

/* Create a new event listener */
static JanetListenerState *janet_listen_impl(JanetPollable *pollable, JanetListener behavior, int mask, size_t size, void *user) {
    if (pollable->_mask & mask) {
        janet_panic("cannot listen for duplicate event on pollable");
    }
    if (janet_vm_root_fiber->waiting != NULL) {
        janet_panic("current fiber is already waiting for event");
    }
    if (size < sizeof(JanetListenerState))
        size = sizeof(JanetListenerState);
    JanetListenerState *state = malloc(size);
    if (NULL == state) {
        JANET_OUT_OF_MEMORY;
    }
    state->machine = behavior;
    if (mask & JANET_ASYNC_LISTEN_SPAWNER) {
        state->fiber = NULL;
    } else {
        state->fiber = janet_vm_root_fiber;
        janet_vm_root_fiber->waiting = state;
    }
    mask |= JANET_ASYNC_LISTEN_SPAWNER;
    state->pollable = pollable;
    state->_mask = mask;
    state->_index = 0;
    pollable->_mask |= mask;
    janet_vm_active_listeners++;
    /* Prepend to linked list */
    state->_next = pollable->state;
    pollable->state = state;
    /* Emit INIT event for convenience */
    state->event = user;
    state->machine(state, JANET_ASYNC_EVENT_INIT);
    return state;
}

/* Indicate we are no longer listening for an event. This
 * frees the memory of the state machine as well. */
static void janet_unlisten_impl(JanetListenerState *state) {
    state->machine(state, JANET_ASYNC_EVENT_DEINIT);
    /* Remove state machine from poll list */
    JanetListenerState **iter = &(state->pollable->state);
    while (*iter && *iter != state)
        iter = &((*iter)->_next);
    janet_assert(*iter, "failed to remove listener");
    *iter = state->_next;
    janet_vm_active_listeners--;
    /* Remove mask */
    state->pollable->_mask &= ~(state->_mask);
    /* Ensure fiber does not reference this state */
    JanetFiber *fiber = state->fiber;
    if (NULL != fiber && fiber->waiting == state) {
        fiber->waiting = NULL;
    }
    free(state);
}

/* Call after creating a pollable */
void janet_pollable_init(JanetPollable *pollable, JanetHandle handle) {
    pollable->handle = handle;
    pollable->flags = 0;
    pollable->state = NULL;
    pollable->_mask = 0;
}

/* Mark a pollable for GC */
void janet_pollable_mark(JanetPollable *pollable) {
    JanetListenerState *state = pollable->state;
    while (NULL != state) {
        if (NULL != state->fiber) {
            janet_mark(janet_wrap_fiber(state->fiber));
        }
        (state->machine)(state, JANET_ASYNC_EVENT_MARK);
        state = state->_next;
    }
}

/* Must be called to close all pollables - does NOT call `close` for you.
 * Also does not free memory of the pollable, so can be used on close. */
void janet_pollable_deinit(JanetPollable *pollable) {
    pollable->flags |= JANET_POLL_FLAG_CLOSED;
    JanetListenerState *state = pollable->state;
    while (NULL != state) {
        state->machine(state, JANET_ASYNC_EVENT_CLOSE);
        JanetListenerState *next_state = state->_next;
        janet_unlisten_impl(state);
        state = next_state;
    }
    pollable->state = NULL;
}

/* Register a fiber to resume with value */
void janet_schedule_signal(JanetFiber *fiber, Janet value, JanetSignal sig) {
    if (fiber->flags & JANET_FIBER_FLAG_SCHEDULED) return;
    fiber->flags |= JANET_FIBER_FLAG_SCHEDULED;
    fiber->sched_id++;
    JanetTask t = { fiber, value, sig };
    janet_q_push(&janet_vm_spawn, &t, sizeof(t));
}

void janet_cancel(JanetFiber *fiber, Janet value) {
    janet_schedule_signal(fiber, value, JANET_SIGNAL_ERROR);
}

void janet_schedule(JanetFiber *fiber, Janet value) {
    janet_schedule_signal(fiber, value, JANET_SIGNAL_OK);
}

void janet_fiber_did_resume(JanetFiber *fiber) {
    /* Cancel any pending fibers */
    if (fiber->waiting) janet_unlisten(fiber->waiting);
}

/* Mark all pending tasks */
void janet_ev_mark(void) {
    JanetTask *tasks = janet_vm_spawn.data;
    if (janet_vm_spawn.head <= janet_vm_spawn.tail) {
        for (int32_t i = janet_vm_spawn.head; i < janet_vm_spawn.tail; i++) {
            janet_mark(janet_wrap_fiber(tasks[i].fiber));
            janet_mark(tasks[i].value);
        }
    } else {
        for (int32_t i = janet_vm_spawn.head; i < janet_vm_spawn.capacity; i++) {
            janet_mark(janet_wrap_fiber(tasks[i].fiber));
            janet_mark(tasks[i].value);
        }
        for (int32_t i = 0; i < janet_vm_spawn.tail; i++) {
            janet_mark(janet_wrap_fiber(tasks[i].fiber));
            janet_mark(tasks[i].value);
        }
    }
    for (size_t i = 0; i < janet_vm_tq_count; i++) {
        janet_mark(janet_wrap_fiber(janet_vm_tq[i].fiber));
    }
}

/* Run a top level task */
static void run_one(JanetFiber *fiber, Janet value, JanetSignal sigin) {
    fiber->flags &= ~JANET_FIBER_FLAG_SCHEDULED;
    Janet res;
    JanetSignal sig = janet_continue_signal(fiber, value, &res, sigin);
    if (sig != JANET_SIGNAL_OK && sig != JANET_SIGNAL_EVENT) {
        janet_stacktrace(fiber, res);
    }
}

/* Common init code */
void janet_ev_init_common(void) {
    janet_q_init(&janet_vm_spawn);
    janet_vm_active_listeners = 0;
    janet_vm_tq = NULL;
    janet_vm_tq_count = 0;
    janet_vm_tq_capacity = 0;
    janet_rng_seed(&janet_vm_ev_rng, 0);
}

/* Common deinit code */
void janet_ev_deinit_common(void) {
    janet_q_deinit(&janet_vm_spawn);
}

/* Short hand to yield to event loop */
void janet_await(void) {
    janet_signalv(JANET_SIGNAL_EVENT, janet_wrap_nil());
}

/* Set timeout for the current root fiber */
void janet_addtimeout(double sec) {
    JanetFiber *fiber = janet_vm_root_fiber;
    JanetTimeout to;
    to.when = ts_delta(ts_now(), sec);
    to.fiber = fiber;
    to.sched_id = fiber->sched_id;
    to.is_error = 1;
    add_timeout(to);
}

/* Channels */

typedef struct {
    JanetFiber *fiber;
    uint32_t sched_id;
    enum {
        JANET_CP_MODE_ITEM,
        JANET_CP_MODE_CHOICE_READ,
        JANET_CP_MODE_CHOICE_WRITE
    } mode;
} JanetChannelPending;

typedef struct {
    JanetQueue items;
    JanetQueue read_pending;
    JanetQueue write_pending;
    int32_t limit;
} JanetChannel;

#define JANET_MAX_CHANNEL_CAPACITY 0xFFFFFF

static void janet_chan_init(JanetChannel *chan, int32_t limit) {
    chan->limit = limit;
    janet_q_init(&chan->items);
    janet_q_init(&chan->read_pending);
    janet_q_init(&chan->write_pending);
}

static void janet_chan_deinit(JanetChannel *chan) {
    janet_q_deinit(&chan->read_pending);
    janet_q_deinit(&chan->write_pending);
    janet_q_deinit(&chan->items);
}

/*
 * Janet Channel abstract type
 */

/*static int janet_chanat_get(void *p, Janet key, Janet *out);*/
static int janet_chanat_mark(void *p, size_t s);
static int janet_chanat_gc(void *p, size_t s);

static const JanetAbstractType ChannelAT = {
    "core/channel",
    janet_chanat_gc,
    janet_chanat_mark,
    NULL, /* janet_chanat_get */
    JANET_ATEND_GET
};

static int janet_chanat_gc(void *p, size_t s) {
    (void) s;
    JanetChannel *channel = p;
    janet_chan_deinit(channel);
    return 0;
}

static void janet_chanat_mark_fq(JanetQueue *fq) {
    JanetChannelPending *pending = fq->data;
    if (fq->head <= fq->tail) {
        for (int32_t i = fq->head; i < fq->tail; i++)
            janet_mark(janet_wrap_fiber(pending[i].fiber));
    } else {
        for (int32_t i = fq->head; i < fq->capacity; i++)
            janet_mark(janet_wrap_fiber(pending[i].fiber));
        for (int32_t i = 0; i < fq->tail; i++)
            janet_mark(janet_wrap_fiber(pending[i].fiber));
    }
}

static int janet_chanat_mark(void *p, size_t s) {
    (void) s;
    JanetChannel *chan = p;
    janet_chanat_mark_fq(&chan->read_pending);
    janet_chanat_mark_fq(&chan->write_pending);
    JanetQueue *items = &chan->items;
    Janet *data = chan->items.data;
    if (items->head <= items->tail) {
        for (int32_t i = items->head; i < items->tail; i++)
            janet_mark(data[i]);
    } else {
        for (int32_t i = items->head; i < items->capacity; i++)
            janet_mark(data[i]);
        for (int32_t i = 0; i < items->tail; i++)
            janet_mark(data[i]);
    }
    return 0;
}

static Janet make_write_result(JanetChannel *channel) {
    Janet *tup = janet_tuple_begin(2);
    tup[0] = janet_ckeywordv("give");
    tup[1] = janet_wrap_abstract(channel);
    return janet_wrap_tuple(janet_tuple_end(tup));
}

static Janet make_read_result(JanetChannel *channel, Janet x) {
    Janet *tup = janet_tuple_begin(3);
    tup[0] = janet_ckeywordv("take");
    tup[1] = janet_wrap_abstract(channel);
    tup[2] = x;
    return janet_wrap_tuple(janet_tuple_end(tup));
}

/* Push a value to a channel, and return 1 if channel should block, zero otherwise.
 * If the push would block, will add to the write_pending queue in the channel. */
static int janet_channel_push(JanetChannel *channel, Janet x, int is_choice) {
    JanetChannelPending reader;
    int is_empty;
    do {
        is_empty = janet_q_pop(&channel->read_pending, &reader, sizeof(reader));
    } while (!is_empty && (reader.sched_id != reader.fiber->sched_id));
    if (is_empty) {
        /* No pending reader */
        if (janet_q_push(&channel->items, &x, sizeof(Janet))) {
            janet_panicf("channel overflow: %v", x);
        } else if (janet_q_count(&channel->items) > channel->limit) {
            /* Pushed successfully, but should block. */
            JanetChannelPending pending;
            pending.fiber = janet_vm_root_fiber,
            pending.sched_id = janet_vm_root_fiber->sched_id,
            pending.mode = is_choice ? JANET_CP_MODE_CHOICE_WRITE : JANET_CP_MODE_ITEM;
            janet_q_push(&channel->write_pending, &pending, sizeof(pending));
            return 1;
        }
    } else {
        /* Pending reader */
        if (reader.mode == JANET_CP_MODE_CHOICE_READ) {
            janet_schedule(reader.fiber, make_read_result(channel, x));
        } else {
            janet_schedule(reader.fiber, x);
        }
    }
    return 0;
}

/* Pop from a channel - returns 1 if item was obtain, 0 otherwise. The item
 * is returned by reference. If the pop would block, will add to the read_pending
 * queue in the channel. */
static int janet_channel_pop(JanetChannel *channel, Janet *item, int is_choice) {
    JanetChannelPending writer;
    if (janet_q_pop(&channel->items, item, sizeof(Janet))) {
        /* Queue empty */
        JanetChannelPending pending;
        pending.fiber = janet_vm_root_fiber,
        pending.sched_id = janet_vm_root_fiber->sched_id;
        pending.mode = is_choice ? JANET_CP_MODE_CHOICE_READ : JANET_CP_MODE_ITEM;
        janet_q_push(&channel->read_pending, &pending, sizeof(pending));
        return 0;
    }
    if (!janet_q_pop(&channel->write_pending, &writer, sizeof(writer))) {
        /* pending writer */
        if (writer.mode == JANET_CP_MODE_CHOICE_WRITE) {
            janet_schedule(writer.fiber, make_write_result(channel));
        } else {
            janet_schedule(writer.fiber, janet_wrap_abstract(channel));
        }
    }
    return 1;
}

/* Channel Methods */

static Janet cfun_channel_push(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);
    JanetChannel *channel = janet_getabstract(argv, 0, &ChannelAT);
    if (janet_channel_push(channel, argv[1], 0)) {
        janet_await();
    }
    return argv[0];
}

static Janet cfun_channel_pop(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    JanetChannel *channel = janet_getabstract(argv, 0, &ChannelAT);
    Janet item;
    if (janet_channel_pop(channel, &item, 0)) {
        janet_schedule(janet_vm_root_fiber, item);
    }
    janet_await();
}

static Janet cfun_channel_choice(int32_t argc, Janet *argv) {
    janet_arity(argc, 1, -1);
    int32_t len;
    const Janet *data;

    /* Check channels for immediate reads and writes */
    for (int32_t i = 0; i < argc; i++) {
        if (janet_indexed_view(argv[i], &data, &len) && len == 2) {
            /* Write */
            JanetChannel *chan = janet_getabstract(data, 0, &ChannelAT);
            if (janet_q_count(&chan->items) < chan->limit) {
                janet_channel_push(chan, data[1], 1);
                return make_write_result(chan);
            }
        } else {
            /* Read */
            JanetChannel *chan = janet_getabstract(argv, i, &ChannelAT);
            if (chan->items.head != chan->items.tail) {
                Janet item;
                janet_channel_pop(chan, &item, 1);
                return make_read_result(chan, item);
            }
        }
    }

    /* Wait for all readers or writers */
    for (int32_t i = 0; i < argc; i++) {
        if (janet_indexed_view(argv[i], &data, &len) && len == 2) {
            /* Write */
            JanetChannel *chan = janet_getabstract(data, 0, &ChannelAT);
            janet_channel_push(chan, data[1], 1);
        } else {
            /* Read */
            Janet item;
            JanetChannel *chan = janet_getabstract(argv, i, &ChannelAT);
            janet_channel_pop(chan, &item, 1);
        }
    }

    janet_await();
}

static Janet cfun_channel_full(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    JanetChannel *channel = janet_getabstract(argv, 0, &ChannelAT);
    return janet_wrap_boolean(janet_q_count(&channel->items) >= channel->limit);
}

static Janet cfun_channel_capacity(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    JanetChannel *channel = janet_getabstract(argv, 0, &ChannelAT);
    return janet_wrap_integer(channel->limit);
}

static Janet cfun_channel_count(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    JanetChannel *channel = janet_getabstract(argv, 0, &ChannelAT);
    return janet_wrap_integer(janet_q_count(&channel->items));
}

/* Fisher yates shuffle of arguments to get fairness */
static void fisher_yates_args(int32_t argc, Janet *argv) {
    for (int32_t i = argc; i > 1; i--) {
        int32_t swap_index = janet_rng_u32(&janet_vm_ev_rng) % i;
        Janet temp = argv[swap_index];
        argv[swap_index] = argv[i - 1];
        argv[i - 1] = temp;
    }
}

static Janet cfun_channel_rchoice(int32_t argc, Janet *argv) {
    fisher_yates_args(argc, argv);
    return cfun_channel_choice(argc, argv);
}

static Janet cfun_channel_new(int32_t argc, Janet *argv) {
    janet_arity(argc, 0, 1);
    int32_t limit = janet_optnat(argv, argc, 0, 0);
    JanetChannel *channel = janet_abstract(&ChannelAT, sizeof(JanetChannel));
    janet_chan_init(channel, limit);
    return janet_wrap_abstract(channel);
}

/* Main event loop */

void janet_loop1_impl(int has_timeout, JanetTimestamp timeout);

void janet_loop1(void) {
    /* Schedule expired timers */
    JanetTimeout to;
    JanetTimestamp now = ts_now();
    while (peek_timeout(&to) && to.when <= now) {
        pop_timeout(0);
        if (to.fiber->sched_id == to.sched_id) {
            if (to.is_error) {
                janet_cancel(to.fiber, janet_cstringv("timeout"));
            } else {
                janet_schedule(to.fiber, janet_wrap_nil());
            }
        }
    }
    /* Run scheduled fibers */
    while (janet_vm_spawn.head != janet_vm_spawn.tail) {
        JanetTask task = {NULL, janet_wrap_nil(), JANET_SIGNAL_OK};
        janet_q_pop(&janet_vm_spawn, &task, sizeof(task));
        run_one(task.fiber, task.value, task.sig);
    }
    /* Poll for events */
    if (janet_vm_active_listeners || janet_vm_tq_count) {
        JanetTimeout to;
        memset(&to, 0, sizeof(to));
        int has_timeout;
        /* Drop timeouts that are no longer needed */
        while ((has_timeout = peek_timeout(&to)) && to.fiber->sched_id != to.sched_id) {
            pop_timeout(0);
        }
        /* Run polling implementation */
        janet_loop1_impl(has_timeout, to.when);
    }
}

void janet_loop(void) {
    while (janet_vm_active_listeners || (janet_vm_spawn.head != janet_vm_spawn.tail) || janet_vm_tq_count) {
        janet_loop1();
    }
}

#ifdef JANET_WINDOWS

/* Epoll global data */
JANET_THREAD_LOCAL HANDLE janet_vm_iocp = NULL;

static JanetTimestamp ts_now(void) {
    return (JanetTimestamp) GetTickCount64();
}

void janet_ev_init(void) {
    janet_ev_init_common();
    janet_vm_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (NULL == janet_vm_iocp) janet_panic("could not create io completion port");
}

void janet_ev_deinit(void) {
    janet_ev_deinit_common();
    CloseHandle(janet_vm_iocp);
}

JanetListenerState *janet_listen(JanetPollable *pollable, JanetListener behavior, int mask, size_t size, void *user) {
    /* Add the handle to the io completion port if not already added */
    JanetListenerState *state = janet_listen_impl(pollable, behavior, mask, size, user);
    if (!(pollable->flags & JANET_POLL_FLAG_IOCP)) {
        if (NULL == CreateIoCompletionPort(pollable->handle, janet_vm_iocp, (ULONG_PTR) pollable, 0)) {
            janet_panic("failed to listen for events");
        }
        pollable->flags |= JANET_POLL_FLAG_IOCP;
    }
    return state;
}


static void janet_unlisten(JanetListenerState *state) {
    janet_unlisten_impl(state);
}

void janet_loop1_impl(int has_timeout, JanetTimestamp to) {
    ULONG_PTR completionKey = 0;
    DWORD num_bytes_transfered = 0;
    LPOVERLAPPED overlapped;

    /* Calculate how long to wait before timeout */
    uint64_t waittime;
    if (has_timeout) {
        JanetTimestamp now = ts_now();
        if (now > to) {
            waittime = 0;
        } else {
            waittime = (uint64_t)(to - now);
        }
    } else {
        waittime = INFINITE;
    }
    BOOL result = GetQueuedCompletionStatus(janet_vm_iocp, &num_bytes_transfered, &completionKey, &overlapped, (DWORD) waittime);

    if (!result) {
        if (!has_timeout) {
            /* queue emptied */
        }
    } else {
        /* Normal event */
        JanetPollable *pollable = (JanetPollable *) completionKey;
        JanetListenerState *state = pollable->state;
        while (state != NULL) {
            if (state->tag == overlapped) {
                state->event = overlapped;
                state->bytes = num_bytes_transfered;
                JanetAsyncStatus status = state->machine(state, JANET_ASYNC_EVENT_COMPLETE);
                if (status == JANET_ASYNC_STATUS_DONE) {
                    janet_unlisten(state);
                }
                break;
            } else {
                state = state->_next;
            }
        }
    }
}

#elif defined(JANET_EV_POLL)

/*
 * Start linux/epoll implementation
 */

static JanetTimestamp ts_now(void) {
    struct timespec now;
    janet_assert(-1 != clock_gettime(CLOCK_MONOTONIC, &now), "failed to get time");
    uint64_t res = 1000 * now.tv_sec;
    res += now.tv_nsec / 1000000;
    return res;
}

/* Epoll global data */
JANET_THREAD_LOCAL int janet_vm_epoll = 0;
JANET_THREAD_LOCAL int janet_vm_timerfd = 0;
JANET_THREAD_LOCAL int janet_vm_timer_enabled = 0;

static int make_epoll_events(int mask) {
    int events = EPOLLET;
    if (mask & JANET_ASYNC_LISTEN_READ)
        events |= EPOLLIN;
    if (mask & JANET_ASYNC_LISTEN_WRITE)
        events |= EPOLLOUT;
    return events;
}

/* Wait for the next event */
JanetListenerState *janet_listen(JanetPollable *pollable, JanetListener behavior, int mask, size_t size, void *user) {
    int is_first = !(pollable->state);
    int op = is_first ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
    JanetListenerState *state = janet_listen_impl(pollable, behavior, mask, size, user);
    struct epoll_event ev;
    ev.events = make_epoll_events(state->pollable->_mask);
    ev.data.ptr = pollable;
    int status;
    do {
        status = epoll_ctl(janet_vm_epoll, op, pollable->handle, &ev);
    } while (status == -1 && errno == EINTR);
    if (status == -1) {
        janet_unlisten_impl(state);
        janet_panicf("failed to schedule event: %s", strerror(errno));
    }
    return state;
}

/* Tell system we are done listening for a certain event */
static void janet_unlisten(JanetListenerState *state) {
    JanetPollable *pollable = state->pollable;
    int is_last = (state->_next == NULL && pollable->state == state);
    int op = is_last ? EPOLL_CTL_DEL : EPOLL_CTL_MOD;
    struct epoll_event ev;
    ev.events = make_epoll_events(pollable->_mask & ~state->_mask);
    ev.data.ptr = pollable;
    int status;
    do {
        status = epoll_ctl(janet_vm_epoll, op, pollable->handle, &ev);
    } while (status == -1 && errno == EINTR);
    if (status == -1) {
        janet_panicf("failed to unschedule event: %s", strerror(errno));
    }
    /* Destroy state machine and free memory */
    janet_unlisten_impl(state);
}

#define JANET_EPOLL_MAX_EVENTS 64
void janet_loop1_impl(int has_timeout, JanetTimestamp timeout) {
    if (janet_vm_timer_enabled || has_timeout) {
        memset(&its, 0, sizeof(its));
        if (has_timeout) {
            its.it_value.tv_sec = timeout / 1000;
            its.it_value.tv_nsec = (timeout % 1000) * 1000000;
        }
        timerfd_settime(janet_vm_timerfd, TFD_TIMER_ABSTIME, &its, NULL);
    }
    janet_vm_timer_enabled = has_timeout;

    /* Poll for events */
    struct epoll_event events[JANET_EPOLL_MAX_EVENTS];
    int ready;
    do {
        ready = epoll_wait(janet_vm_epoll, events, JANET_EPOLL_MAX_EVENTS, -1);
    } while (ready == -1 && errno == EINTR);
    if (ready == -1) {
        JANET_EXIT("failed to poll events");
    }

    /* Step state machines */
    for (int i = 0; i < ready; i++) {
        JanetPollable *pollable = events[i].data.ptr;
        if (NULL != pollable) { /* If NULL, is a timeout */
            int mask = events[i].events;
            JanetListenerState *state = pollable->state;
            state->event = events + i;
            while (NULL != state) {
                JanetListenerState *next_state = state->_next;
                JanetAsyncStatus status1 = JANET_ASYNC_STATUS_NOT_DONE;
                JanetAsyncStatus status2 = JANET_ASYNC_STATUS_NOT_DONE;
                if (mask & EPOLLOUT)
                    status1 = state->machine(state, JANET_ASYNC_EVENT_WRITE);
                if (mask & EPOLLIN)
                    status2 = state->machine(state, JANET_ASYNC_EVENT_READ);
                if (status1 == JANET_ASYNC_STATUS_DONE || status2 == JANET_ASYNC_STATUS_DONE)
                    janet_unlisten(state);
                state = next_state;
            }
        }
    }
}

void janet_ev_init(void) {
    janet_ev_init_common();
    janet_vm_epoll = epoll_create1(EPOLL_CLOEXEC);
    janet_vm_timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    janet_vm_timer_enabled = 0;
    if (janet_vm_epoll == -1 || janet_vm_timerfd == -1) goto error;
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = NULL;
    if (-1 == epoll_ctl(janet_vm_epoll, EPOLL_CTL_ADD, janet_vm_timerfd, &ev)) goto error;
    return;
error:
    JANET_EXIT("failed to initialize event loop");
}

void janet_ev_deinit(void) {
    janet_ev_deinit_common();
    close(janet_vm_epoll);
    close(janet_vm_timerfd);
    janet_vm_epoll = 0;
}

/*
 * End epoll implementation
 */

#else

#include <poll.h>

/* Poll implementation */

static JanetTimestamp ts_now(void) {
    struct timespec now;
    janet_assert(-1 != clock_gettime(CLOCK_REALTIME, &now), "failed to get time");
    uint64_t res = 1000 * now.tv_sec;
    res += now.tv_nsec / 1000000;
    return res;
}

/* Epoll global data */
JANET_THREAD_LOCAL struct pollfd *janet_vm_fds = NULL;
JANET_THREAD_LOCAL JanetListenerState **janet_vm_listener_map = NULL;
JANET_THREAD_LOCAL size_t janet_vm_fdcap = 0;
JANET_THREAD_LOCAL size_t janet_vm_fdcount = 0;

static int make_poll_events(int mask) {
    int events = 0;
    if (mask & JANET_ASYNC_LISTEN_READ)
        events |= POLLIN;
    if (mask & JANET_ASYNC_LISTEN_WRITE)
        events |= POLLOUT;
    return events;
}

static void janet_push_pollfd(struct pollfd pfd) {
    if (janet_vm_fdcap == janet_vm_fdcount) {
        size_t newcap = janet_vm_fdcount ? janet_vm_fdcount * 2 : 16;
        janet_vm_fds = realloc(janet_vm_fds, newcap * sizeof(struct pollfd));
        if (NULL == janet_vm_fds) {
            JANET_OUT_OF_MEMORY;
        }
        janet_vm_listener_map = realloc(janet_vm_listener_map, newcap * sizeof(JanetListenerState *));
        if (NULL == janet_vm_listener_map) {
            JANET_OUT_OF_MEMORY;
        }
        janet_vm_fdcap = newcap;
    }
    janet_vm_fds[janet_vm_fdcount++] = pfd;
}

/* Wait for the next event */
JanetListenerState *janet_listen(JanetPollable *pollable, JanetListener behavior, int mask, size_t size, void *user) {
    JanetListenerState *state = janet_listen_impl(pollable, behavior, mask, size, user);
    struct pollfd ev;
    ev.fd = pollable->handle;
    ev.events = make_poll_events(state->pollable->_mask);
    ev.revents = 0;
    state->_index = janet_vm_fdcount;
    janet_push_pollfd(ev);
    janet_vm_listener_map[state->_index] = state;
    return state;
}

/* Tell system we are done listening for a certain event */
static void janet_unlisten(JanetListenerState *state) {
    janet_vm_fds[state->_index] = janet_vm_fds[--janet_vm_fdcount];
    JanetListenerState *replacer = janet_vm_listener_map[janet_vm_fdcount];
    janet_vm_listener_map[state->_index] = replacer;
    /* Update pointers in replacer */
    replacer->_index = state->_index;
    /* Destroy state machine and free memory */
    janet_unlisten_impl(state);
}

void janet_loop1_impl(int has_timeout, JanetTimestamp timeout) {
    /* Poll for events */
    int ready;
    do {
        if (has_timeout) {
            JanetTimestamp now = ts_now();
            ready = poll(janet_vm_fds, janet_vm_fdcount, now > timeout ? 0 : (int)(timeout - now));
        } else {
            ready = poll(janet_vm_fds, janet_vm_fdcount, -1);
        }
    } while (ready == -1 && errno == EINTR);
    if (ready == -1) {
        JANET_EXIT("failed to poll events");
    }

    /* Step state machines */
    for (size_t i = 0; i < janet_vm_fdcount; i++) {
        struct pollfd *pfd = janet_vm_fds + i;
        /* Skip fds where nothing interesting happened */
        if (!(pfd->revents & (pfd->events | POLLHUP | POLLERR | POLLNVAL))) continue;
        JanetListenerState *state = janet_vm_listener_map[i];
        /* Normal event */
        int mask = janet_vm_fds[i].revents;
        JanetAsyncStatus status1 = JANET_ASYNC_STATUS_NOT_DONE;
        JanetAsyncStatus status2 = JANET_ASYNC_STATUS_NOT_DONE;
        state->event = pfd;
        if (mask & POLLOUT)
            status1 = state->machine(state, JANET_ASYNC_EVENT_WRITE);
        if (mask & POLLIN)
            status2 = state->machine(state, JANET_ASYNC_EVENT_READ);
        if (status1 == JANET_ASYNC_STATUS_DONE || status2 == JANET_ASYNC_STATUS_DONE)
            janet_unlisten(state);
    }
}

void janet_ev_init(void) {
    janet_ev_init_common();
    janet_vm_fds = NULL;
    janet_vm_listener_map = NULL;
    janet_vm_fdcap = 0;
    janet_vm_fdcount = 0;
    return;
}

void janet_ev_deinit(void) {
    janet_ev_deinit_common();
    free(janet_vm_fds);
    free(janet_vm_listener_map);
    janet_vm_fds = NULL;
    janet_vm_listener_map = NULL;
    janet_vm_fdcap = 0;
    janet_vm_fdcount = 0;
}

#endif

/* C functions */

static Janet cfun_ev_go(int32_t argc, Janet *argv) {
    janet_arity(argc, 1, 2);
    JanetFiber *fiber = janet_getfiber(argv, 0);
    Janet value = argc == 2 ? argv[1] : janet_wrap_nil();
    janet_schedule(fiber, value);
    return argv[0];
}

static Janet cfun_ev_call(int32_t argc, Janet *argv) {
    janet_arity(argc, 1, -1);
    JanetFunction *fn = janet_getfunction(argv, 0);
    JanetFiber *fiber = janet_fiber(fn, 64, argc - 1, argv + 1);
    janet_schedule(fiber, janet_wrap_nil());
    return janet_wrap_fiber(fiber);
}

static Janet cfun_ev_sleep(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    double sec = janet_getnumber(argv, 0);
    JanetTimeout to;
    to.when = ts_delta(ts_now(), sec);
    to.fiber = janet_vm_root_fiber;
    to.is_error = 0;
    to.sched_id = to.fiber->sched_id;
    add_timeout(to);
    janet_await();
}

static Janet cfun_ev_cancel(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);
    JanetFiber *fiber = janet_getfiber(argv, 0);
    Janet err = argv[1];
    janet_cancel(fiber, err);
    return argv[0];
}

static const JanetReg ev_cfuns[] = {
    {
        "ev/call", cfun_ev_call,
        JDOC("(ev/call fn & args)\n\n"
             "Call a function asynchronously. Returns a fiber that is scheduled to "
             "run the function.")
    },
    {
        "ev/go", cfun_ev_go,
        JDOC("(ev/go fiber &opt value)\n\n"
             "Put a fiber on the event loop to be resumed later. Optionally pass "
             "a value to resume with, otherwise resumes with nil.")
    },
    {
        "ev/sleep", cfun_ev_sleep,
        JDOC("(ev/sleep sec)\n\n"
             "Suspend the current fiber for sec seconds without blocking the event loop.")
    },
    {
        "ev/chan", cfun_channel_new,
        JDOC("(ev/chan &opt capacity)\n\n"
             "Create a new channel. capacity is the number of values to queue before "
             "blocking writers, defaults to 0 if not provided. Returns a new channel.")
    },
    {
        "ev/give", cfun_channel_push,
        JDOC("(ev/give channel value)\n\n"
             "Write a value to a channel, suspending the current fiber if the channel is full.")
    },
    {
        "ev/take", cfun_channel_pop,
        JDOC("(ev/take channel)\n\n"
             "Read from a channel, suspending the current fiber if no value is available.")
    },
    {
        "ev/full", cfun_channel_full,
        JDOC("(ev/full channel)\n\n"
             "Check if a channel is full or not.")
    },
    {
        "ev/capacity", cfun_channel_capacity,
        JDOC("(ev/capacity channel)\n\n"
             "Get the number of items a channel will store before blocking writers.")
    },
    {
        "ev/count", cfun_channel_count,
        JDOC("(ev/count channel)\n\n"
             "Get the number of items currently waiting in a channel.")
    },
    {
        "ev/cancel", cfun_ev_cancel,
        JDOC("(ev/cancel fiber err)\n\n"
             "Cancel a suspended fiber in the event loop. Differs from cancel in that it returns the canceled fiber immediately")
    },
    {
        "ev/select", cfun_channel_choice,
        JDOC("(ev/select & clauses)\n\n"
             "Block until the first of several channel operations occur. Returns a tuple of the form [:give chan] or [:take chan x], where "
             "a :give tuple is the result of a write and :take tuple is the result of a write. Each clause must be either a channel (for "
             "a channel take operation) or a tuple [channel x] for a channel give operation. Operations are tried in order, such that the first "
             "clauses will take precedence over later clauses.")
    },
    {
        "ev/rselect", cfun_channel_rchoice,
        JDOC("(ev/rselect & clauses)\n\n"
             "Similar to ev/choice, but will try clauses in a random order for fairness.")
    },
    {NULL, NULL, NULL}
};

void janet_lib_ev(JanetTable *env) {
    janet_core_cfuns(env, NULL, ev_cfuns);
}

#endif
