using System;
using System.Diagnostics;
using System.Threading;

namespace HttpPerf
{
    public abstract class AsyncResult : IAsyncResult
    {
        static AsyncCallback asyncCompletionWrapperCallback;
        AsyncCallback callback;
        bool completedSynchronously;
        bool endCalled;
        Exception exception;
        bool isCompleted;
        AsyncCompletion nextAsyncCompletion;
        object state;

        ManualResetEvent manualResetEvent;

        object thisLock;

        protected AsyncResult(AsyncCallback callback, object state)
        {
            this.callback = callback;
            this.state = state;
            this.thisLock = new object();
        }

        public object AsyncState
        {
            get
            {
                return state;
            }
        }

        public WaitHandle AsyncWaitHandle
        {
            get
            {
                if (manualResetEvent != null)
                {
                    return manualResetEvent;
                }

                lock (ThisLock)
                {
                    if (manualResetEvent == null)
                    {
                        manualResetEvent = new ManualResetEvent(isCompleted);
                    }
                }

                return manualResetEvent;
            }
        }

        public bool CompletedSynchronously
        {
            get
            {
                return completedSynchronously;
            }
        }

        public bool HasCallback
        {
            get
            {
                return this.callback != null;
            }
        }

        public bool IsCompleted
        {
            get
            {
                return isCompleted;
            }
        }

        protected Action<Exception> OnCompleting { get; set; }

        object ThisLock
        {
            get
            {
                return this.thisLock;
            }
        }

        protected Action<AsyncCallback, IAsyncResult> VirtualCallback
        {
            get;
            set;
        }

        protected void Complete(bool completedSynchronously)
        {
            this.completedSynchronously = completedSynchronously;
            if (OnCompleting != null)
            {
                try
                {
                    OnCompleting(this.exception);
                }
                catch (Exception exception)
                {
                    this.exception = exception;
                }
            }

            if (completedSynchronously)
            {
                this.isCompleted = true;
            }
            else
            {
                lock (ThisLock)
                {
                    this.isCompleted = true;
                    if (this.manualResetEvent != null)
                    {
                        this.manualResetEvent.Set();
                    }
                }
            }

            if (this.callback != null)
            {
                if (VirtualCallback != null)
                {
                    VirtualCallback(this.callback, this);
                }
                else
                {
                    this.callback(this);
                }
            }
        }

        protected void Complete(bool completedSynchronously, Exception exception)
        {
            this.exception = exception;
            Complete(completedSynchronously);
        }

        static void AsyncCompletionWrapperCallback(IAsyncResult result)
        {
            if (result.CompletedSynchronously)
            {
                return;
            }

            AsyncResult thisPtr = (AsyncResult)result.AsyncState;
            AsyncCompletion callback = thisPtr.GetNextCompletion();

            bool completeSelf = false;
            Exception completionException = null;
            try
            {
                completeSelf = callback(result);
            }
            catch (Exception e)
            {
                completeSelf = true;
                completionException = e;
            }

            if (completeSelf)
            {
                thisPtr.Complete(false, completionException);
            }
        }

        protected AsyncCallback PrepareAsyncCompletion(AsyncCompletion callback)
        {
            this.nextAsyncCompletion = callback;
            if (AsyncResult.asyncCompletionWrapperCallback == null)
            {
                AsyncResult.asyncCompletionWrapperCallback = new AsyncCallback(AsyncCompletionWrapperCallback);
            }

            return AsyncResult.asyncCompletionWrapperCallback;
        }

        AsyncCompletion GetNextCompletion()
        {
            AsyncCompletion result = this.nextAsyncCompletion;
            this.nextAsyncCompletion = null;
            return result;
        }

        protected static TAsyncResult End<TAsyncResult>(IAsyncResult result)
            where TAsyncResult : AsyncResult
        {
            if (result == null)
            {
                throw new ArgumentNullException("result");
            }

            TAsyncResult asyncResult = result as TAsyncResult;

            if (asyncResult == null)
            {
                throw new ArgumentException("result", "Invalid asyncResult");
            }

            if (asyncResult.endCalled)
            {
                throw new InvalidOperationException("AsyncResult has already ended");
            }

            asyncResult.endCalled = true;

            if (!asyncResult.isCompleted)
            {
                asyncResult.AsyncWaitHandle.WaitOne();
            }

            if (asyncResult.manualResetEvent != null)
            {
                asyncResult.manualResetEvent.Close();
            }

            if (asyncResult.exception != null)
            {
                // Rethrow the exception
                throw asyncResult.exception;
            }

            return asyncResult;
        }

        protected delegate bool AsyncCompletion(IAsyncResult result);
    }
}
