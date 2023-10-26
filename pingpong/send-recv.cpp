#include <stdio.h>
#include <getopt.h>
#include <stdlib.h>
#include <string>
#include <unistd.h>
#include <linux/types.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

enum {
	PINGPONG_RECV_WRID = 1,
	PINGPONG_SEND_WRID = 2,
};

struct ibaddr_info {
	int lid;
	int qpn;
	int psn;
	union ibv_gid gid;
};

void wire_gid_to_gid(const char *wgid, union ibv_gid *gid)
{
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

void gid_to_wire_gid(const union ibv_gid *gid, char wgid[])
{
	uint32_t tmp_gid[4];
	int i;

	memcpy(tmp_gid, gid, sizeof(tmp_gid));
	for (i = 0; i < 4; ++i)
		sprintf(&wgid[i * 8], "%08x", htobe32(tmp_gid[i]));
}

static int connect_qp(const ibaddr_info *remote_ibaddr, 
                      const ibaddr_info *local_ibaddr, 
                      int ib_port, int gid_idx, ibv_qp *ib_qp)
{
    ibv_qp_attr attr = {
        .qp_state		    = IBV_QPS_RTR,
        .path_mtu		    = IBV_MTU_1024,
        .rq_psn			    = remote_ibaddr->psn,
        .dest_qp_num	    = remote_ibaddr->qpn,
        .ah_attr		    = {
            .dlid		    = remote_ibaddr->lid,
            .sl		        = 0,
            .src_path_bits  = 0,
            .is_global	    = 0,
            .port_num	    = ib_port
        },
        .max_dest_rd_atomic	= 1,
        .min_rnr_timer		= 12
    };

    if (remote_ibaddr->gid.global.interface_id) {
        attr.ah_attr.is_global      = 1;
        attr.ah_attr.grh.hop_limit  = 1;
        attr.ah_attr.grh.dgid       = remote_ibaddr->gid;
        attr.ah_attr.grh.sgid_index = gid_idx;
    }
    if (ibv_modify_qp(ib_qp, &attr,
            IBV_QP_STATE              |
            IBV_QP_AV                 |
            IBV_QP_PATH_MTU           |
            IBV_QP_DEST_QPN           |
            IBV_QP_RQ_PSN             |
            IBV_QP_MAX_DEST_RD_ATOMIC |
            IBV_QP_MIN_RNR_TIMER)) {
        fprintf(stderr, "Failed to modify QP to RTR\n");
        return 1;
    }

    attr.qp_state	    = IBV_QPS_RTS;
    attr.timeout	    = 14;
    attr.retry_cnt	    = 7;
    attr.rnr_retry	    = 7;
    attr.sq_psn	        = local_ibaddr->psn;
    attr.max_rd_atomic  = 1;
    if (ibv_modify_qp(ib_qp, &attr,
            IBV_QP_STATE              |
            IBV_QP_TIMEOUT            |
            IBV_QP_RETRY_CNT          |
            IBV_QP_RNR_RETRY          |
            IBV_QP_SQ_PSN             |
            IBV_QP_MAX_QP_RD_ATOMIC)) {
        fprintf(stderr, "Failed to modify QP to RTS\n");
        return 1;
    }
    return 0;
}

static ibaddr_info *client_exch_ibaddr(const char *servername, int port,
						 const ibaddr_info *local_ibaddr)
{
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	int n;
	int sockfd = -1;
	ibaddr_info *remote_ibaddr = nullptr;
	char gid_str[33];

	if (asprintf(&service, "%d", port) < 0)
		return nullptr;

	n = getaddrinfo(servername, service, &hints, &res);

	if (n < 0) {
		fprintf(stderr, "%s for %s:%d\n", gai_strerror(n), servername, port);
		free(service);
		return nullptr;
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
		fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
		return nullptr;
	}

	gid_to_wire_gid(&local_ibaddr->gid, gid_str);
	sprintf(msg, "%04x:%06x:%06x:%s", local_ibaddr->lid, local_ibaddr->qpn,
							local_ibaddr->psn, gid_str);
	if (write(sockfd, msg, sizeof msg) != sizeof msg) {
		fprintf(stderr, "Couldn't send local address\n");
		goto out;
	}

	if (read(sockfd, msg, sizeof msg) != sizeof msg ||
	    write(sockfd, "done", sizeof "done") != sizeof "done") {
		perror("client read/write");
		fprintf(stderr, "Couldn't read/write remote address\n");
		goto out;
	}

	remote_ibaddr = (ibaddr_info *)malloc(sizeof *remote_ibaddr);
	if (remote_ibaddr == nullptr)
		goto out;

	sscanf(msg, "%x:%x:%x:%s", &remote_ibaddr->lid, &remote_ibaddr->qpn,
						&remote_ibaddr->psn, gid_str);
	wire_gid_to_gid(gid_str, &remote_ibaddr->gid);

out:
	close(sockfd);
	return remote_ibaddr;
}

static ibaddr_info *server_exch_ibaddr(int port, int ib_port, int gid_idx, ibv_qp *ib_qp, const ibaddr_info *local_ibaddr)
{
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
	ibaddr_info *remote_ibaddr = nullptr;
	char gid_str[33];

	if (asprintf(&service, "%d", port) < 0)
		return nullptr;

	n = getaddrinfo(NULL, service, &hints, &res);

	if (n < 0) {
		fprintf(stderr, "%s for port %d\n", gai_strerror(n), port);
		free(service);
		return nullptr;
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
		fprintf(stderr, "Couldn't listen to port %d\n", port);
		return nullptr;
	}

	listen(sockfd, 1);
	connfd = accept(sockfd, NULL, NULL);
	close(sockfd);
	if (connfd < 0) {
		fprintf(stderr, "accept() failed\n");
		return nullptr;
	}

	n = read(connfd, msg, sizeof msg);
	if (n != sizeof msg) {
		perror("server read");
		fprintf(stderr, "%d/%d: Couldn't read remote address\n", n, (int) sizeof msg);
		goto out;
	}

	remote_ibaddr = (ibaddr_info *)malloc(sizeof *remote_ibaddr);
	if (!remote_ibaddr)
		goto out;

	sscanf(msg, "%x:%x:%x:%s", &remote_ibaddr->lid, &remote_ibaddr->qpn,
							&remote_ibaddr->psn, gid_str);
	wire_gid_to_gid(gid_str, &remote_ibaddr->gid);

    if (connect_qp(remote_ibaddr, local_ibaddr, ib_port, gid_idx, ib_qp)) {
        fprintf(stderr, "Couldn't connect to remote QP\n");
        free(remote_ibaddr);
        remote_ibaddr = NULL;
        goto out;
    }

    gid_to_wire_gid(&local_ibaddr->gid, gid_str);
    sprintf(msg, "%04x:%06x:%06x:%s", local_ibaddr->lid, local_ibaddr->qpn,
                            local_ibaddr->psn, gid_str);
    if (write(connfd, msg, sizeof msg) != sizeof msg ||
        read(connfd, msg, sizeof msg) != sizeof "done") {
        fprintf(stderr, "Couldn't send/recv local address\n");
        free(remote_ibaddr);
        remote_ibaddr = NULL;
        goto out;
    }

out:
	close(connfd);
	return remote_ibaddr;
}

static void usage(const char *argv0) {
    printf("Usage:\n");
	printf("  %s            start a server and wait for connection\n", argv0);
	printf("  %s <host>     connect to server at <host>\n", argv0);
	printf("\n");
	printf("Options:\n");
	printf("  -p, --port=<port>      listen on/connect to port <port> (default 18515)\n");
	printf("  -d, --ib-dev=<dev>     use IB device <dev> (default first device found)\n");
	printf("  -i, --ib-port=<port>   use port <port> of IB device (default 1)\n");
}

int main(int argc, char *argv[]) {

    int ret = 0;

    int                routs;
    int                num_devices;
    int                port          = 18515;
    int                ib_port       = 1;
    int                gid_idx       = -1;
    int                rx_depth      = 5;
    int                send_flags    = IBV_ACCESS_LOCAL_WRITE;
    int                access_flags  = IBV_ACCESS_LOCAL_WRITE;
    unsigned int       size          = 8;
	char			   gid_str[33];
    void              *buf           = nullptr;  
    std::string        ib_devname    = "";
    std::string        server_name   = "";  
    ibv_device       **dev_list      = nullptr;
    ibv_device        *ib_dev        = nullptr;
    ibv_context       *ib_ctx        = nullptr;
    ibv_pd            *ib_pd         = nullptr;
    ibv_mr            *ib_mr         = nullptr;
    ibv_cq            *ib_cq         = nullptr;
    ibv_qp            *ib_qp         = nullptr;
    ibaddr_info       *remote_ibaddr = nullptr;
    ibv_port_attr      port_attr;
    ibaddr_info        local_ibaddr;

    srand48(getpid() * time(NULL));

    /* 处理输入 */
    while (true) {
        int c;

        static struct option long_options[] = {
			{ .name = "port",     .has_arg = 1, .val = 'p' },
			{ .name = "ib-dev",   .has_arg = 1, .val = 'd' },
			{ .name = "ib-port",  .has_arg = 1, .val = 'i' },
			{ .name = "gid-idx",  .has_arg = 1, .val = 'g' },
			{}
		};

        c = getopt_long(argc, argv, "p:d:i:g:", long_options, NULL);
        if (c == -1) break;

        switch (c) {
        case 'p':
            port = strtol(optarg, NULL, 0);
            break;
        case 'd':
            ib_devname = optarg;
            break;
        case 'i':
            ib_port = strtol(optarg, NULL, 0);
            if (ib_port < 0) {
                usage(argv[0]);
                return 1;
            }
            break;
		case 'g':
			gid_idx = strtol(optarg, NULL, 0);
			break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    /* 若是client，读取server的名称或ip */
    if (optind == argc - 1) {
        // client
        server_name = argv[optind];
    } else if (optind == argc) {
        // server
    } else {
        usage(argv[0]);
        return 1;
    }

    int page_size = sysconf(_SC_PAGESIZE);

    /* 解析设备列表并选择设备 */
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list) {
        perror("Failed to get IB devices list");
        return 1;
    }
    if (ib_devname.empty()) {
        ib_dev = dev_list[0];
        if (ib_dev == nullptr) {
            fprintf(stderr, "No IB devices found\n");
            return 1;
        }
    } else {
        for (int i = 0; i < num_devices && dev_list[i] != nullptr; i++) {
            if (strcmp(ibv_get_device_name(dev_list[i]), ib_devname.c_str()) == 0) {
                ib_dev = dev_list[i];
                break;
            }
        }
        if (ib_dev == nullptr) {
            fprintf(stderr, "No IB devices found\n");
            return 1;
        }
    }
    send_flags = IBV_SEND_SIGNALED;
    /* 分配页对齐的内存 */
    buf = aligned_alloc(page_size, size);
    if (buf == nullptr) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		goto clear_and_ret;
	}
    memset(buf, 0x7b, size);

    /* 打开ib设备，初始化上下文ibv_context */
    ib_ctx = ibv_open_device(ib_dev);
    if (ib_ctx == nullptr) {
        fprintf(stderr, "Couldn't get context for %s\n", ibv_get_device_name(ib_dev));
        goto clear_and_ret;
    }
    fprintf(stdout, "Get context for %s\n", ibv_get_device_name(ib_dev));

    /* 分配Protection Domain */
    ib_pd = ibv_alloc_pd(ib_ctx);
    if (ib_pd == nullptr) {
        fprintf(stderr, "Couldn't allocate PD\n");
        goto clear_and_ret;
    }

    /* 注册Memory Region */
    ib_mr = ibv_reg_mr(ib_pd, buf, size, access_flags);
    if (ib_mr == nullptr) {
		fprintf(stderr, "Couldn't register MR\n");
		goto clear_and_ret;
	}

    /* 注册Completion Queue */
    ib_cq = ibv_create_cq(ib_ctx, rx_depth + 1, NULL, NULL, 0);
    if (ib_cq == nullptr) {
		fprintf(stderr, "Couldn't create CQ\n");
		goto clear_and_ret;
	}

    /* 创建Queue Pair */
    {
		struct ibv_qp_attr attr;
		struct ibv_qp_init_attr init_attr = {
			.send_cq = ib_cq,
			.recv_cq = ib_cq,
			.cap     = {
				.max_send_wr  = 1,
				.max_recv_wr  = rx_depth,
				.max_send_sge = 1,
				.max_recv_sge = 1
			},
			.qp_type = IBV_QPT_RC
		};

		ib_qp = ibv_create_qp(ib_pd, &init_attr);

		if (ib_qp == nullptr)  {
			fprintf(stderr, "Couldn't create QP\n");
			goto clear_and_ret;
		}

		ibv_query_qp(ib_qp, &attr, IBV_QP_CAP, &init_attr);
		if (init_attr.cap.max_inline_data >= size)
			send_flags |= IBV_SEND_INLINE;
	}

    /* 改变QP状态到INIT */
	{
		struct ibv_qp_attr attr = {
			.qp_state        = IBV_QPS_INIT,
			.qp_access_flags = 0,
			.pkey_index      = 0,
			.port_num        = ib_port
		};

		if (ibv_modify_qp(ib_qp, &attr,
				  IBV_QP_STATE              |
				  IBV_QP_ACCESS_FLAGS       |
				  IBV_QP_PKEY_INDEX         |
				  IBV_QP_PORT               )) {
			fprintf(stderr, "Failed to modify QP to INIT\n");
            perror("ibv_modify_qp");
			goto clear_and_ret;
		}
	}

    /* 先给Receive Queue里面提交足够多的请求if (server_name.empty())  */
    {
        struct ibv_sge list = {
            .addr	= (uintptr_t) buf,
            .length = size,
            .lkey	= ib_mr->lkey
        };
        struct ibv_recv_wr wr = {
            .wr_id	    = PINGPONG_RECV_WRID,
            .sg_list    = &list,
            .num_sge    = 1,
        };
        struct ibv_recv_wr *bad_wr;
        int i;

        for (i = 0; i < rx_depth; ++i)
            if (ibv_post_recv(ib_qp, &wr, &bad_wr))
                break;

        routs = i;
        if (routs < rx_depth) {
            fprintf(stderr, "Couldn't post receive (%d)\n", routs);
            return 1;
        }
        printf("routs %d\n", routs);
    }

    /* 获取自己qp的信息 */
    ret = ibv_query_port(ib_ctx, ib_port, &port_attr);
    if (ret != 0) {
        fprintf(stderr, "Couldn't get port info\n");
		return 1;
    }
    /* lid */
    local_ibaddr.lid = port_attr.lid;
    if (port_attr.link_layer != IBV_LINK_LAYER_ETHERNET && !port_attr.lid) {
        fprintf(stderr, "Couldn't get local LID\n");
    }
    /* gid */
    if (gid_idx >= 0) {
        ret = ibv_query_gid(ib_ctx, ib_port, gid_idx, &local_ibaddr.gid);
        if (ret != 0) {
            fprintf(stderr, "can't read sgid of index %d\n", gid_idx);
            return 1;
        }
    } else {
        memset(&local_ibaddr.gid, 0, sizeof(local_ibaddr.gid));
    }
    /* qp序号和Packet Sequence Number */
    local_ibaddr.qpn = ib_qp->qp_num;
	local_ibaddr.psn = lrand48() & 0xffffff;

    /* 交换ib地址信息 */
	inet_ntop(AF_INET6, &local_ibaddr.gid, gid_str, sizeof gid_str);
    printf("  local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
	       local_ibaddr.lid, local_ibaddr.qpn, local_ibaddr.psn, gid_str);
    if (server_name.empty()) {
        remote_ibaddr = server_exch_ibaddr(port, ib_port, gid_idx, ib_qp, &local_ibaddr);
    } else {
        remote_ibaddr = client_exch_ibaddr(server_name.c_str(), port, &local_ibaddr);
    }
    if (remote_ibaddr == nullptr)
		return 1;
	inet_ntop(AF_INET6, &remote_ibaddr->gid, gid_str, sizeof gid_str);
	printf("  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
	       remote_ibaddr->lid, remote_ibaddr->qpn, remote_ibaddr->psn, gid_str);
    
    /* ping-pong */
    if (!server_name.empty()) {
        if (connect_qp(remote_ibaddr, &local_ibaddr, ib_port, gid_idx, ib_qp)) return -1;
        /* client提交一个send */
        struct ibv_sge list = {
            .addr	= (uintptr_t) buf,
            .length = size,
            .lkey	= ib_mr->lkey
        };
        struct ibv_send_wr wr = {
            .wr_id	    = PINGPONG_SEND_WRID,
            .sg_list    = &list,
            .num_sge    = 1,
            .opcode     = IBV_WR_SEND,
            .send_flags = send_flags,
        };
        struct ibv_send_wr *bad_wr;

        ret = ibv_post_send(ib_qp, &wr, &bad_wr);
        if (ret != 0) {
			fprintf(stderr, "Couldn't post send\n");
			return 1;
		}
        printf("post send msg:%s\n", (char *)buf);

        /* client等待send完成 */
        int ne;
        ibv_wc wc;
        do {
            ne = ibv_poll_cq(ib_cq, 1, &wc);
            if (ne < 0) {
                fprintf(stderr, "poll CQ failed %d\n", ne);
				return 1;
            }
        } while (ne < 1);
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                ibv_wc_status_str(wc.status),
                wc.status, (int)wc.wr_id);
		    return 1;
        }
        if (wc.wr_id != PINGPONG_SEND_WRID) {
            fprintf(stderr, "Unexpected recv completion %d\n", (int)wc.wr_id);
		    return 1;
        }
        printf("send complete\n");

        /* client等待server回复 */
        do {
            ne = ibv_poll_cq(ib_cq, 1, &wc);
            if (ne < 0) {
                fprintf(stderr, "poll CQ failed %d\n", ne);
				return 1;
            }
        } while (ne < 1);
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                ibv_wc_status_str(wc.status),
                wc.status, (int)wc.wr_id);
		    return 1;
        }
        if (wc.wr_id != PINGPONG_RECV_WRID) {
            fprintf(stderr, "Unexpected send completion\n");
		    return 1;
        }
        printf("recv msg:%s\n", (char *)buf);
    } else {
        /* server等待client回复 */        
        int ne;
        ibv_wc wc;
        do {
            ne = ibv_poll_cq(ib_cq, 1, &wc);
            if (ne < 0) {
                fprintf(stderr, "poll CQ failed %d\n", ne);
				return 1;
            }
        } while (ne < 1);
        // printf("ne: %d\n", ne);
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                ibv_wc_status_str(wc.status),
                wc.status, (int)wc.wr_id);
		    return 1;
        }
        if (wc.wr_id != PINGPONG_RECV_WRID) {
            fprintf(stderr, "Unexpected send completion\n");
		    return 1;
        }
        printf("recv msg:%s\n", (char *)buf);

        /* server提交一个send */
        memset(buf, 0x7c, size);
        struct ibv_sge list = {
            .addr	= (uintptr_t) buf,
            .length = size,
            .lkey	= ib_mr->lkey
        };
        struct ibv_send_wr wr = {
            .wr_id	    = PINGPONG_SEND_WRID,
            .sg_list    = &list,
            .num_sge    = 1,
            .opcode     = IBV_WR_SEND,
            .send_flags = send_flags,
        };
        struct ibv_send_wr *bad_wr;

        ret = ibv_post_send(ib_qp, &wr, &bad_wr);
        if (ret != 0) {
			fprintf(stderr, "Couldn't post send\n");
			return 1;
		}
        printf("post send msg:%s\n", (char *)buf);
        /* server等待send完成 */
        do {
            ne = ibv_poll_cq(ib_cq, 1, &wc);
            if (ne < 0) {
                fprintf(stderr, "poll CQ failed %d\n", ne);
				return 1;
            }
        } while (ne < 1);
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                ibv_wc_status_str(wc.status),
                wc.status, (int)wc.wr_id);
		    return 1;
        }
        if (wc.wr_id != PINGPONG_SEND_WRID) {
            fprintf(stderr, "Unexpected recv completion\n");
		    return 1;
        }
        printf("send complete\n");
    }

clear_and_ret:

    if (ib_qp != nullptr && ibv_destroy_qp(ib_qp)) {
		fprintf(stderr, "Couldn't destroy QP\n");
		return 1;
	}

	if (ib_cq != nullptr && ibv_destroy_cq(ib_cq)) {
		fprintf(stderr, "Couldn't destroy CQ\n");
		return 1;
	}

	if (ib_mr != nullptr && ibv_dereg_mr(ib_mr)) {
		fprintf(stderr, "Couldn't deregister MR\n");
		return 1;
	}

	if (ib_pd != nullptr && ibv_dealloc_pd(ib_pd)) {
		fprintf(stderr, "Couldn't deallocate PD\n");
		return 1;
	}

	if (ib_ctx != nullptr && ibv_close_device(ib_ctx)) {
		fprintf(stderr, "Couldn't release context\n");
		return 1;
	}
    free(buf);

	ibv_free_device_list(dev_list);

    if (remote_ibaddr != nullptr) {
        free(remote_ibaddr);
    }

    return ret;
}