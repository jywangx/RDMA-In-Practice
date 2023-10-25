#include <stdio.h>
#include <getopt.h>
#include <stdlib.h>
#include <string>
#include <unistd.h>
#include <infiniband/verbs.h>

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

    int                port;
    int                ib_port;
    int                num_devices;
    size_t             size        = 8;
    void              *buf         = nullptr;
    std::string        ib_devname  = nullptr;
    std::string        server_name = nullptr;
    ibv_device       **dev_list    = nullptr;
    ibv_device        *ib_dev      = nullptr;
    ibv_context       *ib_ctx      = nullptr;
    ibv_pd            *ib_pd;

    /* 处理输入 */
    while (true) {
        int c;

        static struct option long_options[] = {
			{ .name = "port",     .has_arg = 1, .val = 'p' },
			{ .name = "ib-dev",   .has_arg = 1, .val = 'd' },
			{ .name = "ib-port",  .has_arg = 1, .val = 'i' },
			{}
		};

        c = getopt_long(argc, argv, "p:d:i:", long_options, NULL);
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

    /* 分配Protection Domain */
    ib_pd = ibv_alloc_pd(ib_ctx);
    if (ib_pd == nullptr) {
        fprintf(stderr, "Couldn't allocate PD\n");
        goto clear_and_ret;
    }


clear_and_ret:
    return ret;
}