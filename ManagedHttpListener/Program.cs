namespace HttpPerf
{
    using System;
    using System.Collections.Generic;
    using System.IO;
    using System.Net;
    using System.Threading;

    interface IServer
    {
        void StartListening();
    }

    class HttpServer : IServer
    {
        int maxPendingGetContexts = 10;
        string prefix;
        BufferPool bufferPool;
        byte[] responseData;
        bool enqueueOnReceive;
        HttpListener listener;
        AsyncCallback onGetContext;
        AsyncCallback onReceiveComplete, onWriteComplete;
        WaitCallback onGetContextlater;
        bool readBody;

        public HttpServer(int maxPendingContexts,
                            string serverUri, 
                            bool readBody,
                            bool enqueueOnReceive)
        {
            this.readBody = readBody;
            this.maxPendingGetContexts = maxPendingContexts;
            this.enqueueOnReceive = enqueueOnReceive;
            this.prefix = serverUri;
            this.bufferPool = new BufferPool(1 * 1024);
            //this.responseData = System.Text.UTF8Encoding.UTF8.GetBytes("<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><EchoResponse xmlns=\"http://tempuri.org/\"><EchoResult>Echo : A</EchoResult></EchoResponse></s:Body></s:Envelope>");
            //this.responseData = System.Text.UTF8Encoding.UTF8.GetBytes("HELLO ASYNC");
            this.responseData = System.Text.ASCIIEncoding.ASCII.GetBytes(new String('a', 500));

            this.onGetContext = new AsyncCallback(OnGetContext);
            this.onReceiveComplete = new AsyncCallback(OnReceiveComplete);
            this.onWriteComplete = new AsyncCallback(OnWriteComplete);
            this.onGetContextlater = new WaitCallback(GetContextLater);
        }

        public void StartListening()
        {
            //Setup listener
            this.listener = new HttpListener();
            //this.listener.AuthenticationSchemes =  AuthenticationSchemes.Anonymous;
            //this.listener.UnsafeConnectionNtlmAuthentication = false;
            //this.listener.AuthenticationSchemeSelectorDelegate = new AuthenticationSchemeSelector(SelectAuthenticationScheme);
            this.listener.Prefixes.Add(this.prefix);
            this.listener.Start();
            Console.WriteLine("Server listening on --> " + this.prefix);

            for (int i = 0; i < maxPendingGetContexts; i++)
            {
                this.Enqueue();
            }
        }

        void Enqueue()
        {
            IAsyncResult result = listener.BeginGetContext(onGetContext, null);
            if (result.CompletedSynchronously)
            {
                Schedule(onGetContextlater, result);
            }
        }

        internal static void Schedule(WaitCallback callback, object state)
        {
            ThreadPool.QueueUserWorkItem(callback, state);
        }

        void GetContextLater(object state)
        {
            this.OnGetContextCore((IAsyncResult)state);
        }

        void OnGetContext(IAsyncResult result)
        {
            if (result.CompletedSynchronously)
            {
                return;
            }

            this.OnGetContextCore(result);
        }

        void OnGetContextCore(IAsyncResult result)
        {

            if (this.enqueueOnReceive)
            {
                this.Enqueue();
            }

            HttpListenerContext context = this.listener.EndGetContext(result);

            if (this.readBody)
            {
                new ReceiveAsyncResult(context, bufferPool.Take(), this.onReceiveComplete, this);
            }
            else
            {
                this.SendReply(context);
            }

        }

        private void SendReply(HttpListenerContext context)
        {
            context.Response.StatusCode = (int)HttpStatusCode.OK;
            context.Response.ContentType = "text/html";
            context.Response.ContentLength64 = responseData.Length;
            
            //new WriteAsyncResult(context, responseData, null, null);
            // We write the content in one shot. 
            context.Response.Close(responseData, false);
        }

        void OnReceiveComplete(IAsyncResult result)
        {
            HttpListenerContext context = ReceiveAsyncResult.End(result);
            this.bufferPool.Return(((ReceiveAsyncResult)result).buffer);

            if (!this.enqueueOnReceive)
            {
                this.Enqueue();
            }
            SendReply(context);
        }

        void OnWriteComplete(IAsyncResult result)
        {
            WriteAsyncResult.End(result);
        }


        AuthenticationSchemes SelectAuthenticationScheme(HttpListenerRequest request)
        {
            try
            {
                AuthenticationSchemes result;
                if (this.TryLookupUri(request.Url, request.HttpMethod))
                {
                    result = AuthenticationSchemes.Anonymous;
                }
                else
                {
                    // if we don't match a listener factory, we want to "fall through" the
                    // auth delegate code and run through our normal OnGetContext codepath.
                    // System.Net treats "None" as Access Denied, which is not our intent here.
                    // In most cases this will just fall through to the code that returns a "404 Not Found"
                    result = AuthenticationSchemes.Anonymous;
                }

                return result;
            }
            catch (Exception e)
            {
                Console.WriteLine("Authentication Exception " + e.Message);
                throw;
            }
        }

        private bool TryLookupUri(Uri uri, string p)
        {
            return true;
        }


        class ReceiveAsyncResult : AsyncResult
        {
            static AsyncCallback onComplete = new AsyncCallback(OnReadComplete);
            HttpListenerContext context;
            public byte[] buffer;
            string action;

            public ReceiveAsyncResult(HttpListenerContext context, byte[] buffer, AsyncCallback callback, object state)
                : base(callback, state)
            {
                this.context = context;
                this.buffer = buffer;
                //this.action = context.Request.Headers["SOAPAction"];
                IAsyncResult result = context.Request.InputStream.BeginRead(buffer, 0, buffer.Length, onComplete, this);
                if (result.CompletedSynchronously)
                {
                    this.HandleReadComplete(result);
                    base.Complete(true);
                }
            }

            static void OnReadComplete(IAsyncResult result)
            {
                if (result.CompletedSynchronously)
                {
                    return;
                }
                ReceiveAsyncResult thisPtr = result.AsyncState as ReceiveAsyncResult;
                Exception completionException = null;
                try
                {
                    thisPtr.HandleReadComplete(result);
                }
                catch (Exception ex)
                {
                    completionException = ex;
                }

                thisPtr.Complete(false, completionException);
            }

            bool HandleReadComplete(IAsyncResult result)
            {
                this.context.Request.InputStream.EndRead(result);
                return true;
            }

            public static HttpListenerContext End(IAsyncResult result)
            {
                ReceiveAsyncResult thisPtr = AsyncResult.End<ReceiveAsyncResult>(result);
                return thisPtr.context;
            }
        }

        class WriteAsyncResult : AsyncResult
        {
            HttpListenerContext context;
            IAsyncResult innerResult;

            static AsyncCallback onComplete = new AsyncCallback(OnWriteComplete);

            public WriteAsyncResult(HttpListenerContext context, byte[] responseData, AsyncCallback callback, object state)
                : base(callback, state)
            {
                this.context = context;
                this.innerResult = context.Response.OutputStream.BeginWrite(responseData, 0, responseData.Length, OnWriteComplete, this);
                if (this.innerResult.CompletedSynchronously)
                {
                    this.HandleWriteComplete(innerResult);
                    base.Complete(true);
                }
            }

            static void OnWriteComplete(IAsyncResult result)
            {
                if (result.CompletedSynchronously)
                {
                    return;
                }
                WriteAsyncResult thisPtr = result.AsyncState as WriteAsyncResult;
                Exception completionException = null;
                try
                {
                    thisPtr.HandleWriteComplete(result);
                }
                catch (Exception ex)
                {
                    completionException = ex;
                }

                thisPtr.Complete(false, completionException);
            }

            internal void HandleWriteComplete(IAsyncResult result)
            {
                this.context.Response.OutputStream.EndWrite(result);
                this.context.Response.Close();
            }

            internal static void End(IAsyncResult result)
            {
                AsyncResult.End<WriteAsyncResult>(result);
            }
        }

    }



    class HttpPerformanceTestCase
    {

        public int pendingContexts = 10;

        public int minIOThreads = 100;

        public int minWorkerThreads = 100;

        const string addressFormatString = "http://{0}:80/Server/";

        public string ServerUri { get { return String.Format(addressFormatString, Environment.MachineName); } }        
        #region Performance Harness


        public HttpPerformanceTestCase()
        {
            //Don't throttle connections.
            ServicePointManager.DefaultConnectionLimit = 10000;

            if (this.minWorkerThreads > 0 || this.minIOThreads > 0)
            {
                int workerThread, ioThread;
                ThreadPool.GetMinThreads(out workerThread, out ioThread);
                this.minWorkerThreads = Math.Max(workerThread,minWorkerThreads);
                this.minIOThreads = Math.Max(ioThread, minIOThreads);

                if (!ThreadPool.SetMinThreads(minWorkerThreads, minIOThreads))
                {
                    throw new InvalidOperationException("Could not set minIOThreads.");
                }
                Console.WriteLine("Theadpool settings: MinWorkerThreads={0}, MinIOThreads={1}.", this.minWorkerThreads, this.minIOThreads);
            }

            HttpServer server = new HttpServer(10,                  // maxPendingContext
                                            "http://+:80/Server/",  // url 
                                            false,                  // readBody 
                                            true                    // enqueueOnReceive
                                            );
            server.StartListening();
        }

        static void Main(string[] args)
        {
            HttpPerformanceTestCase testcase = new HttpPerformanceTestCase();
            Console.ReadLine();
        }

        #endregion
    }


    class BufferPool
    {
        Stack<byte[]> items = new Stack<byte[]>();
        int bufferSize;

        public BufferPool(int bufferSize)
        {
            this.bufferSize = bufferSize;
        }

        object ThisLock { get { return this.items; } }

        public byte[] Take()
        {
            lock (this.ThisLock)
            {
                if (this.items.Count > 0)
                {
                    return this.items.Pop();
                }
            }

            return new byte[this.bufferSize];
        }

        public void Return(byte[] buffer)
        {
            lock (this.ThisLock)
            {
                this.items.Push(buffer);
            }
        }
    }
}
