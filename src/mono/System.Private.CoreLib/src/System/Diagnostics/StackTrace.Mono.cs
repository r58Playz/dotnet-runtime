// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace System.Diagnostics
{
    // Need our own stackframe class since the shared version has its own fields
    [StructLayout(LayoutKind.Sequential)]
    internal sealed class MonoStackFrame
    {
        #region Keep in sync with object-internals.h
        internal int ilOffset;
        internal int nativeOffset;
        // Unused
        internal long methodAddress;
        // Unused
        internal uint methodIndex;
        internal MethodBase? methodBase;
        internal string? fileName;
        internal int lineNumber;
        internal int columnNumber;
        // Unused
        internal string? internalMethodName;
        #endregion

        internal bool isLastFrameFromForeignException;
    }

    public partial class StackTrace
    {
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal static extern void GetTrace(ObjectHandleOnStack ex, ObjectHandleOnStack res, int skipFrames, bool needFileInfo);

        // ONE walk: GetTrace with a null exception walks the current thread, drops the frames of this API itself by
        // what they are (System.Diagnostics.StackFrame/StackTrace, and wrappers), then skips the caller's skipFrames.
        // This used to construct `new StackFrame(skipFrames++)` until one came back empty -- a full walk per frame -- with
        // "+2" for this method and the calling constructor, frames inlining can remove.
        [MethodImplAttribute(MethodImplOptions.NoInlining)]
        private void InitializeForCurrentThread(int skipFrames, bool needFileInfo)
        {
            Exception? e = null;
            MonoStackFrame[]? frames = null;
            GetTrace(ObjectHandleOnStack.Create(ref e), ObjectHandleOnStack.Create(ref frames), skipFrames, needFileInfo);

            _numOfFrames = frames!.Length;
            _stackFrames = new StackFrame[_numOfFrames];
            for (int i = 0; i < _numOfFrames; ++i)
            {
                _stackFrames[i] = new StackFrame(frames[i], needFileInfo);
            }
        }

        private void InitializeForException(Exception e, int skipFrames, bool needFileInfo)
        {
            MonoStackFrame[]? frames = null;
            GetTrace(ObjectHandleOnStack.Create(ref e), ObjectHandleOnStack.Create(ref frames), skipFrames, needFileInfo);
            _numOfFrames = frames!.Length;

            int foreignFrames;
            MonoStackFrame[]? foreignExceptions = e.foreignExceptionsFrames;

            if (foreignExceptions != null)
            {
                foreignFrames = foreignExceptions.Length;
                _numOfFrames += foreignFrames;

                _stackFrames = new StackFrame[_numOfFrames];

                for (int i = 0; i < foreignExceptions.Length; ++i)
                {
                    _stackFrames[i] = new StackFrame(foreignExceptions[i], needFileInfo);
                }
            }
            else
            {
                _stackFrames = new StackFrame[_numOfFrames];
                foreignFrames = 0;
            }

            for (int i = 0; i < frames.Length; ++i)
            {
                _stackFrames[foreignFrames + i] = new StackFrame(frames[i], needFileInfo);
            }
        }
    }
}
