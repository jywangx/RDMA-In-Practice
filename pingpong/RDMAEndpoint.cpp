#include "RDMAEndpoint.h"

RDMAEpException::RDMAEpException(std::string msg) {
    this->errorMessage = msg.c_str();
}

RDMAEpException::RDMAEpException(const char* msg) : errorMessage(msg) {}

const char* RDMAEpException::what() const noexcept {
    return errorMessage;
}

RDMAEndpoint::RDMAEndpoint(std::string deviceName, int gidIdx, void* buf,
                           unsigned int size, int txDepth, int rxDepth) {
    int                numDevices;
    ibv_device       **devList      = nullptr;
    ibv_device        *ibDev        = nullptr;

    this->buf     = buf;
    this->size    = size;
    this->txDepth = txDepth;
    this->rxDepth = rxDepth;
    this->gidIdx  = gidIdx;

    devList = ibv_get_device_list(&numDevices);
    if (nullptr == devList) {
        this->endpointStatus = EndpointStatus::FAIL;
        throw std::runtime_error("Fail to get ib device list.");
    }

    if (deviceName.empty()) {
        ibDev = devList[0];
    } else {
        for (int i = 0; i < numDevices && devList[i] != nullptr; i++) {
            if (strcmp(ibv_get_device_name(devList[i]), deviceName.c_str()) == 0) {
                ibDev = devList[i];
                break;
            }
        }
    }
    if (ibDev == nullptr) {
        ibv_free_device_list(devList);
        this->endpointStatus = EndpointStatus::FAIL;
        throw std::runtime_error("No IB devices found");
    }
    this->sendFlags |= IBV_SEND_SIGNALED;

    this->ibCtx = ibv_open_device(ibDev);
    if (this->ibCtx == nullptr) {
        ibv_free_device_list(devList);
        this->endpointStatus = EndpointStatus::FAIL;
        throw std::runtime_error("Fail to open IB device");
    }

    /* 分配Protection Domain */
    this->ibPD = ibv_alloc_pd(this->ibCtx);
    if (nullptr == this->ibPD) {
        ibv_close_device(this->ibCtx);
        ibv_free_device_list(devList);
        this->endpointStatus = EndpointStatus::FAIL;
        throw std::runtime_error("Couldn't allocate PD");
    }

    /* 注册Memory Region */
    this->ibMR = ibv_reg_mr(this->ibPD, buf, size, IBV_ACCESS_LOCAL_WRITE);
    if (nullptr == this->ibMR) {
        ibv_dealloc_pd(ibPD);
        ibv_close_device(this->ibCtx);
        ibv_free_device_list(devList);
        this->endpointStatus = EndpointStatus::FAIL;
        throw std::runtime_error("Couldn't register MR");
    }

    /* 注册Completion Queue */
    this->ibCQ = ibv_create_cq(this->ibCtx, rxDepth + 1, NULL, NULL, 0);
    if (nullptr == this->ibCQ) {
        ibv_dereg_mr(this->ibMR);
        ibv_dealloc_pd(this->ibPD);
        ibv_close_device(this->ibCtx);
        ibv_free_device_list(devList);
        this->endpointStatus = EndpointStatus::FAIL;
        throw std::runtime_error("Couldn't create CQ");
    }

    /* 创建Queue Pair */
    {
        struct ibv_qp_attr attr;
        struct ibv_qp_init_attr init_attr = {
            .send_cq = this->ibCQ,
            .recv_cq = this->ibCQ,
            .cap     = {
                .max_send_wr  = 1,
                .max_recv_wr  = rxDepth,
                .max_send_sge = 1,
                .max_recv_sge = 1
            },
            .qp_type = IBV_QPT_RC
        };

        this->ibQP = ibv_create_qp(this->ibPD, &init_attr);

        if (nullptr == this->ibQP)  {
            ibv_destroy_cq(this->ibCQ);
            ibv_dereg_mr(this->ibMR);
            ibv_dealloc_pd(this->ibPD);
            ibv_close_device(this->ibCtx);
            ibv_free_device_list(devList);
            this->endpointStatus = EndpointStatus::FAIL;
            throw std::runtime_error("Couldn't create QP");
        }

        ibv_query_qp(this->ibQP, &attr, IBV_QP_CAP, &init_attr);
        if (init_attr.cap.max_inline_data >= size)
            this->sendFlags |= IBV_SEND_INLINE;
    }

    /* 改变QP状态到INIT */
    struct ibv_qp_attr attr = {
        .qp_state        = IBV_QPS_INIT,
        .qp_access_flags = 0,
        .pkey_index      = 0,
        .port_num        = this->ibPort
    };

    if (ibv_modify_qp(this->ibQP, &attr,
            IBV_QP_STATE              |
            IBV_QP_ACCESS_FLAGS       |
            IBV_QP_PKEY_INDEX         |
            IBV_QP_PORT               )) {
        ibv_destroy_cq(this->ibCQ);
        ibv_dereg_mr(this->ibMR);
        ibv_dealloc_pd(this->ibPD);
        ibv_close_device(this->ibCtx);
        ibv_free_device_list(devList);
        this->endpointStatus = EndpointStatus::FAIL;
        throw std::runtime_error("Failed to modify QP to INIT");
    }

    /* 先给Receive Queue里面提交足够多的请求if (server_name.empty())  */
    {
        struct ibv_sge list = {
            .addr	= (uintptr_t) this->buf,
            .length = this->size,
            .lkey	= this->ibMR->lkey
        };
        struct ibv_recv_wr wr = {
            .wr_id	    = static_cast<int>(WorkRequestID::REP_RECV_WRID),
            .sg_list    = &list,
            .num_sge    = 1,
        };
        struct ibv_recv_wr *bad_wr;
        int i;

        for (i = 0; i < this->rxDepth; ++i)
            if (ibv_post_recv(this->ibQP, &wr, &bad_wr))
                break;

        if (i < rxDepth) {
            throw std::runtime_error(
                "Couldn't post receive (" + std::to_string(i) + ")"
                );
        }
    }

    this->endpointStatus = EndpointStatus::INIT;
}

RDMAEndpoint::~RDMAEndpoint() {
    if (this->endpointStatus != EndpointStatus::FAIL) {
        ibv_destroy_qp(this->ibQP);
        ibv_destroy_cq(this->ibCQ);
        ibv_dereg_mr(this->ibMR);
        ibv_dealloc_pd(this->ibPD);
        ibv_close_device(this->ibCtx);
    }
}

void RDMAEndpoint::connect_qp() {
    ibv_qp_attr attr = {
        .qp_state		    = IBV_QPS_RTR,
        .path_mtu		    = IBV_MTU_1024,
        .rq_psn			    = this->remoteIBAddr.psn,
        .dest_qp_num	    = this->remoteIBAddr.qpn,
        .ah_attr		    = {
            .dlid		    = this->remoteIBAddr.lid,
            .sl		        = 0,
            .src_path_bits  = 0,
            .is_global	    = 0,
            .port_num	    = this->ibPort
        },
        .max_dest_rd_atomic	= 1,
        .min_rnr_timer		= 12
    };

    if (this->remoteIBAddr.gid.global.interface_id) {
        attr.ah_attr.is_global      = 1;
        attr.ah_attr.grh.hop_limit  = 1;
        attr.ah_attr.grh.dgid       = this->remoteIBAddr.gid;
        attr.ah_attr.grh.sgid_index = this->gidIdx;
    }
    if (ibv_modify_qp(this->ibQP, &attr,
            IBV_QP_STATE              |
            IBV_QP_AV                 |
            IBV_QP_PATH_MTU           |
            IBV_QP_DEST_QPN           |
            IBV_QP_RQ_PSN             |
            IBV_QP_MAX_DEST_RD_ATOMIC |
            IBV_QP_MIN_RNR_TIMER)) {
        throw std::runtime_error("Failed to modify QP to RTR.");
    }

    attr.qp_state	    = IBV_QPS_RTS;
    attr.timeout	    = 14;
    attr.retry_cnt	    = 7;
    attr.rnr_retry	    = 7;
    attr.sq_psn	        = this->localIBAddr.psn;
    attr.max_rd_atomic  = 1;
    if (ibv_modify_qp(this->ibQP, &attr,
            IBV_QP_STATE              |
            IBV_QP_TIMEOUT            |
            IBV_QP_RETRY_CNT          |
            IBV_QP_RNR_RETRY          |
            IBV_QP_SQ_PSN             |
            IBV_QP_MAX_QP_RD_ATOMIC)) {
        throw std::runtime_error("Failed to modify QP to RTS.");
    }
}

void RDMAEndpoint::wire_gid_to_gid(const char* wgid, union ibv_gid* gid) {
    char tmp[9];
    __be32 v32;
    int i;
    uint32_t tmp_gid[4];

    for (tmp[8] = 0, i = 0; i < 4; ++i) {
        memcpy(tmp, wgid + i * 8, 8);
        sscanf(tmp, "%x", &v32);
        tmp_gid[i] = be32toh(v32);
    }
    memcpy(gid, tmp_gid, sizeof(*gid));
}

void RDMAEndpoint::gid_to_wire_gid(const union ibv_gid* gid, char wgid[]) {
    uint32_t tmp_gid[4];
    int i;

    memcpy(tmp_gid, gid, sizeof(tmp_gid));
    for (i = 0; i < 4; ++i)
        sprintf(&wgid[i * 8], "%08x", htobe32(tmp_gid[i]));
}

void RDMAEndpoint::client_exch_ibaddr(const char* servername, int port) {
    struct addrinfo *res, *t;
    struct addrinfo hints = {
        .ai_family   = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM
    };
    char *service;
    char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
    int n;
    int sockfd = -1;
    char gid_str[33];

    if (asprintf(&service, "%d", port) < 0)
        throw std::runtime_error("Fail to allocate buffer for port.");

    n = getaddrinfo(servername, service, &hints, &res);

    if (n < 0) {
        free(service);
        throw std::runtime_error(
            std::string(gai_strerror(n)) + " for " + servername 
            + ":" + std::to_string(port)
        );
    }

    for (t = res; t; t = t->ai_next) {
        sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (sockfd >= 0) {
            if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
                break;
            close(sockfd);
            sockfd = -1;
        }
    }

    freeaddrinfo(res);
    free(service);

    if (sockfd < 0) {
        throw std::runtime_error(
            "Couldn't connect to " + std::string(servername) 
            + ": " + std::to_string(port)
        );
    }

    gid_to_wire_gid(&this->localIBAddr.gid, gid_str);
    sprintf(
        msg, "%04x:%06x:%06x:%s", 
        this->localIBAddr.lid, this->localIBAddr.qpn,
        this->localIBAddr.psn, gid_str
    );
    if (write(sockfd, msg, sizeof msg) != sizeof msg) {
        close(sockfd);
        throw std::runtime_error("Couldn't send local address.");
    }

    if (read(sockfd, msg, sizeof msg) != sizeof msg) {
        close(sockfd);
        throw std::runtime_error("Couldn't read remote address.");
    }

    sscanf(msg, "%x:%x:%x:%s", &this->remoteIBAddr.lid, &this->remoteIBAddr.qpn,
                        &this->remoteIBAddr.psn, gid_str);
    wire_gid_to_gid(gid_str, &this->remoteIBAddr.gid);

    try {
        connect_qp();
    } catch(const std::runtime_error& re) {
        std::cerr << re.what() << '\n';
        this->remoteIBAddr = {0};
        throw std::runtime_error("Couldn't connect to remote QP.");
    }

    if (write(sockfd, "done", sizeof "done") != sizeof "done") {
        close(sockfd);
        throw std::runtime_error("Couldn't write remote address.");
    }

    close(sockfd);
}

void RDMAEndpoint::server_exch_ibaddr(int port) {
    struct addrinfo *res, *t;
    struct addrinfo hints = {
        .ai_flags    = AI_PASSIVE,
        .ai_family   = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM
    };
    char *service;
    char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
    int n;
    int sockfd = -1, connfd;
    char gid_str[33];

    if (asprintf(&service, "%d", port) < 0)
        throw std::runtime_error("Fail to allocate buffer for port.");

    n = getaddrinfo(NULL, service, &hints, &res);

    if (n < 0) {
        free(service);
        throw std::runtime_error(
            std::string(gai_strerror(n)) + " for port " 
            + std::to_string(port) + "."
        );
    }

    for (t = res; t; t = t->ai_next) {
        sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (sockfd >= 0) {
            n = 1;

            setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

            if (!bind(sockfd, t->ai_addr, t->ai_addrlen))
                break;
            close(sockfd);
            sockfd = -1;
        }
    }
    freeaddrinfo(res);
    free(service);

    if (sockfd < 0) {
        throw std::runtime_error(
            "Couldn't listen to port: " + std::to_string(port)
        );
    }

    listen(sockfd, 1);
    connfd = accept(sockfd, NULL, NULL);
    close(sockfd);
    if (connfd < 0) {
        throw std::runtime_error("accept() failed.");
    }

    n = read(connfd, msg, sizeof msg);
    if (n != sizeof msg) {
        close(connfd);
        throw std::runtime_error("Couldn't read remote address.");
    }

    sscanf(
        msg, "%x:%x:%x:%s", 
        &this->remoteIBAddr.lid, &this->remoteIBAddr.qpn,
        &this->remoteIBAddr.psn, gid_str
    );
    wire_gid_to_gid(gid_str, &this->remoteIBAddr.gid);

    try {
        connect_qp();
    } catch(const std::runtime_error& re) {
        std::cerr << re.what() << '\n';
        close(connfd);
        this->remoteIBAddr = {0};
        throw std::runtime_error("Couldn't connect to remote QP.");
    }

    gid_to_wire_gid(&this->localIBAddr.gid, gid_str);
    sprintf(
        msg, "%04x:%06x:%06x:%s", 
        this->localIBAddr.lid, this->localIBAddr.qpn,
        this->localIBAddr.psn, gid_str
    );
    if (write(connfd, msg, sizeof msg) != sizeof msg ||
        read(connfd, msg, sizeof msg) != sizeof "done") {
        close(connfd);
        this->remoteIBAddr = {0};
        throw std::runtime_error("Couldn't send/recv local address.");
    }
    close(connfd);
}

void RDMAEndpoint::connectToPeer(std::string peerHost, int peerPort) {
    /* 获取自己qp的信息 */
    if (this->endpointStatus != EndpointStatus::INIT) {
        throw std::runtime_error("Endpoint status is not INIT.");
    }
    char               gidStr[33];
    ibv_port_attr      port_attr;
    if (ibv_query_port(this->ibCtx, this->ibPort, &port_attr) != 0) {
        throw std::runtime_error("Couldn't get port info.");
    }
    /* lid */
    this->localIBAddr.lid = port_attr.lid;
    if (port_attr.link_layer != IBV_LINK_LAYER_ETHERNET && !port_attr.lid) {
        throw std::runtime_error("Couldn't get local LID.");
    }
    /* gid */
    if (this->gidIdx >= 0) {
        if (0 != ibv_query_gid(
            this->ibCtx, this->ibPort, this->gidIdx, &this->localIBAddr.gid
            )) {
            throw std::runtime_error(
                "can't read sgid of index " + std::to_string(this->gidIdx)
            );
        }
    } else {
        memset(&this->localIBAddr.gid, 0, sizeof(this->localIBAddr.gid));
    }
    /* qp序号和Packet Sequence Number */
    this->localIBAddr.qpn = this->ibQP->qp_num;
    this->localIBAddr.psn = lrand48() & 0xffffff;

    /* 交换ib地址信息 */
    inet_ntop(AF_INET6, &this->localIBAddr.gid, gidStr, sizeof gidStr);
    printf(
        "  local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
        this->localIBAddr.lid, this->localIBAddr.qpn, 
        this->localIBAddr.psn, gidStr
    );
    if (peerHost == "") {
        server_exch_ibaddr(peerPort);
    } else {
        client_exch_ibaddr(peerHost.c_str(), peerPort);
    }
    printf("  connected.\n");
    inet_ntop(AF_INET6, &this->remoteIBAddr.gid, gidStr, sizeof gidStr);
    printf(
        "  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
        this->remoteIBAddr.lid, this->remoteIBAddr.qpn,
            this->remoteIBAddr.psn, gidStr
    );
    this->endpointStatus = EndpointStatus::CONN;
}

void RDMAEndpoint::postSend() {
    postSend(static_cast<int>(WorkRequestID::REP_SEND_WRID));
}

void RDMAEndpoint::postSend(int wrid) {
    int ret;
    struct ibv_sge list = {
        .addr	= (uintptr_t) this->buf,
        .length = this->size,
        .lkey	= this->ibMR->lkey
    };
    struct ibv_send_wr wr = {
        .wr_id	    = wrid,
        .sg_list    = &list,
        .num_sge    = 1,
        .opcode     = IBV_WR_SEND,
        .send_flags = this->sendFlags,
    };
    struct ibv_send_wr *bad_wr;

    ret = ibv_post_send(this->ibQP, &wr, &bad_wr);
    if (ret != 0) {
        throw std::runtime_error("Couldn't post send.");
    }
}

int RDMAEndpoint::postRecv(int n) {
    return postRecv(n, static_cast<int>(WorkRequestID::REP_RECV_WRID));
}

int RDMAEndpoint::postRecv(int n, int wrId) {
    if (this->endpointStatus != EndpointStatus::CONN) {
        throw std::runtime_error("Endpoint status is not CONN.");
    }
    struct ibv_sge list = {
        .addr	= (uintptr_t) this->buf,
        .length = this->size,
        .lkey	= this->ibMR->lkey
    };
    struct ibv_recv_wr wr = {
        .wr_id	    = wrId,
        .sg_list    = &list,
        .num_sge    = 1,
    };
    struct ibv_recv_wr *bad_wr;
    int i;

    for (i = 0; i < n; ++i)
        if (ibv_post_recv(this->ibQP, &wr, &bad_wr))
            break;

    return i;
}

void RDMAEndpoint::pollCompletion(int tag) {
    if (this->endpointStatus != EndpointStatus::CONN) {
        throw std::runtime_error("Endpoint status is not CONN.");
    }
    if (this->wcQueueMap.find(tag) == this->wcQueueMap.end()) {
        this->wcQueueMap[tag] = std::queue<ibv_wc>();
    }
    if (!this->wcQueueMap[tag].empty()) {
        this->wcQueueMap[tag].pop();
        return;
    }
    int ne, wcSize = 5;
    ibv_wc wc, wcs[wcSize];
    do {
        ne = ibv_poll_cq(this->ibCQ, wcSize, wcs);
        if (ne < 0) {
            throw std::runtime_error("poll CQ failed " + std::to_string(ne));
        }
        for (int i = 0; i < ne; i ++) {
            if (this->wcQueueMap.find(wcs[i].wr_id) == this->wcQueueMap.end()) {
                this->wcQueueMap[wcs[i].wr_id] = std::queue<ibv_wc>();
            }
            this->wcQueueMap[wcs[i].wr_id].push(wcs[i]);
        }
    } while (this->wcQueueMap[tag].empty());
    wc = this->wcQueueMap[tag].front();
    this->wcQueueMap[tag].pop();
    if (wc.status != IBV_WC_SUCCESS) {
        throw RDMAEpException(
            "Failed status " + std::string(ibv_wc_status_str(wc.status))
            + " (" + std::to_string(wc.status) + ") for wr_id " 
            + std::to_string(wc.wr_id)
        );
    }
}

void RDMAEndpoint::pollSendCompletion() {
    pollCompletion(static_cast<int>(WorkRequestID::REP_SEND_WRID));
}

void RDMAEndpoint::pollRecvCompletion() {
    pollCompletion(static_cast<int>(WorkRequestID::REP_RECV_WRID));
}