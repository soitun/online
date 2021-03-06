/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <mutex>
#include <thread>
#include <vector>

#include "FakeSocket.hpp"

// A "fake socket" is represented by a number, a smallish integer, just like a real socket.
//
// There is one FakeSocketPair for each two sequential fake socket numbers. When you create one, you
// will always get the lower (even) number in a pair. The higher number wil be returned if you
// sucessfully call fakeSocketConnect() from the lower number to some other fake socket.
//
// After you create a fake socket, there is basically just two things you can do with it:
//
// 1) Call fakeSocketConnect on it giving another fake socket number to connect to. Once the
// connection is successful, you can call fakeSocketRead() and fakeSocketWrite() on your original
// socket.
//
// 2) Call fakeSocketListen() on it, indicating it is a "server" socket. After that, keep calling
// fakeSocketAccept() and each time that returns successfully, it will return a new fake socket that
// is connected to another fake socket that called fakeSocketConnect() to the server socket. You can
// then call fakeSocketRead() and fakeSocketWrite() on it.
//
// This all is complicated a bit by the fact that all the API is non-blocking.

struct FakeSocketPair
{
    int fd[2];
    bool listening;
    int connectingFd;
    bool shutdown[2];
    bool readable[2];
    std::vector<char> buffer[2];
    std::mutex *mutex;

    FakeSocketPair()
    {
        fd[0] = -1;
        fd[1] = -1;
        listening = false;
        connectingFd = -1;
        shutdown[0] = false;
        shutdown[1] = false;
        readable[0] = false;
        readable[1] = false;
        mutex = new std::mutex();
    }
};

static thread_local std::ostringstream loggingBuffer;
static void (*loggingCallback)(const std::string&) = nullptr;

static std::mutex fdsMutex;
static std::mutex cvMutex;
static std::condition_variable cv;

// Avoid problems with order of initialisation of static globals.
static std::vector<FakeSocketPair>& getFds()
{
    static std::vector<FakeSocketPair> fds;

    return fds;
}

static std::string flush()
{
    static bool alwaysStderr = std::getenv("FAKESOCKET_LOG_ALWAYS_STDERR") != nullptr;
    if (alwaysStderr)
        std::cerr << loggingBuffer.str() << std::endl;
    else if (loggingCallback != nullptr)
        loggingCallback(loggingBuffer.str());
    loggingBuffer.str("");
    return "";
}

void fakeSocketSetLoggingCallback(void (*callback)(const std::string&))
{
    loggingCallback = callback;
}

int fakeSocketSocket()
{
    std::vector<FakeSocketPair>& fds = getFds();

    std::lock_guard<std::mutex> fdsLock(fdsMutex);

    // We always allocate a new FakeSocketPair struct. Let's not bother with potential issues with
    // reusing them. It isn't like we would be allocating thousands anyway during the typical
    // lifetime of an app.

    const int i = fds.size();
    fds.resize(i + 1);

    FakeSocketPair& result = fds[i];

    result.fd[0] = i*2;

    loggingBuffer << "FakeSocket Create #" << i*2 << flush();

    return i*2;
}

int fakeSocketPipe2(int pipefd[2])
{
    pipefd[0] = fakeSocketSocket();
    assert(pipefd[0] >= 0);

    std::vector<FakeSocketPair>& fds = getFds();
    FakeSocketPair& pair = fds[pipefd[0]/2];

    std::unique_lock<std::mutex> fdLock(pair.mutex[0]);

    assert(pair.fd[0] == pipefd[0]);

    pair.fd[1] = pair.fd[0] + 1;
    pipefd[1] = pair.fd[1];

    loggingBuffer << "FakeSocket Pipe created (#" << pipefd[0] << ",#" << pipefd[1] << ")" << flush();

    return 0;
}

static std::string pollBits(int bits)
{
    if (bits == 0)
        return "-";

    std::string result;

    if (bits & POLLERR)
    {
        if (result != "")
            result += "+";
        result += "ERR";
    }
    if (bits & POLLHUP)
    {
        if (result != "")
            result += "+";
        result += "HUP";
    }
    if (bits & POLLIN)
    {
        if (result != "")
            result += "+";
        result += "IN";
    }
    if (bits & POLLNVAL)
    {
        if (result != "")
            result += "+";
        result += "NVAL";
    }
    if (bits & POLLOUT)
    {
        if (result != "")
            result += "+";
        result += "OUT";
    }
    if (bits & POLLPRI)
    {
        if (result != "")
            result += "+";
        result += "PRI";
    }

    return result;
}

static bool checkForPoll(std::vector<FakeSocketPair>& fds, struct pollfd *pollfds, int nfds)
{
    bool retval = false;
    for (int i = 0; i < nfds; i++)
    {
        const int K = ((pollfds[i].fd)&1);
        const int N = 1 - K;

        if (pollfds[i].fd < 0 || pollfds[i].fd/2 >= fds.size())
        {
            pollfds[i].revents = POLLNVAL;
            retval = true;
        }
        else
        {
            const int K = ((pollfds[i].fd)&1);
            if (fds[pollfds[i].fd/2].fd[K] == -1)
            {
                pollfds[i].revents = POLLNVAL;
                retval = true;
            }
            else
                pollfds[i].revents = 0;
        }

        if (pollfds[i].revents == 0)
        {
            if (pollfds[i].events & POLLIN)
            {
                if (fds[pollfds[i].fd/2].readable[K] ||
                    (K == 0 && fds[pollfds[i].fd/2].listening && fds[pollfds[i].fd/2].connectingFd != -1))
                {
                    pollfds[i].revents |= POLLIN;
                    retval = true;
                }
            }
            // With our trivial single-message buffering, a socket is writable if the peer socket is
            // open and not readable.
            if (pollfds[i].events & POLLOUT)
            {
                if (fds[pollfds[i].fd/2].fd[N] != -1 && !fds[pollfds[i].fd/2].readable[N] && !fds[pollfds[i].fd/2].shutdown[N])
                {
                    pollfds[i].revents |= POLLOUT;
                    retval = true;
                }
            }
        }
    }
    return retval;
}

int fakeSocketPoll(struct pollfd *pollfds, int nfds, int timeout)
{
    loggingBuffer << "FakeSocket Poll ";
    for (int i = 0; i < nfds; i++)
    {
        if (i > 0)
            loggingBuffer << ",";
        loggingBuffer << "#" << pollfds[i].fd << ":" << pollBits(pollfds[i].events);
    }
    loggingBuffer << ", timeout:" << timeout << flush();

    std::vector<FakeSocketPair>& fds = getFds();
    std::unique_lock<std::mutex> fdsLock(fdsMutex);
    std::unique_lock<std::mutex> cvLock(cvMutex);
    fdsLock.unlock();

    if (timeout > 0)
    {
        auto const now = std::chrono::steady_clock::now();
        auto const end = now + std::chrono::milliseconds(timeout);

        while (!checkForPoll(fds, pollfds, nfds))
            if (cv.wait_until(cvLock, end) == std::cv_status::timeout)
            {
                loggingBuffer << "FakeSocket Poll timeout: 0" << flush();
                return 0;
            }
    }
    else if (timeout == 0)
    {
        checkForPoll(fds, pollfds, nfds);
    }
    else // timeout < 0
    {
        while (!checkForPoll(fds, pollfds, nfds))
            cv.wait(cvLock);
    }

    int result = 0;
    for (int i = 0; i < nfds; i++)
    {
        if (pollfds[i].revents != 0)
            result++;
    }

    loggingBuffer << "FakeSocket Poll result: ";
    for (int i = 0; i < nfds; i++)
    {
        if (i > 0)
            loggingBuffer << ",";
        loggingBuffer << "#" << pollfds[i].fd << ":" << pollBits(pollfds[i].revents);
    }
    loggingBuffer << ": " << result << flush();

    return result;
}

int fakeSocketListen(int fd)
{
    std::vector<FakeSocketPair>& fds = getFds();
    std::unique_lock<std::mutex> fdsLock(fdsMutex);
    if (fd < 0 || fd/2 >= fds.size() || fds[fd/2].fd[fd&1] == -1)
    {
        loggingBuffer << "FakeSocket EBADF: Listening on #" << fd << flush();
        errno = EBADF;
        return -1;
    }
    
    FakeSocketPair& pair = fds[fd/2];
    std::unique_lock<std::mutex> fdLock(pair.mutex[0]);
    fdsLock.unlock();

    if (fd&1 || pair.fd[1] != -1)
    {
        loggingBuffer << "FakeSocket EISCONN: Listening on #" << fd << flush();
        errno = EISCONN;
        return -1;
    }
    
    if (pair.listening)
    {
        loggingBuffer << "FakeSocket EIO: Listening on #" << fd << flush();
        errno = EIO;
        return -1;
    }

    pair.listening = true;
    pair.connectingFd = -1;

    loggingBuffer << "FakeSocket Listen #" << fd << flush();

    return 0;
}

int fakeSocketConnect(int fd1, int fd2)
{
    std::vector<FakeSocketPair>& fds = getFds();
    std::unique_lock<std::mutex> fdsLock(fdsMutex);
    if (fd1 < 0 || fd2 < 0 || fd1/2 >= fds.size() || fd2/2 >= fds.size())
    {
        loggingBuffer << "FakeSocket EBADF: Connect #" << fd1 << " to #" << fd2 << flush();
        errno = EBADF;
        return -1;
    }
    if (fd1/2 == fd2/2)
    {
        loggingBuffer << "FakeSocket EBADF: Connect #" << fd1 << " to #" << fd2 << flush();
        errno = EBADF;
        return -1;
    }

    FakeSocketPair& pair1 = fds[fd1/2];
    FakeSocketPair& pair2 = fds[fd2/2];

    std::unique_lock<std::mutex> fdLock1(pair1.mutex[0]);
    std::unique_lock<std::mutex> fdLock2(pair2.mutex[0]);
    fdsLock.unlock();

    if ((fd1&1) || (fd2&1))
    {
        loggingBuffer << "FakeSocket EISCONN: Connect #" << fd1 << " to #" << fd2 << flush();
        errno = EISCONN;
        return -1;
    }

    if (!pair2.listening || pair2.connectingFd != -1)
    {
        loggingBuffer << "FakeSocket ECONNREFUSED: Connect #" << fd1 << " to #" << fd2 << flush();
        errno = ECONNREFUSED;
        return -1;
    }

    pair2.connectingFd = fd1;
    cv.notify_all();

    std::unique_lock<std::mutex> cvLock(cvMutex);
    fdLock2.unlock();
    fdLock1.unlock();

    while (pair1.fd[1] == -1)
        cv.wait(cvLock);

    assert(pair1.fd[1] == pair1.fd[0] + 1);

    loggingBuffer << "FakeSocket Connect #" << fd1 << " to #" << fd2 << flush();

    return 0;
}

int fakeSocketAccept4(int fd, int flags)
{
    std::vector<FakeSocketPair>& fds = getFds();
    std::unique_lock<std::mutex> fdsLock(fdsMutex);
    if (fd < 0 || fd/2 >= fds.size())
    {
        loggingBuffer << "FakeSocket EBADF: Accept #" << fd << flush();
        errno = EBADF;
        return -1;
    }

    if (fd & 1)
    {
        loggingBuffer << "FakeSocket EISCONN: Accept #" << fd << flush();
        errno = EISCONN;
        return -1;
    }

    FakeSocketPair& pair = fds[fd/2];

    if (!pair.listening)
    {
        loggingBuffer << "FakeSocket EIO: Accept #" << fd << flush();
        errno = EIO;
        return -1;
    }

    fdsLock.unlock();

    std::unique_lock<std::mutex> cvLock(cvMutex);

    while (pair.connectingFd == -1)
        cv.wait(cvLock);
    
    cvLock.unlock();

    assert(pair.connectingFd >= 0 && pair.connectingFd/2 < fds.size());

    FakeSocketPair& pair2 = fds[pair.connectingFd/2];
    
    std::unique_lock<std::mutex> fdLock1(pair2.mutex[0]);

    assert(pair2.fd[1] == -1);
    assert(pair2.fd[0] == pair.connectingFd);

    pair.connectingFd = -1;

    pair2.fd[1] = pair2.fd[0] + 1;

    cv.notify_all();

    loggingBuffer << "FakeSocket Accept #" << fd << ": #" << pair2.fd[1] << flush();

    return pair2.fd[1];
}

int fakeSocketPeer(int fd)
{
    std::vector<FakeSocketPair>& fds = getFds();
    std::unique_lock<std::mutex> fdsLock(fdsMutex);
    if (fd < 0 || fd/2 >= fds.size())
    {
        loggingBuffer << "FakeSocket EBADF: Peer of #" << fd << flush();
        errno = EBADF;
        return -1;
    }

    FakeSocketPair& pair = fds[fd/2];

    std::unique_lock<std::mutex> fdLock(pair.mutex[0]);
    fdsLock.unlock();

    const int K = (fd&1);
    const int N = 1 - K;

    loggingBuffer << "FakeSocket Peer of #" << fd << ": #" << pair.fd[N] << flush();

    return pair.fd[N];
}

ssize_t fakeSocketAvailableDataLength(int fd)
{
    std::vector<FakeSocketPair>& fds = getFds();
    std::unique_lock<std::mutex> fdsLock(fdsMutex);
    if (fd < 0 || fd/2 >= fds.size())
    {
        errno = EBADF;
        return -1;
    }

    FakeSocketPair& pair = fds[fd/2];

    std::unique_lock<std::mutex> fdLock(pair.mutex[0]);
    fdsLock.unlock();

    // K: for this fd
    const int K = (fd&1);

    if (!pair.readable[K])
    {
        loggingBuffer << "FakeSocket EAGAIN: Available data on #" << fd << flush();
        errno = EAGAIN;
        return -1;
    }

    loggingBuffer << "FakeSocket Available data on #" << fd << ": " << pair.buffer[K].size() << flush();

    return pair.buffer[K].size();
}

ssize_t fakeSocketRead(int fd, void *buf, size_t nbytes)
{
    std::vector<FakeSocketPair>& fds = getFds();
    std::unique_lock<std::mutex> fdsLock(fdsMutex);
    if (fd < 0 || fd/2 >= fds.size())
    {
        loggingBuffer << "FakeSocket EBADF: Read from #" << fd << ", " << nbytes << (nbytes == 1 ? " byte" : " bytes") << flush();
        errno = EBADF;
        return -1;
    }

    FakeSocketPair& pair = fds[fd/2];

    std::unique_lock<std::mutex> fdLock(pair.mutex[0]);
    fdsLock.unlock();

    // K: for this fd
    const int K = (fd&1);
    // N: for its peer
    const int N = 1 - K;

    if (pair.fd[K] == -1)
    {
        loggingBuffer << "FakeSocket EBADF: Read from #" << fd << ", " << nbytes << (nbytes == 1 ? " byte" : " bytes") << flush();
        errno = EBADF;
        return -1;
    }

    if (pair.shutdown[K])
    {
        loggingBuffer << "FakeSocket Read from #" << fd << " (shut down) got 0 bytes" << flush();
        return 0;
    }

    if (!pair.readable[K])
    {
        loggingBuffer << "FakeSocket EAGAIN: Read from #" << fd << ", " << nbytes << (nbytes == 1 ? " byte" : " bytes") << flush();
        errno = EAGAIN;
        return -1;
    }

    // These sockets are record-oriented! It won't work to read less than the whole buffer.
    ssize_t result = pair.buffer[K].size();
    if (nbytes < result)
    {
        loggingBuffer << "FakeSocket EAGAIN: Read from #" << fd << ", " << nbytes << (nbytes == 1 ? " byte" : " bytes") << flush();
        errno = EAGAIN; // Not the right errno, but what would be?q
        return -1;
    }

    memmove(buf, pair.buffer[K].data(), result);
    pair.buffer[K].resize(0);
    // If peer is closed or shut down, we continue to be readable
    if (pair.fd[N] == -1 || pair.shutdown[N])
        pair.readable[K] = true;
    else
        pair.readable[K] = false;

    cv.notify_all();

    loggingBuffer << "FakeSocket Read from #" << fd << " got " << result << (result == 1 ? " byte" : " bytes") << flush();

    return result;
}

ssize_t fakeSocketWrite(int fd, const void *buf, size_t nbytes)
{
    std::vector<FakeSocketPair>& fds = getFds();
    std::unique_lock<std::mutex> fdsLock(fdsMutex);
    if (fd < 0 || fd/2 >= fds.size())
    {
        loggingBuffer << "FakeSocket EBADF: Write to #" << fd << ", " << nbytes << (nbytes == 1 ? " byte" : " bytes") << flush();
        errno = EBADF;
        return -1;
    }

    FakeSocketPair& pair = fds[fd/2];

    std::unique_lock<std::mutex> fdLock(pair.mutex[0]);
    fdsLock.unlock();

    // K: for this fd
    // N: for its peer, whose read buffer we want to write into
    const int K = (fd&1);
    const int N = 1 - K;

    if (pair.fd[K] == -1)
    {
        loggingBuffer << "FakeSocket EBADF: Write to #" << fd << ", " << nbytes << (nbytes == 1 ? " byte" : " bytes") << flush();
        errno = EBADF;
        return -1;
    }

    if (pair.shutdown[K])
    {
        // Should we raise(SIGPIPE)? Probably not, Online code does not expect SIGPIPE at all...
        loggingBuffer << "FakeSocket EPIPE: Write to #" << fd << " (shut down), " << nbytes << (nbytes == 1 ? " byte" : " bytes") << flush();
        errno = EPIPE;
        return -1;
    }

    if (pair.readable[N])
    {
        loggingBuffer << "FakeSocket EAGAIN: Write to #" << fd << ", " << nbytes << (nbytes == 1 ? " byte" : " bytes") << flush();
        errno = EAGAIN;
        return -1;
    }

    pair.buffer[N].resize(nbytes);
    memmove(pair.buffer[N].data(), buf, nbytes);
    pair.readable[N] = true;

    cv.notify_all();

    loggingBuffer << "FakeSocket Write to #" << fd << ": " << nbytes << (nbytes == 1 ? " byte" : " bytes") << flush();
    return nbytes;
}

int fakeSocketShutdown(int fd)
{
    std::vector<FakeSocketPair>& fds = getFds();
    std::unique_lock<std::mutex> fdsLock(fdsMutex);
    if (fd < 0 || fd/2 >= fds.size())
    {
        loggingBuffer << "FakeSocket EBADF: Shutdown #" << fd << flush();
        errno = EBADF;
        return -1;
    }

    FakeSocketPair& pair = fds[fd/2];

    std::unique_lock<std::mutex> fdLock(pair.mutex[0]);
    fdsLock.unlock();

    const int K = (fd&1);
    const int N = 1 - K;

    if (pair.fd[K] == -1)
    {
        loggingBuffer << "FakeSocket EBADF: Shutdown #" << fd << flush();
        errno = EBADF;
        return -1;
    }

    if (pair.fd[N] == -1)
    {
        loggingBuffer << "FakeSocket ENOTCONN: Shutdown #" << fd << flush();
        errno = ENOTCONN;
        return -1;
    }

    pair.shutdown[K] = true;
    pair.readable[K] = true;

    loggingBuffer << "FakeSocket Shutdown #" << fd << flush();

    return 0;
}

int fakeSocketClose(int fd)
{
    std::vector<FakeSocketPair>& fds = getFds();
    std::unique_lock<std::mutex> fdsLock(fdsMutex);
    if (fd < 0 || fd/2 >= fds.size())
    {
        loggingBuffer << "FakeSocket EBADF: Close #" << fd << flush();
        errno = EBADF;
        return -1;
    }

    FakeSocketPair& pair = fds[fd/2];

    std::unique_lock<std::mutex> fdLock(pair.mutex[0]);
    fdsLock.unlock();

    const int K = (fd&1);
    const int N = 1 - K;

    if (pair.fd[K] == -1)
    {
        loggingBuffer << "FakeSocket EBADF: Close #" << fd << flush();
        errno = EBADF;
        return -1;
    }

    assert(pair.fd[K] == fd);

    pair.fd[K] = -1;
    pair.buffer[K].resize(0);
    pair.readable[N] = true;

    cv.notify_all();

    loggingBuffer << "FakeSocket Close #" << fd << flush();

    return 0;
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
