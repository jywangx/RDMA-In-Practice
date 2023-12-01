#pragma once

#include <infiniband/verbs.h>
#include <stdlib.h>
#include <queue>
#include <unordered_map>
#include <string>
#include <iostream>
#include <netdb.h>
#include <arpa/inet.h>
#include <linux/types.h>
#include <unistd.h>

class RDMAEpException : public std::exception {
private:
    const char* errorMessage;

public:
    RDMAEpException(std::string msg);
    RDMAEpException(const char* msg);

    const char* what() const noexcept override;
};

class RDMAEndpoint {
private:
    /* Endpoint related members */
    void* buf         = nullptr;
    unsigned int size = -1;
    int txDepth       = -1;
    int rxDepth       = -1;
    std::unordered_map<int, std::queue<ibv_wc>> wcQueueMap;
    enum class EndpointStatus {
        INIT,
        CONN,
        FNLZ,
        FAIL
    } endpointStatus;
    enum class WorkRequestID {
        REP_RECV_WRID = 1,
        REP_SEND_WRID,
    };

    /* IB related members */
    int ibPort         = 1;
    int sendFlags      = IBV_ACCESS_LOCAL_WRITE;
    int gidIdx         = -1;
    ibv_context *ibCtx = nullptr;
    ibv_pd      *ibPD  = nullptr;
    ibv_mr      *ibMR  = nullptr;
    ibv_cq      *ibCQ  = nullptr;
    ibv_qp      *ibQP  = nullptr;
    ibv_mtu      mtu;
    struct ibAddrInfo {
        int lid;
        int qpn;
        int psn;
        union ibv_gid gid;
    } remoteIBAddr = {0}, localIBAddr = {0};

    void connect_qp();
    void wire_gid_to_gid(const char* wgid, union ibv_gid* gid);
    void gid_to_wire_gid(const union ibv_gid* gid, char wgid[]);
    void client_exch_ibaddr(const char* servername, int port);
    void server_exch_ibaddr(int port);

public:
    RDMAEndpoint(std::string deviceName, int gidIdx, void* buf,
                 unsigned int size, int txDepth, int rxDepth, int mtu);

    void connectToPeer(std::string peerHost, int peerPort);

    void postSend();
    void postSend(int wrid);

    int postRecv(int n);
    int postRecv(int n, int wrId);

    void pollCompletion(int tag);
    void pollSendCompletion();
    void pollRecvCompletion();
};
