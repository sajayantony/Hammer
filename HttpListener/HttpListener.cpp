#include "stdafx.h"
#include "HttpListener.h"
#include <http.h>


//#define ALLOC_MEM(cb) HeapAlloc(GetProcessHeap(), 0, (cb))
//#define FREE_MEM(ptr) HeapFree(GetProcessHeap(), 0, (ptr))

#define ALLOC_MEM(cb) malloc((cb))
#define FREE_MEM(ptr) free((ptr))

#define DEBUG_ASSERT(expression) if(!(expression)) DebugBreak();
#define TRACE_ERROR(NtErrorStatus) DisplayWin32Error((NtErrorStatus))


#define SKIP_IOCP_SYNC_COMPLETION

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

void HttpRequestIocompletion(
	PHTTP_LISTENER_OVERLAPPED
);

void
HttpResponseIocompletion
(
	PHTTP_LISTENER_OVERLAPPED plistenerRequest
);

void 
HttpListenerOnRequestDequeued(
	PHTTP_LISTENER listener
);

void 
HttpListenerOnRequestCompleted(
	PHTTP_LISTENER_OVERLAPPED listener

);

DWORD
EnsurePump(
	PHTTP_LISTENER listener
);


DWORD
EnqueueReceive(
	PHTTP_LISTENER listener
);


//
// GLOBAL variables;
//
TP_CALLBACK_ENVIRON gThreadPoolEnvironment;
PTP_POOL global_pThreadPool;

HRESULT HttpListenerInitializeThreadPool()
{	
	memset( (void*) &( gThreadPoolEnvironment ), 0, sizeof( TP_CALLBACK_ENVIRON ) );  
	InitializeThreadpoolEnvironment(&gThreadPoolEnvironment);

	PTP_POOL pThreadPool = CreateThreadpool(NULL);	
	global_pThreadPool = pThreadPool;

	if(pThreadPool == NULL)
	{
		return HRESULT_FROM_WIN32(GetLastError());
	}

	SetThreadpoolCallbackPool(&gThreadPoolEnvironment, pThreadPool );	
	
}

HRESULT HttpListenerCleanupThreadPool()
{	
	DestroyThreadpoolEnvironment(&gThreadPoolEnvironment);
	return GetLastError();
}


PHTTP_LISTENER_OVERLAPPED GetOverlapped()
{
	PHTTP_LISTENER_OVERLAPPED  overlapped = new HTTP_LISTENER_OVERLAPPED;	
	ZeroMemory(overlapped, sizeof(HTTP_LISTENER_OVERLAPPED));
	return overlapped;
}


void ReturnOverlapped(PHTTP_LISTENER_OVERLAPPED overlapped)
{
	delete overlapped;
}


DWORD
CreateHttpListener(
	OUT PHTTP_LISTENER* httpListener
	)
{
	PHTTP_LISTENER _listener = (PHTTP_LISTENER)ALLOC_MEM(sizeof(HttpListener)); 
	RtlZeroMemory(_listener, sizeof(HttpListener));
	(*httpListener) = _listener;

	_listener->hRequestQueue = NULL;
	_listener->errorCode =0;
	_listener->urls = NULL;
	_listener->pthreadPoolIO = NULL;
	_listener->state = HTTP_LISTENER_STATE_FAULTED;	

	ULONG           result;        
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
        wprintf(L"HttpInitialize failed with %lu \n", result);
		_listener->errorCode = result;
        return result;
    }


	
	result = HttpCreateServerSession(HttpApiVersion,&_listener->sessionId, NULL); 
	if(result)
	{
		TRACE_ERROR(result);
		return result;
	}

	result = HttpCreateRequestQueue(HttpApiVersion, NULL, NULL, 0, &_listener->hRequestQueue);
	if(result)
	{
		TRACE_ERROR(result);
		return result;
	}

	if(SetFileCompletionNotificationModes(_listener->hRequestQueue, 
		FILE_SKIP_COMPLETION_PORT_ON_SUCCESS) == FALSE)
	{
		result = GetLastError();
		TRACE_ERROR(result);
		return result;
	}

	HttpListenerInitializeThreadPool();
	_listener->pthreadPoolIO =  CreateThreadpoolIo(
												_listener->hRequestQueue, 
												HttpListenerDemuxer, 
												(void*)_listener, 
												&gThreadPoolEnvironment);
	return result;
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
        wprintf(
          L"we are listening for requests on the following url: %s\n", 
          urls[i]);

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
	PHTTP_LISTENER_OVERLAPPED pListenerRequest = (PHTTP_LISTENER_OVERLAPPED)lpOverlapped;

	if(pListenerRequest->operationState == HTTP_LISTENER_STATE_REQUEST)
	{
		DEBUG_ASSERT(pListenerRequest->pRequest != NULL)
		if(pListenerRequest && pListenerRequest->Pointer)
		{		
			pListenerRequest->bytesRead = (DWORD)NumberOfBytes;
			pListenerRequest->errorCode = errorcode;
			pListenerRequest->completionRoutine(pListenerRequest);
		}
	}
	else if(pListenerRequest->operationState == HTTP_LISTENER_STATE_RESPONSE)
	{
		// TODO Handle response completion and unify
		pListenerRequest->completionRoutine(pListenerRequest);
	}
	return;
}

DWORD
EnsurePump(
	PHTTP_LISTENER listener
)
{
	DWORD dwResult;
	InterlockedIncrement(&listener->ulPendingReceives);	
	dwResult = EnqueueReceive(listener);
	if(dwResult != NO_ERROR)
	{
		// The pump has failed.
		wprintf(L"Pump failed -----> \t");
		TRACE_ERROR(dwResult);
		DEBUG_ASSERT(false);		
	}
	
	return dwResult;
}

DWORD WINAPI 
HttpListenerQueuedRequestCompletion(
	LPVOID lpThreadParameter
)
{
	PHTTP_LISTENER_OVERLAPPED request = (PHTTP_LISTENER_OVERLAPPED)lpThreadParameter;
	request ->completionRoutine(request);
	return NO_ERROR;
}

DWORD
EnqueueReceive
(
    IN PHTTP_LISTENER listener
)
{
	DWORD result;
	VOID* pRequestBuffer = NULL;
	ULONG requestSize = 0;	

	// Create the listener overlapped.
	PHTTP_LISTENER_OVERLAPPED pListenerRequest = GetOverlapped();
	
    // Allocate a 2K buffer. this if required.    
    requestSize = sizeof(HTTP_REQUEST) + 2048;	
    pRequestBuffer = ALLOC_MEM( requestSize  );
	RtlZeroMemory(pRequestBuffer, requestSize );
	pListenerRequest->requestSize = requestSize;

    if (pRequestBuffer == NULL)
    {
        return ERROR_NOT_ENOUGH_MEMORY;
    }

	// Initalize the overlapped fields.
	pListenerRequest->listener = listener;	
	pListenerRequest->completionRoutine = HttpRequestIocompletion;
    pListenerRequest->pRequest = (PHTTP_REQUEST)pRequestBuffer;
	pListenerRequest->Pointer = pListenerRequest->pRequest;
    pListenerRequest->requestSize  = requestSize;	
	HTTP_SET_NULL_ID( &pListenerRequest->requestId);
	pListenerRequest->operationState = HTTP_LISTENER_STATE_REQUEST;
    
	// Enqueue async IO Request.
	StartThreadpoolIo(listener->pthreadPoolIO);

    result = HttpReceiveHttpRequest(
                listener->hRequestQueue,			// Req Queue
                pListenerRequest->requestId,		// Req ID
                0,									// Flags
                pListenerRequest->pRequest,			// HTTP request buffer
                pListenerRequest->requestSize,		// req buffer length
                NULL,								// bytes received
                pListenerRequest					// LPOVERLAPPED
                );
	
	if(result != NO_ERROR && result != ERROR_IO_PENDING)
	{
		// need to call this whenever an async I/O operation fails synchronously
		CancelThreadpoolIo(listener->pthreadPoolIO); 
		wprintf(L"Error during request processing");
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

void
HttpRequestIocompletion
(
	PHTTP_LISTENER_OVERLAPPED plistenerRequest
)
{
	if(plistenerRequest && plistenerRequest->Pointer)
	{
		
		DWORD dwResult = NO_ERROR;
		PHTTP_LISTENER pListener = plistenerRequest->listener;		
		PHTTP_REQUEST pRequest = plistenerRequest->pRequest;

		DEBUG_ASSERT(pListener != NULL);
		DEBUG_ASSERT(plistenerRequest->isCompleted == 0);
		HttpListenerOnRequestDequeued(pListener);
				
		if(plistenerRequest->errorCode == NO_ERROR)
		{
			if(plistenerRequest->listener->OnRequestCompletionRoutine != NULL)
			{
				dwResult = plistenerRequest->listener->OnRequestCompletionRoutine(
					plistenerRequest->listener, 
					plistenerRequest->pRequest);
			}
			else
			{
				dwResult = SendHttpResponse(
								plistenerRequest->listener,
								pRequest, 
								200,
								"OK",
								"ECHO",
								"4");
				if(dwResult != NO_ERROR)
				{
					TRACE_ERROR(dwResult);
				}
			}						
		}
		plistenerRequest->isCompleted++;
		plistenerRequest->responseErrorCode = dwResult;
		HttpListenerOnRequestCompleted(plistenerRequest);
	}
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
    DWORD           bytesSent;

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
    //ADD_KNOWN_HEADER(response, HttpHeaderContentType, "text/html");
	response.Headers.KnownHeaders[HttpHeaderContentType].pRawValue = "text/html"; 
	response.Headers.KnownHeaders[HttpHeaderContentType].RawValueLength = strlen("text/html");

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
	PHTTP_LISTENER_OVERLAPPED pResponseOverlapped = new HTTP_LISTENER_OVERLAPPED;
	ZeroMemory(pResponseOverlapped, sizeof(HTTP_LISTENER_OVERLAPPED));
	pResponseOverlapped->completionRoutine = HttpResponseIocompletion;
	pResponseOverlapped->operationState = HTTP_LISTENER_STATE_RESPONSE;

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
		wprintf(L"Error during request processing");
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

void
HttpResponseIocompletion
(
	PHTTP_LISTENER_OVERLAPPED pListenerResponse
)
{
	// Clean up the allocated response overlapped
	delete pListenerResponse;
}

void HttpListenerOnRequestDequeued(PHTTP_LISTENER listener)
{	
	ULONG pendingReceives = InterlockedDecrement(&listener->ulPendingReceives);
	ULONG state;
	// Check if the pump should shut down
	if( (state = InterlockedCompareExchange(&listener->state, 
								HTTP_LISTENER_STATE_STARTED, 
								HTTP_LISTENER_STATE_STARTED)) == HTTP_LISTENER_STATE_DISPOSING 
								&& pendingReceives == 0 ) // Last pending receive
	{
		// Check if the pump is shutting down
		DisposeHttpListener(listener);
		return;
	}

	if(state == HTTP_LISTENER_STATE_STARTED)
	{
		InterlockedIncrement(&listener->ulActiveRequests);	
		DWORD result = EnsurePump(listener);
		if(result != NO_ERROR)
		{
			DisposeHttpListener(listener);
		}
	}
}


void HttpListenerOnRequestCompleted(PHTTP_LISTENER_OVERLAPPED plistenerRequest)
{
	PHTTP_LISTENER listener = plistenerRequest->listener;
	plistenerRequest->listener = NULL;
	if(listener != NULL)
	{		
		InterlockedDecrement(&listener->ulActiveRequests);
	}

	//Clean up request
	if(plistenerRequest->pRequest != NULL)
	{
		FREE_MEM(plistenerRequest->pRequest);
	}

	delete (plistenerRequest);
}

void DisposeHttpListener(PHTTP_LISTENER listener)
{	
	// TODO cleanup under a critical section
	// and stop the pump.
	wprintf(L"Dispose called...\n");
	if(InterlockedCompareExchange(&listener->state, 
								HTTP_LISTENER_STATE_DISPOSING, 
								HTTP_LISTENER_STATE_STARTED) == HTTP_LISTENER_STATE_STARTED)
	{
		wprintf(L"Http listener shutting down ...\n");
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

	//Only one thread can dispose the listener
	// TODO: Handle pump faulting.
	if(InterlockedCompareExchange(&listener->state, 
								HTTP_LISTENER_STATE_STOPPED, 
								HTTP_LISTENER_STATE_DISPOSING) == HTTP_LISTENER_STATE_DISPOSING)
	{	

		wprintf(L"Cleaningup Listener...\n");
		FREE_MEM(listener);

		//
		// Cleanup threadpool 
		//
		HttpListenerCleanupThreadPool();		
		wprintf(L"Http listener terminated...\n");
	}
}