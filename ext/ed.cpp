/*****************************************************************************

$Id$

File:     ed.cpp
Date:     06Apr06

Copyright (C) 2006-07 by Francis Cianfrocca. All Rights Reserved.
Gmail: blackhedd

This program is free software; you can redistribute it and/or modify
it under the terms of either: 1) the GNU General Public License
as published by the Free Software Foundation; either version 2 of the
License, or (at your option) any later version; or 2) Ruby's License.

See the file COPYING for complete licensing information.

*****************************************************************************/

#include "project.h"



/********************
SetSocketNonblocking
********************/

bool SetSocketNonblocking (SOCKET sd)
{
	#ifdef OS_UNIX
	int val = fcntl (sd, F_GETFL, 0);
	return (fcntl (sd, F_SETFL, val | O_NONBLOCK) != SOCKET_ERROR) ? true : false;
	#endif

	#ifdef OS_WIN32
	#ifdef BUILD_FOR_RUBY
	// 14Jun09 Ruby provides its own wrappers for ioctlsocket. On 1.8 this is a simple wrapper,
	// however, 1.9 keeps its own state about the socket.
	// NOTE: F_GETFL is not supported
	return (fcntl (sd, F_SETFL, O_NONBLOCK) == 0) ? true : false;
	#else
	unsigned long one = 1;
	return (ioctlsocket (sd, FIONBIO, &one) == 0) ? true : false;
	#endif
	#endif
}

/************
SetFdCloexec
************/

#ifdef OS_UNIX
bool SetFdCloexec (int fd)
{
	int flags = fcntl(fd, F_GETFD, 0);
	assert (flags >= 0);
	flags |= FD_CLOEXEC;
	return (fcntl(fd, F_SETFD, FD_CLOEXEC) == 0) ? true : false;
}
#else
bool SetFdCloexec (int fd UNUSED)
{
	return true;
}
#endif

/****************************************
EventableDescriptor::EventableDescriptor
****************************************/

EventableDescriptor::EventableDescriptor (SOCKET sd, EventMachine_t *em):
	bCloseNow (false),
	bCloseAfterWriting (false),
	MySocket (sd),
	bAttached (false),
	bWatchOnly (false),
	EventCallback (NULL),
	bCallbackUnbind (true),
	UnbindReasonCode (0),
	ProxyTarget(NULL),
	ProxiedFrom(NULL),
	ProxiedBytes(0),
	MaxOutboundBufSize(0),
	MyEventMachine (em),
	PendingConnectTimeout(20000000),
	InactivityTimeout (0),
	NextHeartbeat (0),
	bPaused (false)
{
	/* There are three ways to close a socket, all of which should
	 * automatically signal to the event machine that this object
	 * should be removed from the polling scheduler.
	 * First is a hard close, intended for bad errors or possible
	 * security violations. It immediately closes the connection
	 * and puts this object into an error state.
	 * Second is to set bCloseNow, which will cause the event machine
	 * to delete this object (and thus close the connection in our
	 * destructor) the next chance it gets. bCloseNow also inhibits
	 * the writing of new data on the socket (but not necessarily
	 * the reading of new data).
	 * The third way is to set bCloseAfterWriting, which inhibits
	 * the writing of new data and converts to bCloseNow as soon
	 * as everything in the outbound queue has been written.
	 * bCloseAfterWriting is really for use only by protocol handlers
	 * (for example, HTTP writes an HTML page and then closes the
	 * connection). All of the error states we generate internally
	 * cause an immediate close to be scheduled, which may have the
	 * effect of discarding outbound data.
	 */

	if (sd == INVALID_SOCKET)
		throw std::runtime_error ("bad eventable descriptor");
	if (MyEventMachine == NULL)
		throw std::runtime_error ("bad em in eventable descriptor");
	CreatedAt = MyEventMachine->GetCurrentLoopTime();
	LastActivity = MyEventMachine->GetCurrentLoopTime();

	#ifdef HAVE_EPOLL
	EpollEvent.events = 0;
	EpollEvent.data.ptr = this;
	#endif
}


/*****************************************
EventableDescriptor::~EventableDescriptor
*****************************************/

EventableDescriptor::~EventableDescriptor() NO_EXCEPT_FALSE
{
	if (NextHeartbeat)
		MyEventMachine->ClearHeartbeat(NextHeartbeat, this);
	if (EventCallback && bCallbackUnbind)
		(*EventCallback)(GetBinding(), EM_CONNECTION_UNBOUND, NULL, UnbindReasonCode);
	if (ProxiedFrom) {
		(*EventCallback)(ProxiedFrom->GetBinding(), EM_PROXY_TARGET_UNBOUND, NULL, 0);
		ProxiedFrom->StopProxy();
	}
	MyEventMachine->NumCloseScheduled--;
	StopProxy();
	Close();
}


/*************************************
EventableDescriptor::SetEventCallback
*************************************/

void EventableDescriptor::SetEventCallback (EMCallback cb)
{
	EventCallback = cb;
}


/**************************
EventableDescriptor::Close
**************************/

void EventableDescriptor::Close()
{
	/* EventMachine relies on the fact that when close(fd)
	 * is called that the fd is removed from any
	 * epoll event queues.
	 *
	 * However, this is not *always* the behavior of close(fd)
	 *
	 * See man 4 epoll Q6/A6 and then consider what happens
	 * when using pipes with eventmachine.
	 * (As is often done when communicating with a subprocess)
	 *
	 * The pipes end up looking like:
	 *
	 * ls -l /proc/<pid>/fd
	 * ...
	 * lr-x------ 1 root root 64 2011-08-19 21:31 3 -> pipe:[940970]
	 * l-wx------ 1 root root 64 2011-08-19 21:31 4 -> pipe:[940970]
	 *
	 * This meets the critera from man 4 epoll Q6/A4 for not
	 * removing fds from epoll event queues until all fds
	 * that reference the underlying file have been removed.
	 *
	 * If the EventableDescriptor associated with fd 3 is deleted,
	 * its dtor will call EventableDescriptor::Close(),
	 * which will call ::close(int fd).
	 *
	 * However, unless the EventableDescriptor associated with fd 4 is
	 * also deleted before the next call to epoll_wait, events may fire
	 * for fd 3 that were registered with an already deleted
	 * EventableDescriptor.
	 *
	 * Therefore, it is necessary to notify EventMachine that
	 * the fd associated with this EventableDescriptor is
	 * closing.
	 *
	 * EventMachine also never closes fds for STDIN, STDOUT and
	 * STDERR (0, 1 & 2)
	 */

	// Close the socket right now. Intended for emergencies.
	if (MySocket != INVALID_SOCKET) {
		MyEventMachine->Deregister (this);
		
		// Do not close STDIN, STDOUT, STDERR
		if (MySocket > 2 && !bAttached) {
			shutdown (MySocket, 1);
			close (MySocket);
		}
		
		MySocket = INVALID_SOCKET;
	}
}


/*********************************
EventableDescriptor::ShouldDelete
*********************************/

bool EventableDescriptor::ShouldDelete()
{
	/* For use by a socket manager, which needs to know if this object
	 * should be removed from scheduling events and deleted.
	 * Has an immediate close been scheduled, or are we already closed?
	 * If either of these are the case, return true. In theory, the manager will
	 * then delete us, which in turn will make sure the socket is closed.
	 * Note, if bCloseAfterWriting is true, we check a virtual method to see
	 * if there is outbound data to write, and only request a close if there is none.
	 */

	return ((MySocket == INVALID_SOCKET) || bCloseNow || (bCloseAfterWriting && (GetOutboundDataSize() <= 0)));
}


/**********************************
EventableDescriptor::ScheduleClose
**********************************/

void EventableDescriptor::ScheduleClose (bool after_writing)
{
	if (IsCloseScheduled()) {
		if (!after_writing) {
			// If closing has become more urgent, then upgrade the scheduled
			// after_writing close to one NOW.
			bCloseNow = true;
		}
		return;
	}
	MyEventMachine->NumCloseScheduled++;
	// KEEP THIS SYNCHRONIZED WITH ::IsCloseScheduled.
	if (after_writing)
		bCloseAfterWriting = true;
	else
		bCloseNow = true;
}


/*************************************
EventableDescriptor::IsCloseScheduled
*************************************/

bool EventableDescriptor::IsCloseScheduled()
{
	// KEEP THIS SYNCHRONIZED WITH ::ScheduleClose.
	return (bCloseNow || bCloseAfterWriting);
}


/***********************************
EventableDescriptor::EnableKeepalive
***********************************/
int EventableDescriptor::EnableKeepalive(int idle, int intvl, int cnt)
{
	int ret;

#ifdef OS_WIN32
	DWORD len;
	struct tcp_keepalive args;

	args.onoff = 1;

	if (idle > 0)
		args.keepalivetime = idle * 1000;
	if (intvl > 0)
		args.keepaliveinterval = intvl * 1000;

	ret = WSAIoctl(MySocket, SIO_KEEPALIVE_VALS, &args, sizeof(args), NULL, 0, &len, NULL, NULL);
	if (ret < 0) {
		int err = WSAGetLastError();
		char buf[200];
		buf[0] = '\0';

		FormatMessage (FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL, err, MAKELANGID (LANG_NEUTRAL, SUBLANG_DEFAULT), buf, sizeof(buf), NULL);

		if (! buf[0])
			snprintf (buf, sizeof(buf)-1, "unable to enable keepalive: %d", err);

		throw std::runtime_error (buf);
	}

#else
	int val = 1;

	// All the Unixen
	ret = setsockopt(MySocket, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val));
	if (ret < 0) {
		char buf[200];
		snprintf (buf, sizeof(buf)-1, "unable to enable keepalive: %s", strerror(errno));
		throw std::runtime_error (buf);
	}

	#ifdef TCP_KEEPALIVE
	// BSDs and Mac OS X: idle time used when SO_KEEPALIVE is enabled
	// 0 means use the system default value, so we let it through
	if (idle >= 0) {
		ret = setsockopt(MySocket, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
		if (ret < 0) {
			char buf[200];
			snprintf (buf, sizeof(buf)-1, "unable set keepalive idle: %s", strerror(errno));
			throw std::runtime_error (buf);
		}
	}
	#endif
	#ifdef TCP_KEEPIDLE
	// Linux: interval between last data pkt and first keepalive pkt
	if (idle > 0) {
		ret = setsockopt(MySocket, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
		if (ret < 0) {
			char buf[200];
			snprintf (buf, sizeof(buf)-1, "unable set keepalive idle: %s", strerror(errno));
			throw std::runtime_error (buf);
		}
	}
	#endif
	#ifdef TCP_KEEPINTVL
	// Linux (and recent BSDs, Mac OS X, Solaris): interval between keepalives
	if (intvl > 0) {
		ret = setsockopt(MySocket, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
		if (ret < 0) {
			char buf[200];
			snprintf (buf, sizeof(buf)-1, "unable set keepalive interval: %s", strerror(errno));
			throw std::runtime_error (buf);
		}
	}
	#endif
	#ifdef TCP_KEEPCNT
	// Linux (and recent BSDs, Mac OS X, Solaris): number of dropped probes before disconnect
	if (cnt > 0) {
		ret = setsockopt(MySocket, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
		if (ret < 0) {
			char buf[200];
			snprintf (buf, sizeof(buf)-1, "unable set keepalive count: %s", strerror(errno));
			throw std::runtime_error (buf);
		}
	}
	#endif
#endif

	return ret;
}

/***********************************
EventableDescriptor::DisableKeepalive
***********************************/
int EventableDescriptor::DisableKeepalive()
{
	int ret;

#ifdef OS_WIN32
	DWORD len;
	struct tcp_keepalive args;
	args.onoff = 0;
	ret = WSAIoctl(MySocket, SIO_KEEPALIVE_VALS, &args, sizeof(args), NULL, 0, &len, NULL, NULL);
	if (ret < 0) {
		int err = WSAGetLastError();
		char buf[200];
		buf[0] = '\0';

		FormatMessage (FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL, err, MAKELANGID (LANG_NEUTRAL, SUBLANG_DEFAULT), buf, sizeof(buf), NULL);

		if (! buf[0])
			snprintf (buf, sizeof(buf)-1, "unable to enable keepalive: %d", err);

		throw std::runtime_error (buf);
	}
#else
	int val = 0;
	ret = setsockopt(MySocket, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val));
	if (ret < 0) {
		char buf[200];
		snprintf (buf, sizeof(buf)-1, "unable to disable keepalive: %s", strerror(errno));
		throw std::runtime_error (buf);
	}
#endif

	return ret;
}


/*******************************
EventableDescriptor::StartProxy
*******************************/

void EventableDescriptor::StartProxy(const uintptr_t to, const unsigned long bufsize, const unsigned long length)
{
	EventableDescriptor *ed = dynamic_cast <EventableDescriptor*> (Bindable_t::GetObject (to));
	if (ed) {
		StopProxy();
		ProxyTarget = ed;
		BytesToProxy = length;
		ProxiedBytes = 0;
		ed->SetProxiedFrom(this, bufsize);
		return;
	}
	throw std::runtime_error ("Tried to proxy to an invalid descriptor");
}


/******************************
EventableDescriptor::StopProxy
******************************/

void EventableDescriptor::StopProxy()
{
	if (ProxyTarget) {
		ProxyTarget->SetProxiedFrom(NULL, 0);
		ProxyTarget = NULL;
	}
}


/***********************************
EventableDescriptor::SetProxiedFrom
***********************************/

void EventableDescriptor::SetProxiedFrom(EventableDescriptor *from, const unsigned long bufsize)
{
	if (from != NULL && ProxiedFrom != NULL)
		throw std::runtime_error ("Tried to proxy to a busy target");

	ProxiedFrom = from;
	MaxOutboundBufSize = bufsize;
}


/********************************************
EventableDescriptor::_GenericInboundDispatch
********************************************/

void EventableDescriptor::_GenericInboundDispatch(const char *buf, unsigned long size)
{
	assert(EventCallback);

	if (ProxyTarget) {
		if (BytesToProxy > 0) {
			unsigned long proxied = std::min(BytesToProxy, size);
			ProxyTarget->SendOutboundData(buf, proxied);
			ProxiedBytes += (unsigned long) proxied;
			BytesToProxy -= proxied;
			if (BytesToProxy == 0) {
				StopProxy();
				(*EventCallback)(GetBinding(), EM_PROXY_COMPLETED, NULL, 0);
				if (proxied < size) {
					(*EventCallback)(GetBinding(), EM_CONNECTION_READ, buf + proxied, size - proxied);
				}
			}
		} else {
			ProxyTarget->SendOutboundData(buf, size);
			ProxiedBytes += size;
		}
	} else {
		(*EventCallback)(GetBinding(), EM_CONNECTION_READ, buf, size);
	}
}


/*********************************
EventableDescriptor::_GenericGetPeername
*********************************/

bool EventableDescriptor::_GenericGetPeername (struct sockaddr *s, socklen_t *len)
{
	if (!s)
		return false;

	int gp = getpeername (GetSocket(), s, len);
	if (gp == -1) {
		char buf[200];
		snprintf (buf, sizeof(buf)-1, "unable to get peer name: %s", strerror(errno));
		throw std::runtime_error (buf);
	}

	return true;
}

/*********************************
EventableDescriptor::_GenericGetSockname
*********************************/

bool EventableDescriptor::_GenericGetSockname (struct sockaddr *s, socklen_t *len)
{
	if (!s)
		return false;

	int gp = getsockname (GetSocket(), s, len);
	if (gp == -1) {
		char buf[200];
		snprintf (buf, sizeof(buf)-1, "unable to get sock name: %s", strerror(errno));
		throw std::runtime_error (buf);
	}

	return true;
}


/*********************************************
EventableDescriptor::GetPendingConnectTimeout
*********************************************/

uint64_t EventableDescriptor::GetPendingConnectTimeout()
{
	return PendingConnectTimeout / 1000;
}


/*********************************************
EventableDescriptor::SetPendingConnectTimeout
*********************************************/

int EventableDescriptor::SetPendingConnectTimeout (uint64_t value)
{
	if (value > 0) {
		PendingConnectTimeout = value * 1000;
		MyEventMachine->QueueHeartbeat(this);
		return 1;
	}
	return 0;
}


/*************************************
EventableDescriptor::GetNextHeartbeat
*************************************/

uint64_t EventableDescriptor::GetNextHeartbeat()
{
	if (NextHeartbeat)
		MyEventMachine->ClearHeartbeat(NextHeartbeat, this);

	NextHeartbeat = 0;

	if (!ShouldDelete()) {
		uint64_t time_til_next = InactivityTimeout;
		if (IsConnectPending()) {
			if (time_til_next == 0 || PendingConnectTimeout < time_til_next)
				time_til_next = PendingConnectTimeout;
		}
		if (time_til_next == 0)
			return 0;
		NextHeartbeat = time_til_next + MyEventMachine->GetRealTime();
	}

	return NextHeartbeat;
}


/******************************************
ConnectionDescriptor::ConnectionDescriptor
******************************************/

ConnectionDescriptor::ConnectionDescriptor (SOCKET sd, EventMachine_t *em):
	EventableDescriptor (sd, em),
	bConnectPending (false),
	bNotifyReadable (false),
	bNotifyWritable (false),
	bReadAttemptedAfterClose (false),
	bWriteAttemptedAfterClose (false),
	OutboundDataSize (0),
	#ifdef WITH_SSL
	SslBox (NULL),
	bHandshakeSignaled (false),
	bSslVerifyPeer (false),
	bSslPeerAccepted(false),
	#endif
	#ifdef HAVE_KQUEUE
	bGotExtraKqueueEvent(false),
	#endif
	bIsServer (false)
{
	// 22Jan09: Moved ArmKqueueWriter into SetConnectPending() to fix assertion failure in _WriteOutboundData()
	//  5May09: Moved EPOLLOUT into SetConnectPending() so it doesn't happen for attached read pipes
}


/*******************************************
ConnectionDescriptor::~ConnectionDescriptor
*******************************************/

ConnectionDescriptor::~ConnectionDescriptor()
{
	// Run down any stranded outbound data.
	for (size_t i=0; i < OutboundPages.size(); i++)
		OutboundPages[i].Free();

	#ifdef WITH_SSL
	if (SslBox)
		delete SslBox;
	#endif
}


/***********************************
ConnectionDescriptor::_UpdateEvents
************************************/

void ConnectionDescriptor::_UpdateEvents()
{
	_UpdateEvents(true, true);
}

void ConnectionDescriptor::_UpdateEvents(bool read, bool write)
{
	if (MySocket == INVALID_SOCKET)
		return;

	if (!read && !write)
		return;

	#ifdef HAVE_EPOLL
	unsigned int old = EpollEvent.events;

	if (read) {
		if (SelectForRead())
			EpollEvent.events |= EPOLLIN;
		else
			EpollEvent.events &= ~EPOLLIN;
	}

	if (write) {
		if (SelectForWrite())
			EpollEvent.events |= EPOLLOUT;
		else
			EpollEvent.events &= ~EPOLLOUT;
	}

	if (old != EpollEvent.events)
		MyEventMachine->Modify (this);
	#endif

	#ifdef HAVE_KQUEUE
	if (read && SelectForRead())
		MyEventMachine->ArmKqueueReader (this);
	bKqueueArmWrite = SelectForWrite();
	if (write && bKqueueArmWrite)
		MyEventMachine->Modify (this);
	#endif
}

/***************************************
ConnectionDescriptor::SetConnectPending
****************************************/

void ConnectionDescriptor::SetConnectPending(bool f)
{
	bConnectPending = f;
	MyEventMachine->QueueHeartbeat(this);
	_UpdateEvents();
}


/**********************************
ConnectionDescriptor::SetAttached
***********************************/

void ConnectionDescriptor::SetAttached(bool state)
{
	bAttached = state;
}


/**********************************
ConnectionDescriptor::SetWatchOnly
***********************************/

void ConnectionDescriptor::SetWatchOnly(bool watching)
{
	bWatchOnly = watching;
	_UpdateEvents();
}


/*********************************
ConnectionDescriptor::HandleError
*********************************/

void ConnectionDescriptor::HandleError()
{
	if (bWatchOnly) {
		// An EPOLLHUP | EPOLLIN condition will call Read() before HandleError(), in which case the
		// socket is already detached and invalid, so we don't need to do anything.
		if (MySocket == INVALID_SOCKET) return;

		// HandleError() is called on WatchOnly descriptors by the epoll reactor
		// when it gets a EPOLLERR | EPOLLHUP. Usually this would show up as a readable and
		// writable event on other reactors, so we have to fire those events ourselves.
		if (bNotifyReadable) Read();
		if (bNotifyWritable) Write();
	} else {
		ScheduleClose (false);
	}
}


/***********************************
ConnectionDescriptor::ScheduleClose
***********************************/

void ConnectionDescriptor::ScheduleClose (bool after_writing)
{
	if (bWatchOnly)
		throw std::runtime_error ("cannot close 'watch only' connections");

	EventableDescriptor::ScheduleClose(after_writing);
}


/***************************************
ConnectionDescriptor::SetNotifyReadable
****************************************/

void ConnectionDescriptor::SetNotifyReadable(bool readable)
{
	if (!bWatchOnly)
		throw std::runtime_error ("notify_readable must be on 'watch only' connections");

	bNotifyReadable = readable;
	_UpdateEvents(true, false);
}


/***************************************
ConnectionDescriptor::SetNotifyWritable
****************************************/

void ConnectionDescriptor::SetNotifyWritable(bool writable)
{
	if (!bWatchOnly)
		throw std::runtime_error ("notify_writable must be on 'watch only' connections");

	bNotifyWritable = writable;
	_UpdateEvents(false, true);
}


/**************************************
ConnectionDescriptor::SendOutboundData
**************************************/

int ConnectionDescriptor::SendOutboundData (const char *data, unsigned long length)
{
	if (bWatchOnly)
		throw std::runtime_error ("cannot send data on a 'watch only' connection");

	if (ProxiedFrom && MaxOutboundBufSize && (unsigned int)(GetOutboundDataSize() + length) > MaxOutboundBufSize)
		ProxiedFrom->Pause();

	#ifdef WITH_SSL
	if (SslBox) {
		if (length > 0) {
			unsigned long writed = 0;
			char *p = (char*)data;

			while (writed < length) {
				int to_write = SSLBOX_INPUT_CHUNKSIZE;
				int remaining = length - writed;

				if (remaining < SSLBOX_INPUT_CHUNKSIZE)
					to_write = remaining;

				int w = SslBox->PutPlaintext (p, to_write);
				if (w < 0) {
					ScheduleClose (false);
				}else
					_DispatchCiphertext();

				p += to_write;
				writed += to_write;
			}
		}
		// TODO: What's the correct return value?
		return 1; // That's a wild guess, almost certainly wrong.
	}
	else
	#endif
		return _SendRawOutboundData (data, length);
}



/******************************************
ConnectionDescriptor::_SendRawOutboundData
******************************************/

int ConnectionDescriptor::_SendRawOutboundData (const char *data, unsigned long length)
{
	/* This internal method is called to schedule bytes that
	 * will be sent out to the remote peer.
	 * It's not directly accessed by the caller, who hits ::SendOutboundData,
	 * which may or may not filter or encrypt the caller's data before
	 * sending it here.
	 */

	// Highly naive and incomplete implementation.
	// There's no throttle for runaways (which should abort only this connection
	// and not the whole process), and no coalescing of small pages.
	// (Well, not so bad, small pages are coalesced in ::Write)

	if (IsCloseScheduled())
		return 0;
	// 25Mar10: Ignore 0 length packets as they are not meaningful in TCP (as opposed to UDP)
	// and can cause the assert(nbytes>0) to fail when OutboundPages has a bunch of 0 length pages.
	if (length == 0)
		return 0;

	if (!data && (length > 0))
		throw std::runtime_error ("bad outbound data");
	char *buffer = (char *) malloc (length + 1);
	if (!buffer)
		throw std::runtime_error ("no allocation for outbound data");

	memcpy (buffer, data, length);
	buffer [length] = 0;
	OutboundPages.push_back (OutboundPage (buffer, length));
	OutboundDataSize += length;

	_UpdateEvents(false, true);

	return length;
}



/***********************************
ConnectionDescriptor::SelectForRead
***********************************/

bool ConnectionDescriptor::SelectForRead()
{
	/* A connection descriptor is always scheduled for read,
	 * UNLESS it's in a pending-connect state.
	 * On Linux, unlike Unix, a nonblocking socket on which
	 * connect has been called, does NOT necessarily select
	 * both readable and writable in case of error.
	 * The socket will select writable when the disposition
	 * of the connect is known. On the other hand, a socket
	 * which successfully connects and selects writable may
	 * indeed have some data available on it, so it will
	 * select readable in that case, violating expectations!
	 * So we will not poll for readability until the socket
	 * is known to be in a connected state.
	 */

	if (bPaused)
		return false;
	else if (bConnectPending)
		return false;
	else if (bWatchOnly)
		return bNotifyReadable ? true : false;
	else
		return true;
}


/************************************
ConnectionDescriptor::SelectForWrite
************************************/

bool ConnectionDescriptor::SelectForWrite()
{
	/* Cf the notes under SelectForRead.
	 * In a pending-connect state, we ALWAYS select for writable.
	 * In a normal state, we only select for writable when we
	 * have outgoing data to send.
	 */

	if (bPaused)
		return false;
	else if (bConnectPending)
		return true;
	else if (bWatchOnly)
		return bNotifyWritable ? true : false;
	else
		return (GetOutboundDataSize() > 0);
}

/***************************
ConnectionDescriptor::Pause
***************************/

bool ConnectionDescriptor::Pause()
{
	if (bWatchOnly)
		throw std::runtime_error ("cannot pause/resume 'watch only' connections, set notify readable/writable instead");

	bool old = bPaused;
	bPaused = true;
	_UpdateEvents();
	return old == false;
}

/****************************
ConnectionDescriptor::Resume
****************************/

bool ConnectionDescriptor::Resume()
{
	if (bWatchOnly)
		throw std::runtime_error ("cannot pause/resume 'watch only' connections, set notify readable/writable instead");

	bool old = bPaused;
	bPaused = false;
	_UpdateEvents();
	return old == true;
}

/**************************
ConnectionDescriptor::Read
**************************/

void ConnectionDescriptor::Read()
{
	/* Read and dispatch data on a socket that has selected readable.
	 * It's theoretically possible to get and dispatch incoming data on
	 * a socket that has already been scheduled for closing or close-after-writing.
	 * In those cases, we'll leave it up the to protocol handler to "do the
	 * right thing" (which probably means to ignore the incoming data).
	 *
	 * 22Aug06: Chris Ochs reports that on FreeBSD, it's possible to come
	 * here with the socket already closed, after the process receives
	 * a ctrl-C signal (not sure if that's TERM or INT on BSD). The application
	 * was one in which network connections were doing a lot of interleaved reads
	 * and writes.
	 * Since we always write before reading (in order to keep the outbound queues
	 * as light as possible), I think what happened is that an interrupt caused
	 * the socket to be closed in ConnectionDescriptor::Write. We'll then
	 * come here in the same pass through the main event loop, and won't get
	 * cleaned up until immediately after.
	 * We originally asserted that the socket was valid when we got here.
	 * To deal properly with the possibility that we are closed when we get here,
	 * I removed the assert. HOWEVER, the potential for an infinite loop scares me,
	 * so even though this is really clunky, I added a flag to assert that we never
	 * come here more than once after being closed. (FCianfrocca)
	 */

	SOCKET sd = GetSocket();
	//assert (sd != INVALID_SOCKET); (original, removed 22Aug06)
	if (sd == INVALID_SOCKET) {
		assert (!bReadAttemptedAfterClose);
		bReadAttemptedAfterClose = true;
		return;
	}

	if (bWatchOnly) {
		if (bNotifyReadable && EventCallback)
			(*EventCallback)(GetBinding(), EM_CONNECTION_NOTIFY_READABLE, NULL, 0);
		return;
	}

	LastActivity = MyEventMachine->GetCurrentLoopTime();

	int total_bytes_read = 0;
	char readbuffer [16 * 1024 + 1];

	for (int i=0; i < 10; i++) {
		// Don't read just one buffer and then move on. This is faster
		// if there is a lot of incoming.
		// But don't read indefinitely. Give other sockets a chance to run.
		// NOTICE, we're reading one less than the buffer size.
		// That's so we can put a guard byte at the end of what we send
		// to user code.
		

		int r = read (sd, readbuffer, sizeof(readbuffer) - 1);
#ifdef OS_WIN32
		int e = WSAGetLastError();
#else
		int e = errno;
#endif
		//cerr << "<R:" << r << ">";

		if (r > 0) {
			total_bytes_read += r;

			// Add a null-terminator at the the end of the buffer
			// that we will send to the callback.
			// DO NOT EVER CHANGE THIS. We want to explicitly allow users
			// to be able to depend on this behavior, so they will have
			// the option to do some things faster. Additionally it's
			// a security guard against buffer overflows.
			readbuffer [r] = 0;
			_DispatchInboundData (readbuffer, r);
			if (bPaused)
				break;
		}
		else if (r == 0) {
			break;
		}
		else {
			#ifdef OS_UNIX
			if ((e != EINPROGRESS) && (e != EWOULDBLOCK) && (e != EAGAIN) && (e != EINTR)) {
			#endif
			#ifdef OS_WIN32
			if ((e != WSAEINPROGRESS) && (e != WSAEWOULDBLOCK)) {
			#endif
				// 26Mar11: Previously, all read errors were assumed to be EWOULDBLOCK and ignored.
				// Now, instead, we call Close() on errors like ECONNRESET and ENOTCONN.
				UnbindReasonCode = e;
				Close();
				break;
			} else {
				// Basically a would-block, meaning we've read everything there is to read.
				break;
			}
		}

	}


	if (total_bytes_read == 0) {
		// If we read no data on a socket that selected readable,
		// it generally means the other end closed the connection gracefully.
		ScheduleClose (false);
		//bCloseNow = true;
	}

}



/******************************************
ConnectionDescriptor::_DispatchInboundData
******************************************/

#ifdef WITH_SSL
void ConnectionDescriptor::_DispatchInboundData (const char *buffer, unsigned long size)
{
	if (SslBox) {
		SslBox->PutCiphertext (buffer, size);

		int s;
		char B [2048];
		while ((s = SslBox->GetPlaintext (B, sizeof(B) - 1)) > 0) {
			_CheckHandshakeStatus();
			B [s] = 0;
			_GenericInboundDispatch(B, s);
		}

		// If our SSL handshake had a problem, shut down the connection.
		if (s == -2) {
			#ifndef EPROTO // OpenBSD does not have EPROTO
			#define EPROTO EINTR
			#endif
			#ifdef OS_UNIX
			UnbindReasonCode = EPROTO;
			#endif
			#ifdef OS_WIN32
			UnbindReasonCode = WSAECONNABORTED;
			#endif
			ScheduleClose(false);
			return;
		}

		_CheckHandshakeStatus();
		_DispatchCiphertext();
	}
	else {
		_GenericInboundDispatch(buffer, size);
	}
}
#else
void ConnectionDescriptor::_DispatchInboundData (const char *buffer, unsigned long size)
{
	_GenericInboundDispatch(buffer, size);
}
#endif



/*******************************************
ConnectionDescriptor::_CheckHandshakeStatus
*******************************************/

void ConnectionDescriptor::_CheckHandshakeStatus()
{
	#ifdef WITH_SSL
	if (SslBox && (!bHandshakeSignaled) && SslBox->IsHandshakeCompleted()) {
		bHandshakeSignaled = true;
		if (EventCallback)
			(*EventCallback)(GetBinding(), EM_SSL_HANDSHAKE_COMPLETED, NULL, 0);
	}
	#endif
}



/***************************
ConnectionDescriptor::Write
***************************/

void ConnectionDescriptor::Write()
{
	/* A socket which is in a pending-connect state will select
	 * writable when the disposition of the connect is known.
	 * At that point, check to be sure there are no errors,
	 * and if none, then promote the socket out of the pending
	 * state.
	 * TODO: I haven't figured out how Windows signals errors on
	 * unconnected sockets. Maybe it does the untraditional but
	 * logical thing and makes the socket selectable for error.
	 * If so, it's unsupported here for the time being, and connect
	 * errors will have to be caught by the timeout mechanism.
	 */

	if (bConnectPending) {
		int error;
		socklen_t len;
		len = sizeof(error);
		#ifdef OS_UNIX
		int o = getsockopt (GetSocket(), SOL_SOCKET, SO_ERROR, &error, &len);
		#endif
		#ifdef OS_WIN32
		int o = getsockopt (GetSocket(), SOL_SOCKET, SO_ERROR, (char*)&error, &len);
		#endif
		if ((o == 0) && (error == 0)) {
			if (EventCallback)
				(*EventCallback)(GetBinding(), EM_CONNECTION_COMPLETED, "", 0);

			// 5May09: Moved epoll/kqueue read/write arming into SetConnectPending, so it can be called
			// from EventMachine_t::AttachFD as well.
			SetConnectPending (false);
		}
		else {
			if (o == 0)
				UnbindReasonCode = error;
			ScheduleClose (false);
			//bCloseNow = true;
		}
	}
	else {

		if (bNotifyWritable) {
			if (EventCallback)
				(*EventCallback)(GetBinding(), EM_CONNECTION_NOTIFY_WRITABLE, NULL, 0);

			_UpdateEvents(false, true);
			return;
		}

		assert(!bWatchOnly);

		/* 5May09: Kqueue bugs on OSX cause one extra writable event to fire even though we're using
		   EV_ONESHOT. We ignore this extra event once, but only the first time. If it happens again,
		   we should fall through to the assert(nbytes>0) failure to catch any EM bugs which might cause
		   ::Write to be called in a busy-loop.
		*/
		#ifdef HAVE_KQUEUE
		if (MyEventMachine->GetPoller() == Poller_Kqueue) {
			if (OutboundDataSize == 0 && !bGotExtraKqueueEvent) {
				bGotExtraKqueueEvent = true;
				return;
			} else if (OutboundDataSize > 0) {
				bGotExtraKqueueEvent = false;
			}
		}
		#endif

		_WriteOutboundData();
	}
}


/****************************************
ConnectionDescriptor::_WriteOutboundData
****************************************/

void ConnectionDescriptor::_WriteOutboundData()
{
	/* This is a helper function called by ::Write.
	 * It's possible for a socket to select writable and then no longer
	 * be writable by the time we get around to writing. The kernel might
	 * have used up its available output buffers between the select call
	 * and when we get here. So this condition is not an error.
	 *
	 * 20Jul07, added the same kind of protection against an invalid socket
	 * that is at the top of ::Read. Not entirely how this could happen in
	 * real life (connection-reset from the remote peer, perhaps?), but I'm
	 * doing it to address some reports of crashing under heavy loads.
	 */

	SOCKET sd = GetSocket();
	//assert (sd != INVALID_SOCKET);
	if (sd == INVALID_SOCKET) {
		assert (!bWriteAttemptedAfterClose);
		bWriteAttemptedAfterClose = true;
		return;
	}

	LastActivity = MyEventMachine->GetCurrentLoopTime();
	size_t nbytes = 0;

	#ifdef HAVE_WRITEV
	int iovcnt = OutboundPages.size();
	// Max of 16 outbound pages at a time
	if (iovcnt > 16) iovcnt = 16;

	iovec iov[16];

	for(int i = 0; i < iovcnt; i++){
		OutboundPage *op = &(OutboundPages[i]);
		#ifdef CC_SUNWspro
		// TODO: The void * cast works fine on Solaris 11, but
		// I don't know at what point that changed from older Solaris.
		iov[i].iov_base = (char *)(op->Buffer + op->Offset);
		#else
		iov[i].iov_base = (void *)(op->Buffer + op->Offset);
		#endif
		iov[i].iov_len	= op->Length - op->Offset;

		nbytes += iov[i].iov_len;
	}
	#else
	char output_buffer [16 * 1024];

	while ((OutboundPages.size() > 0) && (nbytes < sizeof(output_buffer))) {
		OutboundPage *op = &(OutboundPages[0]);
		if ((nbytes + op->Length - op->Offset) < sizeof (output_buffer)) {
			memcpy (output_buffer + nbytes, op->Buffer + op->Offset, op->Length - op->Offset);
			nbytes += (op->Length - op->Offset);
			op->Free();
			OutboundPages.pop_front();
		}
		else {
			int len = sizeof(output_buffer) - nbytes;
			memcpy (output_buffer + nbytes, op->Buffer + op->Offset, len);
			op->Offset += len;
			nbytes += len;
		}
	}
	#endif

	// We should never have gotten here if there were no data to write,
	// so assert that as a sanity check.
	// Don't bother to make sure nbytes is less than output_buffer because
	// if it were we probably would have crashed already.
	assert (nbytes > 0);

	assert (GetSocket() != INVALID_SOCKET);
	#ifdef HAVE_WRITEV
	int bytes_written = writev (GetSocket(), iov, iovcnt);
	#else
	int bytes_written = write (GetSocket(), output_buffer, nbytes);
	#endif

	bool err = false;
#ifdef OS_WIN32
	int e = WSAGetLastError();
#else
	int e = errno;
#endif
	if (bytes_written < 0) {
		err = true;
		bytes_written = 0;
	}

	assert (bytes_written >= 0);
	OutboundDataSize -= bytes_written;

	if (ProxiedFrom && MaxOutboundBufSize && (unsigned int)GetOutboundDataSize() < MaxOutboundBufSize && ProxiedFrom->IsPaused())
		ProxiedFrom->Resume();

	#ifdef HAVE_WRITEV
	if (!err) {
		unsigned int sent = bytes_written;
		std::deque<OutboundPage>::iterator op = OutboundPages.begin();

		for (int i = 0; i < iovcnt; i++) {
			if (iov[i].iov_len <= sent) {
				// Sent this page in full, free it.
				op->Free();
				OutboundPages.pop_front();

				sent -= iov[i].iov_len;
			} else {
				// Sent part (or none) of this page, increment offset to send the remainder
				op->Offset += sent;
				break;
			}

			// Shouldn't be possible run out of pages before the loop ends
			assert(op != OutboundPages.end());
			*op++;
		}
	}
	#else
	if ((size_t)bytes_written < nbytes) {
		int len = nbytes - bytes_written;
		char *buffer = (char*) malloc (len + 1);
		if (!buffer)
			throw std::runtime_error ("bad alloc throwing back data");
		memcpy (buffer, output_buffer + bytes_written, len);
		buffer [len] = 0;
		OutboundPages.push_front (OutboundPage (buffer, len));
	}
	#endif

	_UpdateEvents(false, true);

	if (err) {
		#ifdef OS_UNIX
		if ((e != EINPROGRESS) && (e != EWOULDBLOCK) && (e != EINTR)) {
		#endif
		#ifdef OS_WIN32
		if ((e != WSAEINPROGRESS) && (e != WSAEWOULDBLOCK)) {
		#endif
			UnbindReasonCode = e;
			Close();
		}
	}
}


/***************************************
ConnectionDescriptor::ReportErrorStatus
***************************************/

int ConnectionDescriptor::ReportErrorStatus()
{
	if (MySocket == INVALID_SOCKET) {
		return -1;
	}

	int error;
	socklen_t len;
	len = sizeof(error);
	#ifdef OS_UNIX
	int o = getsockopt (GetSocket(), SOL_SOCKET, SO_ERROR, &error, &len);
	#endif
	#ifdef OS_WIN32
	int o = getsockopt (GetSocket(), SOL_SOCKET, SO_ERROR, (char*)&error, &len);
	#endif
	if ((o == 0) && (error == 0))
		return 0;
	else if (o == 0)
		return error;
	else
		return -1;
}


/******************************
ConnectionDescriptor::StartTls
******************************/

#ifdef WITH_SSL
void ConnectionDescriptor::StartTls()
{
	if (SslBox)
		throw std::runtime_error ("SSL/TLS already running on connection");

	SslBox = new SslBox_t (bIsServer, PrivateKeyFilename, PrivateKey, PrivateKeyPass, CertChainFilename, Cert, bSslVerifyPeer, bSslFailIfNoPeerCert, SniHostName, CipherList, EcdhCurve, DhParam, Protocols, GetBinding());
	_DispatchCiphertext();

}
#else
void ConnectionDescriptor::StartTls()
{
	throw std::runtime_error ("Encryption not available on this event-machine");
}
#endif


/*********************************
ConnectionDescriptor::SetTlsParms
*********************************/

#ifdef WITH_SSL
void ConnectionDescriptor::SetTlsParms (const char *privkey_filename, const char *privkey, const char *privkeypass, const char *certchain_filename, const char *cert, bool verify_peer, bool fail_if_no_peer_cert, const char *sni_hostname, const char *cipherlist, const char *ecdh_curve, const char *dhparam, int protocols)
{
	if (SslBox)
		throw std::runtime_error ("call SetTlsParms before calling StartTls");
	if (privkey_filename && *privkey_filename)
		PrivateKeyFilename = privkey_filename;
	if (privkey && *privkey)
		PrivateKey = privkey;
	if (privkeypass && *privkeypass)
		PrivateKeyPass = privkeypass;
	if (certchain_filename && *certchain_filename)
		CertChainFilename = certchain_filename;
	if (cert && *cert)
		Cert = cert;
	bSslVerifyPeer     = verify_peer;
	bSslFailIfNoPeerCert = fail_if_no_peer_cert;

	if (sni_hostname && *sni_hostname)
		SniHostName = sni_hostname;
	if (cipherlist && *cipherlist)
		CipherList = cipherlist;
	if (ecdh_curve && *ecdh_curve)
		EcdhCurve = ecdh_curve;
	if (dhparam && *dhparam)
		DhParam = dhparam;
	Protocols = protocols;
}
#else
void ConnectionDescriptor::SetTlsParms (const char *privkey_filename UNUSED, const char *privkey UNUSED, const char *privkeypass UNUSED, const char *certchain_filename UNUSED, const char *cert UNUSED, bool verify_peer UNUSED, bool fail_if_no_peer_cert UNUSED, const char *sni_hostname UNUSED, const char *cipherlist UNUSED, const char *ecdh_curve UNUSED, const char *dhparam UNUSED, int protocols UNUSED)
{
	throw std::runtime_error ("Encryption not available on this event-machine");
}
#endif


/*********************************
ConnectionDescriptor::GetPeerCert
*********************************/

#ifdef WITH_SSL
X509 *ConnectionDescriptor::GetPeerCert()
{
	if (!SslBox)
		throw std::runtime_error ("SSL/TLS not running on this connection");
	return SslBox->GetPeerCert();
}
#endif


/*********************************
ConnectionDescriptor::GetCipherBits
*********************************/

#ifdef WITH_SSL
int ConnectionDescriptor::GetCipherBits()
{
	if (!SslBox)
		throw std::runtime_error ("SSL/TLS not running on this connection");
	return SslBox->GetCipherBits();
}
#endif


/*********************************
ConnectionDescriptor::GetCipherName
*********************************/

#ifdef WITH_SSL
const char *ConnectionDescriptor::GetCipherName()
{
	if (!SslBox)
		throw std::runtime_error ("SSL/TLS not running on this connection");
	return SslBox->GetCipherName();
}
#endif


/*********************************
ConnectionDescriptor::GetCipherProtocol
*********************************/

#ifdef WITH_SSL
const char *ConnectionDescriptor::GetCipherProtocol()
{
	if (!SslBox)
		throw std::runtime_error ("SSL/TLS not running on this connection");
	return SslBox->GetCipherProtocol();
}
#endif


/*********************************
ConnectionDescriptor::GetSNIHostname
*********************************/

#ifdef WITH_SSL
const char *ConnectionDescriptor::GetSNIHostname()
{
	if (!SslBox)
		throw std::runtime_error ("SSL/TLS not running on this connection");
	return SslBox->GetSNIHostname();
}
#endif


/***********************************
ConnectionDescriptor::VerifySslPeer
***********************************/

#ifdef WITH_SSL
bool ConnectionDescriptor::VerifySslPeer(const char *cert)
{
	bSslPeerAccepted = false;

	if (EventCallback)
		(*EventCallback)(GetBinding(), EM_SSL_VERIFY, cert, strlen(cert));

	return bSslPeerAccepted;
}
#endif


/***********************************
ConnectionDescriptor::AcceptSslPeer
***********************************/

#ifdef WITH_SSL
void ConnectionDescriptor::AcceptSslPeer()
{
	bSslPeerAccepted = true;
}
#endif


/*****************************************
ConnectionDescriptor::_DispatchCiphertext
*****************************************/

#ifdef WITH_SSL
void ConnectionDescriptor::_DispatchCiphertext()
{
	assert (SslBox);


	char BigBuf [SSLBOX_OUTPUT_CHUNKSIZE];
	bool did_work;

	do {
		did_work = false;

		// try to drain ciphertext
		while (SslBox->CanGetCiphertext()) {
			int r = SslBox->GetCiphertext (BigBuf, sizeof(BigBuf));
			assert (r > 0);
			_SendRawOutboundData (BigBuf, r);
			did_work = true;
		}

		// Pump the SslBox, in case it has queued outgoing plaintext
		// This will return >0 if data was written,
		// 0 if no data was written, and <0 if there was a fatal error.
		bool pump;
		do {
			pump = false;
			int w = SslBox->PutPlaintext (NULL, 0);
			if (w > 0) {
				did_work = true;
				pump = true;
			}
			else if (w < 0)
				ScheduleClose (false);
		} while (pump);

		// try to put plaintext. INCOMPLETE, doesn't belong here?
		// In SendOutboundData, we're spooling plaintext directly
		// into SslBox. That may be wrong, we may need to buffer it
		// up here!
		/*
		const char *ptr;
		int ptr_length;
		while (OutboundPlaintext.GetPage (&ptr, &ptr_length)) {
			assert (ptr && (ptr_length > 0));
			int w = SslMachine.PutPlaintext (ptr, ptr_length);
			if (w > 0) {
				OutboundPlaintext.DiscardBytes (w);
				did_work = true;
			}
			else
				break;
		}
		*/

	} while (did_work);

}
#endif



/*******************************
ConnectionDescriptor::Heartbeat
*******************************/

void ConnectionDescriptor::Heartbeat()
{
	/* When TLS is enabled, it can skew the delivery of heartbeats and
	 * the LastActivity time-keeping by hundreds of microseconds on fast
	 * machines up to tens of thousands of microseconds on very slow
	 * machines.  To prevent failing to timeout in a timely fashion we use
	 * the timer-quantum to compensate for the discrepancy so the
	 * comparisons are more likely to match when they are nearly equal.
	 */
	uint64_t skew = MyEventMachine->GetTimerQuantum();

	/* Only allow a certain amount of time to go by while waiting
	 * for a pending connect. If it expires, then kill the socket.
	 * For a connected socket, close it if its inactivity timer
	 * has expired.
	 */

	if (bConnectPending) {
		if ((MyEventMachine->GetCurrentLoopTime() - CreatedAt) >= PendingConnectTimeout) {
			UnbindReasonCode = ETIMEDOUT;
			ScheduleClose (false);
			//bCloseNow = true;
		}
	}
	else {
		if (InactivityTimeout && ((skew + MyEventMachine->GetCurrentLoopTime() - LastActivity) >= InactivityTimeout)) {
			UnbindReasonCode = ETIMEDOUT;
			ScheduleClose (false);
			//bCloseNow = true;
		}
	}
}


/****************************************
LoopbreakDescriptor::LoopbreakDescriptor
****************************************/

LoopbreakDescriptor::LoopbreakDescriptor (SOCKET sd, EventMachine_t *parent_em):
	EventableDescriptor (sd, parent_em)
{
	/* This is really bad and ugly. Change someday if possible.
	 * We have to know about an event-machine (probably the one that owns us),
	 * so we can pass newly-created connections to it.
	 */

	bCallbackUnbind = false;

	#ifdef HAVE_EPOLL
	EpollEvent.events = EPOLLIN;
	#endif
	#ifdef HAVE_KQUEUE
	MyEventMachine->ArmKqueueReader (this);
	#endif
}




/*************************
LoopbreakDescriptor::Read
*************************/

void LoopbreakDescriptor::Read()
{
	// TODO, refactor, this code is probably in the wrong place.
	assert (MyEventMachine);
	MyEventMachine->_ReadLoopBreaker();
}


/**************************
LoopbreakDescriptor::Write
**************************/

void LoopbreakDescriptor::Write()
{
	// Why are we here?
	throw std::runtime_error ("bad code path in loopbreak");
}

/**************************************
AcceptorDescriptor::AcceptorDescriptor
**************************************/

AcceptorDescriptor::AcceptorDescriptor (SOCKET sd, EventMachine_t *parent_em):
	EventableDescriptor (sd, parent_em)
{
	#ifdef HAVE_EPOLL
	EpollEvent.events = EPOLLIN;
	#endif
	#ifdef HAVE_KQUEUE
	MyEventMachine->ArmKqueueReader (this);
	#endif
}


/***************************************
AcceptorDescriptor::~AcceptorDescriptor
***************************************/

AcceptorDescriptor::~AcceptorDescriptor()
{
}

/****************************************
STATIC: AcceptorDescriptor::StopAcceptor
****************************************/

void AcceptorDescriptor::StopAcceptor (const uintptr_t binding)
{
	// TODO: This is something of a hack, or at least it's a static method of the wrong class.
	AcceptorDescriptor *ad = dynamic_cast <AcceptorDescriptor*> (Bindable_t::GetObject (binding));
	if (ad)
		ad->ScheduleClose (false);
	else
		throw std::runtime_error ("failed to close nonexistent acceptor");
}


/************************
AcceptorDescriptor::Read
************************/

void AcceptorDescriptor::Read()
{
	/* Accept up to a certain number of sockets on the listening connection.
	 * Don't try to accept all that are present, because this would allow a DoS attack
	 * in which no data were ever read or written. We should accept more than one,
	 * if available, to keep the partially accepted sockets from backing up in the kernel.
	 */

	/* Make sure we use non-blocking i/o on the acceptor socket, since we're selecting it
	 * for readability. According to Stevens UNP, it's possible for an acceptor to select readable
	 * and then block when we call accept. For example, the other end resets the connection after
	 * the socket selects readable and before we call accept. The kernel will remove the dead
	 * socket from the accept queue. If the accept queue is now empty, accept will block.
	 */


	struct sockaddr_in6 pin;
	socklen_t addrlen = sizeof (pin);
	int accept_count = EventMachine_t::GetSimultaneousAcceptCount();

	for (int i=0; i < accept_count; i++) {
#if defined(HAVE_CONST_SOCK_CLOEXEC) && defined(HAVE_ACCEPT4)
		SOCKET sd = accept4 (GetSocket(), (struct sockaddr*)&pin, &addrlen, SOCK_CLOEXEC);
		if (sd == INVALID_SOCKET) {
			// We may be running in a kernel where
			// SOCK_CLOEXEC is not supported - fall back:
			sd = accept (GetSocket(), (struct sockaddr*)&pin, &addrlen);
		}
#else
		SOCKET sd = accept (GetSocket(), (struct sockaddr*)&pin, &addrlen);
#endif
		if (sd == INVALID_SOCKET) {
			// This breaks the loop when we've accepted everything on the kernel queue,
			// up to 10 new connections. But what if the *first* accept fails?
			// Does that mean anything serious is happening, beyond the situation
			// described in the note above?
			break;
		}

		// Set the newly-accepted socket non-blocking and to close on exec.
		// On Windows, this may fail because, weirdly, Windows inherits the non-blocking
		// attribute that we applied to the acceptor socket into the accepted one.
		if (!SetFdCloexec(sd) || !SetSocketNonblocking (sd)) {
		//int val = fcntl (sd, F_GETFL, 0);
		//if (fcntl (sd, F_SETFL, val | O_NONBLOCK) == -1) {
			shutdown (sd, 1);
			close (sd);
			continue;
		}

		// Disable slow-start (Nagle algorithm). Eventually make this configurable.
		int one = 1;
		setsockopt (sd, IPPROTO_TCP, TCP_NODELAY, (char*) &one, sizeof(one));


		ConnectionDescriptor *cd = new ConnectionDescriptor (sd, MyEventMachine);
		if (!cd)
			throw std::runtime_error ("no newly accepted connection");
		cd->SetServerMode();
		if (EventCallback) {
			(*EventCallback) (GetBinding(), EM_CONNECTION_ACCEPTED, NULL, cd->GetBinding());
		}
		#ifdef HAVE_EPOLL
		cd->GetEpollEvent()->events = 0;
		if (cd->SelectForRead())
			cd->GetEpollEvent()->events |= EPOLLIN;
		if (cd->SelectForWrite())
			cd->GetEpollEvent()->events |= EPOLLOUT;
		#endif
		assert (MyEventMachine);
		MyEventMachine->Add (cd);
		#ifdef HAVE_KQUEUE
		bKqueueArmWrite = cd->SelectForWrite();
		if (bKqueueArmWrite)
			MyEventMachine->Modify (cd);
		if (cd->SelectForRead())
			MyEventMachine->ArmKqueueReader (cd);
		#endif
	}

}


/*************************
AcceptorDescriptor::Write
*************************/

void AcceptorDescriptor::Write()
{
	// Why are we here?
	throw std::runtime_error ("bad code path in acceptor");
}


/*****************************
AcceptorDescriptor::Heartbeat
*****************************/

void AcceptorDescriptor::Heartbeat()
{
	// No-op
}


/**************************************
DatagramDescriptor::DatagramDescriptor
**************************************/

DatagramDescriptor::DatagramDescriptor (SOCKET sd, EventMachine_t *parent_em):
	EventableDescriptor (sd, parent_em),
	OutboundDataSize (0)
{
	memset (&ReturnAddress, 0, sizeof(ReturnAddress));

	/* Provisionally added 19Oct07. All datagram sockets support broadcasting.
	 * Until now, sending to a broadcast address would give EACCES (permission denied)
	 * on systems like Linux and BSD that require the SO_BROADCAST socket-option in order
	 * to accept a packet to a broadcast address. Solaris doesn't require it. I think
	 * Windows DOES require it but I'm not sure.
	 *
	 * Ruby does NOT do what we're doing here. In Ruby, you have to explicitly set SO_BROADCAST
	 * on a UDP socket in order to enable broadcasting. The reason for requiring the option
	 * in the first place is so that applications don't send broadcast datagrams by mistake.
	 * I imagine that could happen if a user of an application typed in an address that happened
	 * to be a broadcast address on that particular subnet.
	 *
	 * This is provisional because someone may eventually come up with a good reason not to
	 * do it for all UDP sockets. If that happens, then we'll need to add a usercode-level API
	 * to set the socket option, just like Ruby does. AND WE'LL ALSO BREAK CODE THAT DOESN'T
	 * EXPLICITLY SET THE OPTION.
	 */

	int oval = 1;
	setsockopt (GetSocket(), SOL_SOCKET, SO_BROADCAST, (char*)&oval, sizeof(oval));

	#ifdef HAVE_EPOLL
	EpollEvent.events = EPOLLIN;
	#endif
	#ifdef HAVE_KQUEUE
	MyEventMachine->ArmKqueueReader (this);
	#endif
}


/***************************************
DatagramDescriptor::~DatagramDescriptor
***************************************/

DatagramDescriptor::~DatagramDescriptor()
{
	// Run down any stranded outbound data.
	for (size_t i=0; i < OutboundPages.size(); i++)
		OutboundPages[i].Free();
}


/*****************************
DatagramDescriptor::Heartbeat
*****************************/

void DatagramDescriptor::Heartbeat()
{
	// Close it if its inactivity timer has expired.

	if (InactivityTimeout && ((MyEventMachine->GetCurrentLoopTime() - LastActivity) >= InactivityTimeout))
		ScheduleClose (false);
		//bCloseNow = true;
}


/************************
DatagramDescriptor::Read
************************/

void DatagramDescriptor::Read()
{
	SOCKET sd = GetSocket();
	assert (sd != INVALID_SOCKET);
	LastActivity = MyEventMachine->GetCurrentLoopTime();

	// This is an extremely large read buffer.
	// In many cases you wouldn't expect to get any more than 4K.
	char readbuffer [16 * 1024];

	for (int i=0; i < 10; i++) {
		// Don't read just one buffer and then move on. This is faster
		// if there is a lot of incoming.
		// But don't read indefinitely. Give other sockets a chance to run.
		// NOTICE, we're reading one less than the buffer size.
		// That's so we can put a guard byte at the end of what we send
		// to user code.

		struct sockaddr_in6 sin;
		socklen_t slen = sizeof (sin);
		memset (&sin, 0, slen);

		int r = recvfrom (sd, readbuffer, sizeof(readbuffer) - 1, 0, (struct sockaddr*)&sin, &slen);
		//cerr << "<R:" << r << ">";

		// In UDP, a zero-length packet is perfectly legal.
		if (r >= 0) {

			// Add a null-terminator at the the end of the buffer
			// that we will send to the callback.
			// DO NOT EVER CHANGE THIS. We want to explicitly allow users
			// to be able to depend on this behavior, so they will have
			// the option to do some things faster. Additionally it's
			// a security guard against buffer overflows.
			readbuffer [r] = 0;


			// Set up a "temporary" return address so that callers can "reply" to us
			// from within the callback we are about to invoke. That means that ordinary
			// calls to "send_data_to_connection" (which is of course misnamed in this
			// case) will result in packets being sent back to the same place that sent
			// us this one.
			// There is a different call (evma_send_datagram) for cases where the caller
			// actually wants to send a packet somewhere else.

			memset (&ReturnAddress, 0, sizeof(ReturnAddress));
			memcpy (&ReturnAddress, &sin, slen);

			_GenericInboundDispatch(readbuffer, r);

		}
		else {
			// Basically a would-block, meaning we've read everything there is to read.
			break;
		}

	}


}


/*************************
DatagramDescriptor::Write
*************************/

void DatagramDescriptor::Write()
{
	/* It's possible for a socket to select writable and then no longer
	 * be writable by the time we get around to writing. The kernel might
	 * have used up its available output buffers between the select call
	 * and when we get here. So this condition is not an error.
	 * This code is very reminiscent of ConnectionDescriptor::_WriteOutboundData,
	 * but differs in the that the outbound data pages (received from the
	 * user) are _message-structured._ That is, we send each of them out
	 * one message at a time.
	 * TODO, we are currently suppressing the EMSGSIZE error!!!
	 */

	SOCKET sd = GetSocket();
	assert (sd != INVALID_SOCKET);
	LastActivity = MyEventMachine->GetCurrentLoopTime();

	assert (OutboundPages.size() > 0);

	// Send out up to 10 packets, then cycle the machine.
	for (int i = 0; i < 10; i++) {
		if (OutboundPages.size() <= 0)
			break;
		OutboundPage *op = &(OutboundPages[0]);

		// The nasty cast to (char*) is needed because Windows is brain-dead.
		int s = sendto (sd, (char*)op->Buffer, op->Length, 0, (struct sockaddr*)&(op->From),
		               (op->From.sin6_family == AF_INET6 ? sizeof (struct sockaddr_in6) : sizeof (struct sockaddr_in)));
#ifdef OS_WIN32
		int e = WSAGetLastError();
#else
		int e = errno;
#endif

		OutboundDataSize -= op->Length;
		op->Free();
		OutboundPages.pop_front();

		if (s == SOCKET_ERROR) {
			#ifdef OS_UNIX
			if ((e != EINPROGRESS) && (e != EWOULDBLOCK) && (e != EINTR)) {
			#endif
			#ifdef OS_WIN32
			if ((e != WSAEINPROGRESS) && (e != WSAEWOULDBLOCK)) {
			#endif
				UnbindReasonCode = e;
				Close();
				break;
			}
		}
	}

	#ifdef HAVE_EPOLL
	EpollEvent.events = EPOLLIN;
	if (SelectForWrite())
		EpollEvent.events |= EPOLLOUT;
	assert (MyEventMachine);
	MyEventMachine->Modify (this);
	#endif
	#ifdef HAVE_KQUEUE
	bKqueueArmWrite = SelectForWrite();
	assert (MyEventMachine);
	MyEventMachine->Modify (this);
	#endif
}


/**********************************
DatagramDescriptor::SelectForWrite
**********************************/

bool DatagramDescriptor::SelectForWrite()
{
	/* Changed 15Nov07, per bug report by Mark Zvillius.
	 * The outbound data size will be zero if there are zero-length outbound packets,
	 * so we now select writable in case the outbound page buffer is not empty.
	 * Note that the superclass ShouldDelete method still checks for outbound data size,
	 * which may be wrong.
	 */
	//return (GetOutboundDataSize() > 0); (Original)
	return (OutboundPages.size() > 0);
}


/************************************
DatagramDescriptor::SendOutboundData
************************************/

int DatagramDescriptor::SendOutboundData (const char *data, unsigned long length)
{
	// This is almost an exact clone of ConnectionDescriptor::_SendRawOutboundData.
	// That means most of it could be factored to a common ancestor. Note that
	// empty datagrams are meaningful, which isn't the case for TCP streams.

	if (IsCloseScheduled())
		return 0;

	if (!data && (length > 0))
		throw std::runtime_error ("bad outbound data");
	char *buffer = (char *) malloc (length + 1);
	if (!buffer)
		throw std::runtime_error ("no allocation for outbound data");
	memcpy (buffer, data, length);
	buffer [length] = 0;
	OutboundPages.push_back (OutboundPage (buffer, length, ReturnAddress));
	OutboundDataSize += length;

	#ifdef HAVE_EPOLL
	EpollEvent.events = (EPOLLIN | EPOLLOUT);
	assert (MyEventMachine);
	MyEventMachine->Modify (this);
	#endif
	#ifdef HAVE_KQUEUE
	bKqueueArmWrite = true;
	assert (MyEventMachine);
	MyEventMachine->Modify (this);
	#endif

	return length;
}


/****************************************
DatagramDescriptor::SendOutboundDatagram
****************************************/

int DatagramDescriptor::SendOutboundDatagram (const char *data, unsigned long length, const char *address, int port)
{
	// This is an exact clone of ConnectionDescriptor::SendOutboundData.
	// That means it needs to move to a common ancestor.
	// TODO: Refactor this so there's no overlap with SendOutboundData.

	if (IsCloseScheduled())
	//if (bCloseNow || bCloseAfterWriting)
		return 0;

	if (!address || !*address || !port)
		return 0;

	struct sockaddr_in6 addr_here;
	size_t addr_here_len = sizeof addr_here;
	if (0 != EventMachine_t::name2address (address, port, SOCK_DGRAM, (struct sockaddr *)&addr_here, &addr_here_len))
		return -1;

	if (!data && (length > 0))
		throw std::runtime_error ("bad outbound data");
	char *buffer = (char *) malloc (length + 1);
	if (!buffer)
		throw std::runtime_error ("no allocation for outbound data");
	memcpy (buffer, data, length);
	buffer [length] = 0;
	OutboundPages.push_back (OutboundPage (buffer, length, addr_here));
	OutboundDataSize += length;

	#ifdef HAVE_EPOLL
	EpollEvent.events = (EPOLLIN | EPOLLOUT);
	assert (MyEventMachine);
	MyEventMachine->Modify (this);
	#endif
	#ifdef HAVE_KQUEUE
	bKqueueArmWrite = true;
	assert (MyEventMachine);
	MyEventMachine->Modify (this);
	#endif

	return length;
}


/**********************************************
ConnectionDescriptor::GetCommInactivityTimeout
**********************************************/

uint64_t ConnectionDescriptor::GetCommInactivityTimeout()
{
	return InactivityTimeout / 1000;
}


/**********************************************
ConnectionDescriptor::SetCommInactivityTimeout
**********************************************/

int ConnectionDescriptor::SetCommInactivityTimeout (uint64_t value)
{
	InactivityTimeout = value * 1000;
	MyEventMachine->QueueHeartbeat(this);
	return 1;
}

/*******************************
DatagramDescriptor::GetPeername
*******************************/

bool DatagramDescriptor::GetPeername (struct sockaddr *s, socklen_t *len)
{
	bool ok = false;
	if (s) {
		*len = sizeof(ReturnAddress);
		memset (s, 0, sizeof(ReturnAddress));
		memcpy (s, &ReturnAddress, sizeof(ReturnAddress));
		ok = true;
	}
	return ok;
}


/********************************************
DatagramDescriptor::GetCommInactivityTimeout
********************************************/

uint64_t DatagramDescriptor::GetCommInactivityTimeout()
{
	return InactivityTimeout / 1000;
}

/********************************************
DatagramDescriptor::SetCommInactivityTimeout
********************************************/

int DatagramDescriptor::SetCommInactivityTimeout (uint64_t value)
{
	if (value > 0) {
		InactivityTimeout = value * 1000;
		MyEventMachine->QueueHeartbeat(this);
		return 1;
	}
	return 0;
}


/************************************
InotifyDescriptor::InotifyDescriptor
*************************************/

InotifyDescriptor::InotifyDescriptor (EventMachine_t *em):
	EventableDescriptor(0, em)
{
	bCallbackUnbind = false;

	#ifndef HAVE_INOTIFY
	throw std::runtime_error("no inotify support on this system");
	#else

	int fd = inotify_init();
	if (fd == -1) {
		char buf[200];
		snprintf (buf, sizeof(buf)-1, "unable to create inotify descriptor: %s", strerror(errno));
		throw std::runtime_error (buf);
	}

	MySocket = fd;
	SetSocketNonblocking(MySocket);
	#ifdef HAVE_EPOLL
	EpollEvent.events = EPOLLIN;
	#endif

	#endif
}


/*************************************
InotifyDescriptor::~InotifyDescriptor
**************************************/

InotifyDescriptor::~InotifyDescriptor()
{
	close(MySocket);
	MySocket = INVALID_SOCKET;
}

/***********************
InotifyDescriptor::Read
************************/

void InotifyDescriptor::Read()
{
	assert (MyEventMachine);
	MyEventMachine->_ReadInotifyEvents();
}


/************************
InotifyDescriptor::Write
*************************/

void InotifyDescriptor::Write()
{
	throw std::runtime_error("bad code path in inotify");
}
