#include "stdafx.h"

#define REQUEST_BUFFER_SIZE 4096  

typedef struct _IO_CONTEXT HTTP_IO_CONTEXT;
typedef struct _IO_CONTEXT* PHTTP_IO_CONTEXT;
typedef struct _HTTP_LISTENER HTTP_LISTENER;
typedef HTTP_LISTENER *PHTTP_LISTENER;


typedef void (*HttpIoCompletionRoutine)(PHTTP_IO_CONTEXT);
typedef DWORD (*HttpListenerOnRequest)(PHTTP_LISTENER, PHTTP_REQUEST);

#define HTTP_LISTENER_MAX_PENDING_RECEIVES 10
#define REQUEST_BUFFER_SIZE 4096  


enum HTTP_LISTENER_STATE
{		
	HTTP_LISTENER_STATE_STARTED = 1,
	HTTP_LISTENER_STATE_DISPOSING = 2,
	HTTP_LISTENER_STATE_STOPPED = 3,
	HTTP_LISTENER_STATE_FAULTED = 4,
	HTTP_LISTENER_STATE_REQUEST = 5,
	HTTP_LISTENER_STATE_RESPONSE = 6,
};

typedef struct DECLSPEC_CACHEALIGN _LISTENER_STATS
{
	ULONG					ulActiveRequests;
	ULONG					ulPendingReceives;
} LISTENER_STATS, *PLISTENER_STATS;

typedef struct _HTTP_LISTENER
{
	 _TCHAR** 				urls;
	int						urlsCount;
	HANDLE					hRequestQueue;
	HTTP_URL_GROUP_ID		urlGroupId;
	HTTP_SERVER_SESSION_ID	sessionId;
	PTP_IO					pthreadPoolIO;
	DWORD					errorCode;
	ULONG					state; //HTTP_LISTENER_STATE	
	HttpListenerOnRequest	OnRequestReceiveHandler;
	PLISTENER_STATS			stats;

	TP_CALLBACK_ENVIRON tpEnvironment;
	PTP_POOL global_pThreadPool;
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
    IN PHTTP_LISTENER listener,
    IN PHTTP_REQUEST  pRequest,
    IN USHORT         StatusCode,
    IN PSTR           pReason,
    IN PSTR           pEntity, 
	IN PSTR			  pContentLength
);