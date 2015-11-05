#include <nchan_module.h>
#include "../store/memory/shmem.h"
#include "../store/memory/ipc.h"
#include "../store/memory/store-private.h"
#include "../store/memory/ipc-handlers.h"
#include "internal.h"
#include "memstore_ipc.h"
#include <assert.h>

//#define DEBUG_LEVEL NGX_LOG_WARN
#define DEBUG_LEVEL NGX_LOG_DEBUG

#define DBG(fmt, arg...) ngx_log_error(DEBUG_LEVEL, ngx_cycle->log, 0, "SUB:MEM-IPC:" fmt, ##arg)
#define ERR(fmt, arg...) ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "SUB:MEM-IPC:" fmt, ##arg)


typedef struct sub_data_s sub_data_t;

struct sub_data_s {
  subscriber_t                 *sub;
  ngx_str_t                    *chid;
  ngx_int_t                     originator;
  ngx_int_t                     owner;
  void                         *foreign_chanhead;
//  nchan_store_channel_head_t   *chanhead;
  ngx_event_t                   timeout_ev;
}; //sub_data_t

static ngx_int_t empty_callback(){
  return NGX_OK;
}

static ngx_int_t sub_enqueue(ngx_int_t timeout, void *ptr, sub_data_t *d) {
  DBG("%p (%V) memstore subsriber enqueued ok", d->sub, d->chid);
  return NGX_OK;
}

static ngx_int_t sub_dequeue(ngx_int_t status, void *ptr, sub_data_t* d) {
  ngx_int_t           ret;
  internal_subscriber_t  *fsub = (internal_subscriber_t  *)d->sub;
  DBG("%p (%V) memstore subscriber dequeue: notify owner", d->sub, d->chid);
  if(d->timeout_ev.timer_set) {
    ngx_del_timer(&d->timeout_ev);
  }
  ret = memstore_ipc_send_unsubscribed(d->originator, d->chid, NULL);
  
  if(fsub->sub.reserved > 0) {
    DBG("%p (%V) not ready to destroy (reserved for %i)", fsub, d->chid, fsub->sub.reserved);
    fsub->awaiting_destruction = 1;
  }
  else {
    DBG("%p (%V) destroy", fsub, d->chid);
    ngx_free(d);
  }
  
  return ret;
}

static ngx_int_t sub_respond_message(ngx_int_t status, void *ptr, sub_data_t* d) {
  ngx_int_t               rc;
  nchan_msg_t            *msg = (nchan_msg_t *) ptr;
  internal_subscriber_t  *fsub = (internal_subscriber_t  *)d->sub;
  nchan_loc_conf_t              cf;
    
  cf.buffer_timeout =50;
  cf.min_messages = 0;
  cf.max_messages = 0;
  
  DBG("%p (%V) memstore subscriber (lastid %i:%i) respond with message %i:%i (lastid %i:%i)", d->sub, d->chid, d->sub->last_msg_id.time, d->sub->last_msg_id.tag, msg->id.time, msg->id.tag, msg->prev_id.time, msg->prev_id.tag);
  
  //verify_subscriber_last_msg_id(d->sub, msg);
  
  rc = memstore_ipc_send_publish_message(d->originator, d->chid, msg, &cf, empty_callback, NULL);
  
  fsub->sub.last_msg_id = msg->id;
  
  return rc;
}

static ngx_int_t sub_respond_status(ngx_int_t status, void *ptr, sub_data_t *d) {
  DBG("%p (%V) memstore subscriber respond with status", d->sub, d->chid);
  const ngx_str_t *status_line = NULL;
  switch(status) {
    case NGX_HTTP_GONE: //delete
      status_line = &NCHAN_HTTP_STATUS_410;
      break;
    case NGX_HTTP_CONFLICT:
      status_line = &NCHAN_HTTP_STATUS_409;
      break;
    case NGX_HTTP_NO_CONTENT: //message expired
      break;
    case NGX_HTTP_CLOSE: //delete
      break;
    case NGX_HTTP_NOT_MODIFIED: //timeout?
      break;
    case NGX_HTTP_FORBIDDEN:
      break;
    default:
      ERR("unknown status %i", status);
  }
  memstore_ipc_send_publish_status(d->originator, d->chid, status, status_line, empty_callback, NULL);
  
  return NGX_OK;
}
static void reset_timer(sub_data_t *data) {
  if(data->timeout_ev.timer_set) {
    ngx_del_timer(&data->timeout_ev);
  }
  ngx_add_timer(&data->timeout_ev, MEMSTORE_IPC_SUBSCRIBER_TIMEOUT * 1000);
}


static ngx_int_t keepalive_reply_handler(ngx_int_t renew, void *_, void* pd) {
  sub_data_t *d = (sub_data_t *)pd;
  DBG("%p (%V) keepalive reply - renew: %i.", d->sub, d->chid, renew);
  if(d->sub->fn->release(d->sub, 0) == NGX_OK) {
    if(renew) {
      reset_timer(d);
    }
    else{
      d->sub->fn->dequeue(d->sub);
    }
  }
  return NGX_OK;
}

static void timeout_ev_handler(ngx_event_t *ev) {
  sub_data_t *d = (sub_data_t *)ev->data;
#if FAKESHARD
  memstore_fakeprocess_push(d->owner);
#endif
  DBG("%p (%V), timeout event. Ping originator to see if still needed.", d->sub, d->chid);
  d->sub->fn->reserve(d->sub);
  memstore_ipc_send_memstore_subscriber_keepalive(d->originator, d->chid, d->sub, d->foreign_chanhead, keepalive_reply_handler, d);
#if FAKESHARD
  memstore_fakeprocess_pop();
#endif
}

subscriber_t *memstore_ipc_subscriber_create(ngx_int_t originator_slot, ngx_str_t *chid, void* foreign_chanhead) { //, nchan_channel_head_t *local_chanhead) {
  sub_data_t                 *d;
  d = ngx_alloc(sizeof(*d), ngx_cycle->log);
  if(d == NULL) {
    ERR("couldn't allocate memstore subscriber data");
    return NULL;
  }
  
  assert(originator_slot != memstore_slot());
  subscriber_t *sub = internal_subscriber_create("memstore-ipc", d);
  internal_subscriber_set_enqueue_handler(sub, (callback_pt )sub_enqueue);
  internal_subscriber_set_dequeue_handler(sub, (callback_pt )sub_dequeue);
  internal_subscriber_set_respond_message_handler(sub, (callback_pt )sub_respond_message);
  internal_subscriber_set_respond_status_handler(sub, (callback_pt )sub_respond_status);
  sub->destroy_after_dequeue = 1;
  d->sub = sub;
  d->chid = chid;
  d->originator = originator_slot;
  assert(foreign_chanhead != NULL);
  d->foreign_chanhead = foreign_chanhead;
//  d->chanhead = local_chanhead;
  d->owner = memstore_slot();
  ngx_memzero(&d->timeout_ev, sizeof(d->timeout_ev));
  d->timeout_ev.handler = timeout_ev_handler;
  d->timeout_ev.data = d;
  d->timeout_ev.log = ngx_cycle->log;
  reset_timer(d);
  DBG("%p (%V) memstore-ipc subscriber created with privdata %p", d->sub, d->chid, d);
  return sub;
}