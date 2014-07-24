using System;
using System.Diagnostics;
using System.Threading;

namespace HttpPerf
{
    public abstract class AsyncResult : IAsyncResult
    {
        static AsyncCallback _asyncCompletionWrapperCallback;
        readonly AsyncCallback _callback;
        bool _completedSynchronously;
        bool _endCalled;
        Exception _exception;
        AsyncCompletion _nextAsyncCompletion;
        readonly object _state;

        volatile ManualResetEvent _manualResetEvent;

        readonly object _dangerousSelfLock;

        protected AsyncResult(AsyncCallback callback, object state)
        {
            this._callback = callback;
            this._state = state;
            this._dangerousSelfLock = this;
        }

        public object AsyncState
        {
            get
            {
                return _state;
            }
        }

        public WaitHandle AsyncWaitHandle
        {
            get
            {
                if (_manualResetEvent != null)
                {
                    return _manualResetEvent;
                }

                lock (ThisLock)
                {
                    if (_manualResetEvent == null)
                    {
                        _manualResetEvent = new ManualResetEvent(IsCompleted);
                    }
                }

                return _manualResetEvent;
            }
        }

        public bool CompletedSynchronously
        {
            get
            {
                return _completedSynchronously;
            }
        }

        public bool HasCallback
        {
            get
            {
                return this._callback != null;
            }
        }

        public bool IsCompleted { get; private set; }

        protected Action<Exception> OnCompleting { get; set; }

        object ThisLock
        {
            get
            {
                return this._dangerousSelfLock;
            }
        }

        protected Action<AsyncCallback, IAsyncResult> VirtualCallback
        {
            get;
            set;
        }

        protected void Complete(bool completedSynchronously)
        {
            this._completedSynchronously = completedSynchronously;
            if (OnCompleting != null)
            {
                try
                {
                    OnCompleting(this._exception);
                }
                catch (Exception exception)
                {
                    this._exception = exception;
                }
            }

            if (completedSynchronously)
            {
                this.IsCompleted = true;
            }
            else
            {
                lock (ThisLock)
                {
                    this.IsCompleted = true;
                    if (this._manualResetEvent != null)
                    {
                        this._manualResetEvent.Set();
                    }
                }
            }

            if (this._callback != null)
            {
                if (VirtualCallback != null)
                {
                    VirtualCallback(this._callback, this);
                }
                else
                {
                    this._callback(this);
                }
            }
        }

        protected void Complete(bool completedSynchronously, Exception exception)
        {
            this._exception = exception;
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
            this._nextAsyncCompletion = callback;
            return AsyncResult._asyncCompletionWrapperCallback ??
                   (AsyncResult._asyncCompletionWrapperCallback = new AsyncCallback(AsyncCompletionWrapperCallback));
        }

        AsyncCompletion GetNextCompletion()
        {
            AsyncCompletion result = this._nextAsyncCompletion;
            this._nextAsyncCompletion = null;
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
                throw new ArgumentException("Invalid result", "result");
            }

            if (asyncResult._endCalled)
            {
                throw new InvalidOperationException("AsyncResult has already ended");
            }

            asyncResult._endCalled = true;

            if (!asyncResult.IsCompleted)
            {
                asyncResult.AsyncWaitHandle.WaitOne();
            }

            if (asyncResult._manualResetEvent != null)
            {
                asyncResult._manualResetEvent.Close();
            }

            if (asyncResult._exception != null)
            {
                // Rethrow the exception
                throw asyncResult._exception;
            }

            return asyncResult;
        }

        protected delegate bool AsyncCompletion(IAsyncResult result);
    }
}
