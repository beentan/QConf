#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

#include <unordered_set>
#include <string>
#include <utility>
#include <map>
#include <cstring>

#include "monitor_check_thread.h"
#include "monitor_config.h"
#include "monitor_const.h"
#include "monitor_log.h"
#include "monitor_process.h"
#include "monitor_load_balance.h"
#include "monitor_listener.h"

CheckThread::CheckThread(int pos, WorkThread *workThread) :
    pink::Thread::Thread(p_conf->scanInterval()),
    _service_pos(pos),
    _workThread(workThread) {
}

CheckThread::~CheckThread() {
}

bool CheckThread::_isServiceExist(struct in_addr *addr, string host, int port, int timeout, int curStatus) {
    bool exist = false;
    int sock = -1, val = 1, ret = 0;
    struct timeval conn_tv;
    struct timeval recv_tv;
    struct sockaddr_in serv_addr;
    fd_set readfds, writefds, errfds;

    timeout = timeout <= 0 ? 1 : timeout;

    if((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        LOG(LOG_ERROR, "socket failed. error:%s", strerror(errno));
        // return false is a good idea ?
        return false;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    serv_addr.sin_addr = *addr;

    // set socket non-block
    ioctl(sock, FIONBIO, &val);

    // set connect timeout
    conn_tv.tv_sec = timeout;
    conn_tv.tv_usec = 0;

    // set recv timeout
    recv_tv.tv_sec = 1;
    recv_tv.tv_sec = 0;
    setsockopt( sock, SOL_SOCKET, SO_RCVTIMEO, &recv_tv, sizeof(recv_tv));

    // connect
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) != 0) {
        if (errno != EINPROGRESS) {
            if (curStatus != STATUS_DOWN) {
                LOG(LOG_ERROR, "connect failed. host:%s port:%d error:%s",
                        host.c_str(), port, strerror(errno));
            }
            close(sock);
            return false;
        }
    }
    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);
    FD_ZERO(&writefds);
    FD_SET(sock, &writefds);
    FD_ZERO(&errfds);
    FD_SET(sock, &errfds);
    ret = select(sock+1, &readfds, &writefds, &errfds, &conn_tv);
    if ( ret <= 0 ){
        // connect timeout
        if (curStatus != STATUS_DOWN) {
            if (ret == 0) LOG(LOG_ERROR, "connect timeout. host:%s port:%d timeout:%d error:%s",
                              host.c_str(), port, timeout, strerror(errno));
            else LOG(LOG_ERROR, "select error. host:%s port:%d timeout:%d error:%s",
                     host.c_str(), port, timeout, strerror(errno));
        }
        exist = false;
    }
    else {
        if (! FD_ISSET(sock, &readfds) && ! FD_ISSET(sock, &writefds)) {
            if (curStatus != STATUS_DOWN) {
               LOG(LOG_ERROR, "select not in read fds and write fds.host:%s port:%d error:%s",
                   host.c_str(), port, strerror(errno));
            }
        }
        else if (FD_ISSET(sock, &errfds)) {
            exist = false;
        }
        else if (FD_ISSET(sock, &writefds) && FD_ISSET(sock, &readfds)) {
            exist = false;
        }
        else if (FD_ISSET(sock, &readfds) || FD_ISSET(sock, &writefds)) {
            exist = true;
        }
        else {
            exist = false;
        }
    }
    close(sock);
    return exist;
}

// Try to connect to the ipPort to see weather it's connecteble
int CheckThread::_tryConnect(const string &curServiceFather) {
    auto serviceFatherToIp = p_serviceListerner->getServiceFatherToIp();
    unordered_set<string> ip = serviceFatherToIp[curServiceFather];
    int retryCount = p_conf->connRetryCount();
    for (auto it = ip.begin();
         it != ip.end() && !Process::isStop() && !p_loadBalance->needReBalance();
         ++it) {
        // It's important to get serviceMap in the loop to find zk's change in real time
        auto serviceMap = p_conf->serviceMap();
        string ipPort = curServiceFather + "/" + (*it);
        /*
        some service father don't have services and we add "" to serviceFatherToIp
        so we need to judge weather It's a legal ipPort
        */
        if (serviceMap.find(ipPort) == serviceMap.end())
            continue;

        ServiceItem item = serviceMap[ipPort];
        int oldStatus = item.status();
        //If the node is STATUS_UNKNOWN or STATUS_OFFLINE, we will ignore it
        if (oldStatus == STATUS_UNKNOWN || oldStatus == STATUS_OFFLINE)
            continue;

        struct in_addr addr;
        item.addr(&addr);
        int curTryTimes = (oldStatus == STATUS_UP) ? 1 : 3;
        int timeout = item.connectTimeout() > 0 ? item.connectTimeout() : 3;
        int status = STATUS_DOWN;
        do {
            LOG(LOG_ERROR, "can not connect to service:%s, current try times:%d, max try times:%d", ipPort.c_str(), curTryTimes, retryCount);
            bool res = _isServiceExist(&addr, item.host(), item.port(), timeout, item.status());
            status = (res) ? STATUS_UP : STATUS_DOWN;
        } while (curTryTimes < retryCount && status == STATUS_DOWN);

        LOG(LOG_INFO, "|checkService| service:%s, old status:%d, new status:%d. Have tried times:%d, max try times:%d", ipPort.c_str(), oldStatus, status, curTryTimes, retryCount);
        if (status != oldStatus) {
            pair<string, int> *updateInfo = new pair<string, int>;
            *updateInfo = make_pair(ipPort, status);
            _workThread->getUpdateThread()->Schedule(updateServiceFunc, (void *)updateInfo);
        }
    }
    return 0;
}

void *CheckThread::ThreadMain() {
    struct timeval when;
    gettimeofday(&when, NULL);
    struct timeval now = when;

    when.tv_sec += (cron_interval_ / 1000);
    when.tv_usec += ((cron_interval_ % 1000 ) * 1000);
    int timeout = cron_interval_;

    while (!Process::isStop() && !p_loadBalance->needReBalance()) {
        if (cron_interval_ > 0 ) {
            gettimeofday(&now, NULL);
            if (when.tv_sec > now.tv_sec || (when.tv_sec == now.tv_sec && when.tv_usec > now.tv_usec)) {
                timeout = (when.tv_sec - now.tv_sec) * 1000 + (when.tv_usec - now.tv_usec) / 1000;
            } else {
                when.tv_sec = now.tv_sec + (cron_interval_ / 1000);
                when.tv_usec = now.tv_usec + ((cron_interval_ % 1000 ) * 1000);
                CronHandle();
                timeout = cron_interval_;
            }
        }
        sleep(timeout);
    }
    return NULL;
}

void CheckThread::CronHandle() {
    auto serviceFathers = p_loadBalance->myServiceFather();
    int serviceFatherNum = serviceFathers.size();
    string curServiceFather = serviceFathers[_service_pos];
    LOG(LOG_INFO, "|checkService| pthread id %x, pthread pos %d, current service father %s", \
        (unsigned int)this->thread_id(), (int)_service_pos, curServiceFather.c_str());

    _tryConnect(curServiceFather);

    if (serviceFatherNum > MAX_THREAD_NUM) {
        _workThread->setHasThread(_service_pos, false);
        _service_pos = _workThread->getAndAddWaitingIndex();
        _workThread->setHasThread(_service_pos, true);
    }
}