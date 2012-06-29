#include "stdafx.h"
#include "HttpListener.h"
#include <http.h>

#define MAX_IO_CONTEXT_PROCESSOR_CACHE 256

#define ALLOC_MEM(x) HeapAlloc(GetProcessHeap(), 0, (x))  
#define FREE_MEM(x) HeapFree(GetProcessHeap(), 0, (x)) 

#define DEBUG_ASSERT(expression) if(!(expression)) DebugBreak();
#define __TRACE_ERROR(NtErrorStatus) DisplayWin32Error((NtErrorStatus))
#define LOG_ERROR(formatStringMessage,errorCode) wprintf((formatStringMessage),(errorCode)); __TRACE_ERROR(errorCode);

typedef struct DECLSPEC_CACHEALIGN _LOOKASIDE
{
	SLIST_HEADER Header;
	LONGLONG Length;
} LOOKASIDE, *PLOOKASIDE;

typedef struct _IO_CONTEXT: public OVERLAPPED
{
	SLIST_ENTRY LookAsideEntry;
	HttpIoCompletionRoutine CompletionRoutine;
	DWORD			   operationState;
	PHTTP_LISTENER	   listener;				
    HTTP_REQUEST_ID    requestId;
    DWORD              NumberOfBytes;    
	DWORD			   ErrorCode;
    DWORD              requestSize;
	DWORD			   IsCompleted;
	union {  
        HTTP_REQUEST Request;  
        UCHAR RequestBuffer[REQUEST_BUFFER_SIZE];  
    };  
} HTTP_IO_CONTEXT;

DECLSPEC_CACHEALIGN LOOKASIDE IoContextCacheList[MAX_IO_CONTEXT_PROCESSOR_CACHE];

//
// Prototypes 
//
void CALLBACK 
HttpListenerDemuxer(
	PTP_CALLBACK_INSTANCE instance, 
	PVOID context, 
	PVOID lpOverlapped, 
	ULONG errorcode, 
	ULONG_PTR NumberOfBytes, PTP_IO);


void 
HttpListenerOnRequestDequeued(
	PHTTP_LISTENER listener
);


void 
HttpListenerOnRequestCompleted(
	PHTTP_IO_CONTEXT listener

);

HRESULT HttpListenerInitializeThreadPool(PHTTP_LISTENER listener)
{	
	DWORD dwResult;

	TP_CALLBACK_ENVIRON tpCalbackEnviron;
	PTP_POOL pThreadPool;	

	//I nitialize the threadPool environment
	ZeroMemory(&( tpCalbackEnviron ), sizeof( TP_CALLBACK_ENVIRON ) );  
	InitializeThreadpoolEnvironment(&tpCalbackEnviron);

	// Create the threadpool
	pThreadPool = CreateThreadpool(NULL);
	if(pThreadPool == NULL)
	{
		return HRESULT_FROM_WIN32(GetLastError());
	}
	else
	{
		SetThreadpoolCallbackPool(&tpCalbackEnviron, pThreadPool );	
		dwResult = HRESULT_FROM_WIN32(GetLastError());
	}

	listener->global_pThreadPool = pThreadPool;
	listener->errorCode = dwResult;
	return dwResult;
}

HRESULT HttpListenerCleanupThreadPool(PHTTP_LISTENER listener)
{	
	DestroyThreadpoolEnvironment(&listener->tpEnvironment);
	return GetLastError();
}

PHTTP_IO_CONTEXT GetOverlapped()
{
	LOOKASIDE cacheEntry;
	PSLIST_ENTRY pEntry;

	cacheEntry = IoContextCacheList[GetCurrentProcessorNumber()% MAX_IO_CONTEXT_PROCESSOR_CACHE];
	pEntry = InterlockedPopEntrySList(&cacheEntry.Header);
	if(pEntry != NULL)
	{
		// Return address of the containing structure.
		return CONTAINING_RECORD(pEntry, HTTP_IO_CONTEXT, LookAsideEntry);
	}

	PHTTP_IO_CONTEXT context = (PHTTP_IO_CONTEXT)ALLOC_MEM(sizeof(HTTP_IO_CONTEXT));		
	return context;
}

void ReturnOverlapped(PHTTP_IO_CONTEXT overlapped)
{
	LOOKASIDE cacheEntry = IoContextCacheList[GetCurrentProcessorNumber()% MAX_IO_CONTEXT_PROCESSOR_CACHE];
	InterlockedPushEntrySList(&cacheEntry.Header, &overlapped->LookAsideEntry);
}

void CALLBACK HttpListenerDemuxer
(
	PTP_CALLBACK_INSTANCE instance, 
	PVOID context, 
	PVOID lpOverlapped, 
	ULONG errorcode, 
	ULONG_PTR NumberOfBytes, 
	PTP_IO Io
)
{
	PHTTP_IO_CONTEXT pListenerRequest = (PHTTP_IO_CONTEXT)lpOverlapped;
	pListenerRequest->NumberOfBytes = (DWORD)NumberOfBytes;
	pListenerRequest->ErrorCode = errorcode;
	pListenerRequest->CompletionRoutine(pListenerRequest);	
	return;
}

DWORD WINAPI 
HttpListenerQueuedRequestCompletion(
	LPVOID lpThreadParameter
)
{
	PHTTP_IO_CONTEXT request = (PHTTP_IO_CONTEXT)lpThreadParameter;
	request ->CompletionRoutine(request);
	return NO_ERROR;
}

void
HttpRequestIocompletion
(
	PHTTP_IO_CONTEXT plistenerRequest
)
{
	if(plistenerRequest && plistenerRequest->Pointer)
	{		
		DWORD dwResult = NO_ERROR;
		PHTTP_LISTENER pListener = plistenerRequest->listener;		
		PHTTP_REQUEST pRequest = &(plistenerRequest->Request);

		DEBUG_ASSERT(pListener != NULL);
		DEBUG_ASSERT(plistenerRequest->IsCompleted == 0);
		HttpListenerOnRequestDequeued(pListener);
				
		if(plistenerRequest->ErrorCode == NO_ERROR)
		{
			PHTTP_LISTENER plistener = plistenerRequest->listener;
			if(plistener->OnRequestReceiveHandler != NULL)
			{
				dwResult = plistener->OnRequestReceiveHandler(plistenerRequest->listener, 
															  &plistenerRequest->Request);
			}
			else
			{
				dwResult = SendHttpResponse(
								plistener,
								pRequest, 
								200,
								"OK",
								"ECHO",
								"4");
				if(dwResult != NO_ERROR)
				{
					LOG_ERROR(L"\nSendHttpResponse Error - %lu", dwResult);
				}
			}						
		}
		
		if (dwResult == ERROR_IO_PENDING)
		{
			// Async completion on send not supported.
			DEBUG_ASSERT(false);
		}
		else 
		{
			plistenerRequest->IsCompleted++;
			plistenerRequest->ErrorCode = dwResult;
			HttpListenerOnRequestCompleted(plistenerRequest);
		}
	}
}

DWORD
EnqueueReceive
(
    IN PHTTP_LISTENER listener
)
{
	DWORD result;		

	// Create the listener overlapped.
	PHTTP_IO_CONTEXT pListenerRequest = GetOverlapped();
	ZeroMemory((void*)pListenerRequest, sizeof(HTTP_IO_CONTEXT));

    // TODO fix allocation of the request buffer
	pListenerRequest->requestSize = REQUEST_BUFFER_SIZE;

    if (pListenerRequest == NULL)
    {
        return ERROR_NOT_ENOUGH_MEMORY;
    }

	// Initalize the overlapped fields.
	pListenerRequest->listener = listener;	
	pListenerRequest->CompletionRoutine = HttpRequestIocompletion;    
	pListenerRequest->Pointer = &pListenerRequest->Request;
    pListenerRequest->requestSize  = REQUEST_BUFFER_SIZE;	
	HTTP_SET_NULL_ID( &pListenerRequest->requestId);
	pListenerRequest->operationState = HTTP_LISTENER_STATE_REQUEST;
    
	// Enqueue async IO Request.
	StartThreadpoolIo(listener->pthreadPoolIO);

    result = HttpReceiveHttpRequest(
                listener->hRequestQueue,			// Req Queue
                pListenerRequest->requestId,		// Req ID
                0,									// Flags
				&pListenerRequest->Request,			// HTTP request buffer
                pListenerRequest->requestSize,		// req buffer length
                NULL,								// bytes received
                pListenerRequest					// LPOVERLAPPED
                );
	
	if(result != NO_ERROR && result != ERROR_IO_PENDING)
	{
		// need to call this whenever an async I/O operation fails synchronously
		CancelThreadpoolIo(listener->pthreadPoolIO); 
		LOG_ERROR(L"\nSycnhronous request processing completion error - %lu", result);
		return result;
	}
	else if(result == NO_ERROR)
	{		
		// Synchronous completion	
		QueueUserWorkItem(HttpListenerQueuedRequestCompletion,pListenerRequest, NULL);
	}
	else
	{	
		DEBUG_ASSERT(result == ERROR_IO_PENDING)	
	}

    return NO_ERROR;
}

DWORD
EnsurePump(
	PHTTP_LISTENER listener
)
{
	DWORD dwResult;
	InterlockedIncrement(&listener->stats->ulPendingReceives);	
	dwResult = EnqueueReceive(listener);
	if(dwResult != NO_ERROR)
	{
		// The pump has failed.
		LOG_ERROR(L"\nPump failed with error %lu", dwResult);		
		DEBUG_ASSERT(false);		
	}
	
	return dwResult;
}

DWORD
CreateHttpListener(
	OUT PHTTP_LISTENER* httpListener
	)
{

	ULONG           result;

	PHTTP_LISTENER _listener = (PHTTP_LISTENER)ALLOC_MEM(sizeof(HTTP_LISTENER)); 
	ZeroMemory(_listener, sizeof(HTTP_LISTENER));
	(*httpListener) = _listener;

	_listener->hRequestQueue = NULL;
	_listener->errorCode =0;
	_listener->urls = NULL;
	_listener->pthreadPoolIO = NULL;
	_listener->state = HTTP_LISTENER_STATE_FAULTED;	
	_listener->stats = (PLISTENER_STATS)_aligned_malloc(sizeof(LISTENER_STATS),MEMORY_ALLOCATION_ALIGNMENT);
	HTTPAPI_VERSION HttpApiVersion = HTTPAPI_VERSION_2;	

	 //
    // Initialize HTTP APIs.
    //
    result = HttpInitialize( 
                HttpApiVersion,
                HTTP_INITIALIZE_SERVER,    // Flags
                NULL                       // Reserved
                );

    if (result != NO_ERROR)
    {
		DEBUG_ASSERT(false);
		LOG_ERROR(L"\nHttpInitialize failed with %lu", result);		
    }
	
	if(result == NO_ERROR)
	{
		result = HttpCreateServerSession(HttpApiVersion,
										&_listener->sessionId, 
										NULL); 
		if(result)
		{		
			LOG_ERROR(L"\nHttpCreateServerSession failed with %lu", result);			
		}
	}

	if(result == NO_ERROR)
	{
		result = HttpCreateRequestQueue(HttpApiVersion, 
										NULL, 
										NULL, 
										0,
										&_listener->hRequestQueue);
		if(result)
		{
			LOG_ERROR(L"\nHttpCreateRequestQueue failed with %lu", result);				
		}
	}
	
	if(result == NO_ERROR)
	{
		if(SetFileCompletionNotificationModes(_listener->hRequestQueue, 
			FILE_SKIP_COMPLETION_PORT_ON_SUCCESS) == FALSE)
		{
			result = GetLastError();			
		}
	}

	if(result == NO_ERROR)
	{	
		result = HttpListenerInitializeThreadPool(_listener);
	}

	if(result == NO_ERROR)
	{
		_listener->pthreadPoolIO =  CreateThreadpoolIo(_listener->hRequestQueue, 
														HttpListenerDemuxer, 
														(void*)_listener, 
														&_listener->tpEnvironment);
	}


	_listener->errorCode = result;
	return _listener->errorCode;
}

DWORD 
StartHttpListener(
	PHTTP_LISTENER _listener,
	IN int urlCount, 
	IN _TCHAR* urls[]
)
{
	int UrlAdded = 0;	
	DWORD result;
	
	ZeroMemory(&_listener->urlGroupId, sizeof(HTTP_URL_GROUP_ID));
	result = HttpCreateUrlGroup(
				_listener->sessionId, 
				&_listener->urlGroupId, 
				NULL);
	if(result)
	{		
		DEBUG_ASSERT(false);
		return result;
	}

	HTTP_BINDING_INFO httpBinding; 
	ZeroMemory(&httpBinding, sizeof(httpBinding));
	httpBinding.RequestQueueHandle = _listener->hRequestQueue;
	httpBinding.Flags.Present = true;
	result = HttpSetUrlGroupProperty(
						_listener->urlGroupId, 
						HttpServerBindingProperty, 
						&httpBinding, 
						sizeof(httpBinding));

	// Add the urls to the UrlGroup
	for (int i = 1; i < urlCount; i++)
    {
        wprintf(L"we are listening for requests on the following url: %s\n", urls[i]);

        result = HttpAddUrlToUrlGroup(
									_listener->urlGroupId,
									urls[i],
									NULL,
									NULL);

        if (result != NO_ERROR)
        {
			_listener->errorCode = result;
			wprintf(L"HttpAddUrl failed for %s with %lu \n\t", urls[i], result);			
            return result;
        }
        else
        {
            UrlAdded ++;
        }
    }	
	_listener->urls = urls;
	_listener->urlsCount = UrlAdded;

	// We will always have 10 oustanding requests.
	for(int j = 0 ;j < HTTP_LISTENER_MAX_PENDING_RECEIVES; j++)
	{
		result = EnsurePump(_listener);	
		if(result != NO_ERROR)
		{
			break;
		}
	}

	if(result == NO_ERROR)
	{
		_listener->state = HTTP_LISTENER_STATE_STARTED;		
	}

	return result;
}


void
HttpListenerResponseIocompletion
(
	PHTTP_IO_CONTEXT pListenerResponse
)
{	
	ReturnOverlapped(pListenerResponse);
}

DWORD
SendHttpResponse(
    IN PHTTP_LISTENER listener,
    IN PHTTP_REQUEST  pRequest,
    IN USHORT         StatusCode,
    IN PSTR           pReason,
    IN PSTR           pEntityString,
	IN PSTR			  pContentLength
    )
{
    HTTP_RESPONSE   response;
    HTTP_DATA_CHUNK dataChunk;
    DWORD           result;

    //
    // Initialize the HTTP response structure.
    //
	ZeroMemory( (&response), sizeof(response));
    response.StatusCode = (StatusCode);     
	response.pReason = (pReason);        
    response.ReasonLength = (USHORT) strlen(pReason);  

    //
    // Add a known header.
    //
    //ADD_KNOWN_HEADER
	response.Headers.KnownHeaders[HttpHeaderContentType].pRawValue = "text/html"; 
	response.Headers.KnownHeaders[HttpHeaderContentType].RawValueLength = (USHORT)strlen("text/html");

	DEBUG_ASSERT(pEntityString);

	dataChunk.DataChunkType           = HttpDataChunkFromMemory;
    dataChunk.FromMemory.pBuffer      = pEntityString;
    dataChunk.FromMemory.BufferLength = (ULONG) strlen(pEntityString);
    response.EntityChunkCount         = 1;
    response.pEntityChunks            = &dataChunk;		
		   
    // 
    // Since we are sending all the entity body in one call, we don't have 
    // to specify the Content-Length.
    //
	PHTTP_IO_CONTEXT pResponseOverlapped = GetOverlapped();
	ZeroMemory(pResponseOverlapped, sizeof(HTTP_IO_CONTEXT));

	pResponseOverlapped->operationState = HTTP_LISTENER_STATE_RESPONSE;
	pResponseOverlapped->CompletionRoutine = HttpListenerResponseIocompletion;

	// Enqueue async IO Request.
	StartThreadpoolIo(listener->pthreadPoolIO);
    result = HttpSendHttpResponse(
                    listener->hRequestQueue,    // ReqQueueHandle
                    pRequest->RequestId,		// Request ID
                    0,							// Flags
                    &response,					// HTTP response
                    NULL,						// cache policy
                    NULL,						// bytes sent   (OPTIONAL)
                    NULL,						// pReserved2   (must be NULL)
                    0,							// Reserved3    (must be 0)
                    pResponseOverlapped,		// LPOVERLAPPED (OPTIONAL)
                    NULL						// pReserved4   (must be NULL)
                    );

	if(result != NO_ERROR && result != ERROR_IO_PENDING)
	{
		// need to call this whenever an async I/O operation fails synchronously
		CancelThreadpoolIo(listener->pthreadPoolIO); 
		LOG_ERROR(L"\nSynchronous completion response processing error - %lu", result);
		return result;
	}
	else if(result == NO_ERROR)
	{		
		// Synchronous completion		
		QueueUserWorkItem(HttpListenerQueuedRequestCompletion,pResponseOverlapped, NULL);
	}
	else
	{	
		DEBUG_ASSERT(result == ERROR_IO_PENDING)	
	}

    return NO_ERROR;
}

void HttpListenerOnRequestDequeued(PHTTP_LISTENER listener)
{	
	ULONG pendingReceives = InterlockedDecrement(&listener->stats->ulPendingReceives);
	ULONG state;
	// Check if the pump should shut down
	if( (state = InterlockedCompareExchange(&listener->state, 
								HTTP_LISTENER_STATE_STARTED, 
								HTTP_LISTENER_STATE_STARTED)) == HTTP_LISTENER_STATE_STARTED) // Last pending receive
	{
		InterlockedIncrement(&listener->stats->ulActiveRequests);	
		DWORD result = EnsurePump(listener);
		if(result != NO_ERROR)
		{
			LOG_ERROR(L"\nEnsure pump failed with error - %lu", result);
			ExitProcess(result);
		}
	}
	else if (state == HTTP_LISTENER_STATE_DISPOSING)
	{
		return;
	}
	else {
		// We should never reach this
		DEBUG_ASSERT(false);
	}
}


void HttpListenerOnRequestCompleted(PHTTP_IO_CONTEXT plistenerRequest)
{
	PHTTP_LISTENER listener = plistenerRequest->listener;
	plistenerRequest->listener = NULL;
	ULONG activeRequest;
	activeRequest = InterlockedDecrement(&listener->stats->ulActiveRequests);

	// TODO: Clean up request
	ReturnOverlapped(plistenerRequest);
}

void DisposeHttpListener(PHTTP_LISTENER listener)
{	
	ULONG state;
	if((state = InterlockedCompareExchange(&listener->state, 
								HTTP_LISTENER_STATE_DISPOSING, 
								HTTP_LISTENER_STATE_STARTED) )== HTTP_LISTENER_STATE_STARTED)
	{		
		if(listener && listener->hRequestQueue)
		{
			for(int i=1; i<=listener->urlsCount; i++)
			{
				HttpRemoveUrlFromUrlGroup(
					  listener->urlGroupId,     // Req Queue
					  listener->urls[i],        // Fully qualified URL
					  NULL);
			}

			//Close the request queue.
			CloseHandle(listener->hRequestQueue);
		}

		// 
		// Call HttpTerminate.
		//
		HttpTerminate(HTTP_INITIALIZE_SERVER, NULL);
		return;
	}


	if(state = HTTP_LISTENER_STATE_STOPPED)
	{
		return;
	}

	// Only one thread can dispose the listener	
	if(InterlockedCompareExchange(&listener->state, 
								HTTP_LISTENER_STATE_STOPPED, 
								HTTP_LISTENER_STATE_DISPOSING) == HTTP_LISTENER_STATE_DISPOSING)
	{		
		//
		// Cleanup threadpool 
		//
		HttpListenerCleanupThreadPool(listener);		
		
		// Free the listener
		FREE_MEM(listener);
		printf("Http listener terminated...\n");
	}
}