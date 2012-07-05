#pragma once
#include "stdafx.h"

#define REQUEST_BUFFER_SIZE 4096  
#define HTTP_LISTENER_MAX_PENDING_RECEIVES 10
#define REQUEST_BUFFER_SIZE 4096

#define MAX_IO_CONTEXT_PROCESSOR_CACHE 256
#define IO_CONTEXT_PROC_INDEX GetCurrentProcessorNumber() % MAX_IO_CONTEXT_PROCESSOR_CACHE
#define ALLOC_MEM(x) HeapAlloc(GetProcessHeap(), 0, (x))  
#define FREE_MEM(x) HeapFree(GetProcessHeap(), 0, (x)) 

#define DEBUG_ASSERT(expression) if(!(expression)) DebugBreak();
#define __TRACE_ERROR(NtErrorStatus) DisplayWin32Error((NtErrorStatus))
#define LOG_ERROR(formatStringMessage,errorCode) wprintf((formatStringMessage),(errorCode)); __TRACE_ERROR(errorCode);

typedef struct _IO_CONTEXT HTTP_IO_CONTEXT;
typedef struct _IO_CONTEXT* PHTTP_IO_CONTEXT;
typedef struct _HTTP_LISTENER HTTP_LISTENER;
typedef HTTP_LISTENER *PHTTP_LISTENER;

typedef void (*HttpIoCompletionRoutine)(PHTTP_IO_CONTEXT);
typedef DWORD (*HttpListenerOnRequest)(PHTTP_REQUEST, PHTTP_IO_CONTEXT);

typedef struct DECLSPEC_CACHEALIGN _LOOKASIDE
{
	SLIST_HEADER Header;
} LOOKASIDE, *PLOOKASIDE;

typedef struct _IO_CONTEXT : OVERLAPPED
{
	SLIST_ENTRY				LookAsideEntry;
	HttpIoCompletionRoutine CompletionRoutine; // Future
	USHORT					operationState;
	PHTTP_LISTENER			listener;  
    DWORD					NumberOfBytes;    
	DWORD					IoResult;
    DWORD					RequestSize;
	HTTP_REQUEST_ID			requestId;
	union {  
		HTTP_RESPONSE Reponse;
        HTTP_REQUEST Request;  
        UCHAR RequestBuffer[REQUEST_BUFFER_SIZE];  
    };  
} HTTP_IO_CONTEXT, *PHTTP_IO_CONTEXT;

enum HTTP_LISTENER_STATE
{		
	HTTP_LISTENER_STATE_STARTED		= 1,
	HTTP_LISTENER_STATE_DISPOSING	= 2,
	HTTP_LISTENER_STATE_STOPPED		= 3,
	HTTP_LISTENER_STATE_FAULTED		= 4,
	HTTP_LISTENER_STATE_REQUEST		= 5,
	HTTP_LISTENER_STATE_RESPONSE	= 6,
};

typedef struct DECLSPEC_CACHEALIGN _LISTENER_STATS
{
	ULONG					ulActiveRequests;
	ULONG					ulPendingReceives;
} LISTENER_STATS, *PLISTENER_STATS;

typedef struct _HTTP_LISTENER
{
	 _TCHAR** 				urls;					// TODO:#5 Url cleanup
	int						urlsCount;				// TODO:#5 Url cleanup
	DWORD					State;					// HTTP_LISTENER_STATE	
	HTTP_URL_GROUP_ID		UrlGroupId;				// Url groups used by the Listener
	HTTP_SERVER_SESSION_ID	SessionId;				// Server Session for the listener.	
	HANDLE					hRequestQueue;			// TODO:#6 Request queue size needs to be tweaked
	ULONG					RequestQueueLength;		// Request queue lenght property
	PTP_IO					pthreadPoolIO;			// ThreadPool IO object used for request demuxing
	TP_CALLBACK_ENVIRON		tpEnvironment;			// ThreadPool callback environment
	PTP_POOL				pThreadPool;			// ThreadPool instance
	DWORD					errorCode;				// Error code during starting/ faulting.
	HttpListenerOnRequest	OnRequestReceiveHandler;
	PLISTENER_STATS			stats;		
	PLOOKASIDE				HttpInputQueue;
	ULONG					LookAsideFlushPeriod;	//The IOContextLook aside cache flush period.  
	HANDLE					TimerQueue;
	HANDLE					FlushTimer;
} HTTP_LISTENER;

DWORD
CreateHttpListener(
	PHTTP_LISTENER* listener
);

DWORD
StartHttpListener(
	PHTTP_LISTENER listener, 
	int argc, 
	_TCHAR* urls[]
);

void DisposeHttpListener(
	PHTTP_LISTENER listener
);

DWORD
SendHttpResponse(    
	IN PHTTP_REQUEST	pRequest,
    IN PHTTP_IO_CONTEXT pRequestContext,
    IN USHORT			StatusCode,
    IN PSTR				pReason,
    IN PSTR				pEntity, 
	IN PSTR				pContentLength
);