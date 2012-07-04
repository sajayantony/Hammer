#include "stdafx.h"
#include "HttpListener.h"
#include <http.h>

DECLSPEC_CACHEALIGN LOOKASIDE IoContextCacheList[MAX_IO_CONTEXT_PROCESSOR_CACHE +1];


void HttpListenerCompleteIo(PHTTP_IO_CONTEXT context);


void InitializeHttpInputQueue(PHTTP_LISTENER listener)
{
	listener->HttpInputQueue = &IoContextCacheList[MAX_IO_CONTEXT_PROCESSOR_CACHE];
	InitializeSListHead(&listener->HttpInputQueue->Header);
}

void HttpInputQueueEnqueue(PHTTP_LISTENER listener, 
							PHTTP_IO_CONTEXT context)
{
	InterlockedPushEntrySList(&listener->HttpInputQueue->Header,
							 &context->LookAsideEntry);
}

void HttpInputQueueDrain(PHTTP_LISTENER listener)
{
	PHTTP_IO_CONTEXT context;
	PSLIST_ENTRY entry;

	PLOOKASIDE inputQueue = listener->HttpInputQueue;
	while(true)
	{
		entry = InterlockedPopEntrySList(&inputQueue->Header);
		if(entry != NULL)
		{
			context = CONTAINING_RECORD(entry, HTTP_IO_CONTEXT, LookAsideEntry);
			HttpListenerCompleteIo(context);
		}
		else
		{
			break;
		}
	}
}

//
// Prototypes 
//

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
		SetThreadpoolCallbackPool(&tpCalbackEnviron, pThreadPool);	
		dwResult = HRESULT_FROM_WIN32(GetLastError());
	}

	listener->pThreadPool = pThreadPool;
	listener->errorCode = dwResult;
	return dwResult;
}

HRESULT HttpListenerCleanupThreadPool(PHTTP_LISTENER listener)
{	
	DestroyThreadpoolEnvironment(&listener->tpEnvironment);
	return GetLastError();
}

void
InitializeIOContextCache()
{	
	for(int i=0;i<MAX_IO_CONTEXT_PROCESSOR_CACHE;i++)
	{
		InitializeSListHead(&IoContextCacheList[i].Header);
	}
}

PHTTP_IO_CONTEXT GetIOContext()
{
	PHTTP_IO_CONTEXT pContext;
	PLOOKASIDE pCacheEntry;
	PSLIST_ENTRY pEntry;

	pCacheEntry = &IoContextCacheList[IO_CONTEXT_PROC_INDEX];
	pEntry = InterlockedPopEntrySList(&pCacheEntry->Header);
	if(pEntry != NULL)
	{
		// Return address of the containing structure.
		pContext = CONTAINING_RECORD(pEntry, HTTP_IO_CONTEXT, LookAsideEntry);
	}
	else
	{
		pContext = (PHTTP_IO_CONTEXT)ALLOC_MEM(sizeof(HTTP_IO_CONTEXT));			
	}

	ZeroMemory(pContext, sizeof(HTTP_IO_CONTEXT));	
	return pContext;
}

void ReturnIOContext(
	PHTTP_IO_CONTEXT context
	)
{
	PLOOKASIDE cacheEntry = &IoContextCacheList[IO_CONTEXT_PROC_INDEX];
	InterlockedPushEntrySList(&cacheEntry->Header, &context->LookAsideEntry);		
}

void CALLBACK HttpListenerDemuxer(
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
	pListenerRequest->IoResult = errorcode;	
	HttpListenerCompleteIo(pListenerRequest);	

	// flush all competed requests.
	HttpInputQueueDrain((PHTTP_LISTENER)context);
	return;
}

void
HttpListenerRequestIocompletion(
	PHTTP_IO_CONTEXT pRequestContext
	)
{	
	DWORD dwResult = NO_ERROR;
	PHTTP_LISTENER pListener = pRequestContext->listener;		
	PHTTP_REQUEST pRequest = &(pRequestContext->Request);

	DEBUG_ASSERT(pListener != NULL);
	HttpListenerOnRequestDequeued(pListener);
				
	if(pRequestContext->IoResult == NO_ERROR)
	{
		PHTTP_LISTENER plistener = pRequestContext->listener;
		if(plistener->OnRequestReceiveHandler != NULL)
		{
			dwResult = plistener->OnRequestReceiveHandler(&pRequestContext->Request, 
															pRequestContext);
		}
		else
		{
			dwResult = SendHttpResponse(
							pRequest,
							pRequestContext, 
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
		ExitProcess(ERROR_IO_PENDING);
	}
	else 
	{
		pRequestContext->IoResult = dwResult;
		HttpListenerOnRequestCompleted(pRequestContext);
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
	PHTTP_IO_CONTEXT pListenerRequest = GetIOContext();   

    if (pListenerRequest == NULL)
    {
        return ERROR_NOT_ENOUGH_MEMORY;
    }

	// Initalize the overlapped fields.
	pListenerRequest->RequestSize = REQUEST_BUFFER_SIZE;
	pListenerRequest->listener = listener;
    pListenerRequest->RequestSize  = REQUEST_BUFFER_SIZE;
	HTTP_SET_NULL_ID(&pListenerRequest->requestId);
	pListenerRequest->operationState = HTTP_LISTENER_STATE_REQUEST;
    
	// Enqueue async IO Request.
	StartThreadpoolIo(listener->pthreadPoolIO);

    result = HttpReceiveHttpRequest(
                listener->hRequestQueue,			// Req Queue
                pListenerRequest->requestId,		// Req ID
                0,									// Flags
				&pListenerRequest->Request,			// HTTP request buffer
                pListenerRequest->RequestSize,		// req buffer length
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
		CancelThreadpoolIo(listener->pthreadPoolIO); 
		HttpInputQueueEnqueue(listener, pListenerRequest);
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
	_listener->RequestQueueLength = 5000; // Default request queue length;
	_listener->errorCode = 0;
	_listener->urls = NULL;
	_listener->pthreadPoolIO = NULL;
	_listener->State = HTTP_LISTENER_STATE_FAULTED;	
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
										&_listener->SessionId, 
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
		result = HttpSetRequestQueueProperty(_listener->hRequestQueue, 
											HttpServerQueueLengthProperty, 
											&_listener->RequestQueueLength,
											sizeof(_listener->RequestQueueLength),
											NULL,
											NULL);
		if(result)
		{
			LOG_ERROR(L"\nHttpSetRequestQueueProperty failed with %lu", result);				
		}
	}
	
	if(result == NO_ERROR)
	{
		if(SetFileCompletionNotificationModes(_listener->hRequestQueue, 
											FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | 
											FILE_SKIP_SET_EVENT_ON_HANDLE) == FALSE)
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


	InitializeIOContextCache();
	InitializeHttpInputQueue(_listener);
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
	
	ZeroMemory(&_listener->UrlGroupId, sizeof(HTTP_URL_GROUP_ID));
	result = HttpCreateUrlGroup(
				_listener->SessionId, 
				&_listener->UrlGroupId, 
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
						_listener->UrlGroupId, 
						HttpServerBindingProperty, 
						&httpBinding, 
						sizeof(httpBinding));

	// Add the urls to the UrlGroup
	for (int i = 1; i < urlCount; i++)
    {
        wprintf(L"we are listening for requests on the following url: %s\n", urls[i]);

        result = HttpAddUrlToUrlGroup(
									_listener->UrlGroupId,
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
		_listener->State = HTTP_LISTENER_STATE_STARTED;		
	}

	return result;
}


void
HttpListenerResponseIocompletion
(
	PHTTP_IO_CONTEXT pListenerResponse
)
{	
	ReturnIOContext(pListenerResponse);
}

DWORD
SendHttpResponse(    
    IN PHTTP_REQUEST	pRequest,
	IN PHTTP_IO_CONTEXT pContext,
    IN USHORT			StatusCode,
    IN PSTR				pReason,
    IN PSTR				pEntityString,
	IN PSTR				pContentLength
    )
{    
    HTTP_DATA_CHUNK dataChunk;
    DWORD           result;
	PHTTP_LISTENER listener = pContext->listener;
	PHTTP_IO_CONTEXT pResponseContext = GetIOContext();

	//
    // Initialize the HTTP response structure.
    //	
    pResponseContext->Reponse.StatusCode = (StatusCode);     
	pResponseContext->Reponse.pReason = (pReason);        
    pResponseContext->Reponse.ReasonLength = (USHORT) strlen(pReason);  

    //
    // Add a known header.
    //
    //ADD_KNOWN_HEADER
	pResponseContext->Reponse.Headers.KnownHeaders[HttpHeaderContentType].pRawValue = "text/html"; 
	pResponseContext->Reponse.Headers.KnownHeaders[HttpHeaderContentType].RawValueLength = (USHORT)strlen("text/html");

	DEBUG_ASSERT(pEntityString);

	dataChunk.DataChunkType						= HttpDataChunkFromMemory;
    dataChunk.FromMemory.pBuffer				= pEntityString;
    dataChunk.FromMemory.BufferLength			= (ULONG) strlen(pEntityString);
    pResponseContext->Reponse.EntityChunkCount  = 1;
    pResponseContext->Reponse.pEntityChunks		= &dataChunk;		
		   
    // 
    // Since we are sending all the entity body in one call, we don't have 
    // to specify the Content-Length.
    //	
	pResponseContext->operationState = HTTP_LISTENER_STATE_RESPONSE;
	
	// Enqueue async IO Request.
	StartThreadpoolIo(listener->pthreadPoolIO);
    result = HttpSendHttpResponse(
                    listener->hRequestQueue,		// ReqQueueHandle
                    pRequest->RequestId,			// Request ID
                    0,								// Flags
                    &pResponseContext->Reponse,		// HTTP response
                    NULL,							// cache policy
                    NULL,							// bytes sent   (OPTIONAL)
                    NULL,							// pReserved2   (must be NULL)
                    0,								// Reserved3    (must be 0)
                    pResponseContext,				// LPOVERLAPPED (OPTIONAL)
                    NULL							// pReserved4   (must be NULL)
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
		CancelThreadpoolIo(listener->pthreadPoolIO);
		HttpInputQueueEnqueue(listener, 
							  pResponseContext);
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
	if( (state = InterlockedCompareExchange(&listener->State, 
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
	else 
	{
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

	// We are done with the RequestContext;
	ReturnIOContext(plistenerRequest);
}


void HttpListenerCompleteIo(PHTTP_IO_CONTEXT context)
{
	if(context->operationState == HTTP_LISTENER_STATE_REQUEST)
	{
		HttpListenerRequestIocompletion(context);
	}
	else if(context->operationState == HTTP_LISTENER_STATE_RESPONSE)
	{
		HttpListenerResponseIocompletion(context);
	}
}

void DisposeHttpListener(PHTTP_LISTENER listener)
{	
	ULONG state = InterlockedCompareExchange(&listener->State, 
											HTTP_LISTENER_STATE_DISPOSING, 
											HTTP_LISTENER_STATE_STARTED);
	if(state = HTTP_LISTENER_STATE_STOPPED)
	{
		return;
	}


	// Thread responsible for disposing the listener.
	if(state = HTTP_LISTENER_STATE_STARTED)
	{		
		if(listener && listener->hRequestQueue)
		{
			for(int i=1; i<=listener->urlsCount; i++)
			{
				HttpRemoveUrlFromUrlGroup(
					  listener->UrlGroupId,     // Req Queue
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
	else if (state == HTTP_LISTENER_STATE_DISPOSING)
	{
		// Only one thread can dispose the listener	
		if(InterlockedCompareExchange(&listener->State, 
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
}