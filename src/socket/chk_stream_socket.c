#define _CORE_
#include <assert.h>
#include "util/chk_error.h"
#include "util/chk_log.h"
#include "socket/chk_socket_helper.h"
#include "socket/chk_stream_socket.h"
#include "event/chk_event_loop.h"
#include "socket/chk_stream_socket_define.h"

#ifndef  cast
# define  cast(T,P) ((T)(P))
#endif

uint32_t chunkcount  = 0;
uint32_t buffercount = 0;

/*status*/
enum{
	SOCKET_CLOSE     = 1 << 1,  /*连接完全关闭,对象可以被销毁*/
	SOCKET_RCLOSE    = 1 << 2,  /*本端读关闭,写等剩余包发完关闭*/
	SOCKET_PEERCLOSE = 1 << 3,  /*对端关闭*/
	SOCKET_INLOOP    = 1 << 4,
};

/*
* 默认解包器,将已经接收到的数据全部置入chk_bytebuffer
*/
typedef struct default_decoder {
	void (*update)(chk_decoder*,chk_bytechunk *b,uint32_t spos,uint32_t size);
	chk_bytebuffer *(*unpack)(chk_decoder*,int32_t *err);
	void (*dctor)(chk_decoder*);
	uint32_t       spos;
	uint32_t       size;
	chk_bytechunk *b;
	void          *ud;
}default_decoder;

static void default_update(chk_decoder *d,chk_bytechunk *b,uint32_t spos,uint32_t size) {
	cast(default_decoder*,d)->spos = spos;
	cast(default_decoder*,d)->b    = b;
	cast(default_decoder*,d)->size = size;
}

static chk_bytebuffer *default_unpack(chk_decoder *d,int32_t *err) {
	default_decoder *_d  = cast(default_decoder*,d);
	chk_bytebuffer  *ret = NULL;
	*err = 0;
	if(_d->b) {
		ret = chk_bytebuffer_new_bychunk_readonly(_d->b,_d->spos,_d->size);
		if(!ret) {
			CHK_SYSLOG(LOG_ERROR,"chk_bytebuffer_new_bychunk_readonly() failed size:%d",_d->size);  
			*err = chk_error_no_memory;
			return NULL;
		}
		_d->b = NULL;
	}
	return ret;
}

static default_decoder *default_decoder_new() {
	default_decoder *d = calloc(1,sizeof(*d));
	if(!d){
		CHK_SYSLOG(LOG_ERROR,"calloc default_decoder failed");		 
		return NULL;
	}
	d->update = default_update;
	d->unpack = default_unpack;
	d->dctor  = (void (*)(chk_decoder*))free;
	return d;
}

static inline int32_t send_list_empty(chk_stream_socket *s) {
	if(chk_list_empty(&s->send_list) && chk_list_empty(&s->urgent_list))
		return 1;
	return 0;
}

/*数据发送成功之后更新buffer list信息*/
static inline void update_send_list(chk_stream_socket *s,int32_t _bytes) {
	chk_bytebuffer *b;
	chk_bytechunk  *head;
	chk_list       *list;
	uint32_t        bytes = cast(uint32_t,_bytes);
	uint32_t        size;

	if(s->sending_urgent) list = &s->urgent_list;
	else list = &s->send_list;
	for(;bytes;) {
		b = cast(chk_bytebuffer*,chk_list_begin(list));
		if(bytes >= b->datasize) {
			/*一个buffer已经发送完毕,将其出列并删除*/
			chk_list_pop(list);
			bytes -= b->datasize;
			chk_bytebuffer_del(b);
		}else {
			/*只完成一个buffer中部分数据的发送*/
			for(;bytes;) {
				head = b->head;
				size = MIN(head->cap - b->spos,bytes);
				bytes -= size;
				b->spos += size;/*调整buffer数据的起始位置*/
				b->datasize -= size;/*调整待发送数据的大小*/
				if(b->spos >= head->cap) {
					/*发送完一个chunk*/
					b->spos = 0;
					b->head = chk_bytechunk_retain(head->next);
					chk_bytechunk_release(head);
				}
			}
		}
	}
}

/*准备缓冲用于发起写请求*/
static inline int32_t prepare_send(chk_stream_socket *s) {
	int32_t          i = 0;
	chk_bytebuffer  *b;
	chk_bytechunk   *chunk;
	uint32_t    datasize,size,pos,send_size;
	send_size = 0;
	b = cast(chk_bytebuffer*,chk_list_begin(&s->send_list));

	do{

		if(chk_list_empty(&s->urgent_list)) {
			/*没有urgent buffer需要发送*/
			break;
		}
		if(!b || b->internal == b->datasize){
			/*send list中没有只完成部分发送的buffer*/
			break;
		}
		/*先将send list中只发送了部分的buffer发送出去*/
		pos   = b->spos;
		chunk = b->head;
		datasize = b->datasize;
		while(i < MAX_WBAF && chunk && datasize) {
			s->wsendbuf[i].iov_base = chunk->data + pos;
			size = MIN(chunk->cap - pos,datasize);
			datasize    -= size;
			send_size   += size;
			s->wsendbuf[i].iov_len = size;
			++i;

			if(s->ssl.ssl) {
				break;
			}

			chunk = chunk->next;
			pos = 0;
		}
		return i;
	}while(0);

	if(!chk_list_empty(&s->urgent_list)){
		b = cast(chk_bytebuffer*,chk_list_begin(&s->urgent_list));
		s->sending_urgent = 1;/*标记当前正在发送urgent_list*/ 
	}else
		s->sending_urgent = 0;

	while(b && i < MAX_WBAF && send_size < MAX_SEND_SIZE) {
		pos   = b->spos;
		chunk = b->head;
		datasize = b->datasize;
		while(i < MAX_WBAF && chunk && datasize) {
			s->wsendbuf[i].iov_base = chunk->data + pos;
			size = MIN(chunk->cap - pos,datasize);
			size = MIN(size,MAX_SEND_SIZE - send_size);
			datasize    -= size;
			send_size   += size;
			s->wsendbuf[i].iov_len = size;
			++i;
			if(s->ssl.ssl) {
				break;
			}			
			chunk = chunk->next;
			pos = 0;
		}
		if(send_size < MAX_SEND_SIZE) 
			b = cast(chk_bytebuffer*,cast(chk_list_entry*,b)->next);
	}		
	return i;
}

/*准备缓冲用于读*/
static inline int32_t prepare_recv(chk_stream_socket *s) {
	chk_bytechunk  *chunk;
	int32_t         i = 0;
	uint32_t        recv_size,pos,recv_buffer_size;
	recv_buffer_size = s->option.recv_buffer_size;
	if(!s->next_recv_buf) {
		s->next_recv_buf = chk_bytechunk_new(NULL,recv_buffer_size);
		if(!s->next_recv_buf){
			CHK_SYSLOG(LOG_ERROR,"chk_bytechunk_new() failed recv_buffer_size:%d",recv_buffer_size);
			return -1;
		}
		s->next_recv_pos = 0;
	}
	for(pos = s->next_recv_pos,chunk = s->next_recv_buf;;) {
		recv_size = chunk->cap - pos;
		s->wrecvbuf[i].iov_len  = recv_size;
		s->wrecvbuf[i].iov_base = chunk->data + pos;
		++i;
		if(s->ssl.ssl) {
			break;
		}
		if(recv_size != recv_buffer_size) {
			pos = 0;
			if(!chunk->next){ 
				chunk->next = chk_bytechunk_new(NULL,recv_buffer_size);
				if(!chunk->next){
					CHK_SYSLOG(LOG_ERROR,"chk_bytechunk_new() failed recv_buffer_size:%d",recv_buffer_size);				
					return -1;
				}
			}
			chunk = chunk->next;
		}else break;
	}
	return i;
}

/*数据接收完成,更新接收缓冲信息*/
static inline int32_t update_next_recv_pos(chk_stream_socket *s,int32_t bytes) {
	uint32_t       size;
	chk_bytechunk *head;
	for(;bytes;) {
		head = s->next_recv_buf;
		size = MIN(head->cap - s->next_recv_pos,bytes);
		s->next_recv_pos += size;
		bytes -= size;
		if(s->next_recv_pos >= head->cap) {
			s->next_recv_pos = 0;
			head = s->next_recv_buf;			
			if(!head->next){
				head->next = chk_bytechunk_new(NULL,s->option.recv_buffer_size);
				if(!head->next){
					CHK_SYSLOG(LOG_ERROR,"chk_bytechunk_new() failed recv_buffer_size:%d",s->option.recv_buffer_size);						
					return -1;
				}
			}
			s->next_recv_buf = chk_bytechunk_retain(head->next);
			chk_bytechunk_release(head);					
		}
	};
	return 0;
}

static void release_socket(chk_stream_socket *s) {
	chk_bytebuffer  *b;
	chk_decoder *d = s->option.decoder;	
	chk_unwatch_handle(cast(chk_handle*,s));	
	if(s->next_recv_buf) chk_bytechunk_release(s->next_recv_buf);
	if(d && d->dctor) d->dctor(d);
	if(s->timer) chk_timer_unregister(s->timer);
	while((b = cast(chk_bytebuffer*,chk_list_pop(&s->send_list))))
		chk_bytebuffer_del(b);

	if(s->fd >= 0) { 
		close(s->fd);
	}

	if(s->ssl.ctx) {
		SSL_CTX_free(s->ssl.ctx);
	}

	if(s->ssl.ssl) {
       	SSL_free(s->ssl.ssl);
	}

	if(s->create_by_new) free(s); /*stream_socket是通过new接口创建的，需要释放内存*/
}

int32_t last_timer_cb(uint64_t tick,void*ud) {
	chk_stream_socket *s = cast(chk_stream_socket*,ud);
	s->timer   = NULL;
	/*timer事件在所有套接口事件之后才处理,所以这里释放是安全的*/
	release_socket(s);
	return -1;
}

void chk_stream_socket_close(chk_stream_socket *s,uint32_t delay) {
	if(s->status & (SOCKET_CLOSE | SOCKET_RCLOSE))
		return;
	
	if(delay > 0 && 
	   !(s->status & SOCKET_PEERCLOSE) && 
	   !send_list_empty(s) && s->loop) {
		s->status |= SOCKET_RCLOSE;
		chk_disable_read(cast(chk_handle*,s));
		shutdown(s->fd,SHUT_RD);
		/*数据还没发送完,设置delay豪秒超时等待数据发送出去*/
		s->timer = chk_loop_addtimer(s->loop,delay,last_timer_cb,s);
	}else {
		s->status |= SOCKET_CLOSE;		
		if(!(s->status & SOCKET_INLOOP)){
			release_socket(s);
		}
	}
}

void chk_stream_socket_setUd(chk_stream_socket *s,void*ud) {
	s->ud = ud;
}

void *chk_stream_socket_getUd(chk_stream_socket *s) {
	return s->ud;
}


static int32_t loop_add(chk_event_loop *e,chk_handle *h,chk_event_callback cb) {
	int32_t ret,flags;
	chk_stream_socket *s = cast(chk_stream_socket*,h);
	if(!e || !h || !cb){		
		if(!e) {
			CHK_SYSLOG(LOG_ERROR,"chk_event_loop == NULL");				
		}

		if(!h) {
			CHK_SYSLOG(LOG_ERROR,"chk_handle == NULL");				
		}

		if(!cb) {
			CHK_SYSLOG(LOG_ERROR,"chk_event_callback == NULL");				
		}				

		return chk_error_invaild_argument;
	}
	if(s->status & (SOCKET_CLOSE | SOCKET_RCLOSE)){
		CHK_SYSLOG(LOG_ERROR,"chk_stream_socket close");			
		return chk_error_socket_close;
	}
	if(!send_list_empty(s))
		flags = CHK_EVENT_READ | CHK_EVENT_WRITE;
	else
		flags = CHK_EVENT_READ;	
	if(chk_error_ok == (ret = chk_watch_handle(e,h,flags))) {
		easy_noblock(h->fd,1);
		s->cb = cast(chk_stream_socket_cb,cb);
	}
	else {
		CHK_SYSLOG(LOG_ERROR,"chk_watch_handle() failed:%d",ret);		
	}
	return ret;
}

static int32_t ssl_again(int32_t ssl_error) {
	if(ssl_error == SSL_ERROR_WANT_WRITE || ssl_error == SSL_ERROR_WANT_READ) {
		return 1;
	}
	else {
		return 0;
	}
}

static int32_t do_write(chk_stream_socket *s,int32_t bc) {
	errno = 0;
	if(s->ssl.ssl) {
		int32_t bytes_transfer = TEMP_FAILURE_RETRY(SSL_write(s->ssl.ssl,s->wsendbuf[0].iov_base,s->wsendbuf[0].iov_len));
		int ssl_error = SSL_get_error(s->ssl.ssl,bytes_transfer);
		if(bytes_transfer <= 0 && ssl_again(ssl_error)){
			errno = EAGAIN;
			//printf("do_write ssl_again\n");
			return 0;
		}		
		return bytes_transfer;
	}
	else{
		return TEMP_FAILURE_RETRY(writev(s->fd,&s->wsendbuf[0],bc));
	}
}


static void process_write(chk_stream_socket *s) {
	int32_t bc,bytes;
	bc = prepare_send(s);
	
	if(bc <= 0) {
		CHK_SYSLOG(LOG_ERROR,"bc <= 0");
		return;
	}

	if((bytes = do_write(s,bc)) > 0) {
		update_send_list(s,bytes);
		/*没有数据需要发送了,停止写监听*/
		if(send_list_empty(s)) {
		 	if(s->status & SOCKET_RCLOSE)
				s->status |= SOCKET_CLOSE;
			else
		 		chk_disable_write(cast(chk_handle*,s));
		}	
	}else {
		if(errno == EAGAIN){
			return;
		}
		if(s->status & SOCKET_RCLOSE)
			s->status |= SOCKET_CLOSE;
		else {
			s->status |= SOCKET_PEERCLOSE;
			s->cb(s,NULL,chk_error_stream_write);
			chk_stream_socket_close(s,0);
			CHK_SYSLOG(LOG_ERROR,"writev() failed errno:%d",errno);
		}
	}
}

static int32_t do_read(chk_stream_socket *s,int32_t bc) {
	errno = 0;
	if(s->ssl.ssl) {
		int32_t bytes_transfer = TEMP_FAILURE_RETRY(SSL_read(s->ssl.ssl,s->wrecvbuf[0].iov_base,s->wrecvbuf[0].iov_len));
		int ssl_error = SSL_get_error(s->ssl.ssl,bytes_transfer);
		if(bytes_transfer <= 0 && ssl_again(ssl_error)){
			errno = EAGAIN;
			//printf("do_read ssl_again\n");			
			return 0;
		}
		return 	bytes_transfer;	
	}
	else {
		return TEMP_FAILURE_RETRY(readv(s->fd,&s->wrecvbuf[0],bc));
	}
}

static void process_read(chk_stream_socket *s) {
	int32_t bc,bytes,unpackerr,error_code;
	chk_decoder *decoder;
	chk_bytebuffer *b;
	bc    = prepare_recv(s);
	if(bc <= 0) {
		s->cb(s,NULL,chk_error_no_memory);
		chk_stream_socket_close(s,0);		
		return;
	}
	bytes =  do_read(s,bc);
	if(bytes > 0 ) {
		decoder = s->option.decoder;
		decoder->update(decoder,s->next_recv_buf,s->next_recv_pos,bytes);
		for(;;) {
			unpackerr = 0;
			if((b = decoder->unpack(decoder,&unpackerr))) {
				s->cb(s,b,chk_error_ok);
				chk_bytebuffer_del(b);
				if(s->status & (SOCKET_CLOSE | SOCKET_RCLOSE)) 
					break;
			}else {
				if(unpackerr){
					CHK_SYSLOG(LOG_ERROR,"decoder->unpack error:%d",unpackerr);					
					s->cb(s,NULL,chk_error_unpack);
					chk_stream_socket_close(s,0);
				}
				break;
			}
		};
		if(!(s->status & (SOCKET_CLOSE | SOCKET_RCLOSE)))
			update_next_recv_pos(s,bytes);
	}else {
		s->status |= SOCKET_PEERCLOSE;
		if(bytes == 0)
			error_code = chk_error_stream_peer_close;
		else {
			CHK_SYSLOG(LOG_ERROR,"read failed errno:%d",errno);
			error_code = chk_error_stream_read;
		}
		s->cb(s,NULL,error_code);
		chk_stream_socket_close(s,0);
	}
}

int32_t chk_stream_socket_send(chk_stream_socket *s,chk_bytebuffer *b) {
	int32_t ret = chk_error_ok;

	if(b->flags & READ_ONLY) {
		CHK_SYSLOG(LOG_ERROR,"chk_bytebuffer is read only");		
		return chk_error_buffer_read_only;
	}

	if(b->datasize == 0) {
		CHK_SYSLOG(LOG_ERROR,"b->datasize == 0");		
		return chk_error_invaild_buffer;
	}
	
	do {
		if(s->status & (SOCKET_CLOSE | SOCKET_RCLOSE | SOCKET_PEERCLOSE)) {
			CHK_SYSLOG(LOG_ERROR,"chk_stream_socket close");	
			chk_bytebuffer_del(b);	
			ret = chk_error_socket_close;
			break;
		}
		b->internal = b->datasize;//记录最初需要发送的数据大小
		chk_list_pushback(&s->send_list,cast(chk_list_entry*,b));
		if(s->loop && !chk_is_write_enable(cast(chk_handle*,s))) 
			chk_enable_write(cast(chk_handle*,s));
	}while(0);
	return ret;	
}


int32_t chk_stream_socket_send_urgent(chk_stream_socket *s,chk_bytebuffer *b) {
	int32_t ret = chk_error_ok;

	if(b->flags & READ_ONLY) {
		CHK_SYSLOG(LOG_ERROR,"chk_bytebuffer is read only");		
		return chk_error_buffer_read_only;
	}

	if(b->datasize == 0) {
		CHK_SYSLOG(LOG_ERROR,"b->datasize == 0");		
		return chk_error_invaild_buffer;
	}
	
	do {
		if(s->status & (SOCKET_CLOSE | SOCKET_RCLOSE | SOCKET_PEERCLOSE)) {
			CHK_SYSLOG(LOG_ERROR,"chk_stream_socket close");			
			chk_bytebuffer_del(b);	
			ret = chk_error_socket_close;
			break;
		}
		b->internal = b->datasize;//记录最初需要发送的数据大小		
		chk_list_pushback(&s->urgent_list,cast(chk_list_entry*,b));
		if(s->loop && !chk_is_write_enable(cast(chk_handle*,s))) 
			chk_enable_write(cast(chk_handle*,s));
	}while(0);
	return ret;	
}

static void on_events(chk_handle *h,int32_t events) {
	chk_stream_socket *s = cast(chk_stream_socket*,h);
	if(!s->loop || s->status & SOCKET_CLOSE)
		return;
	if(events == CHK_EVENT_LOOPCLOSE) {
		s->cb(s,NULL,chk_error_loop_close);
		return;
	}
	s->status |= SOCKET_INLOOP;
	do {
		if(events & CHK_EVENT_READ){
			if(s->status & SOCKET_RCLOSE) {
				s->status |= SOCKET_CLOSE;
				break;
			}
			process_read(s);
			if((s->status & SOCKET_CLOSE) || !s->loop) 
				break;
		}		
		if(events & CHK_EVENT_WRITE) process_write(s);			
	}while(0);
	s->status ^= SOCKET_INLOOP;
	if(s->status & SOCKET_CLOSE) {
		release_socket(s);		
	}
}

int32_t chk_stream_socket_init(chk_stream_socket *s,int32_t fd,const chk_stream_socket_option *op) {
	assert(s);
	easy_close_on_exec(fd);
	s->fd = fd;
	s->on_events = on_events;
	s->handle_add = loop_add;
	s->option = *op;
	s->loop   = NULL;
	s->create_by_new = 0;
	if(!s->option.decoder) { 
		s->option.decoder = cast(chk_decoder*,default_decoder_new());
		if(!s->option.decoder) { 
			CHK_SYSLOG(LOG_ERROR,"default_decoder_new() failed");			
			return -1;
		}
	}
	return 0;	
}

chk_stream_socket *chk_stream_socket_new(int32_t fd,const chk_stream_socket_option *op) {
	chk_stream_socket *s = calloc(1,sizeof(*s));
	if(!s) {
		CHK_SYSLOG(LOG_ERROR,"calloc chk_stream_socket failed");			
		return NULL;
	}
	if(0 != chk_stream_socket_init(s,fd,op)) {
		free(s);
		return NULL;
	}
	s->create_by_new = 1;
	return s;
}

void  chk_stream_socket_pause(chk_stream_socket *s) {
	if(s->loop) {
		chk_disable_read(cast(chk_handle*,s));
		chk_disable_write(cast(chk_handle*,s));
	}
}

void  chk_stream_socket_resume(chk_stream_socket *s) {
	if(s->loop) {
		chk_enable_read(cast(chk_handle*,s));
		if(!send_list_empty(s))
			chk_enable_write(cast(chk_handle*,s));
	}
}


int32_t chk_ssl_connect(chk_stream_socket *s) {
	SSL_CTX *ctx = SSL_CTX_new(SSLv23_client_method());
	if (ctx == NULL) {
	    ERR_print_errors_fp(stdout);
	    return -1;
	}
	s->ssl.ssl = SSL_new(ctx);
	if(!s->ssl.ssl) {
		SSL_CTX_free(ctx);
		return -1;
	}
	s->ssl.ctx = ctx;
	
	if(1 != SSL_set_fd(s->ssl.ssl,s->fd)) {
		ERR_print_errors_fp(stdout);
		SSL_CTX_free(ctx);
       	SSL_free(s->ssl.ssl);
       	s->ssl.ssl = NULL;
       	s->ssl.ctx = NULL;		
	}

/*	int32_t ret = SSL_connect(s->ssl.ssl);
	if(ret > 0) {
		return 0;
	}
	else {
		int32_t ssl_error = SSL_get_error(s->ssl.ssl,ret);
		if(ssl_again(ssl_error)){
			return 0;
		}
		else {
			ERR_print_errors_fp(stdout);
			SSL_CTX_free(ctx);
       		SSL_free(s->ssl.ssl);
       		s->ssl.ssl = NULL;
       		s->ssl.ctx = NULL;		
			return -1;			
		}
	}
*/

	easy_noblock(s->fd,0);

	if(SSL_connect(s->ssl.ssl) == -1){
		ERR_print_errors_fp(stdout);
		SSL_CTX_free(ctx);
       	SSL_free(s->ssl.ssl);
       	s->ssl.ssl = NULL;
       	s->ssl.ctx = NULL;		
		return -1;
	}
	return 0;

}

int32_t chk_ssl_accept(chk_stream_socket *s,SSL_CTX *ctx) {
	s->ssl.ssl = SSL_new(ctx);
	if(!s->ssl.ssl) {
	    ERR_print_errors_fp(stdout);		
		return -1;
	}

	if(1 != SSL_set_fd(s->ssl.ssl,s->fd)){
		printf("SSL_set_fd() error\n");
		ERR_print_errors_fp(stdout);		
		SSL_free(s->ssl.ssl);
		s->ssl.ssl = NULL;		
		return -1;
	}

	/*easy_noblock(s->fd,1);	

	int32_t ret = SSL_accept(s->ssl.ssl);

	if(ret > 0) {
		return 0;
	}
	else {
		int32_t ssl_error = SSL_get_error(s->ssl.ssl,ret);
		if(ssl_again(ssl_error)){
			return 0;
		}
		else {
			printf("SSL_accept() error:%d\n",SSL_get_error(s->ssl.ssl,ret));	
			SSL_free(s->ssl.ssl);
			s->ssl.ssl = NULL;
	    	ERR_print_errors_fp(stdout);
			return -1;			
		}
	}*/

	easy_noblock(s->fd,0);
	
	int32_t ret = SSL_accept(s->ssl.ssl);

	if(ret != 1) {
		printf("SSL_accept() error:%d\n",SSL_get_error(s->ssl.ssl,ret));	
		SSL_free(s->ssl.ssl);
		s->ssl.ssl = NULL;
	    ERR_print_errors_fp(stdout);
		return -1;
	}
	return 0;
}
