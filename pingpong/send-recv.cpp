#include <stdio.h>
#include <getopt.h>
#include <stdlib.h>
#include "RDMAEndpoint.h"

static void usage(const char *argv0) {
    printf("Usage:\n");
	printf("  %s            start a server and wait for connection\n", argv0);
	printf("  %s <host>     connect to server at <host>\n", argv0);
	printf("\n");
	printf("Options:\n");
	printf("  -p, --port=<port>      listen on/connect to port <port> (default 18515)\n");
	printf("  -m, --mtu=<size>       path MTU (default 1024)\n");
	printf("  -d, --ib-dev=<dev>     use IB device <dev> (default first device found)\n");
	printf("  -i, --ib-port=<port>   use port <port> of IB device (default 1)\n");
	printf("  -l, --sl=<sl>          service level value\n");
	printf("  -e, --events           sleep on CQ events (default poll)\n");
	printf("  -o, --odp              use on demand paging\n");
}

int main(int argc, char *argv[]) {
    int          ret           = 0;
    int          sl            = 0;
    int          mtu           = 1024;
    int          port          = 18515;
  	int   	     gid_idx       = 0;
    bool         use_odp       = false;
    bool         use_event     = false;
    unsigned int size          = 8;
    void        *buf           = nullptr;  
    std::string  ib_devname    = "";
    std::string  server_name   = "";

    srand48(getpid() * time(NULL));

    /* 处理输入 */
    while (true) {
        int c;

        static struct option long_options[] = {
			{ .name = "port",     .has_arg = 1, .val = 'p' },
			{ .name = "mtu",      .has_arg = 1, .val = 'm' },
			{ .name = "ib-dev",   .has_arg = 1, .val = 'd' },
			{ .name = "gid-idx",  .has_arg = 1, .val = 'g' },
			{ .name = "sl",       .has_arg = 1, .val = 'l' },
			{ .name = "events",   .has_arg = 0, .val = 'e' },
			{ .name = "odp",      .has_arg = 0, .val = 'o' },
			{}
		};

        c = getopt_long(argc, argv, "p:d:i:g:m:l:eo", long_options, NULL);
        if (c == -1) break;

        switch (c) {
        case 'p':
            port = strtol(optarg, NULL, 0);
            break;
        case 'm':
			mtu = strtol(optarg, NULL, 0);
			break;
        case 'd':
            ib_devname = optarg;
            break;
        case 'l':
            sl = strtol(optarg, NULL, 0);
            break;
		case 'g':
			gid_idx = strtol(optarg, NULL, 0);
			break;
		case 'o':
			use_odp = true;
			break;
		case 'e':
			use_event = true;
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
    /* 分配页对齐的内存 */
    buf = aligned_alloc(page_size, size);
    if (buf == nullptr) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
        return 1;
	}
    memset(buf, 0x7b, size);

    RDMAEndpoint ep = RDMAEndpoint(
        ib_devname, gid_idx, buf, size, 1, 5, 
        mtu, sl, use_odp, use_event
    );

    ep.connectToPeer(server_name, port);
    printf("Connected to peer.\n");
    if (server_name.empty()) {
        ep.postRecv(1);
        ep.pollRecvCompletion();
        printf("Recv from client: %s\n", buf);
        memset(buf, 0x5b, size);
        ep.postSend();
        ep.pollSendCompletion();
        printf("Send to client: %s\n", buf);
    } else {
        ep.postSend();
        ep.pollSendCompletion();
        printf("Send to server: %s\n", buf);
        ep.postRecv(1);
        ep.pollRecvCompletion();
        printf("Recv from server: %s\n", buf);
    }

    free(buf);

    return ret;
}