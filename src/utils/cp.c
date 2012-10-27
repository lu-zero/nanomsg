/*
    Copyright (c) 2012 250bpm s.r.o.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#include "cp.h"

#if defined SP_HAVE_WINDOWS

#include "err.h"
#include "cont.h"

void sp_cp_init (struct sp_cp *self)
{
    self->hndl = CreateIoCompletionPort (INVALID_HANDLE_VALUE, NULL, 0, 0);
    win_assert (self->hndl);
}

void sp_cp_term (struct sp_cp *self)
{
    BOOL brc;

    brc = CloseHandle (self->hndl);
    win_assert (brc);
}

void sp_cp_post (struct sp_cp *self, struct sp_cp_task *task)
{
    BOOL brc;

    brc = PostQueuedCompletionStatus (self->hndl, 0,
        (ULONG_PTR) NULL, &task->olpd);
    win_assert (brc);
}

int sp_cp_wait (struct sp_cp *self, int timeout, struct sp_cp_task **task)
{
    BOOL brc;
    LPOVERLAPPED olpd;

    brc = GetQueuedCompletionStatus (self->hndl, NULL, NULL,
        &olpd, timeout < 0 ? INFINITE : timeout);
    if (!brc && !olpd)
        return -ETIMEDOUT;
    win_assert (brc);

    *task = sp_cont (olpd, struct sp_cp_task, olpd);

    return 0;
}

#else

#include "alloc.h"
#include "fast.h"
#include "cont.h"
#include "err.h"

#define SP_CP_INITIAL_CAPACITY 64

void sp_cp_init (struct sp_cp *self)
{
    sp_mutex_init (&self->sync, 0);
    sp_cond_init (&self->cond);
    self->capacity = SP_CP_INITIAL_CAPACITY;
    self->head = 0;
    self->tail = 0;
    self->items = sp_alloc (self->capacity * sizeof (struct sp_cp_item));
    alloc_assert (self->items);
}

void sp_cp_term (struct sp_cp *self)
{
    sp_free (self->items);
    sp_cond_term (&self->cond);
    sp_mutex_term (&self->sync);
}

void sp_cp_post (struct sp_cp *self, int op, void *arg)
{
    int empty;

    sp_mutex_lock (&self->sync);

    /*  Fill in new item in the circular buffer. */
    self->items [self->tail].op = op;
    self->items [self->tail].arg = arg;

    /*  Move tail by 1 position. */
    empty = self->tail == self->head ? 1 : 0;
    self->tail = (self->tail + 1) % self->capacity;

    /*  If the capacity of the circular buffer is exhausted, allocate some
        more memory. */
    if (sp_slow (self->head == self->tail)) {
        self->items = sp_realloc (self->items,
            self->capacity * 2 * sizeof (struct sp_cp_item));
        alloc_assert (self->items);
        memcpy (self->items + self->capacity, self->items,
            self->tail * sizeof (struct sp_cp_item));
        self->tail += self->capacity;
        self->capacity *= 2;
    }
    
    if (empty)
        sp_cond_signal (&self->cond);

    sp_mutex_unlock (&self->sync);
}

int sp_cp_wait (struct sp_cp *self, int timeout, int *op, void **arg)
{
    int rc;

    sp_mutex_lock (&self->sync);

    /*  If there's an item available, return it. */
    if (sp_fast (self->head != self->tail)) {
        *op = self->items [self->head].op;
        *arg = self->items [self->head].arg;
        self->head = (self->head + 1) % self->capacity;
        sp_mutex_unlock (&self->sync);
        return 0;
    }

    /*  Wait for new item. */
    rc = sp_cond_wait (&self->cond, &self->sync, timeout);
    if (sp_slow (rc == -ETIMEDOUT)) {
        sp_mutex_unlock (&self->sync);
        return -ETIMEDOUT;
    }

    /*  If there's an item available now, return it. */
    if (sp_fast (self->head != self->tail)) {
        *op = self->items [self->head].op;
        *arg = self->items [self->head].arg;
        self->head = (self->head + 1) % self->capacity;
        sp_mutex_unlock (&self->sync);
        return 0;
    }

    /*  Spurious wake-up. */
    sp_mutex_unlock (&self->sync);
    return -ETIMEDOUT;
}

#endif
