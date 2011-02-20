// ekam -- http://code.google.com/p/ekam
// Copyright (c) 2010 Kenton Varda and contributors.  All rights reserved.
// Portions copyright Google, Inc.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of the ekam project nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "EventGroup.h"

#include "base/Debug.h"

namespace ekam {

EventGroup::ExceptionHandler::~ExceptionHandler() {}

#define HANDLE_EXCEPTIONS(STATEMENT)                             \
  EventGroup* group = this->group;                               \
  try {                                                          \
    STATEMENT;                                                   \
    if (group->eventCount == 0) {                                \
      group->callNoMoreEventsLater();                            \
    }                                                            \
  } catch (const std::exception& exception) {                    \
    group->exceptionHandler->threwException(exception);          \
  } catch (...) {                                                \
    group->exceptionHandler->threwUnknownException();            \
  }

class EventGroup::RunnableWrapper : public Runnable {
public:
  RunnableWrapper(EventGroup* group, OwnedPtr<Runnable> wrapped)
      : group(group), wrapped(wrapped.release()) {
    ++group->eventCount;
  }
  ~RunnableWrapper() {
    --group->eventCount;
  }

  // implements Runnable -----------------------------------------------------------------
  void run() { HANDLE_EXCEPTIONS(wrapped->run()); }

private:
  EventGroup* group;
  OwnedPtr<Runnable> wrapped;
};

class EventGroup::CallbackWrapper : public Callback, public AsyncOperation {
public:
  CallbackWrapper(EventGroup* group, Callback* wrapped)
      : group(group), wrapped(wrapped) {
    ++group->eventCount;
  }
  ~CallbackWrapper() {
    --group->eventCount;
  }

  OwnedPtr<AsyncOperation> inner;

  // implements Callback -----------------------------------------------------------------
  void run() { HANDLE_EXCEPTIONS(wrapped->run()); }

private:
  EventGroup* group;
  Callback* wrapped;
};

class EventGroup::IoCallbackWrapper : public IoCallback, public AsyncOperation {
public:
  IoCallbackWrapper(EventGroup* group, IoCallback* wrapped)
      : group(group), wrapped(wrapped) {
    ++group->eventCount;
  }
  ~IoCallbackWrapper() {
    --group->eventCount;
  }

  OwnedPtr<AsyncOperation> inner;

  // implements IoCallback ---------------------------------------------------------------
  void ready() { HANDLE_EXCEPTIONS(wrapped->ready()); }

private:
  EventGroup* group;
  IoCallback* wrapped;
};

class EventGroup::FileChangeCallbackWrapper : public FileChangeCallback, public AsyncOperation {
public:
  FileChangeCallbackWrapper(EventGroup* group, FileChangeCallback* wrapped)
      : group(group), wrapped(wrapped) {
    ++group->eventCount;
  }
  ~FileChangeCallbackWrapper() {
    --group->eventCount;
  }

  OwnedPtr<AsyncOperation> inner;

  // implements FileChangeCallback -------------------------------------------------------
  void modified() { HANDLE_EXCEPTIONS(wrapped->modified()); }
  void deleted() { HANDLE_EXCEPTIONS(wrapped->deleted()); }

private:
  EventGroup* group;
  FileChangeCallback* wrapped;
};

#undef HANDLE_EXCEPTIONS

// =======================================================================================

EventGroup::EventGroup(EventManager* inner, ExceptionHandler* exceptionHandler)
    : inner(inner), exceptionHandler(exceptionHandler), eventCount(0) {}

EventGroup::~EventGroup() {}

OwnedPtr<PendingRunnable> EventGroup::runLater(OwnedPtr<Runnable> runnable) {
  auto wrappedCallback = newOwned<RunnableWrapper>(this, runnable.release());
  return inner->runLater(wrappedCallback.release());
}

OwnedPtr<AsyncOperation> EventGroup::runAsynchronously(Callback* callback) {
  auto wrappedCallback = newOwned<CallbackWrapper>(this, callback);
  wrappedCallback->inner = inner->runAsynchronously(wrappedCallback.get());
  return wrappedCallback.release();
}

Promise<ProcessExitCode> EventGroup::onProcessExit(pid_t pid) {
  Promise<ProcessExitCode> innerPromise = inner->onProcessExit(pid);
  ++eventCount;
  return when(innerPromise)(
    [this](ProcessExitCode exitCode) -> ProcessExitCode {
      if (--eventCount == 0) {
        callNoMoreEventsLater();
      }
      return exitCode;
    });
}

OwnedPtr<AsyncOperation> EventGroup::onReadable(int fd, IoCallback* callback) {
  auto wrappedCallback = newOwned<IoCallbackWrapper>(this, callback);
  wrappedCallback->inner = inner->onReadable(fd, wrappedCallback.get());
  return wrappedCallback.release();
}

OwnedPtr<AsyncOperation> EventGroup::onWritable(int fd, IoCallback* callback) {
  auto wrappedCallback = newOwned<IoCallbackWrapper>(this, callback);
  wrappedCallback->inner = inner->onWritable(fd, wrappedCallback.get());
  return wrappedCallback.release();
}

OwnedPtr<AsyncOperation> EventGroup::onFileChange(const std::string& filename,
                                                  FileChangeCallback* callback) {
  auto wrappedCallback = newOwned<FileChangeCallbackWrapper>(this, callback);
  wrappedCallback->inner = inner->onFileChange(filename, wrappedCallback.get());
  return wrappedCallback.release();
}

void EventGroup::callNoMoreEventsLater() {
  pendingNoMoreEvents = inner->when()(
    [this](){
      if (eventCount == 0) {
        exceptionHandler->noMoreEvents();
      }
      pendingNoMoreEvents.release();
    });
}

}  // namespace ekam