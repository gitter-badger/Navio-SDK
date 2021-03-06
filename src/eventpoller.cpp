#include "eventpoller.h"
#include "fdevent.h"
#include "log.h"

#include <sys/epoll.h>
#include <string.h>
#include <sched.h>
#include <stdio.h>
#include <cassert>
#include <errno.h>

EventPoller::EventPoller():
    _epoll_mono_time(0), _callback_mono_time(0), _epoll_cpu_time(0), _callback_cpu_time(0),
    _fd(-1), _fd_read_pool(), _fd_write_pool()
{
    _fd = epoll_create(1);
    assert(_fd >= 0);

    sched_param schedparm;
    schedparm.sched_priority = sched_get_priority_min(SCHED_FIFO);
    if (schedparm.sched_priority == -1 || sched_setscheduler(0, SCHED_FIFO, &schedparm) == -1) {
        Error() << "Unable to set scheduller priority. Events may coalesce.";
    }
}

EventPoller::~EventPoller()
{
    if (!_fd_read_pool.empty()) {
        Error() << "Read event pool is not empty";
    }
    if (!_fd_write_pool.empty()) {
        Error() << "Write event pool is not empty";
    }
}

void EventPoller::pollEvents()
{
    int count;
    timespec a_mono_time, b_mono_time, c_mono_time, a_cpu_time, b_cpu_time, c_cpu_time;
    epoll_event events[16];

    while (true) {
        clock_gettime(CLOCK_MONOTONIC, &a_mono_time);
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &a_cpu_time);

        count = epoll_wait(_fd, events, 16, -1);

        clock_gettime(CLOCK_MONOTONIC, &b_mono_time);
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &b_cpu_time);

        if (count) {
            for (int i=0; i<count; i++) {
                if (events[i].events & EPOLLOUT) {
                    _fd_write_pool[events[i].data.fd]->_onWrite();
                } else {
                    _fd_read_pool[events[i].data.fd]->_onRead();
                }
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &c_mono_time);
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c_cpu_time);

        _epoll_mono_time += ((float)(b_mono_time.tv_sec - a_mono_time.tv_sec) * 1000
                             + (float)(b_mono_time.tv_nsec - a_mono_time.tv_nsec) / 1000000);
        _callback_mono_time += ((float)(c_mono_time.tv_sec - b_mono_time.tv_sec) * 1000
                                + (float)(c_mono_time.tv_nsec - b_mono_time.tv_nsec) / 1000000);
        _epoll_cpu_time += ((float)(b_cpu_time.tv_sec - a_cpu_time.tv_sec) * 1000
                            + (float)(b_cpu_time.tv_nsec - a_cpu_time.tv_nsec) / 1000000);
        _callback_cpu_time += ((float)(c_cpu_time.tv_sec - b_cpu_time.tv_sec) * 1000
                               + (float)(c_cpu_time.tv_nsec - b_cpu_time.tv_nsec) / 1000000);
    }
}
void EventPoller::getTimings(float &epoll_mono, float &callback_mono, float &epoll_cpu, float &callback_cpu)
{
    epoll_mono = _epoll_mono_time;
    callback_mono = _callback_mono_time;
    epoll_cpu = _epoll_cpu_time;
    callback_cpu = _callback_cpu_time;
    _epoll_mono_time = _callback_mono_time = _epoll_cpu_time = _callback_cpu_time = 0;
}

bool EventPoller::_registerFDRead(int fd, FDEvent *fd_event)
{
    if (!_fd_read_pool.empty() && _fd_read_pool.count(fd)) {
        Error() << "Read event already registred. fd:" << fd;
        return false;
    }

    epoll_event e;
    e.events = EPOLLIN | EPOLLPRI | EPOLLERR | EPOLLHUP;
    e.data.fd = fd;
    int ret = epoll_ctl(_fd, EPOLL_CTL_ADD, fd, &e);
    if (ret != 0) {
        Error() << "Unable to register read event for fd:" << fd << "errno:" << errno << strerror(errno);
        return false;
    }

    _fd_read_pool[fd] = fd_event;
    return true;
}

bool EventPoller::_registerFDWrite(int fd, FDEvent *fd_event)
{
    if (!_fd_write_pool.empty() && _fd_write_pool.count(fd)) {
        Error() << "Write event already registred. fd:" << fd;
        return false;
    }

    epoll_event e;
    e.events = EPOLLOUT;
    int ret = epoll_ctl(_fd, EPOLL_CTL_ADD, fd, &e);
    if (ret != 0) {
        Error() << "Unable to register write event for fd:" << fd << "errno:" << errno << strerror(errno);
        return false;
    }

    _fd_write_pool[fd] = fd_event;
    return true;
}

bool EventPoller::_unregisterFDRead(int fd)
{
    if (_fd_read_pool.empty() || !_fd_read_pool.count(fd)) {
        Error() << "Read event not exists. fd:" << fd;
        return false;
    }

    epoll_event e;
    e.events = EPOLLIN;
    int ret = epoll_ctl(_fd, EPOLL_CTL_DEL, fd, &e);
    if (ret != 0) {
        Error() << "Unable to unregister write event for fd:" << fd << "errno:" << errno << strerror(errno);
        return false;
    }

    _fd_read_pool.erase(fd);
    return true;
}

bool EventPoller::_unregisterFDWrite(int fd)
{
    if (_fd_write_pool.empty() || !_fd_write_pool.count(fd)) {
        Error() << "Write event not exists. fd:" << fd;
        return false;
    }

    epoll_event e;
    e.events = EPOLLOUT;
    int ret = epoll_ctl(_fd, EPOLL_CTL_DEL, fd, &e);
    if (ret != 0) {
        Error() << "Unable to register write event for fd:" << fd << "errno:" << errno << strerror(errno);
        return false;
    }

    _fd_write_pool.erase(fd);
    return true;
}
