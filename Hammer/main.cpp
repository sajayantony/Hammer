// HttpTest.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "HttpListener.h"

char* global_responseBuffer;


void DisplayWin32Error(DWORD NTStatusMessage)
{
   LPVOID lpMessageBuffer;
   HMODULE Hand = LoadLibrary(L"NTDLL.DLL");
   
   FormatMessage(
       FORMAT_MESSAGE_ALLOCATE_BUFFER | 
       FORMAT_MESSAGE_FROM_SYSTEM | 
       FORMAT_MESSAGE_FROM_HMODULE,
       Hand, 
       NTStatusMessage,  
       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
       (LPTSTR) &lpMessageBuffer,  
       0,  
       NULL );

   
   wprintf(L"%s",lpMessageBuffer);
   
   // Free the buffer allocated by the system.
   LocalFree( lpMessageBuffer ); 
   FreeLibrary(Hand);
}

DWORD 
HandleRequest(
	PHTTP_REQUEST pRequest,
	PHTTP_IO_CONTEXT pContext
)
{   
	DWORD result = SendHttpResponse(
							pRequest,
							pContext, 
							200,
							"OK",
							global_responseBuffer,
							"500"
							);

	if(result != NO_ERROR)
	{
		DisplayWin32Error(result);
	}

	return result;
}

int _tmain(int argc, _TCHAR* argv[])
{	
    if (argc < 2)
    {
        wprintf(L"%ws: <Url1> [Url2] ... \n", argv[0]);
        return -1;
    }

	// Create the response data
	global_responseBuffer =
"HTTP / 1.1 200 OK\n\
Content - Length: 15\n\
Content - Type : text / plain; charset = UTF - 8\n\
Server: Example\n\
Date : Wed, 17 Apr 2013 12 : 00 : 00 GMT\n\
Hello, World!";
	
	PHTTP_LISTENER listener;
	DWORD result = CreateHttpListener(&listener);
	if(result != NO_ERROR)
	{
		DisplayWin32Error(result);
		return result;	
	}

	// Setup the callback to handle the request.
	listener->OnRequestReceiveHandler = HandleRequest;

	result = StartHttpListener(listener, argc, argv);
	if(result != NO_ERROR)
	{
		DisplayWin32Error(result);
		return result;	
	}
	else
	{
		printf("Listener Started ...\n");
		printf("Press any key to TERMINATE...\n");
		_gettch();
	}
    return result;
}

