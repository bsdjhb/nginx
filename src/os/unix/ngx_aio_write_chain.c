
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>

static ssize_t
ngx_aio_completed(ngx_connection_t *c)
{
    ngx_event_t  *wev;
    ngx_aio_chain_t *aio;
    ssize_t completed, n;

    wev = c->write;
    completed = 0;
    while ((aio = c->aio_chain) != NULL) {
	n = aio_error(&aio->aiocb);
	if (n == -1) {
	    ngx_log_error(NGX_LOG_CRIT, wev->log, ngx_errno,
		"aio_error() failed");
	    wev->error = 1;
	    return NGX_ERROR;
	}

	if (n == NGX_EINPROGRESS)
	    break;

	if (n != 0) {
	    ngx_log_error(NGX_LOG_CRIT, wev->log, n, "aio_write() failed");
	    wev->error = 1;
	    wev->ready = 0;

	    aio_return (&aio->aiocb);

	    c->aio_chain = aio->next;
	    free(aio);

	    return NGX_ERROR;
	}

	n = aio_return (&aio->aiocb);
	if (n == -1) {
	    ngx_log_error(NGX_LOG_ALERT, wev->log, ngx_errno,
		"aio_return() failed");
	    wev->error = 1;
	    wev->ready = 0;

	    c->aio_chain = aio->next;
	    free(aio);

	    return NGX_ERROR;
	}

	completed += n;

	c->aio_chain = aio->next;
	free(aio);
    }

    if (c->aio_chain == NULL)
	wev->active = 0;

    return completed;
}

ngx_chain_t *
ngx_aio_write_chain(ngx_connection_t *c, ngx_chain_t *in, off_t limit)
{
    u_char       *buf, *prev;
    off_t         send, sent;
    size_t        len, pending;
    ssize_t       n, size;
    ngx_chain_t  *cl, *wcl;

    /* the maximum limit size is the maximum size_t value - the page size */

    if (limit == 0 || limit > (off_t) (NGX_MAX_SIZE_T_VALUE - ngx_pagesize)) {
        limit = NGX_MAX_SIZE_T_VALUE - ngx_pagesize;
    }

    send = 0;
    sent = 0;
    cl = in;

    while (cl) {

        if (cl->buf->pos == cl->buf->last) {
            cl = cl->next;
            continue;
        }

	/* handle any completed requests */

	n = ngx_aio_completed(c);
        if (n == NGX_ERROR) {
            return NGX_CHAIN_ERROR;
        }

        if (n > 0) {
            sent += n;
            c->sent += n;
	    c->aio_inflight -= n;
        }

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "aio_write sent: %O", c->sent);

        for (cl = in; cl; cl = cl->next) {

            if (sent >= cl->buf->last - cl->buf->pos) {
                sent -= cl->buf->last - cl->buf->pos;
                cl->buf->pos = cl->buf->last;

                continue;
            }

            cl->buf->pos += sent;

            break;
        }

	sent = 0;

	if (cl == NULL)
	    break;

	/* skip over buffers currently in-flight */

	n = c->aio_inflight;
	wcl = cl;
	while (wcl && n != 0) {
            size = wcl->buf->last - wcl->buf->pos;
	    if (size > n)
		break;

	    n -= size;
	    wcl = wcl->next;
	}

	if (wcl == NULL || c->aio_inflight >= limit)
	    break;

	/* send anything not yet queued */

	pending = n;
	send = c->aio_inflight;

	while (wcl && send < limit) {
	    buf = wcl->buf->pos + pending;
	    prev = buf;
	    len = 0;

	    /* coalesce the neighbouring bufs */

	    while (wcl && prev == wcl->buf->pos && send < limit) {
		if (ngx_buf_special(wcl->buf)) {
		    continue;
		}

		size = wcl->buf->last - wcl->buf->pos;

		if (send + size > limit) {
		    size = limit - send;
		}

		len += size;
		prev = wcl->buf->pos + size;
		send += size;
		if (size == wcl->buf->last - wcl->buf->pos) {
		    pending = 0;
		    wcl = wcl->next;
		} else
		    pending = prev - wcl->buf->pos;
	    }

	    n = ngx_aio_write(c, buf, len);
	    if (n == NGX_ERROR)
		return NGX_CHAIN_ERROR;

	    c->aio_inflight += len;
	}
    }

    return cl;
}
