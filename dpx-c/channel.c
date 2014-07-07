#include "dpx-internal.h"
#include <string.h> // for strcmp

struct _dpx_channel_error_hs {
	dpx_channel *c;
	DPX_ERROR err;
};

void* _dpx_channel_error_helper(void* v) {
	struct _dpx_channel_error_hs *h = (struct _dpx_channel_error_hs*) v;
	h->err = _dpx_channel_error(h->c);
	return NULL;
}

DPX_ERROR dpx_channel_error(dpx_channel *c) {
	_dpx_a a;
	a.function = &_dpx_channel_error_helper;

	struct _dpx_channel_error_hs h;
	h.c = c;

	a.args = &h;

	_dpx_joinfunc(&a);
	return h.err;
}

void* _dpx_channel_receive_frame_helper(void* v) {
	return _dpx_channel_receive_frame((dpx_channel*)v);
}

dpx_frame* dpx_channel_receive_frame(dpx_channel *c) {
	_dpx_a a;
	a.function = &_dpx_channel_receive_frame_helper;
	a.args = c;

	return (dpx_frame*) _dpx_joinfunc(&a);
}

struct _dpx_channel_send_frame_hs {
	dpx_channel *c;
	dpx_frame *frame;
	DPX_ERROR err;
};

void* _dpx_channel_send_frame_helper(void* v) {
	struct _dpx_channel_send_frame_hs *h = (struct _dpx_channel_send_frame_hs*) v;
	h->err = _dpx_channel_send_frame(h->c, h->frame);
	return NULL;
}

DPX_ERROR dpx_channel_send_frame(dpx_channel *c, dpx_frame *frame) {
	_dpx_a a;
	a.function = &_dpx_channel_send_frame_helper;

	struct _dpx_channel_send_frame_hs h;
	h.c = c;
	h.frame = frame;

	a.args = &h;

	_dpx_joinfunc(&a);
	return h.err;
}

struct _dpx_channel_handle_incoming_hs {
	dpx_channel *c;
	dpx_frame *frame;
	int ret;
};

void* _dpx_channel_handle_incoming_helper(void* v) {
	struct _dpx_channel_handle_incoming_hs *h = (struct _dpx_channel_handle_incoming_hs*) v;
	h->ret = _dpx_channel_handle_incoming(h->c, h->frame);
	return NULL;
}

int dpx_channel_handle_incoming(dpx_channel *c, dpx_frame *frame) {
	_dpx_a a;
	a.function = &_dpx_channel_handle_incoming_helper;

	struct _dpx_channel_handle_incoming_hs h;
	h.c = c;
	h.frame = frame;

	a.args = &h;

	_dpx_joinfunc(&a);
	return h.ret;
}

char* dpx_channel_method_get(dpx_channel *c) {
	return c->method;
}

struct _dpx_channel_method_set_hs {
	dpx_channel *c;
	char* method;
};

void* _dpx_channel_method_set_helper(void* v) {
	struct _dpx_channel_method_set_hs *h = (struct _dpx_channel_method_set_hs*) v;
	return _dpx_channel_method_set(h->c, h->method);
}

char* dpx_channel_method_set(dpx_channel *c, char* method) {
	_dpx_a a;
	a.function = &_dpx_channel_method_set_helper;

	struct _dpx_channel_method_set_hs h;
	h.c = c;
	h.method = method;

	a.args = &h;

	void* ret = _dpx_joinfunc(&a);

	return (char*)ret;
}

// ----------------------------------------------------------------------------

dpx_channel* _dpx_channel_new() {
	dpx_channel* ptr = (dpx_channel*) malloc(sizeof(dpx_channel));

	ptr->lock = (QLock*) calloc(1, sizeof(QLock));
	ptr->id = 0;

	ptr->peer = NULL;
	ptr->connCh = chancreate(sizeof(dpx_duplex_conn*), 0);
	ptr->conn = NULL;

	ptr->server = 0;
	ptr->closed = 0;
	ptr->last = 0;

	ptr->incoming = chancreate(sizeof(dpx_frame*), DPX_CHANNEL_QUEUE_HWM);
	ptr->outgoing = chancreate(sizeof(dpx_frame*), DPX_CHANNEL_QUEUE_HWM);

	ptr->err = 0;
	ptr->method = NULL;

	return ptr;
}

void _dpx_channel_free(dpx_channel* c) {
	_dpx_channel_close(c, DPX_ERROR_FREEING);
	free(c->lock);
	free(c->method);
	free(c);
}

dpx_channel* _dpx_channel_new_client(dpx_peer *p, char* method) {
	dpx_channel* ptr = _dpx_channel_new();
	ptr->peer = p;
	ptr->id = p->chanIndex;
	ptr->method = method;

	p->chanIndex += 1;

	taskcreate(&_dpx_channel_pump_outgoing, ptr, DPX_TASK_STACK_SIZE);
	return ptr;
}

dpx_channel* _dpx_channel_new_server(dpx_duplex_conn *conn, dpx_frame *frame) {
	dpx_channel* ptr = _dpx_channel_new();
	
	ptr->server = 1;
	ptr->peer = conn->peer;

	ptr->id = frame->channel;

	char* methodcpy = (char*) malloc(strlen(frame->method) + 1);
	strcpy(methodcpy, frame->method);
	ptr->method = methodcpy;

	taskcreate(&_dpx_channel_pump_outgoing, ptr, DPX_TASK_STACK_SIZE);
	_dpx_duplex_conn_link_channel(conn, ptr);
	return ptr;
}

void _dpx_channel_close(dpx_channel *c, DPX_ERROR err) {
	qlock(c->lock);

	if (c->closed) {
		goto _dpx_channel_close_cleanup;
	}
	c->closed = 1;
	c->err = err;

	_dpx_duplex_conn_unlink_channel(c->conn, c);

	chanfree(c->connCh);
	c->connCh = NULL;
	chanfree(c->incoming);
	c->incoming = NULL;
	chanfree(c->outgoing);
	c->outgoing = NULL;

_dpx_channel_close_cleanup:
	qunlock(c->lock);
}

struct _dpx_channel_close_via_task_struct {
	dpx_channel *c;
	DPX_ERROR err;
};

void _dpx_channel_close_via_task(struct _dpx_channel_close_via_task_struct *s) {
	taskname("_dpx_channel_close_via_task_%d", s->c->id);
	_dpx_channel_close(s->c, s->err);
	free(s);
}

DPX_ERROR _dpx_channel_error(dpx_channel *c) {
	qlock(c->lock);
	DPX_ERROR ret = c->err;
	qunlock(c->lock);
	return ret;
}

dpx_frame* _dpx_channel_receive_frame(dpx_channel *c) {
	if (c->server && c->last) {
		return NULL;
	}

	dpx_frame* frame;
	int ok = chanrecv(c->incoming, &frame);
	// FIXME is cleanup needed on this dpx_frame if not okay? (e.g. free)
	if (!ok) {
		return NULL;
	}

	if (frame->last) {
		if (c->server) {
			c->last = 1;
		} else {
			_dpx_channel_close(c, DPX_ERROR_NONE);
		}
	}

	return frame;
}

DPX_ERROR _dpx_channel_send_frame(dpx_channel *c, dpx_frame *frame) {
	qlock(c->lock);
	DPX_ERROR ret = DPX_ERROR_NONE;

	// FIXME if err != NONE, then the caller might have to free frame

	if (c->err) {
		ret = c->err;
		goto _dpx_channel_send_frame_cleanup;
	}

	if (c->closed) {
		ret = DPX_ERROR_CHAN_CLOSED;
		goto _dpx_channel_send_frame_cleanup;
	}

	frame->chanRef = c;
	frame->channel = c->id;
	frame->type = DPX_FRAME_DATA;

	printf("sending DATA frame channel: %d\n", c->id);

	chansend(c->outgoing, &frame);

_dpx_channel_send_frame_cleanup:
	qunlock(c->lock);
	return ret;
}

int _dpx_channel_handle_incoming(dpx_channel *c, dpx_frame *frame) {
	qlock(c->lock);

	int ret = 0;

	if (c->closed) {
		goto _dpx_channel_handle_incoming_cleanup;
	}

	if (!frame->last && strcmp(frame->error, "") != 0) {
		struct _dpx_channel_close_via_task_struct *s = (struct _dpx_channel_close_via_task_struct*) malloc(sizeof(struct _dpx_channel_close_via_task_struct));
		s->c = c;
		s->err = DPX_ERROR_CHAN_FRAME;
		taskcreate(&_dpx_channel_close_via_task, s, DPX_TASK_STACK_SIZE);
		ret = 1;
		goto _dpx_channel_handle_incoming_cleanup;
	}

	chansend(c->incoming, &frame);
	ret = 1;

_dpx_channel_handle_incoming_cleanup:
	qunlock(c->lock);
	return ret;
}

void _dpx_channel_pump_outgoing(dpx_channel *c) {
	taskname("dpx_channel_pump_outgoing_%d_%d", c->peer->index, c->id);
	printf("(%d) Pumping started for channel %d\n", c->peer->index, c->id);
	while(1) {
		taskdelay(20);

		if (c->connCh == NULL) {
			c->conn = NULL;
			goto _dpx_channel_pump_outgoing_cleanup;
		}
		dpx_duplex_conn* conn;
		int hasConn = channbrecv(c->connCh, &conn);
		if (hasConn == -1)
			continue;

		c->conn = conn;

		dpx_frame *frame;
		int hasFrame;

_dpx_channel_pump_outgoing_frame:
		taskdelay(20);

		if (c->outgoing == NULL) {
			goto _dpx_channel_pump_outgoing_cleanup;
		}

		hasFrame = channbrecv(c->outgoing, &frame);
		if (hasFrame == -1)
			goto _dpx_channel_pump_outgoing_frame;

		while(1) {
			printf("(%d) Sending frame: %d bytes\n", c->peer->index, frame->payloadSize);
			DPX_ERROR err = _dpx_duplex_conn_write_frame(c->conn, frame);
			if (err) {
				printf("(%d) Error sending frame: %lu\n", c->peer->index, err);

				// check if we have a new connection...
				if (c->connCh == NULL) {
					// nope, our channel is closed
					c->conn = NULL;
					goto _dpx_channel_pump_outgoing_cleanup;
				}

				chanrecv(c->connCh, &c->conn);
				continue;
			}
			if (strcmp(frame->error, "")) {
				_dpx_channel_close(c, DPX_ERROR_CHAN_FRAME);
			} else if (frame->last && c->server) {
				_dpx_channel_close(c, DPX_ERROR_NONE);
			}
			break;
		}
	}

_dpx_channel_pump_outgoing_cleanup:
	printf("(%d) Pumping finished for channel %d\n", c->peer->index, c->id);
}

char* _dpx_channel_method_set(dpx_channel *c, char* method) {
	char* ret = c->method;
	c->method = method;
	return ret;
}