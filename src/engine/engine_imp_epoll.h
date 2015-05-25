#include <fcntl.h>              /* Obtain O_* constant definitions */
#include <unistd.h>
#include <assert.h>
#include "util/time.h"
#include "thread/thread.h"

extern int32_t pipe2(int pipefd[2], int flags);

enum{
	INLOOP  =  1 << 1,
	CLOSING =  1 << 2,
	LUAOBj  =  1 << 3,
};

typedef struct engine{
	engine_head;
	int32_t    epfd;
	struct     epoll_event* events;
	int32_t    maxevents;
}engine;

int32_t 
event_add(engine *e,handle *h,
		  int32_t events)
{
	assert((events & EPOLLET) == 0);
	struct epoll_event ev = {0};
	ev.data.ptr = h;
	ev.events = events;
	errno = 0;
	if(0 != epoll_ctl(e->epfd,EPOLL_CTL_ADD,h->fd,&ev)) 
		return -errno;
	h->events = events;
	h->e = e;
	dlist_pushback(&e->handles,(dlistnode*)h);
	return 0;
}


int32_t 
event_remove(handle *h)
{
	struct epoll_event ev = {0};
	errno = 0;
	engine *e = h->e;
	if(0 != epoll_ctl(e->epfd,EPOLL_CTL_DEL,h->fd,&ev)) 
		return -errno; 
	h->events = 0;
	h->e = NULL;
	dlist_remove((dlistnode*)h);
	return 0;	
}

int32_t 
event_mod(handle *h,int32_t events)
{
	assert((events & EPOLLET) == 0);
	engine *e = h->e;	
	struct epoll_event ev = {0};
	ev.data.ptr = h;
	ev.events = events;
	errno = 0;
	if(0 != epoll_ctl(e->epfd,EPOLL_CTL_MOD,h->fd,&ev)) 
		return -errno; 
	h->events = events;		
	return 0;	
}


int32_t 
event_enable(handle *h,int32_t events)
{
	return event_mod(h,h->events | events);
}

int32_t 
event_disable(handle *h,int32_t events)
{
	return event_mod(h,h->events & (~events));
}

void 
timerfd_callback(void *ud)
{
	wheelmgr *mgr = (wheelmgr*)ud;
	wheelmgr_tick(mgr,systick64());
}

timer*
engine_regtimer(engine *e,uint32_t timeout,
			    int32_t(*cb)(uint32_t,uint64_t,void*),
			    void *ud)
{
	if(!e->tfd){
		e->timermgr = wheelmgr_new();
		e->tfd      = timerfd_new(1,e->timermgr);
		engine_associate(e,e->tfd,timerfd_callback);
	}
	return wheelmgr_register(e->timermgr,timeout,cb,ud,systick64());
}


static int32_t 
engine_init(engine *e)
{
	int32_t epfd = epoll_create1(EPOLL_CLOEXEC);
	if(epfd < 0) return -1;
	int32_t tmp[2];
	if(pipe2(tmp,O_NONBLOCK|O_CLOEXEC) != 0){
		close(epfd);
		return -1;
	}		
	e->epfd = epfd;
	e->maxevents = 64;
	e->events = calloc(1,(sizeof(*e->events)*e->maxevents));
	e->notifyfds[0] = tmp[0];
	e->notifyfds[1] = tmp[1];

	struct epoll_event ev = {0};
	ev.data.fd = e->notifyfds[0];
	ev.events = EPOLLIN;
	if(0 != epoll_ctl(e->epfd,EPOLL_CTL_ADD,ev.data.fd,&ev)){
		close(epfd);
		close(tmp[0]);
		close(tmp[1]);
		free(e->events);
		return -1;
	}
	e->threadid = thread_id();
	dlist_init(&e->handles);	
	return 0;
} 

engine* 
engine_new()
{
	engine *ep = calloc(1,sizeof(*ep));
	if(0 != engine_init(ep)){
		free(ep);
		ep = NULL;
	}
	return ep;
}

static int32_t 
lua_engine_new(lua_State *L)
{
	engine *ep = (engine*)lua_newuserdata(L, sizeof(*ep));
	memset(ep,0,sizeof(*ep));
	if(0 != engine_init(ep)){
		free(ep);
		lua_pushnil(L);
		return 1;
	}
	ep->status &= LUAOBj;	
	luaL_getmetatable(L, LUAENGINE_METATABLE);
	lua_setmetatable(L, -2);
	return 1;
}

static inline void
_engine_del(engine *e)
{
	handle *h;
	if(e->tfd){
		engine_remove((handle*)e->tfd);
		wheelmgr_del(e->timermgr);
	}
	while((h = (handle*)dlist_pop(&e->handles))){
		event_remove(h);
		h->on_events(h,EENGCLOSE);
	}
	close(e->epfd);
	close(e->notifyfds[0]);
	close(e->notifyfds[1]);
	free(e->events);
}
void 
engine_del(engine *e)
{
	assert(e->threadid == thread_id());
	if(e->status & INLOOP)
		e->status |= CLOSING;
	else{
		_engine_del(e);
		free(e);
	}
}

void
engine_del_lua(engine *e)
{
	assert(e->threadid == thread_id());
	if(e->status & INLOOP)
		e->status |= CLOSING;
	else{	
		_engine_del(e);
	}
}

int32_t
engine_runonce(engine *e,uint32_t timeout)
{
	errno = 0;
	int32_t i;
	int32_t ret = 0;
	handle *h;
	int32_t nfds = TEMP_FAILURE_RETRY(epoll_wait(e->epfd,e->events,e->maxevents,timeout));
	if(nfds > 0){
		e->status |= INLOOP;
		for(i=0; i < nfds ; ++i)
		{
			if(e->events[i].data.fd == e->notifyfds[0]){
				int32_t _;
				while(TEMP_FAILURE_RETRY(read(e->notifyfds[0],&_,sizeof(_))) > 0);
				break;	
			}else{
				h = (handle*)e->events[i].data.ptr;
				h->on_events(h,e->events[i].events);;
			}
		}
		e->status ^= INLOOP;
		if(nfds == e->maxevents){
			free(e->events);
			e->maxevents <<= 2;
			e->events = calloc(1,sizeof(*e->events)*e->maxevents);
		}				
	}else if(nfds < 0){
		ret = -errno;
	}
	if(e->status & CLOSING){
		ret = -EENGCLOSE;
		if(e->status & LUAOBj)
			engine_del_lua(e);
		else
			engine_del(e);
	}
	return ret;	
}

int32_t 
engine_run(engine *e)
{
	int32_t ret = 0;
	for(;;){
		errno = 0;
		int32_t i;
		handle *h;
		int32_t nfds = TEMP_FAILURE_RETRY(epoll_wait(e->epfd,e->events,e->maxevents,-1));
		if(nfds > 0){
			e->status |= INLOOP;
			for(i=0; i < nfds ; ++i)
			{
				if(e->events[i].data.fd == e->notifyfds[0]){
					int32_t _;
					while(TEMP_FAILURE_RETRY(read(e->notifyfds[0],&_,sizeof(_))) > 0);
					break;	
				}else{
					h = (handle*)e->events[i].data.ptr;
					h->on_events(h,e->events[i].events);
				}
			}
			e->status ^= INLOOP;
			if(e->status & CLOSING)
				break;
			if(nfds == e->maxevents){
				free(e->events);
				e->maxevents <<= 2;
				e->events = calloc(1,sizeof(*e->events)*e->maxevents);
			}				
		}else if(nfds < 0){
			ret = -errno;
		}	
	}
	if(e->status & CLOSING){
		ret = -EENGCLOSE;
		if(e->status & LUAOBj)
			engine_del_lua(e);
		else
			engine_del(e);
	}	
	return ret;
}


void 
engine_stop(engine *e)
{
	int32_t _;
	TEMP_FAILURE_RETRY(write(e->notifyfds[1],&_,sizeof(_)));
}

