
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


extern int  ngx_kqueue;


ssize_t
ngx_aio_write(ngx_connection_t *c, u_char *buf, size_t size)
{
    ngx_event_t  *wev;
    ngx_aio_chain_t *aio;

    wev = c->write;

    aio = ngx_alloc(sizeof(*aio), wev->log);
    ngx_memzero(aio, sizeof(*aio));

    aio->aiocb.aio_fildes = c->fd;
    aio->aiocb.aio_buf = buf;
    aio->aiocb.aio_nbytes = size;

#if (NGX_HAVE_KQUEUE)
    aio->aiocb.aio_sigevent.sigev_notify_kqueue = ngx_kqueue;
    aio->aiocb.aio_sigevent.sigev_notify = SIGEV_KEVENT;
    aio->aiocb.aio_sigevent.sigev_value.sigval_ptr = wev;
#endif

    if (aio_write(&aio->aiocb) == -1) {
	ngx_log_error(NGX_LOG_CRIT, wev->log, ngx_errno,
	    "aio_write() failed");
	free(aio);
	return NGX_ERROR;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, wev->log, 0, "aio_write: OK");

    wev->active = 1;
    wev->complete = 0;

    if (c->aio_chain == NULL)
	c->aio_chain = aio;
    else
	c->aio_tail->next = aio;
    c->aio_tail = aio;

    return 0;
}
